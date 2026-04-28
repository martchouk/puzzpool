const express = require('express');
const Database = require('better-sqlite3');
const crypto = require('crypto');
const fs = require('fs');

// --- Configuration ---
const PORT            = parseInt(process.env.PORT || '8888', 10);
const DB_PATH         = process.env.DB_PATH || 'pool.db';
const TARGET_MINUTES  = parseInt(process.env.TARGET_MINUTES || '10', 10);
const TIMEOUT_MINUTES = parseInt(process.env.TIMEOUT_MINUTES || '15', 10);
const TARGET_SECTORS  = BigInt(parseInt(process.env.TARGET_SECTORS || '65536', 10));

// Dashboard freshness threshold only. Green = recent heartbeat; gray = visible but stale.
const DEFAULT_ACTIVE_MINUTES = 1.167;
const rawActiveMinutes = parseFloat(process.env.ACTIVE_MINUTES || String(DEFAULT_ACTIVE_MINUTES));
const ACTIVE_MINUTES_REQUESTED =
    Number.isFinite(rawActiveMinutes) && rawActiveMinutes > 0 ? rawActiveMinutes : DEFAULT_ACTIVE_MINUTES;
const ACTIVE_MINUTES = Math.max(0.1, Math.min(ACTIVE_MINUTES_REQUESTED, TIMEOUT_MINUTES / 2));

const REACTIVATE_MINUTES = TIMEOUT_MINUTES;

// --- Allocator strategies ---
const ALLOC_STRATEGY_LEGACY  = 'legacy_random_shards_v1';
const ALLOC_STRATEGY_VCHUNKS = 'virtual_random_chunks_v1';
const DEFAULT_ALLOC_STRATEGY = process.env.DEFAULT_ALLOC_STRATEGY || ALLOC_STRATEGY_VCHUNKS;

// 1 virtual chunk should be ~1 minute on the slowest client.
// Default assumes ~500k keys/s slowest worker => 30,000,000 keys.
const DEFAULT_VIRTUAL_CHUNK_SIZE_KEYS = BigInt(process.env.DEFAULT_VIRTUAL_CHUNK_SIZE_KEYS || '30000000');

// Optional safety valve for existing DBs:
// if enabled, puzzles using virtual chunks and having NO issued non-test chunks yet
// are automatically re-seeded to the current default virtual chunk size on startup /
// first use. This is useful when an older test DB still contains oversized chunks.
const AUTO_RESEED_EMPTY_VCHUNK_PUZZLES = process.env.AUTO_RESEED_EMPTY_VCHUNK_PUZZLES === '1';

// Virtual chunk universe is mathematical, not materialized in DB.
// Still keep counts/indexes within JS safe integer range because they are
// stored in SQLite INTEGER columns and read back into JS numbers.
const MAX_STORED_VCHUNKS = BigInt(Number.MAX_SAFE_INTEGER);

// Allocation does not scan the full universe. It probes a bounded number of
// pseudo-random candidate positions each request.
const MAX_ALLOC_PROBES = parseInt(process.env.MAX_ALLOC_PROBES || '8192', 10);

// GPU batch size kept for admin test-chunk default
const GPU_BATCH_KEYS = 4278190080n;

// --- Pure helpers ---

function isValidHex(s) {
    return typeof s === 'string' && /^(0x)?[0-9a-fA-F]{1,64}$/.test(s);
}

function normalizeHex(s) {
    return BigInt('0x' + s.replace(/^0x/i, '')).toString(16).padStart(64, '0');
}

function randomBigIntInRange(min, max) {
    const range = max - min;
    if (range <= 0n) return min;
    const hexLen = range.toString(16).length;
    const byteLen = Math.ceil(hexLen / 2) + 8;
    const bytes = crypto.randomBytes(byteLen);
    const n = BigInt('0x' + bytes.toString('hex'));
    return min + (n % range);
}

function normalizeHashrate(input, fallback = 1000000) {
    const n = Number(input);
    if (!Number.isFinite(n) || n <= 0) return BigInt(fallback);
    return BigInt(Math.max(1, Math.floor(n)));
}

function sha256Hex(s) {
    return crypto.createHash('sha256').update(String(s)).digest('hex');
}

function bigIntMin(a, b) {
    return a < b ? a : b;
}

function bigIntMax(a, b) {
    return a > b ? a : b;
}

function ceilDiv(a, b) {
    return (a + b - 1n) / b;
}

function roundUpToQuantum(value, quantum) {
    if (quantum <= 1n) return value;
    return ceilDiv(value, quantum) * quantum;
}

function parsePositiveBigInt(value, fallback = null) {
    if (value === undefined || value === null || String(value) === '') return fallback;
    try {
        const n = BigInt(String(value));
        if (n <= 0n) return fallback;
        return n;
    } catch (_) {
        return fallback;
    }
}

function normalizedRange(startHex, endHex) {
    const start = BigInt('0x' + startHex);
    const end   = BigInt('0x' + endHex);
    if (end <= start) throw new Error('Puzzle range must be > 0');
    return { start, end, range: end - start };
}

function gcdBigInt(a, b) {
    a = a < 0n ? -a : a;
    b = b < 0n ? -b : b;
    while (b !== 0n) {
        const t = a % b;
        a = b;
        b = t;
    }
    return a;
}

function deriveAffinePermutationParams(seedHex, n) {
    if (n <= 1n) return { a: 1n, b: 0n };

    let counter = 0n;
    let a = 1n;
    while (true) {
        const ha = sha256Hex(`${seedHex}:a:${counter.toString()}`);
        a = BigInt('0x' + ha) % n;
        if (a === 0n) a = 1n;
        if (gcdBigInt(a, n) === 1n) break;
        counter++;
    }

    const hb = sha256Hex(`${seedHex}:b`);
    const b = BigInt('0x' + hb) % n;

    return { a, b };
}

function permuteIndexAffine(orderIndex, n, a, b) {
    return (a * orderIndex + b) % n;
}

function defaultAllocSeedForPuzzle(puzzle, strategy = ALLOC_STRATEGY_VCHUNKS) {
    return sha256Hex(`${puzzle.name}|${puzzle.start_hex}|${puzzle.end_hex}|${strategy}`);
}

function chooseDefaultVirtualChunkSize(range) {
    let size = DEFAULT_VIRTUAL_CHUNK_SIZE_KEYS > 0n ? DEFAULT_VIRTUAL_CHUNK_SIZE_KEYS : 1n;
    if (size > range) size = range;

    // Keep count storable / safely readable in JS.
    while (ceilDiv(range, size) > MAX_STORED_VCHUNKS) {
        size <<= 1n;
        if (size > range) {
            size = range;
            break;
        }
    }
    return size;
}

function validateVirtualChunkCountOrThrow(chunkCountBig, context) {
    if (chunkCountBig > MAX_STORED_VCHUNKS) {
        throw new Error(
            `${context}: virtual chunk count ${chunkCountBig.toString()} exceeds MAX_STORED_VCHUNKS ` +
            `(${MAX_STORED_VCHUNKS.toString()}). Use a larger virtual_chunk_size_keys value.`
        );
    }
}

function computeWorkerRequestedKeys({ hashrateBig, minChunkKeys, chunkQuantumKeys }) {
    const targetKeys = hashrateBig * BigInt(TARGET_MINUTES * 60);
    const minKeys = minChunkKeys && minChunkKeys > 0n ? minChunkKeys : targetKeys;
    const raw = bigIntMax(targetKeys, minKeys);
    const quantum = chunkQuantumKeys && chunkQuantumKeys > 0n ? chunkQuantumKeys : 1n;
    return roundUpToQuantum(raw, quantum);
}

function parseUtcDateMs(value) {
    if (!value || typeof value !== 'string') return null;
    let s = value.trim();
    if (!s) return null;
    if (!s.includes('T')) s = s.replace(' ', 'T');
    if (!/[zZ]$|[+\-]\d{2}:\d{2}$/.test(s)) s += 'Z';
    const ms = Date.parse(s);
    return Number.isFinite(ms) ? ms : null;
}

function computeWorkerProgressPercent(assignedAt, hashrate, jobKeys) {
    if (!assignedAt || !hashrate || !jobKeys) return null;
    const assignedMs = parseUtcDateMs(assignedAt);
    if (assignedMs === null) return null;
    const elapsedSeconds = (Date.now() - assignedMs) / 1000;
    if (elapsedSeconds < 0) return null;
    const jobKeysBig = typeof jobKeys === 'string' ? BigInt(jobKeys) : BigInt(Math.round(Number(jobKeys)));
    if (jobKeysBig === 0n) return null;
    const scannedEstimate = BigInt(Math.round(hashrate * elapsedSeconds));
    const pct = Number(scannedEstimate * 10000n / jobKeysBig) / 100;
    return Math.min(100, Math.max(0, pct));
}

function normalizeRunStartForCandidate(candidateIndex, neededChunks, totalChunks) {
    if (neededChunks >= totalChunks) return 0;
    let start = candidateIndex;
    if (start + neededChunks > totalChunks) {
        start = totalChunks - neededChunks;
    }
    if (start < 0) start = 0;
    return start;
}

