const express = require('express');
const Database = require('better-sqlite3');
const crypto = require('crypto');
const fs = require('fs');

// --- Configuration ---
const PORT            = parseInt(process.env.PORT           || '8888', 10);
const DB_PATH         = process.env.DB_PATH                 || 'pool.db';
const TARGET_MINUTES  = parseInt(process.env.TARGET_MINUTES  || '5',    10);
const TIMEOUT_MINUTES = parseInt(process.env.TIMEOUT_MINUTES || '15',   10);

// --- Pure helpers (no db dependency) ---

// Validate that a string is a non-empty hex string (0x prefix optional, any case).
// Callers must normalize (strip 0x, lowercase, padStart) before any DB write or comparison.
function isValidHex(s) {
    if (typeof s !== 'string' || s.length === 0) return false;
    return /^(0x)?[0-9a-fA-F]+$/.test(s);
}

// Exported for consumers that need arbitrary-range random sampling.
// Generate a uniformly random BigInt in [min, max) with negligible bias.
function randomBigIntInRange(min, max) {
    const range = max - min;
    if (range <= 0n) return min;
    const hexLen = range.toString(16).length;
    const byteLen = Math.ceil(hexLen / 2) + 8; // 8 extra bytes → bias < 1/2^64
    const bytes = crypto.randomBytes(byteLen);
    const n = BigInt('0x' + bytes.toString('hex'));
    return min + (n % range);
}

// Coerce any client-supplied hashrate to a safe positive BigInt.
// Returns BigInt(fallback) for NaN, Infinity, negative, zero, or non-numeric input.
function normalizeHashrate(input, fallback = 1_000_000) {
    const n = Number(input);
    if (!Number.isFinite(n) || n <= 0) return BigInt(fallback);
    return BigInt(Math.max(1, Math.floor(n)));
}

// Divide puzzle keyspace into sectors with independent frontiers.
// All intervals are half-open [start, end). numSectors clamped to [1, TARGET_SECTORS].
function seedSectors(db, puzzleId, startHex, endHex) {
    const TARGET_SECTORS  = 65536n;
    const MIN_SECTOR_SIZE = 1_000_000_000n;

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

    console.log(`[System] Seeded ${numSectors} sectors for puzzle ${puzzleId}`);
}

// --- App factory (accepts db for testability) ---

