const express = require('express');
const Database = require('better-sqlite3');
const crypto = require('crypto');
const fs = require('fs');

// --- Configuration ---
const PORT            = parseInt(process.env.PORT || '8888', 10);
const DB_PATH         = process.env.DB_PATH || 'pool.db';
const TARGET_MINUTES  = parseInt(process.env.TARGET_MINUTES || '5', 10);
const TIMEOUT_MINUTES = parseInt(process.env.TIMEOUT_MINUTES || '15', 10);
const TARGET_SECTORS  = BigInt(parseInt(process.env.TARGET_SECTORS || '65536', 10));

// Dashboard freshness threshold only. Green = recent heartbeat; gray = visible but stale.
// This controls UI state only and must not affect chunk ownership/reclaim logic.
// Accepts fractional minutes (e.g. 0.333 ≈ 20 seconds).
const DEFAULT_ACTIVE_MINUTES = 1.167;
const rawActiveMinutes = parseFloat(process.env.ACTIVE_MINUTES || String(DEFAULT_ACTIVE_MINUTES));
const ACTIVE_MINUTES_REQUESTED =
    Number.isFinite(rawActiveMinutes) && rawActiveMinutes > 0 ? rawActiveMinutes : DEFAULT_ACTIVE_MINUTES;
const ACTIVE_MINUTES = Math.max(0.1, Math.min(ACTIVE_MINUTES_REQUESTED, TIMEOUT_MINUTES / 2));

// Ownership/reactivation threshold — separate from ACTIVE_MINUTES on purpose.
// ACTIVE_MINUTES controls dashboard coloring; REACTIVATE_MINUTES controls when a
// returning worker loses its previous assignment. Keep aligned with chunk-timeout semantics.
const REACTIVATE_MINUTES = TIMEOUT_MINUTES;

// New allocator defaults
const ALLOC_STRATEGY_LEGACY = 'legacy_random_shards_v1';
const ALLOC_STRATEGY_GLOBAL = 'random_global_blocks_v1';
const DEFAULT_ALLOC_STRATEGY = process.env.DEFAULT_ALLOC_STRATEGY || ALLOC_STRATEGY_GLOBAL;

// Auto-sizing keeps block count <= DEFAULT_TARGET_BLOCKS (2^18) by doubling the
// minimum block size (2^30) until the target is met. Hard cap at MAX_PRECOMPUTED_BLOCKS.
const DEFAULT_TARGET_BLOCKS  = 262144;   // 2^18 — target block count
const MAX_PRECOMPUTED_BLOCKS = 1048576;  // 2^20 — absolute upper limit

// --- Pure helpers (no db dependency) ---

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
    const byteLen = Math.ceil(hexLen / 2) + 8; // bias < 1/2^64
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

function defaultAllocSeedForPuzzle(puzzle) {
    return sha256Hex(`${puzzle.name}|${puzzle.start_hex}|${puzzle.end_hex}|random_global_blocks_v1`);
}

function bigIntMin(a, b) {
    return a < b ? a : b;
}


function normalizedRange(startHex, endHex) {
    const start = BigInt('0x' + startHex);
    const end = BigInt('0x' + endHex);
    if (end <= start) throw new Error('Puzzle range must be > 0');
    return { start, end, range: end - start };
}

function deriveBlockSizeForRange(range, requestedBlockSize) {
    let size = requestedBlockSize > 0n ? requestedBlockSize : 1n;
    if (size > range) size = range;
    return size;
}

function computeBlockCount(range, blockSize) {
    return (range + blockSize - 1n) / blockSize;
}

function chooseDefaultAllocBlockSize(range) {
    let size = 1n << 30n;
    while (computeBlockCount(range, size) > BigInt(DEFAULT_TARGET_BLOCKS)) {
        size <<= 1n;
    }
    return size;
}

function validateBlockCountOrThrow(blockCountBig, context) {
    if (blockCountBig > BigInt(MAX_PRECOMPUTED_BLOCKS)) {
        throw new Error(
            `${context}: block count ${blockCountBig.toString()} exceeds MAX_PRECOMPUTED_BLOCKS ` +
            `(${MAX_PRECOMPUTED_BLOCKS}). Use a larger alloc_block_size_keys value.`
        );
    }
}