function virtualChunkRangeToHex(puzzle, vchunkStart, vchunkEndExclusive) {
    const pStart = BigInt('0x' + puzzle.start_hex);
    const pEnd   = BigInt('0x' + puzzle.end_hex);
    const size   = BigInt(puzzle.virtual_chunk_size_keys);
    const start  = pStart + BigInt(vchunkStart) * size;
    const end    = bigIntMin(pStart + BigInt(vchunkEndExclusive) * size, pEnd);
    return {
        startHex: start.toString(16).padStart(64, '0'),
        endHex:   end.toString(16).padStart(64, '0'),
    };
}

// --- Legacy sector seeding retained for rollback compatibility ---
function seedSectors(db, puzzleId, startHex, endHex) {
    const MIN_SECTOR_SIZE = 1000000000n;

    const start = BigInt('0x' + startHex);
    const end   = BigInt('0x' + endHex);
    const range = end - start;
    if (range <= 0n) throw new Error('Puzzle range must be > 0');

    let numSectors = range / MIN_SECTOR_SIZE;
    if (numSectors < 1n)             numSectors = 1n;
    if (numSectors > TARGET_SECTORS) numSectors = TARGET_SECTORS;

    const sectorSize = range / numSectors;
    const stmt = db.prepare(
        "INSERT INTO sectors (puzzle_id, start_hex, end_hex, current_hex, status) VALUES (?, ?, ?, ?, 'open')"
    );

    db.transaction(() => {
        for (let i = 0n; i < numSectors; i++) {
            const s = start + i * sectorSize;
            const e = (i === numSectors - 1n) ? end : s + sectorSize;
            const sHex = s.toString(16).padStart(64, '0');
            const eHex = e.toString(16).padStart(64, '0');
            stmt.run(puzzleId, sHex, eHex, sHex);
        }
    })();

    console.log(`[System] Seeded ${numSectors} legacy sectors for puzzle ${puzzleId}`);
}

// --- New allocator seeding: virtual chunks ---
function seedVirtualChunks(db, puzzleId, startHex, endHex, allocSeed, virtualChunkSizeKeys) {
    const { range } = normalizedRange(startHex, endHex);
    const chunkSize = bigIntMin(
        virtualChunkSizeKeys && virtualChunkSizeKeys > 0n ? virtualChunkSizeKeys : chooseDefaultVirtualChunkSize(range),
        range
    );
    const chunkCountBig = ceilDiv(range, chunkSize);
    validateVirtualChunkCountOrThrow(chunkCountBig, 'seedVirtualChunks');
    const chunkCount = Number(chunkCountBig);

    db.transaction(() => {
        db.prepare(`
            UPDATE puzzles
            SET alloc_strategy = ?,
                alloc_seed = ?,
                alloc_cursor = 0,
                virtual_chunk_size_keys = ?,
                virtual_chunk_count = ?,
                bootstrap_stage = 0
            WHERE id = ?
        `).run(
            ALLOC_STRATEGY_VCHUNKS,
            allocSeed,
            chunkSize.toString(),
            chunkCount,
            puzzleId
        );
    })();

    console.log(`[System] Seeded ${chunkCount} virtual chunks for puzzle ${puzzleId} (virtual chunk size ${chunkSize.toString()} keys)`);
}

function ensureAllocatorForPuzzle(db, puzzleId) {
    const puzzle = db.prepare("SELECT * FROM puzzles WHERE id = ?").get(puzzleId);
    if (!puzzle) throw new Error(`Puzzle ${puzzleId} not found`);

    const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;

    if (strategy === ALLOC_STRATEGY_VCHUNKS) {
        const issuedCount = db.prepare(
            "SELECT COUNT(*) AS c FROM chunks WHERE puzzle_id = ? AND is_test = 0"
        ).get(puzzleId).c;

        const seed = puzzle.alloc_seed || defaultAllocSeedForPuzzle(puzzle, ALLOC_STRATEGY_VCHUNKS);
        const { range } = normalizedRange(puzzle.start_hex, puzzle.end_hex);
        const desiredDefaultSize = chooseDefaultVirtualChunkSize(range);

        const size = (() => {
            if (puzzle.virtual_chunk_size_keys) {
                try {
                    const stored = BigInt(puzzle.virtual_chunk_size_keys);
                    const actual = bigIntMin(stored, range);
                    if (ceilDiv(range, actual) <= MAX_STORED_VCHUNKS) {
                        return actual;
                    }
                } catch (_) {}
            }
            return desiredDefaultSize;
        })();

        const expectedCount = ceilDiv(range, size);
        const storedCount = parsePositiveBigInt(puzzle.virtual_chunk_count, null);

        if (!storedCount || storedCount !== expectedCount) {
            seedVirtualChunks(db, puzzleId, puzzle.start_hex, puzzle.end_hex, seed, size);
        } else if (
            AUTO_RESEED_EMPTY_VCHUNK_PUZZLES &&
            issuedCount === 0 &&
            size !== desiredDefaultSize
        ) {
            console.log(
                `[System] Re-seeding empty virtual-chunk puzzle ${puzzleId} ` +
                `from size ${size.toString()} to ${desiredDefaultSize.toString()} keys`
            );
            seedVirtualChunks(db, puzzleId, puzzle.start_hex, puzzle.end_hex, seed, desiredDefaultSize);
        }
    } else {
        const sectorCount = db.prepare("SELECT COUNT(*) AS c FROM sectors WHERE puzzle_id = ?").get(puzzleId).c;
        if (sectorCount === 0) {
            seedSectors(db, puzzle.id, puzzle.start_hex, puzzle.end_hex);
        }
    }
}