function createApp(db) {
    // Prepared statements hoisted here so they are compiled once, not on every request.
    const stmtOpenSector      = db.prepare("SELECT * FROM sectors WHERE puzzle_id = ? AND status = 'open' ORDER BY RANDOM() LIMIT 1");
    const stmtWorkerHash      = db.prepare("SELECT hashrate FROM workers WHERE name = ?");
    const stmtSectorDone      = db.prepare("UPDATE sectors SET current_hex = end_hex, status = 'done' WHERE id = ?");
    const stmtSectorAdvance   = db.prepare("UPDATE sectors SET current_hex = ? WHERE id = ?");
    const stmtInsertChunk     = db.prepare("INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at, is_test) VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, 0)");
    // Both taken/reclaim queries match on start_hex + end_hex + is_test = 1 so a different
    // test chunk with the same start but different end cannot interfere.
    const stmtTestChunkTaken  = db.prepare("SELECT id FROM chunks WHERE puzzle_id = ? AND start_hex = ? AND end_hex = ? AND is_test = 1 AND status IN ('assigned', 'completed', 'FOUND') LIMIT 1");
    const stmtTestChunkReclaim = db.prepare(`
        UPDATE chunks SET status = 'assigned', worker_name = ?, assigned_at = CURRENT_TIMESTAMP
        WHERE id = (SELECT id FROM chunks WHERE puzzle_id = ? AND start_hex = ? AND end_hex = ? AND is_test = 1 AND status = 'reclaimed' LIMIT 1)
        RETURNING *
    `);
    const stmtTestChunkInsert = db.prepare(`
        INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at, is_test)
        VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, 1)
        RETURNING *
    `);

    // Fresh allocation from sector frontier.
    // BEGIN IMMEDIATE acquires the write lock before the SELECT so two concurrent
    // connections cannot observe the same open sector. ORDER BY RANDOM() over
    // ≤65,536 rows is a deliberate tradeoff. Returns null only when all sectors
    // are exhausted; stale open sectors (effective=0) are cleaned up and retried.
    const assignRandomChunk = db.transaction((name, hashrate, puzzle) => {
        const hashrateBig = normalizeHashrate(hashrate || stmtWorkerHash.get(name)?.hashrate);
        const chunkSize   = hashrateBig * BigInt(TARGET_MINUTES * 60);

        for (;;) {
            const sector = stmtOpenSector.get(puzzle.id);
            if (!sector) return null;

            const current   = BigInt('0x' + sector.current_hex);
            const sectorEnd = BigInt('0x' + sector.end_hex);
            const effective = chunkSize < (sectorEnd - current) ? chunkSize : (sectorEnd - current);

            if (effective <= 0n) {
                // Stale open sector — clean up and try another
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

            const info = stmtInsertChunk.run(puzzle.id, startHex, endHex, name);
            return { chunkId: info.lastInsertRowid, startHex, endHex };
        }
    });

    // Test chunk claiming — IMMEDIATE transaction to prevent two concurrent requests
    // from both passing the "not yet taken" check before either inserts.
    // is_test = 1 is set on insert so these rows are excluded from puzzle stats.
    // Completed/FOUND test chunks count as permanently taken; admins who want to rerun
    // a test should call set-test-chunk again (even with the same range) — the next
    // /work request will issue a fresh insert because the old row has a different end_hex
    // or the taken check won't match a re-set range.
    const claimTestChunk = db.transaction((name, puzzle) => {
        const taken = stmtTestChunkTaken.get(puzzle.id, puzzle.test_start_hex, puzzle.test_end_hex);
        if (taken) return null;
        const reclaimed = stmtTestChunkReclaim.get(name, puzzle.id, puzzle.test_start_hex, puzzle.test_end_hex);
        if (reclaimed) return reclaimed;
        return stmtTestChunkInsert.get(puzzle.id, puzzle.test_start_hex, puzzle.test_end_hex, name);
    });

    const app = express();
    app.use(express.json());
    // Serve index.html with no-store so browsers always fetch the latest version.
    // Other static assets (images, etc.) can still be cached normally.
    app.get('/', (req, res) => {
        res.set('Cache-Control', 'no-store');
        res.sendFile('public/index.html', { root: '.' });
    });
    app.use(express.static('public'));

    // Optional admin token guard — enabled only when ADMIN_TOKEN env var is set.
    if (process.env.ADMIN_TOKEN) {
        app.use('/api/v1/admin', (req, res, next) => {
            if (req.headers['x-admin-token'] === process.env.ADMIN_TOKEN) return next();
            return res.status(401).json({ error: 'unauthorized' });
        });
    }

// --- API Endpoints ---

// 1. Request Work
app.post('/api/v1/work', (req, res) => {
    const { name, hashrate } = req.body;
    if (!name) return res.status(400).json({ error: "Missing name" });

    // Upsert worker — store normalized hashrate so /stats sorting and totals are safe
    const hashrateNum = Number(normalizeHashrate(hashrate));
    db.prepare(`
        INSERT INTO workers (name, hashrate, last_seen) VALUES (?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(name) DO UPDATE SET hashrate = excluded.hashrate, last_seen = CURRENT_TIMESTAMP
    `).run(name, hashrateNum);

    // Fetch active puzzle
    const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
    if (!puzzle) return res.status(503).json({ error: "No active puzzle configured" });

    let chunkId, startHex, endHex;

    // PRIORITY 1: Test chunk — atomic IMMEDIATE claim; only one worker ever receives it.
    if (puzzle.test_start_hex && puzzle.test_end_hex) {
        const claimed = claimTestChunk.immediate(name, puzzle);
        if (claimed) {
            console.log(`[Test] Assigned test chunk #${claimed.id} to ${name}`);
            return res.json({ job_id: claimed.id, start_key: claimed.start_hex, end_key: claimed.end_hex });
        }
    }

    // PRIORITY 2: Reissue reclaimed (timed-out) chunks — atomic fetch-and-assign.
    // Sector frontiers are never rolled back on reclaim; chunks table is the reissue source.
    // is_test = 0 guard prevents a reclaimed test chunk from leaking into normal work.
    const reclaimed = db.prepare(`
        UPDATE chunks SET status = 'assigned', worker_name = ?, assigned_at = CURRENT_TIMESTAMP
        WHERE id = (
            SELECT id FROM chunks WHERE status = 'reclaimed' AND puzzle_id = ? AND is_test = 0 LIMIT 1
        )
        RETURNING *
    `).get(name, puzzle.id);

    if (reclaimed) {
        chunkId  = reclaimed.id;
        startHex = reclaimed.start_hex;
        endHex   = reclaimed.end_hex;
    } else {
        // PRIORITY 3: Fresh allocation from sector frontier (BEGIN IMMEDIATE — write lock acquired
        // before the sector SELECT so two concurrent connections cannot pick the same sector)
        const result = assignRandomChunk.immediate(name, hashrate, puzzle);
        if (!result) return res.status(503).json({ error: 'All keyspace has been assigned' });
        ({ chunkId, startHex, endHex } = result);
    }

    res.json({ job_id: chunkId, start_key: startHex, end_key: endHex });
});

// 2. Submit Results
app.post('/api/v1/submit', (req, res) => {
    const { name, job_id, status, found_key, found_address } = req.body;

    if (status === "FOUND") {
        const msg = `[${new Date().toISOString()}] BINGO! Job: ${job_id} | Worker: ${name} | KEY: ${found_key} | ADDR: ${found_address || 'Unknown'}\n`;
        console.log(`\n🚨🚨🚨 ${msg}`);
        fs.appendFileSync('BINGO_FOUND_KEYS.txt', msg);

        db.prepare("INSERT INTO findings (chunk_id, worker_name, found_key, found_address) VALUES (?, ?, ?, ?)")
            .run(job_id, name, found_key, found_address || null);

        db.prepare("UPDATE chunks SET status = 'FOUND', found_key = COALESCE(found_key, ?), found_address = COALESCE(found_address, ?) WHERE id = ? AND worker_name = ?")
            .run(found_key, found_address || null, job_id, name);
    } else {
        db.prepare("UPDATE chunks SET status = 'completed' WHERE id = ? AND worker_name = ?")
            .run(job_id, name);
    }

    res.json({ accepted: true });
});

// 3. Dashboard Stats
app.get('/api/v1/stats', (req, res) => {
    // Optional ?puzzle_id=N lets the dashboard view a non-active puzzle's stats.
    const puzzleIdParam = req.query.puzzle_id ? parseInt(req.query.puzzle_id, 10) : null;
    const puzzle = puzzleIdParam
        ? (db.prepare("SELECT * FROM puzzles WHERE id = ?").get(puzzleIdParam) || null)
        : (db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get() || null);

    // All queries below are scoped to the requested (or active) puzzle so that
    // switching tabs shows only that puzzle's stats, workers, scores, and findings.
    const pid = puzzle ? puzzle.id : null;

    const activeWorkers = pid ? db.prepare(`
        SELECT DISTINCT w.name, w.hashrate, w.last_seen
        FROM workers w
        JOIN chunks c ON c.worker_name = w.name AND c.status = 'assigned' AND c.puzzle_id = ? AND c.is_test = 0
        WHERE w.last_seen >= datetime('now', '-10 minutes')
        ORDER BY w.hashrate DESC
    `).all(pid) : [];
    const totalHashrate = activeWorkers.reduce((sum, w) => sum + w.hashrate, 0);

    const completedChunks = pid ? db.prepare(
        "SELECT COUNT(*) as count FROM chunks WHERE puzzle_id = ? AND (status = 'completed' OR status = 'FOUND') AND is_test = 0"
    ).get(pid).count : 0;

    const doneChunks = pid ? db.prepare(`
        SELECT worker_name, start_hex, end_hex
        FROM chunks
        WHERE puzzle_id = ? AND (status = 'completed' OR status = 'FOUND') AND worker_name IS NOT NULL AND is_test = 0
    `).all(pid) : [];

    // Merge overlapping intervals before summing. After the sector-frontier migration,
    // fresh allocations never overlap; merging is retained to correctly handle duplicate
    // history rows that can arise from reclaimed chunk reissue.
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
        SELECT f.worker_name, f.found_key, f.found_address, f.created_at
        FROM findings f
        JOIN chunks c ON c.id = f.chunk_id
        WHERE c.puzzle_id = ? AND c.is_test = 0
        ORDER BY f.id ASC
    `).all(pid) : [];

    const assignedNow = puzzle
        ? db.prepare("SELECT id, worker_name FROM chunks WHERE status = 'assigned' AND puzzle_id = ? AND is_test = 0").all(puzzle.id)
        : [];
    const workerChunkMap = {};
    for (const c of assignedNow) workerChunkMap[c.worker_name] = c.id;

    const workers = activeWorkers.map(w => ({
        ...w,
        current_chunk: workerChunkMap[w.name] ?? null
    }));

    let chunks_vis = [];
    if (puzzle) {
        const pStart = BigInt('0x' + puzzle.start_hex);
        const pEnd   = BigInt('0x' + puzzle.end_hex);
        const pRange = pEnd - pStart;
        const rawChunks = db.prepare(`
            SELECT id, status, worker_name, start_hex, end_hex
            FROM chunks WHERE puzzle_id = ? AND is_test = 0
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
                end_hex:   puzzle.test_end_hex,
            } : null,
        } : null,
        active_workers_count: activeWorkers.length,
        total_hashrate: totalHashrate,
        completed_chunks: completedChunks,
        total_keys_completed: totalKeysCompleted.toString(),
        workers,
        scores,
        finders,
        chunks_vis,
    });
});