function makePermutationRng(seedHex) {
    let counter = 0n;
    return function nextU64() {
        const material = `${seedHex}:${counter.toString()}`;
        counter++;
        const h = crypto.createHash('sha256').update(material).digest('hex');
        return BigInt('0x' + h.slice(0, 16));
    };
}

function buildDeterministicPermutation(count, seedHex) {
    if (count < 0) throw new Error('count must be >= 0');
    if (count > MAX_PRECOMPUTED_BLOCKS) {
        throw new Error(`buildDeterministicPermutation: count ${count} exceeds MAX_PRECOMPUTED_BLOCKS (${MAX_PRECOMPUTED_BLOCKS})`);
    }
    const arr = Array.from({ length: count }, (_, i) => i);
    const nextU64 = makePermutationRng(seedHex);

    for (let i = count - 1; i > 0; i--) {
        const j = Number(nextU64() % BigInt(i + 1));
        const tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
    return arr;
}

// Legacy sector seeding retained for old puzzles / compatibility.
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

// New allocator seeding
function seedGlobalBlocks(db, puzzleId, startHex, endHex, allocSeed, blockSizeKeys) {
    const { start, end, range } = normalizedRange(startHex, endHex);
    const blockSize = deriveBlockSizeForRange(range, blockSizeKeys);
    const blockCountBig = computeBlockCount(range, blockSize);
    validateBlockCountOrThrow(blockCountBig, 'seedGlobalBlocks');
    const blockCount = Number(blockCountBig);

    const permutation = buildDeterministicPermutation(blockCount, allocSeed);

    const insertBlock = db.prepare(`
        INSERT INTO alloc_blocks (
            puzzle_id, block_index, start_hex, end_hex, current_hex, status
        ) VALUES (?, ?, ?, ?, ?, 'open')
    `);

    const insertOrder = db.prepare(`
        INSERT INTO alloc_order (puzzle_id, order_index, block_index)
        VALUES (?, ?, ?)
    `);

    db.transaction(() => {
        // Clear any partial state so reseeding is always safe after an interrupted boot.
        db.prepare("DELETE FROM alloc_order WHERE puzzle_id = ?").run(puzzleId);
        db.prepare("DELETE FROM alloc_blocks WHERE puzzle_id = ?").run(puzzleId);

        db.prepare(`
            UPDATE puzzles
            SET alloc_strategy = ?,
                alloc_seed = ?,
                alloc_cursor = 0,
                alloc_block_size_keys = ?,
                alloc_block_count = ?
            WHERE id = ?
        `).run(
            ALLOC_STRATEGY_GLOBAL,
            allocSeed,
            blockSize.toString(),
            blockCount,
            puzzleId
        );

        for (let i = 0; i < blockCount; i++) {
            const blockStart = start + BigInt(i) * blockSize;
            const blockEnd = bigIntMin(blockStart + blockSize, end);
            const blockStartHex = blockStart.toString(16).padStart(64, '0');
            const blockEndHex = blockEnd.toString(16).padStart(64, '0');
            insertBlock.run(puzzleId, i, blockStartHex, blockEndHex, blockStartHex);
        }

        for (let orderIndex = 0; orderIndex < blockCount; orderIndex++) {
            insertOrder.run(puzzleId, orderIndex, permutation[orderIndex]);
        }
    })();

    console.log(`[System] Seeded ${blockCount} global blocks for puzzle ${puzzleId} (block size ${blockSize.toString()} keys)`);
}

function ensureAllocatorForPuzzle(db, puzzleId) {
    const puzzle = db.prepare("SELECT * FROM puzzles WHERE id = ?").get(puzzleId);
    if (!puzzle) throw new Error(`Puzzle ${puzzleId} not found`);

    const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;

    if (strategy === ALLOC_STRATEGY_GLOBAL) {
        const blockCount = db.prepare("SELECT COUNT(*) AS c FROM alloc_blocks WHERE puzzle_id = ?").get(puzzleId).c;
        const orderCount = db.prepare("SELECT COUNT(*) AS c FROM alloc_order WHERE puzzle_id = ?").get(puzzleId).c;

        if (blockCount === 0 || orderCount === 0) {
            const seed = puzzle.alloc_seed || defaultAllocSeedForPuzzle(puzzle);
            const { range } = normalizedRange(puzzle.start_hex, puzzle.end_hex);
            const blockSize = (() => {
                if (puzzle.alloc_block_size_keys) {
                    try {
                        const stored = BigInt(puzzle.alloc_block_size_keys);
                        const actual = deriveBlockSizeForRange(range, stored);
                        if (computeBlockCount(range, actual) <= BigInt(MAX_PRECOMPUTED_BLOCKS)) {
                            return stored;
                        }
                    } catch (_) {}
                }
                return chooseDefaultAllocBlockSize(range);
            })();
            seedGlobalBlocks(db, puzzle.id, puzzle.start_hex, puzzle.end_hex, seed, blockSize);
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
    // Shared prepared statements
    const stmtWorkerHash = db.prepare("SELECT hashrate FROM workers WHERE name = ?");
    const stmtChunkCount = db.prepare("SELECT COUNT(*) as cnt FROM chunks WHERE puzzle_id = ? AND is_test = 0");

    // Legacy allocator statements
    const stmtOpenSector      = db.prepare("SELECT * FROM sectors WHERE puzzle_id = ? AND status = 'open' ORDER BY RANDOM() LIMIT 1");
    const stmtOpenSectorAt    = db.prepare("SELECT * FROM sectors WHERE puzzle_id = ? AND status = 'open' ORDER BY id ASC LIMIT 1 OFFSET ?");
    const stmtSectorDone      = db.prepare("UPDATE sectors SET current_hex = end_hex, status = 'done' WHERE id = ?");
    const stmtSectorAdvance   = db.prepare("UPDATE sectors SET current_hex = ? WHERE id = ?");

    // New allocator statements
    const stmtAllocPuzzle = db.prepare(`
        SELECT id, alloc_strategy, alloc_seed, alloc_cursor, alloc_block_size_keys, alloc_block_count, bootstrap_done,
               start_hex, end_hex, name
        FROM puzzles
        WHERE id = ?
    `);

    const stmtAllocOrderAt = db.prepare(`
        SELECT ao.block_index, ab.id AS alloc_block_id, ab.start_hex, ab.end_hex, ab.current_hex, ab.status
        FROM alloc_order ao
        JOIN alloc_blocks ab
          ON ab.puzzle_id = ao.puzzle_id
         AND ab.block_index = ao.block_index
        WHERE ao.puzzle_id = ? AND ao.order_index = ?
        LIMIT 1
    `);

    const stmtAllocCursorAdvance = db.prepare(`
        UPDATE puzzles
        SET alloc_cursor = ?
        WHERE id = ? AND alloc_cursor = ?
    `);

    const stmtAllocBlockAdvance = db.prepare(`
        UPDATE alloc_blocks SET current_hex = ? WHERE id = ?
    `);

    const stmtAllocBlockDone = db.prepare(`
        UPDATE alloc_blocks SET current_hex = end_hex, status = 'done' WHERE id = ?
    `);

    const stmtAllocBlocksCompleted = db.prepare(`
        SELECT COUNT(*) AS c FROM alloc_blocks WHERE puzzle_id = ? AND status = 'done'
    `);

    const stmtInsertChunk = db.prepare(`
        INSERT INTO chunks (
            puzzle_id, start_hex, end_hex, status, worker_name, assigned_at, is_test, sector_id, alloc_block_id
        ) VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, 0, ?, ?)
    `);

    // Test chunk statements
    const stmtTestChunkTaken  = db.prepare(`
        SELECT id
        FROM chunks
        WHERE puzzle_id = ? AND start_hex = ? AND end_hex = ? AND is_test = 1 AND status = 'assigned'
        LIMIT 1
    `);

    const stmtTestChunkReclaim = db.prepare(`
        UPDATE chunks
        SET status = 'assigned', worker_name = ?, assigned_at = CURRENT_TIMESTAMP
        WHERE id = (
            SELECT id
            FROM chunks
            WHERE puzzle_id = ? AND start_hex = ? AND end_hex = ? AND is_test = 1 AND status = 'reclaimed'
            LIMIT 1
        )
        RETURNING *
    `);

    const stmtTestChunkInsert = db.prepare(`
        INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at, is_test)
        VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, 1)
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

            const info = stmtInsertChunk.run(puzzle.id, startHex, endHex, name, sector.id, null);
            return { chunkId: info.lastInsertRowid, startHex, endHex };
        }
    });

    const assignGlobalBlockChunk = db.transaction((name, hashrate, puzzle) => {
        const freshChunkCount = stmtChunkCount.get(puzzle.id).cnt;
        const hashrateBig = normalizeHashrate(hashrate || stmtWorkerHash.get(name)?.hashrate);
        const requestedChunkSize = hashrateBig * BigInt(TARGET_MINUTES * 60);

        // Bootstrap: the very first fresh chunk goes to the physical midpoint block
        // (block_index = floor(blockCount / 2)), preserving the old allocator's
        // "known mid-keyspace first" behavior so operators can verify end-to-end
        // scanning. This block is consumed OUTSIDE the randomized alloc_order sequence:
        // alloc_cursor stays at 0 after bootstrap. When the normal loop later reaches
        // the position in alloc_order that maps to this same block, it finds
        // alloc_blocks.current_hex already advanced and continues from there — no gap,
        // no overlap. bootstrap_done prevents this path from running again.
        if (!puzzle.bootstrap_done && freshChunkCount === 0) {
            const storedBlockCount = puzzle.alloc_block_count ? Number(puzzle.alloc_block_count) : 0;
            let blockIndex = 0;
            if (storedBlockCount > 1) {
                blockIndex = Math.floor(storedBlockCount / 2);
            }

            const bootstrapBlock = db.prepare(`
                SELECT id, block_index, start_hex, end_hex, current_hex, status
                FROM alloc_blocks
                WHERE puzzle_id = ? AND block_index = ?
                LIMIT 1
            `).get(puzzle.id, blockIndex);

            if (bootstrapBlock) {
                const current = BigInt('0x' + bootstrapBlock.current_hex);
                const blockEnd = BigInt('0x' + bootstrapBlock.end_hex);
                const effective = bigIntMin(requestedChunkSize, blockEnd - current);

                if (effective > 0n) {
                    const startHex = current.toString(16).padStart(64, '0');
                    const endHex = (current + effective).toString(16).padStart(64, '0');

                    if (current + effective >= blockEnd) {
                        stmtAllocBlockDone.run(bootstrapBlock.id);
                    } else {
                        stmtAllocBlockAdvance.run(endHex, bootstrapBlock.id);
                    }

                    db.prepare("UPDATE puzzles SET bootstrap_done = 1 WHERE id = ?").run(puzzle.id);
                    const info = stmtInsertChunk.run(puzzle.id, startHex, endHex, name, null, bootstrapBlock.id);
                    return { chunkId: info.lastInsertRowid, startHex, endHex };
                }

                // Defensive cleanup if the bootstrap target is already exhausted
                stmtAllocBlockDone.run(bootstrapBlock.id);
                db.prepare("UPDATE puzzles SET bootstrap_done = 1 WHERE id = ?").run(puzzle.id);
            } else {
                db.prepare("UPDATE puzzles SET bootstrap_done = 1 WHERE id = ?").run(puzzle.id);
            }
        }

        for (;;) {
            const allocPuzzle = stmtAllocPuzzle.get(puzzle.id);
            if (!allocPuzzle) return null;

            const cursor = Number(allocPuzzle.alloc_cursor || 0);
            const blockCount = Number(allocPuzzle.alloc_block_count || 0);
            if (cursor >= blockCount) return null;

            const slot = stmtAllocOrderAt.get(puzzle.id, cursor);
            if (!slot) {
                // Broken order row — skip it defensively.
                stmtAllocCursorAdvance.run(cursor + 1, puzzle.id, cursor);
                continue;
            }

            if (slot.status === 'done') {
                stmtAllocCursorAdvance.run(cursor + 1, puzzle.id, cursor);
                continue;
            }

            const current = BigInt('0x' + slot.current_hex);
            const blockEnd = BigInt('0x' + slot.end_hex);
            const effective = bigIntMin(requestedChunkSize, blockEnd - current);

            if (effective <= 0n) {
                stmtAllocBlockDone.run(slot.alloc_block_id);
                stmtAllocCursorAdvance.run(cursor + 1, puzzle.id, cursor);
                continue;
            }

            const startHex = current.toString(16).padStart(64, '0');
            const endHex   = (current + effective).toString(16).padStart(64, '0');

            if (current + effective >= blockEnd) {
                stmtAllocBlockDone.run(slot.alloc_block_id);
                stmtAllocCursorAdvance.run(cursor + 1, puzzle.id, cursor);
            } else {
                stmtAllocBlockAdvance.run(endHex, slot.alloc_block_id);
            }

            db.prepare("UPDATE puzzles SET bootstrap_done = 1 WHERE id = ?").run(puzzle.id);
            const info = stmtInsertChunk.run(puzzle.id, startHex, endHex, name, null, slot.alloc_block_id);
            return { chunkId: info.lastInsertRowid, startHex, endHex };
        }
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
        const { name, hashrate, version } = req.body;
        if (!name) return res.status(400).json({ error: 'Missing name' });

        const hashrateNum = Number(normalizeHashrate(hashrate));

        const prevWorker = db.prepare(
            `SELECT CASE WHEN last_seen < datetime('now', '-${REACTIVATE_MINUTES} minutes') THEN 1 ELSE 0 END AS inactive FROM workers WHERE name = ?`
        ).get(name);
        const isReactivating = prevWorker?.inactive === 1;

        if (isReactivating) {
            db.prepare(`
                UPDATE chunks
                SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL, assigned_at = NULL
                WHERE worker_name = ? AND status = 'assigned'
            `).run(name);
        }

        db.prepare(`
            INSERT INTO workers (name, hashrate, last_seen, version)
            VALUES (?, ?, CURRENT_TIMESTAMP, ?)
            ON CONFLICT(name) DO UPDATE
            SET hashrate = excluded.hashrate,
                last_seen = CURRENT_TIMESTAMP,
                version = COALESCE(excluded.version, workers.version)
        `).run(name, hashrateNum, version || null);

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
            UPDATE chunks SET status = 'assigned', worker_name = ?, assigned_at = CURRENT_TIMESTAMP
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

            if (strategy === ALLOC_STRATEGY_GLOBAL) {
                result = assignGlobalBlockChunk.immediate(name, hashrate, puzzle);
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
                        SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL, assigned_at = NULL
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
            SELECT w.name, w.hashrate, w.last_seen, w.version,
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
                   CASE WHEN c.alloc_block_id IS NOT NULL THEN c.id
                        WHEN c.sector_id IS NOT NULL THEN c.id
                        ELSE NULL
                   END AS chunk_global,
                   CASE
                       WHEN c.alloc_block_id IS NOT NULL THEN
                           (SELECT COUNT(*) FROM chunks c2 WHERE c2.alloc_block_id = c.alloc_block_id AND c2.id < c.id)
                       WHEN c.sector_id IS NOT NULL THEN
                           (SELECT COUNT(*) FROM chunks c2 WHERE c2.sector_id = c.sector_id AND c2.id < c.id)
                       ELSE NULL
                   END AS chunk,
                   CASE
                       WHEN c.alloc_block_id IS NOT NULL THEN
                           (SELECT ab.block_index FROM alloc_blocks ab WHERE ab.id = c.alloc_block_id)
                       WHEN c.sector_id IS NOT NULL THEN
                           (SELECT COUNT(*) FROM sectors s2 WHERE s2.puzzle_id = c.puzzle_id AND s2.id < c.sector_id)
                       ELSE NULL
                   END AS shard
            FROM findings f
            JOIN chunks c ON c.id = f.chunk_id
            WHERE c.puzzle_id = ? AND c.is_test = 0
            ORDER BY f.id ASC
        `).all(pid) : [];

        const assignedNow = puzzle ? db.prepare(`
            SELECT c.id, c.worker_name,
                   CASE
                       WHEN c.alloc_block_id IS NOT NULL THEN
                           (SELECT ab.block_index FROM alloc_blocks ab WHERE ab.id = c.alloc_block_id)
                       WHEN c.sector_id IS NOT NULL THEN
                           (SELECT COUNT(*) FROM sectors s2 WHERE s2.puzzle_id = c.puzzle_id AND s2.id < c.sector_id)
                       ELSE NULL
                   END AS unit_num,
                   CASE
                       WHEN c.alloc_block_id IS NOT NULL THEN
                           (SELECT COUNT(*) FROM chunks c2 WHERE c2.alloc_block_id = c.alloc_block_id AND c2.id < c.id)
                       WHEN c.sector_id IS NOT NULL THEN
                           (SELECT COUNT(*) FROM chunks c2 WHERE c2.sector_id = c.sector_id AND c2.id < c.id)
                       ELSE NULL
                   END AS chunk_in_shard
            FROM chunks c
            WHERE c.status = 'assigned' AND c.puzzle_id = ? AND c.is_test = 0
        `).all(puzzle.id) : [];

        const workerChunkMap = {};
        const workerShardMap = {};
        const workerChunkInShardMap = {};
        for (const c of assignedNow) {
            workerChunkMap[c.worker_name] = c.id;
            workerShardMap[c.worker_name] = c.unit_num;
            workerChunkInShardMap[c.worker_name] = c.chunk_in_shard;
        }

        const workers = visibleWorkers.map(w => ({
            ...w,
            fresh: w.fresh === 1,
            assigned_here: w.assigned_here === 1,
            active: w.active === 1,
            current_chunk: workerChunkMap[w.name] ?? null,
            current_shard: workerShardMap[w.name] ?? null,
            current_chunk_in_shard: workerChunkInShardMap[w.name] ?? null,
        }));

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
                    w: c.worker_name,
                    s: Number(cs * 1000000n / pRange) / 1000000,
                    e: Number(ce * 1000000n / pRange) / 1000000,
                };
            });
        }

        const allPuzzles = db.prepare("SELECT id, name, active FROM puzzles ORDER BY id ASC").all();

        const shardsTotal = pid
            ? (() => {
                const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;
                if (strategy === ALLOC_STRATEGY_GLOBAL) {
                    return puzzle.alloc_block_count || 0;
                }
                return db.prepare("SELECT COUNT(*) AS c FROM sectors WHERE puzzle_id = ?").get(pid).c;
            })()
            : 0;

        const shardsCompleted = pid
            ? (() => {
                const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;
                if (strategy === ALLOC_STRATEGY_GLOBAL) {
                    return stmtAllocBlocksCompleted.get(pid).c;
                }
                return db.prepare("SELECT COUNT(*) AS c FROM sectors WHERE puzzle_id = ? AND status = 'done'").get(pid).c;
            })()
            : 0;

        const shardsStarted = pid
            ? (() => {
                const strategy = puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY;
                if (strategy === ALLOC_STRATEGY_GLOBAL) {
                    return db.prepare(`
                        SELECT COUNT(DISTINCT alloc_block_id) AS c
                        FROM chunks
                        WHERE puzzle_id = ? AND is_test = 0 AND alloc_block_id IS NOT NULL
                    `).get(pid).c;
                }
                return db.prepare(`
                    SELECT COUNT(DISTINCT sector_id) AS c
                    FROM chunks
                    WHERE puzzle_id = ? AND is_test = 0 AND sector_id IS NOT NULL
                `).get(pid).c;
            })()
            : 0;

        res.json({
            stage: process.env.STAGE || 'PROD',
            puzzles: allPuzzles,
            puzzle: puzzle ? {
                id: puzzle.id,
                name: puzzle.name,
                start_hex: puzzle.start_hex,
                end_hex: puzzle.end_hex,
                total_keys: (BigInt('0x' + puzzle.end_hex) - BigInt('0x' + puzzle.start_hex)).toString(),
                test_chunk: puzzle.test_start_hex ? {
                    start_hex: puzzle.test_start_hex,
                    end_hex: puzzle.test_end_hex,
                } : null,
                alloc_strategy: puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY,
                alloc_cursor: puzzle.alloc_cursor || 0,
                alloc_block_count: puzzle.alloc_block_count || null,
                alloc_blocks_completed: (puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY) === ALLOC_STRATEGY_GLOBAL
                    ? stmtAllocBlocksCompleted.get(puzzle.id).c
                    : null,
            } : null,
            active_workers_count: visibleWorkers.filter(w => w.active).length,
            inactive_workers_count: visibleWorkers.filter(w => !w.active).length,
            total_hashrate: totalHashrate,
            completed_chunks: completedChunks,
            reclaimed_chunks: reclaimedChunks,
            total_keys_completed: totalKeysCompleted.toString(),
            shards: { total: shardsTotal, started: shardsStarted, completed: shardsCompleted },
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
            UPDATE chunks SET assigned_at = CURRENT_TIMESTAMP
            WHERE id = ? AND worker_name = ? AND status = 'assigned'
        `).run(job_id, name);

        res.json({ ok: true });
    });

    app.post('/api/v1/admin/set-puzzle', (req, res) => {
        const { name, start_hex, end_hex, alloc_strategy, alloc_seed, alloc_block_size_keys } = req.body;

        if (!name || !start_hex || !end_hex) {
            return res.status(400).json({ error: 'Missing name, start_hex, or end_hex' });
        }
        if (!isValidHex(start_hex) || !isValidHex(end_hex)) {
            return res.status(400).json({ error: 'start_hex and end_hex must be valid hex strings' });
        }

        const strategy = alloc_strategy || DEFAULT_ALLOC_STRATEGY;
        if (strategy !== ALLOC_STRATEGY_LEGACY && strategy !== ALLOC_STRATEGY_GLOBAL) {
            return res.status(400).json({ error: `alloc_strategy must be ${ALLOC_STRATEGY_LEGACY} or ${ALLOC_STRATEGY_GLOBAL}` });
        }

        const startNorm = start_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();
        const endNorm   = end_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();

        if (BigInt('0x' + endNorm) <= BigInt('0x' + startNorm)) {
            return res.status(400).json({ error: 'end_hex must be greater than start_hex' });
        }

        const puzzleRange = BigInt('0x' + endNorm) - BigInt('0x' + startNorm);
        let blockSize;
        if (alloc_block_size_keys !== undefined && alloc_block_size_keys !== null && String(alloc_block_size_keys) !== '') {
            try {
                blockSize = BigInt(String(alloc_block_size_keys));
                if (blockSize <= 0n) throw new Error('bad');
            } catch (_) {
                return res.status(400).json({ error: 'alloc_block_size_keys must be a positive integer string' });
            }
        } else {
            blockSize = chooseDefaultAllocBlockSize(puzzleRange);
        }

        db.transaction(() => {
            db.prepare("UPDATE puzzles SET active = 0").run();

            const existing = db.prepare("SELECT * FROM puzzles WHERE name = ?").get(name);
            let puzzleId;

            const seed = alloc_seed || defaultAllocSeedForPuzzle({ name, start_hex: startNorm, end_hex: endNorm });
            const seedMatches = !existing || existing.alloc_seed === seed;
            const blockSizeMatches = !existing || !existing.alloc_block_size_keys ||
                existing.alloc_block_size_keys === blockSize.toString();

            if (
                existing &&
                existing.start_hex === startNorm &&
                existing.end_hex === endNorm &&
                (existing.alloc_strategy || ALLOC_STRATEGY_LEGACY) === strategy &&
                seedMatches &&
                blockSizeMatches
            ) {
                db.prepare("UPDATE puzzles SET active = 1 WHERE id = ?").run(existing.id);
                puzzleId = existing.id;
                ensureAllocatorForPuzzle(db, puzzleId);
            } else {
                const info = db.prepare(`
                    INSERT INTO puzzles (
                        name, start_hex, end_hex, active,
                        alloc_strategy, alloc_seed, alloc_cursor, alloc_block_size_keys, alloc_block_count, bootstrap_done
                    )
                    VALUES (?, ?, ?, 1, ?, ?, 0, ?, NULL, 0)
                `).run(
                    name,
                    startNorm,
                    endNorm,
                    strategy,
                    seed,
                    strategy === ALLOC_STRATEGY_GLOBAL ? blockSize.toString() : null
                );

                puzzleId = info.lastInsertRowid;

                if (strategy === ALLOC_STRATEGY_GLOBAL) {
                    seedGlobalBlocks(db, puzzleId, startNorm, endNorm, seed, blockSize);
                } else {
                    seedSectors(db, puzzleId, startNorm, endNorm);
                }
            }
        })();

        const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
        console.log(`[Admin] Active puzzle set to: ${puzzle.name} [${puzzle.start_hex} .. ${puzzle.end_hex}] strategy=${puzzle.alloc_strategy || ALLOC_STRATEGY_LEGACY}`);
        res.json({ ok: true, puzzle });
    });

    const GPU_BATCH_KEYS = 4278190080n;

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
            SELECT id, name, active, start_hex, end_hex, alloc_strategy, alloc_seed,
                   alloc_cursor, alloc_block_size_keys, alloc_block_count, bootstrap_done
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

      CREATE TABLE IF NOT EXISTS alloc_blocks (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        puzzle_id INTEGER NOT NULL,
        block_index INTEGER NOT NULL,
        start_hex TEXT NOT NULL,
        end_hex TEXT NOT NULL,
        current_hex TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'open'
      );

      CREATE UNIQUE INDEX IF NOT EXISTS idx_alloc_blocks_unique ON alloc_blocks (puzzle_id, block_index);
      CREATE INDEX IF NOT EXISTS idx_alloc_blocks_puzzle_status ON alloc_blocks (puzzle_id, status);
      CREATE INDEX IF NOT EXISTS idx_alloc_blocks_puzzle_id ON alloc_blocks (puzzle_id, id);

      CREATE TABLE IF NOT EXISTS alloc_order (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        puzzle_id INTEGER NOT NULL,
        order_index INTEGER NOT NULL,
        block_index INTEGER NOT NULL
      );

      CREATE UNIQUE INDEX IF NOT EXISTS idx_alloc_order_unique_order ON alloc_order (puzzle_id, order_index);
      CREATE UNIQUE INDEX IF NOT EXISTS idx_alloc_order_unique_block ON alloc_order (puzzle_id, block_index);
    `);

    try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_start_hex TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_end_hex TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN is_test INTEGER NOT NULL DEFAULT 0").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN prev_worker_name TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN sector_id INTEGER").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE chunks ADD COLUMN alloc_block_id INTEGER").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE workers ADD COLUMN version TEXT").run(); } catch (_) {}

    try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_strategy TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_seed TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_cursor INTEGER NOT NULL DEFAULT 0").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_block_size_keys TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_block_count INTEGER").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN bootstrap_done INTEGER NOT NULL DEFAULT 0").run(); } catch (_) {}

    try { db.prepare("CREATE UNIQUE INDEX IF NOT EXISTS idx_findings_dedup ON findings (chunk_id, worker_name, found_key)").run(); } catch (_) {}

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
            const seed = defaultAllocSeedForPuzzle({ name, start_hex: startNorm, end_hex: endNorm });
            const ksRange = BigInt('0x' + endNorm) - BigInt('0x' + startNorm);
            const ksBlockSize = strategy === ALLOC_STRATEGY_GLOBAL ? chooseDefaultAllocBlockSize(ksRange) : null;

            const info = db.prepare(`
                INSERT INTO puzzles (
                    name, start_hex, end_hex, active,
                    alloc_strategy, alloc_seed, alloc_cursor, alloc_block_size_keys, alloc_block_count, bootstrap_done
                )
                VALUES (?, ?, ?, 0, ?, ?, 0, ?, NULL, 0)
            `).run(
                name,
                startNorm,
                endNorm,
                strategy,
                seed,
                ksBlockSize ? ksBlockSize.toString() : null
            );

            console.log(`[Config] Seeded keyspace: ${name}`);
            if (strategy === ALLOC_STRATEGY_GLOBAL) {
                seedGlobalBlocks(db, info.lastInsertRowid, startNorm, endNorm, seed, ksBlockSize);
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
        const seed = defaultAllocSeedForPuzzle({ name, start_hex: startHex, end_hex: endHex });
        const p71Range = BigInt('0x' + endHex) - BigInt('0x' + startHex);
        const p71BlockSize = strategy === ALLOC_STRATEGY_GLOBAL ? chooseDefaultAllocBlockSize(p71Range) : null;

        const info = db.prepare(`
            INSERT INTO puzzles (
                name, start_hex, end_hex, active,
                alloc_strategy, alloc_seed, alloc_cursor, alloc_block_size_keys, alloc_block_count, bootstrap_done
            )
            VALUES (?, ?, ?, 1, ?, ?, 0, ?, NULL, 0)
        `).run(
            name,
            startHex,
            endHex,
            strategy,
            seed,
            p71BlockSize ? p71BlockSize.toString() : null
        );

        console.log('[Init] Seeded Puzzle #71 as active puzzle.');
        if (strategy === ALLOC_STRATEGY_GLOBAL) {
            seedGlobalBlocks(db, info.lastInsertRowid, startHex, endHex, seed, p71BlockSize);
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
            SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL
            WHERE status = 'assigned'
              AND assigned_at < datetime('now', '-${TIMEOUT_MINUTES} minutes')
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
    seedGlobalBlocks,
    buildDeterministicPermutation,
    defaultAllocSeedForPuzzle,
    chooseDefaultAllocBlockSize,
    ACTIVE_MINUTES,
    REACTIVATE_MINUTES,
    ALLOC_STRATEGY_LEGACY,
    ALLOC_STRATEGY_GLOBAL,
    DEFAULT_TARGET_BLOCKS,
    MAX_PRECOMPUTED_BLOCKS,
};