// --- App factory ---
function createApp(db) {
    const stmtWorkerHash = db.prepare("SELECT hashrate FROM workers WHERE name = ?");
    const stmtChunkCount = db.prepare("SELECT COUNT(*) as cnt FROM chunks WHERE puzzle_id = ? AND is_test = 0");

    // Legacy allocator statements
    const stmtOpenSector    = db.prepare("SELECT * FROM sectors WHERE puzzle_id = ? AND status = 'open' ORDER BY RANDOM() LIMIT 1");
    const stmtOpenSectorAt  = db.prepare("SELECT * FROM sectors WHERE puzzle_id = ? AND status = 'open' ORDER BY id ASC LIMIT 1 OFFSET ?");
    const stmtSectorDone    = db.prepare("UPDATE sectors SET current_hex = end_hex, status = 'done' WHERE id = ?");
    const stmtSectorAdvance = db.prepare("UPDATE sectors SET current_hex = ? WHERE id = ?");

    // Virtual-chunk allocator statements
    const stmtVChunkPuzzle = db.prepare(`
        SELECT id, name, start_hex, end_hex,
               alloc_strategy, alloc_seed, alloc_cursor,
               virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage,
               test_start_hex, test_end_hex
        FROM puzzles
        WHERE id = ?
    `);

    const stmtVChunkCursorSet = db.prepare(`
        UPDATE puzzles
        SET alloc_cursor = ?
        WHERE id = ?
    `);

    const stmtBootstrapStageSet = db.prepare(`
        UPDATE puzzles
        SET bootstrap_stage = ?
        WHERE id = ?
    `);

    const stmtVChunkAnyOverlap = db.prepare(`
        SELECT 1
        FROM chunks
        WHERE puzzle_id = ?
          AND is_test = 0
          AND status IN ('assigned', 'reclaimed', 'completed', 'FOUND')
          AND vchunk_start IS NOT NULL
          AND vchunk_end IS NOT NULL
          AND vchunk_start < ?
          AND vchunk_end > ?
        LIMIT 1
    `);

    const stmtInsertChunk = db.prepare(`
        INSERT INTO chunks (
            puzzle_id, start_hex, end_hex, status,
            worker_name, assigned_at, heartbeat_at, is_test,
            sector_id, alloc_block_id,
            vchunk_start, vchunk_end
        ) VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 0, NULL, NULL, ?, ?)
    `);

    const stmtTestChunkTaken = db.prepare(`
        SELECT id
        FROM chunks
        WHERE puzzle_id = ? AND start_hex = ? AND end_hex = ? AND is_test = 1 AND status = 'assigned'
        LIMIT 1
    `);

    const stmtTestChunkReclaim = db.prepare(`
        UPDATE chunks
        SET status = 'assigned',
            worker_name = ?,
            assigned_at = CURRENT_TIMESTAMP,
            heartbeat_at = CURRENT_TIMESTAMP
        WHERE id = (
            SELECT id
            FROM chunks
            WHERE puzzle_id = ? AND start_hex = ? AND end_hex = ? AND is_test = 1 AND status = 'reclaimed'
            LIMIT 1
        )
        RETURNING *
    `);

    const stmtTestChunkInsert = db.prepare(`
        INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at, heartbeat_at, is_test)
        VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 1)
        RETURNING *
    `);

    const MIDPOINT_SECTOR = Number(TARGET_SECTORS / 2n);

    const assignLegacyRandomChunk = db.transaction((name, hashrate, puzzle) => {
        const hashrateBig = normalizeHashrate(hashrate || stmtWorkerHash.get(name)?.hashrate);
        const chunkSize   = hashrateBig * BigInt(TARGET_MINUTES * 60);
        const isFirst = stmtChunkCount.get(puzzle.id).cnt === 0;

        for (;;) {
            const sector = isFirst
                ? (stmtOpenSectorAt.get(puzzle.id, MIDPOINT_SECTOR) || stmtOpenSector.get(puzzle.id))
                : stmtOpenSector.get(puzzle.id);
            if (!sector) return null;

            const current   = BigInt('0x' + sector.current_hex);
            const sectorEnd = BigInt('0x' + sector.end_hex);
            const effective = bigIntMin(chunkSize, sectorEnd - current);

            if (effective <= 0n) {
                stmtSectorDone.run(sector.id);
                continue;
            }

            const startHex = current.toString(16).padStart(64, '0');
            const endHex   = (current + effective).toString(16).padStart(64, '0');

            if (current + effective >= sectorEnd) {
                stmtSectorDone.run(sector.id);
            } else {
                stmtSectorAdvance.run(endHex, sector.id);
            }

            const info = db.prepare(`
                INSERT INTO chunks (
                    puzzle_id, start_hex, end_hex, status,
                    worker_name, assigned_at, heartbeat_at, is_test,
                    sector_id, alloc_block_id, vchunk_start, vchunk_end
                ) VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 0, ?, NULL, NULL, NULL)
            `).run(puzzle.id, startHex, endHex, name, sector.id);

            return { chunkId: info.lastInsertRowid, startHex, endHex };
        }
    });

    function rangeIsFree(puzzleId, vchunkStart, vchunkEndExclusive) {
        if (vchunkStart < 0 || vchunkEndExclusive <= vchunkStart) return false;
        return !stmtVChunkAnyOverlap.get(puzzleId, vchunkEndExclusive, vchunkStart);
    }

    function findBeginBootstrapRun(puzzleId, totalChunks, neededChunks) {
        const maxStart = totalChunks - neededChunks;
        if (maxStart < 0) return null;

        const probes = Math.min(maxStart + 1, MAX_ALLOC_PROBES);
        for (let i = 0; i < probes; i++) {
            const start = i;
            if (rangeIsFree(puzzleId, start, start + neededChunks)) return start;
        }
        return null;
    }

    function findEndBootstrapRun(puzzleId, totalChunks, neededChunks) {
        const maxStart = totalChunks - neededChunks;
        if (maxStart < 0) return null;

        const probes = Math.min(maxStart + 1, MAX_ALLOC_PROBES);
        for (let i = 0; i < probes; i++) {
            const start = maxStart - i;
            if (rangeIsFree(puzzleId, start, start + neededChunks)) return start;
        }
        return null;
    }

    function findMidBootstrapRun(puzzleId, totalChunks, neededChunks) {
        if (totalChunks <= 0) return null;

        const anchor = Math.floor(totalChunks / 2);
        const maxStart = totalChunks - neededChunks;
        if (maxStart < 0) return null;

        const tried = new Set();
        const probes = Math.min(totalChunks, MAX_ALLOC_PROBES);

        for (let dist = 0; dist < probes; dist++) {
            const leftAnchor = anchor - dist;
            if (leftAnchor >= 0) {
                const start = normalizeRunStartForCandidate(leftAnchor, neededChunks, totalChunks);
                if (!tried.has(start)) {
                    tried.add(start);
                    if (rangeIsFree(puzzleId, start, start + neededChunks)) return start;
                }
            }

            if (dist === 0) continue;

            const rightAnchor = anchor + dist;
            if (rightAnchor < totalChunks) {
                const start = normalizeRunStartForCandidate(rightAnchor, neededChunks, totalChunks);
                if (!tried.has(start)) {
                    tried.add(start);
                    if (rangeIsFree(puzzleId, start, start + neededChunks)) return start;
                }
            }
        }

        return null;
    }

    function assignVirtualChunkRun(name, puzzle, runStart, runCount) {
        const runEnd = runStart + runCount;
        const { startHex, endHex } = virtualChunkRangeToHex(puzzle, runStart, runEnd);
        const info = stmtInsertChunk.run(puzzle.id, startHex, endHex, name, runStart, runEnd);
        return { chunkId: info.lastInsertRowid, startHex, endHex, vchunkStart: runStart, vchunkEnd: runEnd };
    }

    const assignVirtualChunkJob = db.transaction((name, workHints, puzzle) => {
        const currentPuzzle = stmtVChunkPuzzle.get(puzzle.id);
        if (!currentPuzzle) return null;
        if (!currentPuzzle.virtual_chunk_size_keys || !currentPuzzle.virtual_chunk_count) return null;

        const totalChunksBig = BigInt(currentPuzzle.virtual_chunk_count);
        if (totalChunksBig <= 0n) return null;

        const totalChunks = Number(totalChunksBig);
        const hashrateBig = normalizeHashrate(workHints.hashrate || stmtWorkerHash.get(name)?.hashrate);
        const minChunkKeys = parsePositiveBigInt(workHints.min_chunk_keys, null);
        const chunkQuantumKeys = parsePositiveBigInt(workHints.chunk_quantum_keys, 1n);
        const requestedKeys = computeWorkerRequestedKeys({ hashrateBig, minChunkKeys, chunkQuantumKeys });

        const vchunkSize = BigInt(currentPuzzle.virtual_chunk_size_keys);
        let neededChunks = Number(ceilDiv(requestedKeys, vchunkSize));
        if (neededChunks < 1) neededChunks = 1;
        if (BigInt(neededChunks) > totalChunksBig) neededChunks = totalChunks;

        console.log(
            `[Alloc] ${name} requested ${requestedKeys.toString()} keys ` +
            `(hashrate=${hashrateBig.toString()}, min=${minChunkKeys ? minChunkKeys.toString() : 'null'}, ` +
            `quantum=${chunkQuantumKeys ? chunkQuantumKeys.toString() : '1'}) ` +
            `=> need ${neededChunks} virtual chunks of ${vchunkSize.toString()} keys ` +
            `(bootstrap_stage=${Number(currentPuzzle.bootstrap_stage || 0)}, cursor=${Number(currentPuzzle.alloc_cursor || 0)})`
        );

        const freshChunkCount = stmtChunkCount.get(currentPuzzle.id).cnt;
        const stage = Number(currentPuzzle.bootstrap_stage || 0);

        // Mandatory 3-step bootstrap for first three fresh jobs only.
        if (freshChunkCount < 3 && stage < 3) {
            let runStart = null;

            if (stage === 0) runStart = findMidBootstrapRun(currentPuzzle.id, totalChunks, neededChunks);
            else if (stage === 1) runStart = findBeginBootstrapRun(currentPuzzle.id, totalChunks, neededChunks);
            else if (stage === 2) runStart = findEndBootstrapRun(currentPuzzle.id, totalChunks, neededChunks);

            if (runStart !== null) {
                const result = assignVirtualChunkRun(name, currentPuzzle, runStart, neededChunks);
                console.log(
                    `[Alloc] bootstrap stage ${stage} -> assigned ${name} ` +
                    `vchunks ${result.vchunkStart}..${result.vchunkEnd - 1} ` +
                    `(${result.startHex} .. ${result.endHex})`
                );
                stmtBootstrapStageSet.run(stage + 1, currentPuzzle.id);
                return result;
            }

            for (let fallback = neededChunks - 1; fallback >= 1; fallback--) {
                if (stage === 0) runStart = findMidBootstrapRun(currentPuzzle.id, totalChunks, fallback);
                else if (stage === 1) runStart = findBeginBootstrapRun(currentPuzzle.id, totalChunks, fallback);
                else if (stage === 2) runStart = findEndBootstrapRun(currentPuzzle.id, totalChunks, fallback);

                if (runStart !== null) {
                    const result = assignVirtualChunkRun(name, currentPuzzle, runStart, fallback);
                    console.log(
                        `[Alloc] bootstrap fallback stage ${stage} -> assigned ${name} ` +
                        `vchunks ${result.vchunkStart}..${result.vchunkEnd - 1} ` +
                        `(${result.startHex} .. ${result.endHex})`
                    );
                    stmtBootstrapStageSet.run(stage + 1, currentPuzzle.id);
                    return result;
                }
            }

            stmtBootstrapStageSet.run(stage + 1, currentPuzzle.id);
        }

        const seed = currentPuzzle.alloc_seed || defaultAllocSeedForPuzzle(currentPuzzle, ALLOC_STRATEGY_VCHUNKS);
        const { a, b } = deriveAffinePermutationParams(seed, totalChunksBig);
        const rawCursorBig = BigInt(currentPuzzle.alloc_cursor || 0);
        const probeLimit = Math.min(totalChunks, MAX_ALLOC_PROBES);

        // Full-size search first.
        let triedStarts = new Set();
        for (let offset = 0; offset < probeLimit; offset++) {
            const orderIndex = (rawCursorBig + BigInt(offset)) % totalChunksBig;
            const candidateIndex = Number(permuteIndexAffine(orderIndex, totalChunksBig, a, b));
            const runStart = normalizeRunStartForCandidate(candidateIndex, neededChunks, totalChunks);

            if (triedStarts.has(runStart)) continue;
            triedStarts.add(runStart);

            if (rangeIsFree(currentPuzzle.id, runStart, runStart + neededChunks)) {
                const nextCursor = Number((orderIndex + 1n) % totalChunksBig);
                stmtVChunkCursorSet.run(nextCursor, currentPuzzle.id);

                const result = assignVirtualChunkRun(name, currentPuzzle, runStart, neededChunks);
                console.log(
                    `[Alloc] affine full-run -> assigned ${name} ` +
                    `vchunks ${result.vchunkStart}..${result.vchunkEnd - 1} ` +
                    `(${result.startHex} .. ${result.endHex})`
                );
                if (stage < 3) stmtBootstrapStageSet.run(3, currentPuzzle.id);
                return result;
            }
        }

        // Fallback: smaller contiguous run.
        for (let fallback = neededChunks - 1; fallback >= 1; fallback--) {
            triedStarts = new Set();

            for (let offset = 0; offset < probeLimit; offset++) {
                const orderIndex = (rawCursorBig + BigInt(offset)) % totalChunksBig;
                const candidateIndex = Number(permuteIndexAffine(orderIndex, totalChunksBig, a, b));
                const runStart = normalizeRunStartForCandidate(candidateIndex, fallback, totalChunks);

                if (triedStarts.has(runStart)) continue;
                triedStarts.add(runStart);

                if (rangeIsFree(currentPuzzle.id, runStart, runStart + fallback)) {
                    const nextCursor = Number((orderIndex + 1n) % totalChunksBig);
                    stmtVChunkCursorSet.run(nextCursor, currentPuzzle.id);

                    const result = assignVirtualChunkRun(name, currentPuzzle, runStart, fallback);
                    console.log(
                        `[Alloc] affine fallback -> assigned ${name} ` +
                        `vchunks ${result.vchunkStart}..${result.vchunkEnd - 1} ` +
                        `(${result.startHex} .. ${result.endHex})`
                    );
                    if (stage < 3) stmtBootstrapStageSet.run(3, currentPuzzle.id);
                    return result;
                }
            }
        }

        console.log(
            `[Alloc] no free run found for ${name} after ${probeLimit} probes ` +
            `(need=${neededChunks}, total=${totalChunks})`
        );
        return null;
    });

    const claimTestChunk = db.transaction((name, puzzle) => {
        const taken = stmtTestChunkTaken.get(puzzle.id, puzzle.test_start_hex, puzzle.test_end_hex);
        if (taken) return null;
        const reclaimed = stmtTestChunkReclaim.get(name, puzzle.id, puzzle.test_start_hex, puzzle.test_end_hex);
        if (reclaimed) return reclaimed;
        return stmtTestChunkInsert.get(puzzle.id, puzzle.test_start_hex, puzzle.test_end_hex, name);
    });

    const app = express();
    app.use(express.json());

    app.get('/', (req, res) => {
        res.set('Cache-Control', 'no-store');
        res.sendFile('public/index.html', { root: '.' });
    });
    app.use(express.static('public'));

    if (process.env.ADMIN_TOKEN) {
        app.use('/api/v1/admin', (req, res, next) => {
            if (req.headers['x-admin-token'] === process.env.ADMIN_TOKEN) return next();
            return res.status(401).json({ error: 'unauthorized' });
        });
    }

    // --- API Endpoints ---

    app.post('/api/v1/work', (req, res) => {
        const { name, hashrate, version, min_chunk_keys, chunk_quantum_keys } = req.body;
        if (!name) return res.status(400).json({ error: 'Missing name' });

        const hashrateNum = Number(normalizeHashrate(hashrate));
        const minChunkKeys = parsePositiveBigInt(min_chunk_keys, null);
        const chunkQuantumKeys = parsePositiveBigInt(chunk_quantum_keys, null);

        const prevWorker = db.prepare(
            `SELECT CASE WHEN last_seen < datetime('now', '-${REACTIVATE_MINUTES} minutes') THEN 1 ELSE 0 END AS inactive FROM workers WHERE name = ?`
        ).get(name);
        const isReactivating = prevWorker?.inactive === 1;

        if (isReactivating) {
            db.prepare(`
                UPDATE chunks
                SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL, assigned_at = NULL, heartbeat_at = NULL
                WHERE worker_name = ? AND status = 'assigned'
            `).run(name);
        }

        db.prepare(`
            INSERT INTO workers (name, hashrate, last_seen, version, min_chunk_keys, chunk_quantum_keys)
            VALUES (?, ?, CURRENT_TIMESTAMP, ?, ?, ?)
            ON CONFLICT(name) DO UPDATE
            SET hashrate = excluded.hashrate,
                last_seen = CURRENT_TIMESTAMP,
                version = COALESCE(excluded.version, workers.version),
                min_chunk_keys = COALESCE(excluded.min_chunk_keys, workers.min_chunk_keys),
                chunk_quantum_keys = COALESCE(excluded.chunk_quantum_keys, workers.chunk_quantum_keys)
        `).run(
            name,
            hashrateNum,
            version || null,
            minChunkKeys ? minChunkKeys.toString() : null,
            chunkQuantumKeys ? chunkQuantumKeys.toString() : null
        );

        const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
        if (!puzzle) return res.status(503).json({ error: 'No active puzzle configured' });

        ensureAllocatorForPuzzle(db, puzzle.id);

        const existing = db.prepare(`
            SELECT id, start_hex, end_hex
            FROM chunks
            WHERE worker_name = ? AND puzzle_id = ? AND status = 'assigned'
            LIMIT 1
        `).get(name, puzzle.id);
        if (existing) {
            // Strict liveness model: only POST /heartbeat resets the reclaim timer.
            // A repeated /work call for an already-assigned chunk re-issues the job_id
            // but does NOT update heartbeat_at. Workers must call /heartbeat to stay alive.
            return res.json({ job_id: existing.id, start_key: existing.start_hex, end_key: existing.end_hex });
        }

        if (puzzle.test_start_hex && puzzle.test_end_hex) {
            const claimed = claimTestChunk.immediate(name, puzzle);
            if (claimed) {
                console.log(`[Test] Assigned test chunk #${claimed.id} to ${name}`);
                return res.json({ job_id: claimed.id, start_key: claimed.start_hex, end_key: claimed.end_hex });
            }
        }

        const reclaimed = isReactivating ? null : db.prepare(`
            UPDATE chunks
            SET status = 'assigned',
                worker_name = ?,
                assigned_at = CURRENT_TIMESTAMP,
                heartbeat_at = CURRENT_TIMESTAMP
            WHERE id = (
                SELECT id
                FROM chunks
                WHERE status = 'reclaimed' AND puzzle_id = ? AND is_test = 0
                LIMIT 1
            )
            RETURNING *
        `).get(name, puzzle.id);

        let chunkId, startHex, endHex;

        if (reclaimed) {
            chunkId  = reclaimed.id;
            startHex = reclaimed.start_hex;
            endHex   = reclaimed.end_hex;
        } else {
            let result = null;
            const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;

            if (strategy === ALLOC_STRATEGY_VCHUNKS) {
                result = assignVirtualChunkJob.immediate(name, {
                    hashrate,
                    min_chunk_keys,
                    chunk_quantum_keys
                }, puzzle);
            } else {
                result = assignLegacyRandomChunk.immediate(name, hashrate, puzzle);
            }

            if (!result) return res.status(503).json({ error: 'All keyspace has been assigned' });
            ({ chunkId, startHex, endHex } = result);
        }

        res.json({ job_id: chunkId, start_key: startHex, end_key: endHex });
    });

    app.post('/api/v1/submit', (req, res) => {
        const { name, job_id, status, findings } = req.body;
        if (status !== 'done' && status !== 'FOUND') {
            return res.status(400).json({ error: 'status must be "done" or "FOUND"' });
        }

        if (status === 'FOUND') {
            if (!Array.isArray(findings) || findings.length === 0) {
                return res.status(400).json({ error: 'findings must be a non-empty array' });
            }
            for (const f of findings) {
                if (!f || typeof f !== 'object' || Array.isArray(f)) {
                    return res.status(400).json({ error: 'each finding must be a plain object' });
                }
                if (!f.found_key) {
                    return res.status(400).json({ error: 'each finding must include found_key' });
                }
                if (!isValidHex(f.found_key)) {
                    return res.status(400).json({ error: 'each found_key must be a valid hex string' });
                }
            }

            const normalized = findings.map(f => ({ ...f, found_key: normalizeHex(f.found_key) }));
            const seen = new Set();
            const allFindings = normalized.filter(f => seen.has(f.found_key) ? false : (seen.add(f.found_key), true));
            const primaryKey  = allFindings[0].found_key;
            const primaryAddr = allFindings[0].found_address || null;

            const stmtInsertFinding = db.prepare(`
                INSERT OR IGNORE INTO findings (chunk_id, worker_name, found_key, found_address)
                VALUES (?, ?, ?, ?)
            `);

            const result = db.transaction(() => {
                const info = db.prepare(`
                    UPDATE chunks
                    SET status = 'FOUND',
                        found_key = COALESCE(found_key, ?),
                        found_address = COALESCE(found_address, ?)
                    WHERE id = ? AND worker_name = ? AND status = 'assigned'
                `).run(primaryKey, primaryAddr, job_id, name);

                let isLate = false;
                if (!info.changes) {
                    const alreadyRecorded = db.prepare(`
                        SELECT id FROM findings WHERE chunk_id = ? AND worker_name = ? AND found_key = ?
                    `).get(job_id, name, primaryKey);

                    if (!alreadyRecorded) {
                        const latePrev = db.prepare(`
                            SELECT id, status
                            FROM chunks
                            WHERE id = ? AND prev_worker_name = ? AND status IN ('assigned', 'reclaimed')
                        `).get(job_id, name);

                        if (!latePrev) return { accepted: false, inserted: [], isLate: false };

                        db.prepare(`
                            UPDATE chunks
                            SET status = 'FOUND',
                                worker_name = ?,
                                found_key = COALESCE(found_key, ?),
                                found_address = COALESCE(found_address, ?)
                            WHERE id = ?
                        `).run(name, primaryKey, primaryAddr, job_id);
                        isLate = true;
                    }
                }

                const inserted = [];
                for (const f of allFindings) {
                    const r = stmtInsertFinding.run(job_id, name, f.found_key, f.found_address || null);
                    if (r.changes) inserted.push(f);
                }
                return { accepted: true, inserted, isLate };
            })();

            if (!result.accepted) return res.json({ accepted: false });
            if (result.isLate) {
                console.log(`[Late FOUND] Job: ${job_id} | Worker: ${name} (prev assignee) | KEY: ${primaryKey}`);
            }

            for (const f of result.inserted) {
                const msg = `[${new Date().toISOString()}] BINGO! Job: ${job_id} | Worker: ${name} | KEY: ${f.found_key} | ADDR: ${f.found_address || 'Unknown'}\n`;
                console.log(`\n🚨🚨🚨 ${msg}`);
                fs.appendFileSync('BINGO_FOUND_KEYS.txt', msg);
            }
        } else {
            const { keys_scanned } = req.body;

            if (keys_scanned === undefined) {
                return res.status(400).json({ accepted: false, error: 'keys_scanned is required for status: done' });
            }

            if (typeof keys_scanned !== 'number' || !Number.isInteger(keys_scanned) || keys_scanned < 0) {
                return res.status(400).json({ accepted: false, error: 'keys_scanned must be a non-negative integer' });
            }

            const result = db.transaction(() => {
                const chunk = db.prepare(`
                    SELECT start_hex, end_hex
                    FROM chunks
                    WHERE id = ? AND worker_name = ? AND status = 'assigned'
                `).get(job_id, name);
                if (!chunk) return { notOwned: true };

                const expectedSize = BigInt('0x' + chunk.end_hex) - BigInt('0x' + chunk.start_hex);
                const reported = BigInt(keys_scanned);

                if (reported < expectedSize) {
                    const upd = db.prepare(`
                        UPDATE chunks
                        SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL, assigned_at = NULL, heartbeat_at = NULL
                        WHERE id = ? AND worker_name = ? AND status = 'assigned'
                    `).run(job_id, name);
                    return { reclaimed: upd.changes > 0, expectedSize, reported };
                }

                return { sufficient: true };
            })();

            if (result.notOwned) return res.json({ accepted: false });
            if (result.reclaimed) {
                return res.status(400).json({
                    accepted: false,
                    error: `chunk #${job_id} not accepted, reported size: ${result.reported}, expected size: ${result.expectedSize}. Chunk reclaimed.`
                });
            }
            if (!result.sufficient) return res.json({ accepted: false });

            const info = db.prepare(`
                UPDATE chunks SET status = 'completed'
                WHERE id = ? AND worker_name = ? AND status = 'assigned'
            `).run(job_id, name);
            if (!info.changes) return res.json({ accepted: false });
        }

        const chunk = db.prepare(`
            SELECT is_test, puzzle_id, start_hex, end_hex
            FROM chunks
            WHERE id = ?
        `).get(job_id);

        if (chunk?.is_test) {
            const cleared = db.prepare(`
                UPDATE puzzles
                SET test_start_hex = NULL, test_end_hex = NULL
                WHERE id = ? AND test_start_hex = ? AND test_end_hex = ?
            `).run(chunk.puzzle_id, chunk.start_hex, chunk.end_hex);

            if (cleared.changes) {
                console.log(`[Test] Test chunk #${job_id} completed — test config cleared from puzzle ${chunk.puzzle_id}`);
            }
        }

        res.json({ accepted: true });
    });

    app.get('/api/v1/stats', (req, res) => {
        const puzzleIdParam = req.query.puzzle_id ? parseInt(req.query.puzzle_id, 10) : null;
        const puzzle = puzzleIdParam
            ? (db.prepare("SELECT * FROM puzzles WHERE id = ?").get(puzzleIdParam) || null)
            : (db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get() || null);

        const pid = puzzle ? puzzle.id : null;

        const visibleWorkers = pid ? db.prepare(`
            SELECT w.name, w.hashrate, w.last_seen, w.version, w.min_chunk_keys, w.chunk_quantum_keys,
                   CASE WHEN w.last_seen >= datetime('now', '-${ACTIVE_MINUTES} minutes')
                   THEN 1 ELSE 0 END AS fresh,
                   CASE WHEN EXISTS (
                       SELECT 1 FROM chunks c2
                       WHERE c2.worker_name = w.name AND c2.puzzle_id = ? AND c2.status = 'assigned'
                   ) THEN 1 ELSE 0 END AS assigned_here,
                   CASE WHEN w.last_seen >= datetime('now', '-${ACTIVE_MINUTES} minutes')
                        AND EXISTS (
                            SELECT 1 FROM chunks c2
                            WHERE c2.worker_name = w.name AND c2.puzzle_id = ? AND c2.status = 'assigned'
                        )
                   THEN 1 ELSE 0 END AS active
            FROM workers w
            WHERE w.last_seen >= datetime('now', '-${TIMEOUT_MINUTES} minutes')
              AND EXISTS (
                SELECT 1 FROM chunks c
                WHERE (c.worker_name = w.name OR c.prev_worker_name = w.name)
                  AND c.puzzle_id = ?
              )
            ORDER BY w.hashrate DESC
        `).all(pid, pid, pid) : [];

        const totalHashrate = visibleWorkers.filter(w => w.active).reduce((sum, w) => sum + w.hashrate, 0);

        const completedChunks = pid ? db.prepare(`
            SELECT COUNT(*) AS count
            FROM chunks
            WHERE puzzle_id = ? AND (status = 'completed' OR status = 'FOUND') AND is_test = 0
        `).get(pid).count : 0;

        const reclaimedChunks = pid ? db.prepare(`
            SELECT COUNT(*) AS count
            FROM chunks
            WHERE puzzle_id = ? AND status = 'reclaimed' AND is_test = 0
        `).get(pid).count : 0;

        const doneChunks = pid ? db.prepare(`
            SELECT worker_name, start_hex, end_hex
            FROM chunks
            WHERE puzzle_id = ? AND (status = 'completed' OR status = 'FOUND') AND worker_name IS NOT NULL AND is_test = 0
        `).all(pid) : [];

        let totalKeysCompleted = 0n;
        if (doneChunks.length > 0) {
            const sorted = [...doneChunks].sort((a, b) => {
                const diff = BigInt('0x' + a.start_hex) - BigInt('0x' + b.start_hex);
                return diff < 0n ? -1 : diff > 0n ? 1 : 0;
            });
            let mergeStart = BigInt('0x' + sorted[0].start_hex);
            let mergeEnd   = BigInt('0x' + sorted[0].end_hex);
            for (let i = 1; i < sorted.length; i++) {
                const s = BigInt('0x' + sorted[i].start_hex);
                const e = BigInt('0x' + sorted[i].end_hex);
                if (s <= mergeEnd) {
                    if (e > mergeEnd) mergeEnd = e;
                } else {
                    totalKeysCompleted += mergeEnd - mergeStart;
                    mergeStart = s;
                    mergeEnd   = e;
                }
            }
            totalKeysCompleted += mergeEnd - mergeStart;
        }

        const workerStats = {};
        for (const c of doneChunks) {
            const keys = BigInt('0x' + c.end_hex) - BigInt('0x' + c.start_hex);
            if (!workerStats[c.worker_name]) workerStats[c.worker_name] = { chunks: 0, keys: 0n };
            workerStats[c.worker_name].chunks++;
            workerStats[c.worker_name].keys += keys;
        }

        const scores = Object.entries(workerStats)
            .map(([worker_name, { chunks, keys }]) => ({
                worker_name,
                completed_chunks: chunks,
                total_keys: keys.toString()
            }))
            .sort((a, b) => {
                const d = BigInt(b.total_keys) - BigInt(a.total_keys);
                return d > 0n ? 1 : d < 0n ? -1 : 0;
            });

        const finders = pid ? db.prepare(`
            SELECT f.worker_name, f.found_key, f.found_address, f.created_at,
                   c.id AS chunk_global,
                   CASE
                       WHEN c.vchunk_start IS NOT NULL THEN c.vchunk_start
                       ELSE NULL
                   END AS vchunk_start,
                   CASE
                       WHEN c.vchunk_end IS NOT NULL THEN c.vchunk_end
                       ELSE NULL
                   END AS vchunk_end
            FROM findings f
            JOIN chunks c ON c.id = f.chunk_id
            WHERE c.puzzle_id = ? AND c.is_test = 0
            ORDER BY f.id ASC
        `).all(pid) : [];

        const assignedNow = puzzle ? db.prepare(`
            SELECT
                c.id,
                c.worker_name,
                c.vchunk_start,
                c.vchunk_end,
                c.assigned_at,
                c.heartbeat_at,
                c.start_hex,
                c.end_hex
            FROM chunks c
            WHERE c.status = 'assigned' AND c.puzzle_id = ? AND c.is_test = 0
        `).all(puzzle.id) : [];

        const workerAssignedMap = {};
        for (const c of assignedNow) {
            const currentJobKeys =
                c.start_hex && c.end_hex
                    ? (BigInt('0x' + c.end_hex) - BigInt('0x' + c.start_hex)).toString()
                    : null;
            const assignedMs = parseUtcDateMs(c.assigned_at);
            const elapsedSeconds = assignedMs !== null ? Math.max(0, (Date.now() - assignedMs) / 1000) : null;

            workerAssignedMap[c.worker_name] = {
                current_chunk: c.id,
                current_vchunk_run: (c.vchunk_start !== null && c.vchunk_end !== null)
                    ? `${c.vchunk_start}..${c.vchunk_end - 1}`
                    : null,
                current_vchunk_run_start: c.vchunk_start ?? null,
                current_vchunk_run_end: c.vchunk_end ?? null, // exclusive
                assigned_at: c.assigned_at ?? null,
                heartbeat_at: c.heartbeat_at ?? null,
                current_job_start_hex: c.start_hex ?? null,
                current_job_end_hex: c.end_hex ?? null,
                current_job_keys: currentJobKeys,
                current_job_elapsed_seconds: elapsedSeconds !== null ? Math.round(elapsedSeconds) : null,
            };
        }

        const workers = visibleWorkers.map(w => {
            const assigned = workerAssignedMap[w.name] || {};
            return {
                ...w,
                fresh: w.fresh === 1,
                assigned_here: w.assigned_here === 1,
                active: w.active === 1,
                current_chunk: assigned.current_chunk ?? null,
                current_vchunk_run: assigned.current_vchunk_run ?? null,
                current_vchunk_run_start: assigned.current_vchunk_run_start ?? null,
                current_vchunk_run_end: assigned.current_vchunk_run_end ?? null,
                assigned_at: assigned.assigned_at ?? null,
                heartbeat_at: assigned.heartbeat_at ?? null,
                current_job_start_hex: assigned.current_job_start_hex ?? null,
                current_job_end_hex: assigned.current_job_end_hex ?? null,
                current_job_keys: assigned.current_job_keys ?? null,
                current_job_elapsed_seconds: assigned.current_job_elapsed_seconds ?? null,
                current_job_progress_percent: assigned.assigned_at
                    ? computeWorkerProgressPercent(assigned.assigned_at, w.hashrate, assigned.current_job_keys)
                    : null,
            };
        });

        let chunks_vis = [];
        if (puzzle) {
            const pStart = BigInt('0x' + puzzle.start_hex);
            const pEnd   = BigInt('0x' + puzzle.end_hex);
            const pRange = pEnd - pStart;

            const rawChunks = db.prepare(`
                SELECT id, status, worker_name, start_hex, end_hex
                FROM chunks
                WHERE puzzle_id = ? AND is_test = 0
                ORDER BY id ASC
            `).all(puzzle.id);

            chunks_vis = rawChunks.map(c => {
                const cs = BigInt('0x' + c.start_hex) - pStart;
                const ce = BigInt('0x' + c.end_hex)   - pStart;
                return {
                    id: c.id,
                    st: c.status,
                    w:  c.worker_name,
                    s:  Number(cs * 1000000n / pRange) / 1000000,
                    e:  Number(ce * 1000000n / pRange) / 1000000,
                };
            });
        }

        const allPuzzles = db.prepare("SELECT id, name, active FROM puzzles ORDER BY id ASC").all();

        const virtualChunksTotal = pid
            ? (() => {
                const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;
                if (strategy === ALLOC_STRATEGY_VCHUNKS) return puzzle.virtual_chunk_count || 0;
                return db.prepare("SELECT COUNT(*) AS c FROM sectors WHERE puzzle_id = ?").get(pid).c;
            })()
            : 0;

        const virtualChunksStarted = pid
            ? (() => {
                const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;
                if (strategy === ALLOC_STRATEGY_VCHUNKS) {
                    const row = db.prepare(`
                        SELECT COALESCE(SUM(vchunk_end - vchunk_start), 0) AS c
                        FROM chunks
                        WHERE puzzle_id = ? AND is_test = 0 AND vchunk_start IS NOT NULL AND vchunk_end IS NOT NULL
                    `).get(pid);
                    return row.c || 0;
                }
                return db.prepare(`
                    SELECT COUNT(DISTINCT sector_id) AS c
                    FROM chunks
                    WHERE puzzle_id = ? AND is_test = 0 AND sector_id IS NOT NULL
                `).get(pid).c;
            })()
            : 0;

        const virtualChunksCompleted = pid
            ? (() => {
                const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;
                if (strategy === ALLOC_STRATEGY_VCHUNKS) {
                    const row = db.prepare(`
                        SELECT COALESCE(SUM(vchunk_end - vchunk_start), 0) AS c
                        FROM chunks
                        WHERE puzzle_id = ? AND is_test = 0 AND status IN ('completed', 'FOUND')
                          AND vchunk_start IS NOT NULL AND vchunk_end IS NOT NULL
                    `).get(pid);
                    return row.c || 0;
                }
                return db.prepare("SELECT COUNT(*) AS c FROM sectors WHERE puzzle_id = ? AND status = 'done'").get(pid).c;
            })()
            : 0;

        res.json({
            stage: process.env.STAGE || 'PROD',
            target_minutes: TARGET_MINUTES,
            timeout_minutes: TIMEOUT_MINUTES,
            active_minutes: ACTIVE_MINUTES,
            puzzles: allPuzzles,
            puzzle: puzzle ? {
                id: puzzle.id,
                name: puzzle.name,
                start_hex: puzzle.start_hex,
                end_hex: puzzle.end_hex,
                total_keys: (BigInt('0x' + puzzle.end_hex) - BigInt('0x' + puzzle.start_hex)).toString(),
                test_chunk: puzzle.test_start_hex ? {
                    start_hex: puzzle.test_start_hex,
                    end_hex:   puzzle.test_end_hex,
                } : null,
                alloc_strategy: puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY,
                alloc_cursor: puzzle.alloc_cursor || 0,
                virtual_chunk_size_keys: puzzle.virtual_chunk_size_keys || null,
                virtual_chunk_count: puzzle.virtual_chunk_count || null,
                bootstrap_stage: puzzle.bootstrap_stage || 0,
            } : null,
            active_workers_count: visibleWorkers.filter(w => w.active).length,
            inactive_workers_count: visibleWorkers.filter(w => !w.active).length,
            total_hashrate: totalHashrate,
            completed_chunks: completedChunks,
            reclaimed_chunks: reclaimedChunks,
            total_keys_completed: totalKeysCompleted.toString(),
            virtual_chunks: {
                total: virtualChunksTotal,
                started: virtualChunksStarted,
                completed: virtualChunksCompleted,
            },
            // Backward-compatible alias for the old dashboard
            shards: {
                total: virtualChunksTotal,
                started: virtualChunksStarted,
                completed: virtualChunksCompleted,
            },
            workers,
            scores,
            finders,
            chunks_vis,
        });
    });

    app.post('/api/v1/heartbeat', (req, res) => {
        const { name, job_id } = req.body;
        if (!name || job_id === null || job_id === undefined) {
            return res.status(400).json({ error: 'Missing name or job_id' });
        }

        db.prepare(`
            INSERT INTO workers (name, hashrate, last_seen)
            VALUES (?, 0, CURRENT_TIMESTAMP)
            ON CONFLICT(name) DO UPDATE SET last_seen = CURRENT_TIMESTAMP
        `).run(name);

        db.prepare(`
            UPDATE chunks
            SET heartbeat_at = CURRENT_TIMESTAMP
            WHERE id = ? AND worker_name = ? AND status = 'assigned'
        `).run(job_id, name);

        res.json({ ok: true });
    });

    app.post('/api/v1/admin/set-puzzle', (req, res) => {
        const {
            name,
            start_hex,
            end_hex,
            alloc_strategy,
            alloc_seed,
            virtual_chunk_size_keys
        } = req.body;

        if (!name || !start_hex || !end_hex) {
            return res.status(400).json({ error: 'Missing name, start_hex, or end_hex' });
        }
        if (!isValidHex(start_hex) || !isValidHex(end_hex)) {
            return res.status(400).json({ error: 'start_hex and end_hex must be valid hex strings' });
        }

        const strategy = alloc_strategy || DEFAULT_ALLOC_STRATEGY;
        if (strategy !== ALLOC_STRATEGY_LEGACY && strategy !== ALLOC_STRATEGY_VCHUNKS) {
            return res.status(400).json({ error: `alloc_strategy must be ${ALLOC_STRATEGY_LEGACY} or ${ALLOC_STRATEGY_VCHUNKS}` });
        }

        const startNorm = start_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();
        const endNorm   = end_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();

        if (BigInt('0x' + endNorm) <= BigInt('0x' + startNorm)) {
            return res.status(400).json({ error: 'end_hex must be greater than start_hex' });
        }

        const puzzleRange = BigInt('0x' + endNorm) - BigInt('0x' + startNorm);

        let vchunkSize = null;
        if (strategy === ALLOC_STRATEGY_VCHUNKS) {
            const parsed = parsePositiveBigInt(virtual_chunk_size_keys, null);
            vchunkSize = parsed ? bigIntMin(parsed, puzzleRange) : chooseDefaultVirtualChunkSize(puzzleRange);
            validateVirtualChunkCountOrThrow(ceilDiv(puzzleRange, vchunkSize), 'set-puzzle');
        }

        db.transaction(() => {
            db.prepare("UPDATE puzzles SET active = 0").run();

            const existing = db.prepare("SELECT * FROM puzzles WHERE name = ?").get(name);
            let puzzleId;

            const seed = alloc_seed || defaultAllocSeedForPuzzle({ name, start_hex: startNorm, end_hex: endNorm }, strategy);
            const sameSeed = !existing || existing.alloc_seed === seed;
            const sameVChunkSize = !existing || !existing.virtual_chunk_size_keys || existing.virtual_chunk_size_keys === (vchunkSize ? vchunkSize.toString() : null);

            if (
                existing &&
                existing.start_hex === startNorm &&
                existing.end_hex === endNorm &&
                (existing.alloc_strategy || ALLOC_STRATEGY_LEGACY) === strategy &&
                sameSeed &&
                sameVChunkSize
            ) {
                db.prepare("UPDATE puzzles SET active = 1 WHERE id = ?").run(existing.id);
                puzzleId = existing.id;
                ensureAllocatorForPuzzle(db, puzzleId);
            } else {
                const info = db.prepare(`
                    INSERT INTO puzzles (
                        name, start_hex, end_hex, active,
                        alloc_strategy, alloc_seed, alloc_cursor,
                        virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
                    )
                    VALUES (?, ?, ?, 1, ?, ?, 0, ?, NULL, 0)
                `).run(
                    name,
                    startNorm,
                    endNorm,
                    strategy,
                    seed,
                    vchunkSize ? vchunkSize.toString() : null
                );

                puzzleId = info.lastInsertRowid;

                if (strategy === ALLOC_STRATEGY_VCHUNKS) {
                    seedVirtualChunks(db, puzzleId, startNorm, endNorm, seed, vchunkSize);
                } else {
                    seedSectors(db, puzzleId, startNorm, endNorm);
                }
            }
        })();

        const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
        console.log(`[Admin] Active puzzle set to: ${puzzle.name} [${puzzle.start_hex} .. ${puzzle.end_hex}] strategy=${puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY}`);
        res.json({ ok: true, puzzle });
    });

    app.post('/api/v1/admin/set-test-chunk', (req, res) => {
        const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
        if (!puzzle) return res.status(503).json({ error: 'No active puzzle' });

        const { start_hex, end_hex } = req.body;

        if (!start_hex) {
            db.prepare("UPDATE puzzles SET test_start_hex = NULL, test_end_hex = NULL WHERE id = ?").run(puzzle.id);
            console.log(`[Admin] Test chunk cleared for puzzle ${puzzle.name}`);
            return res.json({ ok: true, test_chunk: null });
        }

        if (!isValidHex(start_hex)) {
            return res.status(400).json({ error: 'start_hex must be a valid hex string' });
        }

        const startNorm = start_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();
        const ts = BigInt('0x' + startNorm);

        let endNorm;
        if (end_hex) {
            if (!isValidHex(end_hex)) {
                return res.status(400).json({ error: 'end_hex must be a valid hex string' });
            }
            endNorm = end_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();
        } else {
            const MAX_256 = (1n << 256n) - 1n;
            if (ts + GPU_BATCH_KEYS > MAX_256) {
                return res.status(400).json({ error: 'auto-resolved end_hex exceeds 256-bit range' });
            }
            endNorm = (ts + GPU_BATCH_KEYS).toString(16).padStart(64, '0');
        }

        const te = BigInt('0x' + endNorm);
        if (te <= ts) {
            return res.status(400).json({ error: 'end_hex must be greater than start_hex' });
        }

        db.prepare(`
            UPDATE puzzles SET test_start_hex = ?, test_end_hex = ?
            WHERE id = ?
        `).run(startNorm, endNorm, puzzle.id);

        console.log(`[Admin] Test chunk set: ${startNorm} .. ${endNorm}`);
        res.json({ ok: true, test_chunk: { start_hex: startNorm, end_hex: endNorm } });
    });

    app.post('/api/v1/admin/activate-puzzle', (req, res) => {
        const { id } = req.body;
        if (!id) return res.status(400).json({ error: 'Missing id' });

        const target = db.prepare("SELECT * FROM puzzles WHERE id = ?").get(id);
        if (!target) return res.status(404).json({ error: 'Puzzle not found' });

        db.transaction(() => {
            db.prepare("UPDATE puzzles SET active = 0").run();
            db.prepare("UPDATE puzzles SET active = 1 WHERE id = ?").run(id);
        })();

        ensureAllocatorForPuzzle(db, target.id);

        const puzzle = db.prepare("SELECT * FROM puzzles WHERE id = ?").get(id);
        console.log(`[Admin] Active puzzle switched to: ${puzzle.name}`);
        res.json({ ok: true, puzzle });
    });

    app.get('/api/v1/admin/puzzles', (req, res) => {
        const puzzles = db.prepare(`
            SELECT id, name, active, start_hex, end_hex,
                   alloc_strategy, alloc_seed, alloc_cursor,
                   virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
            FROM puzzles
            ORDER BY id ASC
        `).all();
        res.json({ puzzles });
    });

    return app;
}