// 4. Heartbeat — keep worker visible and prevent chunk reclaim during long jobs
// POST /api/v1/heartbeat  body: { name, job_id }
app.post('/api/v1/heartbeat', (req, res) => {
    const { name, job_id } = req.body;
    if (!name || job_id === null || job_id === undefined) return res.status(400).json({ error: "Missing name or job_id" });

    db.prepare(`
        INSERT INTO workers (name, hashrate, last_seen) VALUES (?, 0, CURRENT_TIMESTAMP)
        ON CONFLICT(name) DO UPDATE SET last_seen = CURRENT_TIMESTAMP
    `).run(name);

    // Reset assigned_at so the 15-min reclaim timer doesn't fire
    db.prepare(`
        UPDATE chunks SET assigned_at = CURRENT_TIMESTAMP
        WHERE id = ? AND worker_name = ? AND status = 'assigned'
    `).run(job_id, name);

    res.json({ ok: true });
});

// 5. Admin: switch active puzzle (or create + activate a new one).
// When the range changes for an existing named puzzle, a new puzzle row is inserted
// so that old chunk history stays attached to the old puzzle_id and /stats remains correct.
app.post('/api/v1/admin/set-puzzle', (req, res) => {
    const { name, start_hex, end_hex } = req.body;
    if (!name || !start_hex || !end_hex) {
        return res.status(400).json({ error: "Missing name, start_hex, or end_hex" });
    }
    if (!isValidHex(start_hex) || !isValidHex(end_hex)) {
        return res.status(400).json({ error: "start_hex and end_hex must be valid hex strings" });
    }

    const startNorm = start_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();
    const endNorm   = end_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();

    if (BigInt('0x' + endNorm) <= BigInt('0x' + startNorm)) {
        return res.status(400).json({ error: "end_hex must be greater than start_hex" });
    }

    db.transaction(() => {
        db.prepare("UPDATE puzzles SET active = 0").run();
        const existing = db.prepare("SELECT * FROM puzzles WHERE name = ?").get(name);

        let puzzleId;
        if (existing && existing.start_hex === startNorm && existing.end_hex === endNorm) {
            // Same range — reactivate; seed sectors only if missing (idempotent on restart)
            db.prepare("UPDATE puzzles SET active = 1 WHERE id = ?").run(existing.id);
            puzzleId = existing.id;
            const count = db.prepare("SELECT COUNT(*) as c FROM sectors WHERE puzzle_id = ?").get(puzzleId).c;
            if (count === 0) seedSectors(db, puzzleId, startNorm, endNorm);
        } else {
            // New puzzle or changed range — create a new row so old chunk history is preserved
            const info = db.prepare(
                "INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 1)"
            ).run(name, startNorm, endNorm);
            puzzleId = info.lastInsertRowid;
            seedSectors(db, puzzleId, startNorm, endNorm);
        }
    })();

    const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
    console.log(`[Admin] Active puzzle set to: ${puzzle.name} [${puzzle.start_hex} .. ${puzzle.end_hex}]`);
    res.json({ ok: true, puzzle });
});

