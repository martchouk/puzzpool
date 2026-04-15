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

// Validate that a string is a non-empty hex string (0x prefix optional)
function isValidHex(s) {
    if (typeof s !== 'string' || s.length === 0) return false;
    return /^(0x)?[0-9a-fA-F]+$/.test(s);
}

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

// --- App factory (accepts db for testability) ---

function createApp(db) {
    // Assign a chunk at a random position within the active puzzle range.
    const assignRandomChunk = db.transaction((name, hashrate, puzzle) => {
        const stored = db.prepare("SELECT hashrate FROM workers WHERE name = ?").get(name);
        const hashrateBig = BigInt(Math.floor(hashrate || stored?.hashrate || 1000000));
        const chunkSize = hashrateBig * BigInt(TARGET_MINUTES * 60);

        const puzzleStart = BigInt('0x' + puzzle.start_hex);
        const puzzleEnd   = BigInt('0x' + puzzle.end_hex);

        const effectiveChunk = chunkSize < (puzzleEnd - puzzleStart) ? chunkSize : (puzzleEnd - puzzleStart);
        const start = randomBigIntInRange(puzzleStart, puzzleEnd - effectiveChunk + 1n);
        const end   = start + effectiveChunk;

        const startHex = start.toString(16).padStart(64, '0');
        const endHex   = end.toString(16).padStart(64, '0');

        const info = db.prepare(`
            INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at)
            VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP)
        `).run(puzzle.id, startHex, endHex, name);

        return { chunkId: info.lastInsertRowid, startHex, endHex };
    });

    const app = express();
    app.use(express.json());
    app.use(express.static('public'));

    // Optional admin token guard — enabled only when ADMIN_TOKEN env var is set
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

    // Upsert worker
    db.prepare(`
        INSERT INTO workers (name, hashrate, last_seen) VALUES (?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(name) DO UPDATE SET hashrate = excluded.hashrate, last_seen = CURRENT_TIMESTAMP
    `).run(name, hashrate || 0);

    // Fetch active puzzle
    const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
    if (!puzzle) return res.status(503).json({ error: "No active puzzle configured" });

    let chunkId, startHex, endHex;

    // PRIORITY 1: Test chunk — assign it to the very first requester if not yet taken.
    // "Not yet taken" means no chunk row with this exact start_hex exists in
    // assigned/completed/FOUND state. Reclaimed test chunks are reissued normally below.
    if (puzzle.test_start_hex && puzzle.test_end_hex) {
        const testTaken = db.prepare(`
            SELECT id FROM chunks
            WHERE puzzle_id = ? AND start_hex = ?
              AND status IN ('assigned', 'completed', 'FOUND')
            LIMIT 1
        `).get(puzzle.id, puzzle.test_start_hex);

        if (!testTaken) {
            // Check if it was previously reclaimed — reuse that row rather than inserting a new one
            const testReclaimed = db.prepare(`
                SELECT * FROM chunks
                WHERE puzzle_id = ? AND start_hex = ? AND status = 'reclaimed'
                LIMIT 1
            `).get(puzzle.id, puzzle.test_start_hex);

            if (testReclaimed) {
                chunkId  = testReclaimed.id;
                startHex = testReclaimed.start_hex;
                endHex   = testReclaimed.end_hex;
                db.prepare("UPDATE chunks SET status = 'assigned', worker_name = ?, assigned_at = CURRENT_TIMESTAMP WHERE id = ?")
                    .run(name, chunkId);
            } else {
                startHex = puzzle.test_start_hex;
                endHex   = puzzle.test_end_hex;
                const info = db.prepare(`
                    INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at)
                    VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP)
                `).run(puzzle.id, startHex, endHex, name);
                chunkId = info.lastInsertRowid;
            }

            console.log(`[Test] Assigned test chunk #${chunkId} to ${name}`);
            return res.json({ job_id: chunkId, start_key: startHex, end_key: endHex });
        }
    }

    // PRIORITY 2: Reissue reclaimed (timed-out) chunks
    const reclaimed = db.prepare(
        "SELECT * FROM chunks WHERE status = 'reclaimed' AND puzzle_id = ? LIMIT 1"
    ).get(puzzle.id);

    if (reclaimed) {
        chunkId  = reclaimed.id;
        startHex = reclaimed.start_hex;
        endHex   = reclaimed.end_hex;
        db.prepare("UPDATE chunks SET status = 'assigned', worker_name = ?, assigned_at = CURRENT_TIMESTAMP WHERE id = ?")
            .run(name, chunkId);
    } else {
        // PRIORITY 3: New random chunk
        ({ chunkId, startHex, endHex } = assignRandomChunk(name, hashrate, puzzle));
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
    const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get() || null;

    const activeWorkers = db.prepare(`
        SELECT name, hashrate, last_seen FROM workers
        WHERE last_seen >= datetime('now', '-10 minutes')
        ORDER BY hashrate DESC
    `).all();
    const totalHashrate = activeWorkers.reduce((sum, w) => sum + w.hashrate, 0);
    const completedChunks = db.prepare(
        "SELECT COUNT(*) as count FROM chunks WHERE status = 'completed' OR status = 'FOUND'"
    ).get().count;

    const doneChunks = db.prepare(`
        SELECT worker_name, start_hex, end_hex
        FROM chunks
        WHERE (status = 'completed' OR status = 'FOUND') AND worker_name IS NOT NULL
    `).all();

    let totalKeysCompleted = 0n;
    for (const c of doneChunks) {
        totalKeysCompleted += BigInt('0x' + c.end_hex) - BigInt('0x' + c.start_hex);
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

    const finders = db.prepare(`
        SELECT worker_name, found_key, found_address, created_at
        FROM findings ORDER BY id ASC
    `).all();

    const assignedNow = puzzle
        ? db.prepare("SELECT id, worker_name FROM chunks WHERE status = 'assigned' AND puzzle_id = ?").all(puzzle.id)
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
            FROM chunks WHERE puzzle_id = ?
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

    res.json({
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

// 5. Admin: switch active puzzle (or create + activate a new one)
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

    db.transaction(() => {
        db.prepare("UPDATE puzzles SET active = 0").run();
        const existing = db.prepare("SELECT id FROM puzzles WHERE name = ?").get(name);
        if (existing) {
            db.prepare("UPDATE puzzles SET start_hex = ?, end_hex = ?, active = 1 WHERE id = ?")
                .run(startNorm, endNorm, existing.id);
        } else {
            db.prepare("INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 1)")
                .run(name, startNorm, endNorm);
        }
    })();

    const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
    console.log(`[Admin] Active puzzle set to: ${puzzle.name} [${puzzle.start_hex} .. ${puzzle.end_hex}]`);
    res.json({ ok: true, puzzle });
});

// 5. Admin: set (or clear) a test chunk on the active puzzle
// POST /api/v1/admin/set-test-chunk
// Body: { start_hex, end_hex }  — set a test chunk
//       { start_hex: null }     — clear the test chunk
app.post('/api/v1/admin/set-test-chunk', (req, res) => {
    const puzzle = db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
    if (!puzzle) return res.status(503).json({ error: "No active puzzle" });

    const { start_hex, end_hex } = req.body;

    if (!start_hex) {
        // Clear
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

    db.prepare("UPDATE puzzles SET test_start_hex = ?, test_end_hex = ? WHERE id = ?")
        .run(startNorm, endNorm, puzzle.id);

    console.log(`[Admin] Test chunk set: ${startNorm} .. ${endNorm}`);
    res.json({ ok: true, test_chunk: { start_hex: startNorm, end_hex: endNorm } });
});

// 6. Admin: list all puzzles
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
        found_address TEXT
      );
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

    const puzzleCount = db.prepare("SELECT COUNT(*) as count FROM puzzles").get().count;
    if (puzzleCount === 0) {
        db.prepare("INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 1)")
            .run('Puzzle #71',
                '0400000000000000000'.padStart(64, '0'),
                '07fffffffffffffffff'.padStart(64, '0'));
        console.log('[Init] Seeded Puzzle #71 as active puzzle.');
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

module.exports = { createApp, isValidHex, randomBigIntInRange };