// --- Production entry point ---
if (require.main === module) {
    const db = new Database(DB_PATH);
    db.pragma('journal_mode = WAL');

    db.exec(`
      CREATE TABLE IF NOT EXISTS puzzles (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL,
        start_hex TEXT NOT NULL,
        end_hex TEXT NOT NULL,
        active INTEGER NOT NULL DEFAULT 0
      );

      CREATE TABLE IF NOT EXISTS workers (
        name TEXT PRIMARY KEY,
        hashrate REAL,
        last_seen DATETIME DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE IF NOT EXISTS chunks (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        puzzle_id INTEGER,
        start_hex TEXT,
        end_hex TEXT,
        status TEXT,
        worker_name TEXT,
        prev_worker_name TEXT,
        assigned_at DATETIME,
        heartbeat_at DATETIME,
        found_key TEXT,
        found_address TEXT,
        is_test INTEGER NOT NULL DEFAULT 0,
        sector_id INTEGER
      );

      CREATE INDEX IF NOT EXISTS idx_chunks_puzzle_status ON chunks (puzzle_id, status);

      CREATE TABLE IF NOT EXISTS sectors (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        puzzle_id   INTEGER NOT NULL,
        start_hex   TEXT NOT NULL,
        end_hex     TEXT NOT NULL,
        current_hex TEXT NOT NULL,
        status      TEXT NOT NULL DEFAULT 'open'
      );

      CREATE INDEX IF NOT EXISTS idx_sectors_puzzle_status ON sectors (puzzle_id, status);
      CREATE INDEX IF NOT EXISTS idx_sectors_puzzle_id     ON sectors (puzzle_id, id);
      CREATE UNIQUE INDEX IF NOT EXISTS idx_sectors_unique_span ON sectors (puzzle_id, start_hex, end_hex);

      CREATE TABLE IF NOT EXISTS findings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        chunk_id INTEGER NOT NULL,
        worker_name TEXT NOT NULL,
        found_key TEXT NOT NULL,
        found_address TEXT,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
      );

      CREATE UNIQUE INDEX IF NOT EXISTS idx_findings_dedup ON findings (chunk_id, worker_name, found_key);
    `);

    try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_start_hex TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_end_hex TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_strategy TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_seed TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_cursor INTEGER NOT NULL DEFAULT 0").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN virtual_chunk_size_keys TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN virtual_chunk_count INTEGER").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN bootstrap_stage INTEGER NOT NULL DEFAULT 0").run(); } catch (_) {}

    try { db.prepare("ALTER TABLE workers ADD COLUMN version TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE workers ADD COLUMN min_chunk_keys TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE workers ADD COLUMN chunk_quantum_keys TEXT").run(); } catch (_) {}

    try { db.prepare("ALTER TABLE chunks ADD COLUMN is_test INTEGER NOT NULL DEFAULT 0").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN prev_worker_name TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN alloc_block_id INTEGER").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN vchunk_start INTEGER").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN vchunk_end INTEGER").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN heartbeat_at DATETIME").run(); } catch (_) {}
    try {
        db.prepare("UPDATE chunks SET heartbeat_at = assigned_at WHERE heartbeat_at IS NULL AND assigned_at IS NOT NULL").run();
    } catch (_) {}

    try { db.prepare("CREATE UNIQUE INDEX IF NOT EXISTS idx_findings_dedup ON findings (chunk_id, worker_name, found_key)").run(); } catch (_) {}
    try { db.prepare("CREATE INDEX IF NOT EXISTS idx_chunks_vchunk_span ON chunks (puzzle_id, vchunk_start, vchunk_end, status)").run(); } catch (_) {}

    try {
        db.prepare(`
            UPDATE chunks SET is_test = 1
            WHERE is_test = 0
              AND EXISTS (
                SELECT 1 FROM puzzles p
                WHERE p.id = chunks.puzzle_id
                  AND p.test_start_hex IS NOT NULL
                  AND chunks.start_hex = p.test_start_hex
                  AND chunks.end_hex   = p.test_end_hex
              )
        `).run();
    } catch (_) {}

    // Normalize allocator defaults on pre-existing puzzles
    try {
        db.prepare(`
            UPDATE puzzles
            SET alloc_strategy = ?
            WHERE alloc_strategy IS NULL
        `).run(ALLOC_STRATEGY_LEGACY);
    } catch (_) {}

    // Seed from KEYSPACE_<NAME>=<start_hex>:<end_hex>
    for (const [key, value] of Object.entries(process.env)) {
        if (!key.startsWith('KEYSPACE_')) continue;

        const name = key.slice('KEYSPACE_'.length).replace(/_/g, ' ');
        const [startRaw, endRaw] = (value || '').split(':');

        if (!startRaw || !endRaw || !isValidHex(startRaw) || !isValidHex(endRaw)) {
            console.warn(`[Config] Skipping invalid keyspace ${key}=${value} — expected start_hex:end_hex`);
            continue;
        }

        const startNorm = startRaw.replace(/^0x/i, '').padStart(64, '0').toLowerCase();
        const endNorm   = endRaw.replace(/^0x/i, '').padStart(64, '0').toLowerCase();

        if (BigInt('0x' + endNorm) <= BigInt('0x' + startNorm)) {
            console.warn(`[Config] Skipping ${key} — end_hex must be greater than start_hex`);
            continue;
        }

        const existing = db.prepare("SELECT id FROM puzzles WHERE name = ?").get(name);
        if (!existing) {
            const strategy = DEFAULT_ALLOC_STRATEGY;
            const seed = defaultAllocSeedForPuzzle({ name, start_hex: startNorm, end_hex: endNorm }, strategy);

            let virtualChunkSize = null;
            if (strategy === ALLOC_STRATEGY_VCHUNKS) {
                const range = BigInt('0x' + endNorm) - BigInt('0x' + startNorm);
                virtualChunkSize = chooseDefaultVirtualChunkSize(range);
            }

            const info = db.prepare(`
                INSERT INTO puzzles (
                    name, start_hex, end_hex, active,
                    alloc_strategy, alloc_seed, alloc_cursor,
                    virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
                )
                VALUES (?, ?, ?, 0, ?, ?, 0, ?, NULL, 0)
            `).run(
                name,
                startNorm,
                endNorm,
                strategy,
                seed,
                virtualChunkSize ? virtualChunkSize.toString() : null
            );

            console.log(`[Config] Seeded keyspace: ${name}`);
            if (strategy === ALLOC_STRATEGY_VCHUNKS) {
                seedVirtualChunks(db, info.lastInsertRowid, startNorm, endNorm, seed, virtualChunkSize);
            } else {
                seedSectors(db, info.lastInsertRowid, startNorm, endNorm);
            }
        }
    }

    const puzzleCount = db.prepare("SELECT COUNT(*) AS count FROM puzzles").get().count;
    if (puzzleCount === 0) {
        const startHex = '0400000000000000000'.padStart(64, '0');
        const endHex   = '07fffffffffffffffff'.padStart(64, '0');
        const name = 'Puzzle #71';
        const strategy = DEFAULT_ALLOC_STRATEGY;
        const seed = defaultAllocSeedForPuzzle({ name, start_hex: startHex, end_hex: endHex }, strategy);

        let virtualChunkSize = null;
        if (strategy === ALLOC_STRATEGY_VCHUNKS) {
            const range = BigInt('0x' + endHex) - BigInt('0x' + startHex);
            virtualChunkSize = chooseDefaultVirtualChunkSize(range);
        }

        const info = db.prepare(`
            INSERT INTO puzzles (
                name, start_hex, end_hex, active,
                alloc_strategy, alloc_seed, alloc_cursor,
                virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
            )
            VALUES (?, ?, ?, 1, ?, ?, 0, ?, NULL, 0)
        `).run(
            name,
            startHex,
            endHex,
            strategy,
            seed,
            virtualChunkSize ? virtualChunkSize.toString() : null
        );

        console.log('[Init] Seeded Puzzle #71 as active puzzle.');
        if (strategy === ALLOC_STRATEGY_VCHUNKS) {
            seedVirtualChunks(db, info.lastInsertRowid, startHex, endHex, seed, virtualChunkSize);
        } else {
            seedSectors(db, info.lastInsertRowid, startHex, endHex);
        }
    }

    const activeCount = db.prepare("SELECT COUNT(*) AS count FROM puzzles WHERE active = 1").get().count;
    if (activeCount === 0) {
        db.prepare("UPDATE puzzles SET active = 1 WHERE id = (SELECT MIN(id) FROM puzzles)").run();
        console.log('[Init] No active puzzle found — activated the first one.');
    } else if (activeCount > 1) {
        db.prepare("UPDATE puzzles SET active = 0 WHERE id != (SELECT MAX(id) FROM puzzles WHERE active = 1)").run();
        console.log('[Init] Multiple active puzzles found — deactivated all but the latest.');
    }

    const allPuzzles = db.prepare("SELECT id FROM puzzles").all();
    for (const p of allPuzzles) {
        try {
            ensureAllocatorForPuzzle(db, p.id);
        } catch (e) {
            console.error(`[Init] Failed to ensure allocator for puzzle ${p.id}: ${e.message}`);
            throw e;
        }
    }

    const app = createApp(db);

    setInterval(() => {
        const info = db.prepare(`
            UPDATE chunks
            SET status = 'reclaimed',
                prev_worker_name = worker_name,
                worker_name = NULL,
                assigned_at = NULL,
                heartbeat_at = NULL
            WHERE status = 'assigned'
              AND COALESCE(heartbeat_at, assigned_at) < datetime('now', '-${TIMEOUT_MINUTES} minutes')
        `).run();

        if (info.changes > 0) {
            console.log(`[System] Reclaimed ${info.changes} abandoned chunks back to the pool.`);
        }
    }, 60000);

    app.listen(PORT, '127.0.0.1', () => {
        console.log(`[puzzpool] server running on http://127.0.0.1:${PORT}`);
        console.log(`[puzzpool] database: ${DB_PATH}`);
        if (process.env.ADMIN_TOKEN) console.log('[puzzpool] admin token auth: enabled');
    });
}

module.exports = {
    createApp,
    isValidHex,
    randomBigIntInRange,
    normalizeHashrate,
    seedSectors,
    seedVirtualChunks,
    deriveAffinePermutationParams,
    permuteIndexAffine,
    defaultAllocSeedForPuzzle,
    chooseDefaultVirtualChunkSize,
    ACTIVE_MINUTES,
    REACTIVATE_MINUTES,
    ALLOC_STRATEGY_LEGACY,
    ALLOC_STRATEGY_VCHUNKS,
    DEFAULT_VIRTUAL_CHUNK_SIZE_KEYS,
    MAX_STORED_VCHUNKS,
    MAX_ALLOC_PROBES,
};