// 5b. Admin: set (or clear) a test chunk on the active puzzle
// POST /api/v1/admin/set-test-chunk
// Body: { start_hex, end_hex }  — set a test chunk
//       { start_hex: null }     — clear the test chunk
app.post('/api/v1/admin/set-test-chunk', (req, res) => {
    const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
    if (!puzzle) return res.status(503).json({ error: "No active puzzle" });

    const { start_hex, end_hex } = req.body;

    if (!start_hex) {
        db.prepare("UPDATE puzzles SET test_start_hex = NULL, test_end_hex = NULL WHERE id = ?")
            .run(puzzle.id);
        console.log(`[Admin] Test chunk cleared for puzzle ${puzzle.name}`);
        return res.json({ ok: true, test_chunk: null });
    }

    if (!end_hex) return res.status(400).json({ error: "Missing end_hex" });
    if (!isValidHex(start_hex) || !isValidHex(end_hex)) {
        return res.status(400).json({ error: "start_hex and end_hex must be valid hex strings" });
    }

    const startNorm = start_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();
    const endNorm   = end_hex.replace(/^0x/i, '').padStart(64, '0').toLowerCase();

    const ts = BigInt('0x' + startNorm);
    const te = BigInt('0x' + endNorm);
    const ps = BigInt('0x' + puzzle.start_hex);
    const pe = BigInt('0x' + puzzle.end_hex);

    if (te <= ts) {
        return res.status(400).json({ error: "end_hex must be greater than start_hex" });
    }
    // Test chunks must lie OUTSIDE the active puzzle range so the sector allocator
    // can never hand out the same keys as a fresh production chunk.
    const overlaps = ts < pe && ps < te;
    if (overlaps) {
        return res.status(400).json({ error: "Test chunk must not overlap the active puzzle range" });
    }

    db.prepare("UPDATE puzzles SET test_start_hex = ?, test_end_hex = ? WHERE id = ?")
        .run(startNorm, endNorm, puzzle.id);

    console.log(`[Admin] Test chunk set: ${startNorm} .. ${endNorm}`);
    res.json({ ok: true, test_chunk: { start_hex: startNorm, end_hex: endNorm } });
});

// 6. Admin: switch active puzzle
    app.post('/api/v1/admin/activate-puzzle', (req, res) => {
        const { id } = req.body;
        if (!id) return res.status(400).json({ error: 'Missing id' });

        const target = db.prepare("SELECT * FROM puzzles WHERE id = ?").get(id);
        if (!target) return res.status(404).json({ error: 'Puzzle not found' });

        db.transaction(() => {
            db.prepare("UPDATE puzzles SET active = 0").run();
            db.prepare("UPDATE puzzles SET active = 1 WHERE id = ?").run(id);
        })();

        // Seed sectors lazily if this puzzle has none (e.g. imported rows, pre-migration data)
        const sectorCount = db.prepare("SELECT COUNT(*) as c FROM sectors WHERE puzzle_id = ?").get(id).c;
        if (sectorCount === 0) seedSectors(db, target.id, target.start_hex, target.end_hex);

        const puzzle = db.prepare("SELECT * FROM puzzles WHERE id = ?").get(id);
        console.log(`[Admin] Active puzzle switched to: ${puzzle.name}`);
        res.json({ ok: true, puzzle });
    });

// 7. Admin: list all puzzles
    app.get('/api/v1/admin/puzzles', (req, res) => {
        const puzzles = db.prepare("SELECT * FROM puzzles ORDER BY id ASC").all();
        res.json({ puzzles });
    });

    return app;
} // end createApp()

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
        assigned_at DATETIME,
        found_key TEXT,
        found_address TEXT,
        is_test INTEGER NOT NULL DEFAULT 0
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
    `);

    try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_start_hex TEXT").run(); } catch (_) {}
    try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_end_hex   TEXT").run(); } catch (_) {}
    // is_test defaults to 0 for all pre-existing rows. Best-effort backfill: mark any chunk
    // whose range exactly matches the puzzle's current test_start_hex/test_end_hex as is_test=1.
    // Chunks from previously-used test ranges that no longer match cannot be recovered
    // automatically — manual cleanup is required for those rows if they cause stats noise.
    try {
        db.prepare("ALTER TABLE chunks ADD COLUMN is_test INTEGER NOT NULL DEFAULT 0").run();
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

    // Seed puzzles from KEYSPACE_<NAME>=<start_hex>:<end_hex> env vars
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
        const existing  = db.prepare("SELECT id FROM puzzles WHERE name = ?").get(name);
        if (!existing) {
            const info = db.prepare("INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 0)")
                .run(name, startNorm, endNorm);
            console.log(`[Config] Seeded keyspace: ${name}`);
            seedSectors(db, info.lastInsertRowid, startNorm, endNorm);
        }
    }

    // Fall back to built-in Puzzle #71 if DB is still empty
    const puzzleCount = db.prepare("SELECT COUNT(*) as count FROM puzzles").get().count;
    if (puzzleCount === 0) {
        const info = db.prepare("INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 1)")
            .run('Puzzle #71',
                '0400000000000000000'.padStart(64, '0'),
                '07fffffffffffffffff'.padStart(64, '0'));
        console.log('[Init] Seeded Puzzle #71 as active puzzle.');
        seedSectors(db, info.lastInsertRowid,
            '0400000000000000000'.padStart(64, '0'),
            '07fffffffffffffffff'.padStart(64, '0'));
    }

    // Ensure exactly one puzzle is active (normalize both zero and multiple active)
    const activeCount = db.prepare("SELECT COUNT(*) as count FROM puzzles WHERE active = 1").get().count;
    if (activeCount === 0) {
        db.prepare("UPDATE puzzles SET active = 1 WHERE id = (SELECT MIN(id) FROM puzzles)").run();
        console.log('[Init] No active puzzle found — activated the first one.');
    } else if (activeCount > 1) {
        db.prepare("UPDATE puzzles SET active = 0 WHERE id != (SELECT MAX(id) FROM puzzles WHERE active = 1)").run();
        console.log('[Init] Multiple active puzzles found — deactivated all but the latest.');
    }

    // Seed sectors for any existing puzzle that doesn't have them yet (migration path)
    const unsectored = db.prepare(`
        SELECT p.id, p.start_hex, p.end_hex FROM puzzles p
        WHERE NOT EXISTS (SELECT 1 FROM sectors s WHERE s.puzzle_id = p.id)
    `).all();
    for (const p of unsectored) {
        seedSectors(db, p.id, p.start_hex, p.end_hex);
    }

    const app = createApp(db);

    // Background reclaim task
    setInterval(() => {
        const info = db.prepare(`
            UPDATE chunks
            SET status = 'reclaimed', worker_name = NULL
            WHERE status = 'assigned'
            AND assigned_at < datetime('now', '-${TIMEOUT_MINUTES} minutes')
        `).run();
        if (info.changes > 0)
            console.log(`[System] Reclaimed ${info.changes} abandoned chunks back to the pool.`);
    }, 60000);

    app.listen(PORT, '127.0.0.1', () => {
        console.log(`[puzzpool] server running on http://127.0.0.1:${PORT}`);
        console.log(`[puzzpool] database: ${DB_PATH}`);
        if (process.env.ADMIN_TOKEN) console.log('[puzzpool] admin token auth: enabled');
    });
}

module.exports = { createApp, isValidHex, randomBigIntInRange, normalizeHashrate, seedSectors };
