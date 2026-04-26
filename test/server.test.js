'use strict';

const request = require('supertest');
const {
    createApp,
    ACTIVE_MINUTES,
    REACTIVATE_MINUTES,
    buildDeterministicPermutation,
    seedGlobalBlocks,
    defaultAllocSeedForPuzzle,
    ALLOC_STRATEGY_LEGACY,
    ALLOC_STRATEGY_GLOBAL,
} = require('../server');
const { createTestDb, seedPuzzle } = require('./helpers');

const STALE_MINUTES          = (ACTIVE_MINUTES    || 1)  + 1; // stale for dashboard color
const REACTIVATE_STALE_MINUTES = (REACTIVATE_MINUTES || 15) + 1; // stale for chunk reclaim

let db, app;

beforeEach(() => {
    db  = createTestDb();
    app = createApp(db);
});

afterEach(() => {
    db.close();
});

function chunkSize(db, job_id) {
    const c = db.prepare("SELECT start_hex, end_hex FROM chunks WHERE id=?").get(job_id);
    return Number(BigInt('0x' + c.end_hex) - BigInt('0x' + c.start_hex));
}

// ─── /api/v1/work ────────────────────────────────────────────────────────────

describe('POST /api/v1/work', () => {
    test('returns 400 when name is missing', async () => {
        await request(app).post('/api/v1/work').send({}).expect(400);
    });

    test('returns 503 when no active puzzle', async () => {
        const res = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 })
            .expect(503);
        expect(res.body.error).toMatch(/no active puzzle/i);
    });

    test('returns job_id and hex range for a new worker', async () => {
        seedPuzzle(db);
        const res = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 })
            .expect(200);
        expect(res.body.job_id).toBeGreaterThan(0);
        expect(res.body.start_key).toHaveLength(64);
        expect(res.body.end_key).toHaveLength(64);
    });

    test('inserts a chunk row with status=assigned', async () => {
        seedPuzzle(db);
        const res = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 });
        const chunk = db.prepare("SELECT * FROM chunks WHERE id = ?").get(res.body.job_id);
        expect(chunk.status).toBe('assigned');
        expect(chunk.worker_name).toBe('w1');
    });

    test('reclaimed chunk is offered before a new one', async () => {
        seedPuzzle(db);
        // Insert a reclaimed chunk
        const puzzle = db.prepare("SELECT * FROM puzzles WHERE active=1").get();
        db.prepare(`INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at)
                    VALUES (?, ?, ?, 'reclaimed', NULL, CURRENT_TIMESTAMP)`)
            .run(puzzle.id,
                '0000000000000000000000000000000000000000000000000400000000000001',
                '0000000000000000000000000000000000000000000000000400000000100001');
        const reclaimed = db.prepare("SELECT id FROM chunks WHERE status='reclaimed'").get();

        const res = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 });
        expect(res.body.job_id).toBe(reclaimed.id);
    });

    test('test chunk is offered first and not offered twice', async () => {
        const puzzle = seedPuzzle(db);
        const tStart = '0000000000000000000000000000000000000000000000000400000000000001';
        const tEnd   = '0000000000000000000000000000000000000000000000000400000000001001';
        db.prepare("UPDATE puzzles SET test_start_hex=?, test_end_hex=? WHERE id=?")
            .run(tStart, tEnd, puzzle.id);

        // First worker gets the test chunk
        const r1 = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 });
        expect(r1.body.start_key).toBe(tStart);

        // Second worker gets a different chunk
        const r2 = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w2', hashrate: 1000000 });
        expect(r2.body.start_key).not.toBe(tStart);
    });
});

// ─── /api/v1/submit ──────────────────────────────────────────────────────────

describe('POST /api/v1/submit', () => {
    let jobId;

    beforeEach(async () => {
        seedPuzzle(db);
        const res = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 });
        jobId = res.body.job_id;
    });

    test('marks chunk completed on status=done', async () => {
        const chunk = db.prepare("SELECT start_hex, end_hex FROM chunks WHERE id=?").get(jobId);
        const chunkSize = Number(BigInt('0x' + chunk.end_hex) - BigInt('0x' + chunk.start_hex));
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id: jobId, status: 'done', keys_scanned: chunkSize })
            .expect(200, { accepted: true });
        expect(db.prepare("SELECT status FROM chunks WHERE id=?").get(jobId).status).toBe('completed');
    });

    test('marks chunk FOUND and inserts findings row', async () => {
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id: jobId, status: 'FOUND',
                findings: [{ found_key: '0'.repeat(64), found_address: '1TestAddress' }] })
            .expect(200);
        const chunk = db.prepare("SELECT status FROM chunks WHERE id=?").get(jobId);
        expect(chunk.status).toBe('FOUND');
        const finding = db.prepare("SELECT * FROM findings WHERE chunk_id=?").get(jobId);
        expect(finding).toBeTruthy();
        expect(finding.worker_name).toBe('w1');
    });

    test('wrong worker cannot complete another worker\'s chunk', async () => {
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w_other', job_id: jobId, status: 'done', keys_scanned: chunkSize(db, jobId) });
        const chunk = db.prepare("SELECT status FROM chunks WHERE id=?").get(jobId);
        expect(chunk.status).toBe('assigned'); // unchanged
    });

    test('returns 400 on invalid keys_scanned', async () => {
        for (const bad of ['abc', -1, 1.5, null, []]) {
            const r = await request(app).post('/api/v1/submit')
                .send({ name: 'w1', job_id: jobId, status: 'done', keys_scanned: bad });
            expect(r.status).toBe(400);
            expect(r.body.accepted).toBe(false);
            expect(r.body.error).toMatch(/keys_scanned/);
        }
    });

    test('rejects done without keys_scanned', async () => {
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id: jobId, status: 'done' })
            .expect(400, { accepted: false, error: 'keys_scanned is required for status: done' });
    });

    test('accepts done when keys_scanned == chunk size', async () => {
        const chunk = db.prepare("SELECT start_hex, end_hex FROM chunks WHERE id=?").get(jobId);
        const chunkSize = BigInt('0x' + chunk.end_hex) - BigInt('0x' + chunk.start_hex);
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id: jobId, status: 'done', keys_scanned: Number(chunkSize) })
            .expect(200, { accepted: true });
        expect(db.prepare("SELECT status FROM chunks WHERE id=?").get(jobId).status).toBe('completed');
    });

    test('accepts done when keys_scanned > chunk size (batch overshoot)', async () => {
        const chunk = db.prepare("SELECT start_hex, end_hex FROM chunks WHERE id=?").get(jobId);
        const chunkSize = BigInt('0x' + chunk.end_hex) - BigInt('0x' + chunk.start_hex);
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id: jobId, status: 'done', keys_scanned: Number(chunkSize) + 4096 })
            .expect(200, { accepted: true });
        expect(db.prepare("SELECT status FROM chunks WHERE id=?").get(jobId).status).toBe('completed');
    });

    test('reclaims chunk when keys_scanned < chunk size', async () => {
        const res = await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id: jobId, status: 'done', keys_scanned: 0 })
            .expect(400);
        expect(res.body.accepted).toBe(false);
        expect(res.body.error).toMatch(/not accepted/);
        expect(res.body.error).toMatch(/Chunk reclaimed/);
        const chunk = db.prepare("SELECT status, prev_worker_name, worker_name FROM chunks WHERE id=?").get(jobId);
        expect(chunk.status).toBe('reclaimed');
        expect(chunk.prev_worker_name).toBe('w1');
        expect(chunk.worker_name).toBeNull();
    });
});

// ─── /api/v1/heartbeat ───────────────────────────────────────────────────────

describe('POST /api/v1/heartbeat', () => {
    test('returns 400 when name or job_id missing', async () => {
        await request(app).post('/api/v1/heartbeat').send({ name: 'w1' }).expect(400);
        await request(app).post('/api/v1/heartbeat').send({ job_id: 1 }).expect(400);
    });

    test('updates assigned_at on a valid job', async () => {
        seedPuzzle(db);
        const r = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 });
        const jobId = r.body.job_id;

        const before = db.prepare("SELECT assigned_at FROM chunks WHERE id=?").get(jobId).assigned_at;
        // Small delay so timestamps differ
        await new Promise(res => setTimeout(res, 10));

        await request(app)
            .post('/api/v1/heartbeat')
            .send({ name: 'w1', job_id: jobId })
            .expect(200, { ok: true });

        const after = db.prepare("SELECT assigned_at FROM chunks WHERE id=?").get(jobId).assigned_at;
        expect(after >= before).toBe(true);
    });
});

// ─── GET /api/v1/stats ───────────────────────────────────────────────────────

describe('GET /api/v1/stats', () => {
    test('returns puzzle=null when no active puzzle', async () => {
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.puzzle).toBeNull();
    });

    test('returns expected shape with active puzzle', async () => {
        seedPuzzle(db);
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.puzzle.name).toBe('Test Puzzle');
        expect(res.body.workers).toBeInstanceOf(Array);
        expect(res.body.scores).toBeInstanceOf(Array);
        expect(res.body.finders).toBeInstanceOf(Array);
        expect(res.body.chunks_vis).toBeInstanceOf(Array);
        expect(typeof res.body.total_keys_completed).toBe('string');
    });

    test('worker active=true when holding assigned chunk in puzzle', async () => {
        seedPuzzle(db);
        await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.workers).toHaveLength(1);
        expect(res.body.workers[0].active).toBe(true);
        expect(res.body.active_workers_count).toBe(1);
    });

    test('worker active=false when chunk reclaimed (fresh heartbeat but no assigned chunk)', async () => {
        seedPuzzle(db);
        const r = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        db.prepare("UPDATE chunks SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL WHERE id = ?").run(r.body.job_id);
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.workers).toHaveLength(1);
        expect(res.body.workers[0].fresh).toBe(true);
        expect(res.body.workers[0].active).toBe(false);
        expect(res.body.active_workers_count).toBe(0);
        expect(res.body.total_hashrate).toBe(0);
    });

    test('worker active=false when assigned but heartbeat stale', async () => {
        seedPuzzle(db);
        await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        db.prepare(`UPDATE workers SET last_seen = datetime('now', '-${STALE_MINUTES} minutes') WHERE name = 'w1'`).run();
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.workers).toHaveLength(1);
        expect(res.body.workers[0].active).toBe(false);
        expect(res.body.active_workers_count).toBe(0);
    });

    test('inactive_workers_count: one active and one inactive worker', async () => {
        seedPuzzle(db);
        await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        await request(app).post('/api/v1/work').send({ name: 'w2', hashrate: 1000000 });
        // Age w2 past the active threshold but keep it within TIMEOUT_MINUTES
        db.prepare(`UPDATE workers SET last_seen = datetime('now', '-${STALE_MINUTES} minutes') WHERE name = 'w2'`).run();
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.active_workers_count).toBe(1);
        expect(res.body.inactive_workers_count).toBe(1);
    });

    test('worker removed from table after TIMEOUT_MINUTES', async () => {
        seedPuzzle(db);
        await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        db.prepare(`UPDATE workers SET last_seen = datetime('now', '-${parseInt(process.env.TIMEOUT_MINUTES || '15') + 1} minutes') WHERE name = 'w1'`).run();
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.workers).toHaveLength(0);
    });

    test('workers scoped to requested puzzle — worker from other puzzle not shown', async () => {
        const p1 = seedPuzzle(db, { name: 'P1', start_hex: '0'.repeat(64), end_hex: '000000000000000000000000000000000000000000000000000000003b9aca00' });
        await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });

        // Switch to a new puzzle
        await request(app).post('/api/v1/admin/set-puzzle').send({ name: 'P2', start_hex: '0x400', end_hex: '0x800' });
        await request(app).post('/api/v1/work').send({ name: 'w2', hashrate: 1000000 });

        // Stats for p1 should only show w1
        const res = await request(app).get(`/api/v1/stats?puzzle_id=${p1.id}`).expect(200);
        const names = res.body.workers.map(w => w.name);
        expect(names).toContain('w1');
        expect(names).not.toContain('w2');
    });

    test('re-activating worker gets fresh chunk, old chunk reclaimed', async () => {
        seedPuzzle(db);
        const r1 = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        const oldJobId = r1.body.job_id;

        // Age worker past the ownership reactivation threshold
        db.prepare(`UPDATE workers SET last_seen = datetime('now', '-${REACTIVATE_STALE_MINUTES} minutes') WHERE name = 'w1'`).run();

        // Worker re-activates
        const r2 = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        expect(r2.body.job_id).not.toBe(oldJobId);

        // Old chunk must be reclaimed
        const oldChunk = db.prepare("SELECT status FROM chunks WHERE id = ?").get(oldJobId);
        expect(oldChunk.status).toBe('reclaimed');
    });

    test('current_chunk_in_shard is 0 for first chunk and 1 for second chunk in same shard', async () => {
        seedPuzzle(db);
        await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        await request(app).post('/api/v1/work').send({ name: 'w2', hashrate: 1000000 });

        const res = await request(app).get('/api/v1/stats').expect(200);
        const w1 = res.body.workers.find(w => w.name === 'w1');
        const w2 = res.body.workers.find(w => w.name === 'w2');

        expect(w1.current_shard).toBe(0);
        expect(w1.current_chunk_in_shard).toBe(0);
        expect(w2.current_shard).toBe(0);
        expect(w2.current_chunk_in_shard).toBe(1);
    });

    test('finders entry includes shard, chunk, and chunk_global', async () => {
        seedPuzzle(db);
        const r = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        const { job_id } = r.body;
        await request(app).post('/api/v1/submit')
            .send({ name: 'w1', job_id, status: 'FOUND', findings: [{ found_key: '0'.repeat(64), found_address: '1Test' }] });
        const stats = await request(app).get('/api/v1/stats');
        const finder = stats.body.finders[0];
        expect(typeof finder.chunk_global).toBe('number');
        expect(typeof finder.chunk).toBe('number');
        expect(typeof finder.shard).toBe('number');
        expect(finder.chunk_global).toBe(job_id);
        expect(finder.chunk).toBe(0);   // first chunk in its sector
        expect(finder.shard).toBe(0);   // first sector
    });

    test('total_keys_completed reflects submitted chunks', async () => {
        seedPuzzle(db);
        const r = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 });
        const { job_id, start_key, end_key } = r.body;
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id, status: 'done', keys_scanned: chunkSize(db, job_id) });

        const stats = await request(app).get('/api/v1/stats');
        const expected = (BigInt('0x' + end_key) - BigInt('0x' + start_key)).toString();
        expect(stats.body.total_keys_completed).toBe(expected);
    });
});

// ─── Admin: /api/v1/admin/set-puzzle ─────────────────────────────────────────

describe('POST /api/v1/admin/set-puzzle', () => {
    test('creates a new puzzle and sets it active', async () => {
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x100', end_hex: '0x200' })
            .expect(200);
        const p = db.prepare("SELECT * FROM puzzles WHERE active=1").get();
        expect(p.name).toBe('P1');
    });

    test('returns 400 for missing fields', async () => {
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1' })
            .expect(400);
    });

    test('returns 400 for invalid hex', async () => {
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: 'nothex', end_hex: '0x200' })
            .expect(400);
    });
});

// ─── Admin: /api/v1/admin/activate-puzzle ────────────────────────────────────

describe('POST /api/v1/admin/activate-puzzle', () => {
    test('switches active puzzle', async () => {
        // Create two puzzles, first active
        db.prepare("INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 1)")
            .run('P1', '0'.repeat(63) + '1', '0'.repeat(63) + '2');
        const p2 = db.prepare("INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 0)")
            .run('P2', '0'.repeat(63) + '3', '0'.repeat(63) + '4');

        await request(app)
            .post('/api/v1/admin/activate-puzzle')
            .send({ id: p2.lastInsertRowid })
            .expect(200);

        const active = db.prepare("SELECT name FROM puzzles WHERE active = 1").get();
        expect(active.name).toBe('P2');
    });

    test('returns 400 when id is missing', async () => {
        await request(app)
            .post('/api/v1/admin/activate-puzzle')
            .send({})
            .expect(400);
    });

    test('returns 404 for unknown id', async () => {
        await request(app)
            .post('/api/v1/admin/activate-puzzle')
            .send({ id: 9999 })
            .expect(404);
    });
});

// ─── GET /api/v1/stats includes puzzles array ─────────────────────────────────

describe('GET /api/v1/stats puzzles field', () => {
    test('returns puzzles array with active flag', async () => {
        seedPuzzle(db);
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.puzzles).toBeInstanceOf(Array);
        expect(res.body.puzzles.length).toBeGreaterThan(0);
        expect(res.body.puzzles[0]).toHaveProperty('id');
        expect(res.body.puzzles[0]).toHaveProperty('name');
        expect(res.body.puzzles[0]).toHaveProperty('active');
    });
});

// ─── Sharded Frontier Allocator ──────────────────────────────────────────────

describe('Sharded Frontier Allocator', () => {
    test('0. first work request without test chunk targets sector #32768', async () => {
        // Puzzle with exactly 65536 sectors (range = 65536 × MIN_SECTOR_SIZE = 65536 × 1B)
        const end = (65536n * 1_000_000_000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { name: 'Large', end_hex: end, strategy: ALLOC_STRATEGY_LEGACY });

        const res = await request(app).post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1e6 }).expect(200);

        // Sector #32768 start = 32768 × 1,000,000,000
        const expected = (32768n * 1_000_000_000n).toString(16).padStart(64, '0');
        expect(res.body.start_key).toBe(expected);
    }, 15000);

    test('1. no-overlap: fresh assignments do not share any key', async () => {
        seedPuzzle(db);
        const chunks = [];
        for (let i = 0; i < 3; i++) {
            const r = await request(app).post('/api/v1/work').send({ name: `w${i}`, hashrate: 1000000 });
            if (r.status === 200) chunks.push(r.body);
        }
        expect(chunks.length).toBe(3);
        for (let i = 0; i < chunks.length; i++) {
            for (let j = i + 1; j < chunks.length; j++) {
                const aStart = BigInt('0x' + chunks[i].start_key);
                const aEnd   = BigInt('0x' + chunks[i].end_key);
                const bStart = BigInt('0x' + chunks[j].start_key);
                const bEnd   = BigInt('0x' + chunks[j].end_key);
                expect(aStart < bEnd && bStart < aEnd).toBe(false);
            }
        }
    });

    test('2. sector boundary: chunk capped at sector end, next starts exactly there', async () => {
        // hashrate=1 → chunk = 1 × 5 × 60 = 300 keys; puzzle range = 1000 → 2 chunks fit
        const end = (1000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end });

        const r1 = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1 });
        const r2 = await request(app).post('/api/v1/work').send({ name: 'w2', hashrate: 1 });
        expect(r1.status).toBe(200);
        expect(r2.status).toBe(200);
        expect(r1.body.start_key).toBe('0'.repeat(64));
        expect(r2.body.start_key).toBe(r1.body.end_key);
    });

    test('3. exhaustion: 503 when all sectors are done', async () => {
        // range=100 < 300 (chunk at hashrate=1) → first request covers entire sector
        const end = (100n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end });

        await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1 }).expect(200);
        const r = await request(app).post('/api/v1/work').send({ name: 'w2', hashrate: 1 });
        expect(r.status).toBe(503);
        expect(r.body.error).toMatch(/all keyspace/i);
    });

    test('4. reclaim priority: reclaimed chunk offered before fresh sector allocation', async () => {
        seedPuzzle(db);
        const puzzle = db.prepare("SELECT * FROM puzzles WHERE active=1").get();
        const reclaimedInfo = db.prepare(`
            INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at)
            VALUES (?, ?, ?, 'reclaimed', NULL, CURRENT_TIMESTAMP)
        `).run(puzzle.id, '0'.repeat(64), (1n).toString(16).padStart(64, '0'));

        const r = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        expect(r.body.job_id).toBe(reclaimedInfo.lastInsertRowid);

        // Block frontier must not have moved — reclaim took priority over fresh allocation
        const block = db.prepare("SELECT * FROM alloc_blocks WHERE puzzle_id = ? ORDER BY block_index ASC LIMIT 1").get(puzzle.id);
        expect(block.current_hex).toBe(block.start_hex);
    });

    test('5. progress accounting: overlapping done chunks not double-counted', async () => {
        seedPuzzle(db);
        const puzzle = db.prepare("SELECT * FROM puzzles WHERE active=1").get();
        // [0, 300) and [200, 500) → merged [0, 500) = 500 keys, not 600
        const s1 = '0'.repeat(64);
        const e1 = (300n).toString(16).padStart(64, '0');
        const s2 = (200n).toString(16).padStart(64, '0');
        const e2 = (500n).toString(16).padStart(64, '0');
        db.prepare("INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name) VALUES (?, ?, ?, 'completed', 'w1')")
            .run(puzzle.id, s1, e1);
        db.prepare("INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name) VALUES (?, ?, ?, 'completed', 'w2')")
            .run(puzzle.id, s2, e2);

        const stats = await request(app).get('/api/v1/stats');
        expect(stats.body.total_keys_completed).toBe('500');
    });

    test('6. puzzle range change: creates new puzzle row, old data stays on old id', async () => {
        await request(app).post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x100', end_hex: '0x200' });
        const p1 = db.prepare("SELECT * FROM puzzles WHERE active=1").get();
        const p1Blocks = db.prepare("SELECT COUNT(*) as c FROM alloc_blocks WHERE puzzle_id=?").get(p1.id).c;
        expect(p1Blocks).toBe(1);

        // Change range → new puzzle row
        await request(app).post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x300', end_hex: '0x400' });
        const p2 = db.prepare("SELECT * FROM puzzles WHERE active=1").get();

        expect(p2.id).not.toBe(p1.id);
        expect(p2.start_hex).toContain('3');   // normalized 0x300
        const p2Blocks = db.prepare("SELECT COUNT(*) as c FROM alloc_blocks WHERE puzzle_id=?").get(p2.id).c;
        expect(p2Blocks).toBe(1);
        // Old row still exists and is inactive
        const p1Row = db.prepare("SELECT * FROM puzzles WHERE id=?").get(p1.id);
        expect(p1Row).toBeTruthy();
        expect(p1Row.active).toBe(0);
    });

    test('7. consecutive allocations return non-overlapping sequential ranges', async () => {
        seedPuzzle(db);
        const r1 = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        const r2 = await request(app).post('/api/v1/work').send({ name: 'w2', hashrate: 1000000 });
        expect(r1.status).toBe(200);
        expect(r2.status).toBe(200);
        expect(r2.body.start_key).toBe(r1.body.end_key);
    });

    test('8. stats reset: new puzzle_id shows no chunks from previous range', async () => {
        await request(app).post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x100', end_hex: '0x200' });
        const r = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        await request(app).post('/api/v1/submit').send({ name: 'w1', job_id: r.body.job_id, status: 'done', keys_scanned: chunkSize(db, r.body.job_id) });

        // Change range → new puzzle row
        await request(app).post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x300', end_hex: '0x400' });

        const stats = await request(app).get('/api/v1/stats');
        expect(stats.body.total_keys_completed).toBe('0');
        expect(stats.body.completed_chunks).toBe(0);
    });

    test('9. invalid range: set-puzzle rejects end_hex <= start_hex', async () => {
        await request(app).post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x200', end_hex: '0x200' })
            .expect(400);
        await request(app).post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x300', end_hex: '0x100' })
            .expect(400);
    });

    test('10. test chunks excluded from stats: completed_chunks, total_keys_completed, scores', async () => {
        // Puzzle range: [0x0, 0x3b9aca00). Test chunk inside the puzzle range (mid-keyspace).
        seedPuzzle(db);

        // Set test chunk inside puzzle range — normal production setup
        const testStart = '000000000000000000000000000000000000000000000000000000001dcde500';
        const testEnd   = '000000000000000000000000000000000000000000000000000000001dcde600';
        await request(app).post('/api/v1/admin/set-test-chunk')
            .send({ start_hex: testStart, end_hex: testEnd })
            .expect(200);

        // Worker receives test chunk first
        const work = await request(app).post('/api/v1/work').send({ name: 'tester', hashrate: 1e9 }).expect(200);
        expect(work.body.start_key).toBe(testStart);

        // Submit it as done
        await request(app).post('/api/v1/submit')
            .send({ name: 'tester', job_id: work.body.job_id, status: 'done', keys_scanned: chunkSize(db, work.body.job_id) })
            .expect(200);

        // Stats must not reflect the test chunk
        const stats = await request(app).get('/api/v1/stats').expect(200);
        expect(stats.body.completed_chunks).toBe(0);
        expect(stats.body.total_keys_completed).toBe('0');
        expect(stats.body.scores).toHaveLength(0);
        expect(stats.body.chunks_vis).toHaveLength(0);
    });

    test('11. late FOUND from previous assignee is accepted after reclaim', async () => {
        seedPuzzle(db);
        // Worker A gets a chunk
        const work = await request(app).post('/api/v1/work').send({ name: 'workerA', hashrate: 1e9 }).expect(200);
        const jobId = work.body.job_id;

        // Simulate reclaim by directly updating the DB (background task not running in tests)
        db.prepare("UPDATE chunks SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL WHERE id = ?")
            .run(jobId);

        // Worker A submits FOUND late (chunk no longer assigned to them)
        const res = await request(app).post('/api/v1/submit')
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', findings: [{ found_key: 'deadbeef'.padStart(64, '0'), found_address: '1Test' }] })
            .expect(200);
        expect(res.body.accepted).toBe(true);

        // Finding should be recorded
        const finding = db.prepare("SELECT * FROM findings WHERE chunk_id = ?").get(jobId);
        expect(finding).toBeTruthy();
        expect(finding.found_key).toBe('deadbeef'.padStart(64, '0'));
    });

    test('11b. late FOUND finalizes chunk even when currently assigned to another worker', async () => {
        seedPuzzle(db);
        const workA = await request(app).post('/api/v1/work').send({ name: 'workerA', hashrate: 1e9 }).expect(200);
        const jobId = workA.body.job_id;
        // Reclaim and reassign to Worker B
        db.prepare("UPDATE chunks SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL WHERE id = ?").run(jobId);
        await request(app).post('/api/v1/work').send({ name: 'workerB', hashrate: 1e9 }).expect(200);

        // Worker A submits late FOUND while B holds it
        const late = await request(app).post('/api/v1/submit')
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', findings: [{ found_key: 'aabb'.padStart(64, '0'), found_address: '1A' }] })
            .expect(200);
        expect(late.body.accepted).toBe(true);

        // Chunk must be finalized — Worker B cannot now submit completed or FOUND
        const chunk = db.prepare("SELECT status FROM chunks WHERE id = ?").get(jobId);
        expect(chunk.status).toBe('FOUND');

        const bSubmit = await request(app).post('/api/v1/submit')
            .send({ name: 'workerB', job_id: jobId, status: 'done', keys_scanned: chunkSize(db, jobId) })
            .expect(200);
        expect(bSubmit.body.accepted).toBe(false);
    });

    test('11c. late FOUND is idempotent — retries do not duplicate findings', async () => {
        seedPuzzle(db);
        const work = await request(app).post('/api/v1/work').send({ name: 'workerA', hashrate: 1e9 }).expect(200);
        const jobId = work.body.job_id;
        db.prepare("UPDATE chunks SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL WHERE id = ?").run(jobId);

        const key = 'cafe'.padStart(64, '0');
        // First late FOUND
        const r1 = await request(app).post('/api/v1/submit')
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', findings: [{ found_key: key, found_address: '1A' }] })
            .expect(200);
        expect(r1.body.accepted).toBe(true);

        // Retry — same request
        const r2 = await request(app).post('/api/v1/submit')
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', findings: [{ found_key: key, found_address: '1A' }] })
            .expect(200);
        expect(r2.body.accepted).toBe(true);

        // Only one findings row
        const findings = db.prepare("SELECT * FROM findings WHERE chunk_id = ?").all(jobId);
        expect(findings).toHaveLength(1);
    });

    test('11d. late FOUND on reclaimed chunk transitions it to FOUND (removes from pool)', async () => {
        seedPuzzle(db);
        const work = await request(app).post('/api/v1/work').send({ name: 'workerA', hashrate: 1e9 }).expect(200);
        const jobId = work.body.job_id;
        db.prepare("UPDATE chunks SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL WHERE id = ?").run(jobId);

        const res = await request(app).post('/api/v1/submit')
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', findings: [{ found_key: 'beef'.padStart(64, '0'), found_address: '1B' }] })
            .expect(200);
        expect(res.body.accepted).toBe(true);

        // Chunk must be FOUND now, not reclaimed
        const chunk = db.prepare("SELECT status FROM chunks WHERE id = ?").get(jobId);
        expect(chunk.status).toBe('FOUND');
    });

    test('12. FOUND rejected from worker with no provenance after reclaim', async () => {
        seedPuzzle(db);
        const work = await request(app).post('/api/v1/work').send({ name: 'workerA', hashrate: 1e9 }).expect(200);
        const jobId = work.body.job_id;

        // Reclaim without any connection to workerB
        db.prepare("UPDATE chunks SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL WHERE id = ?")
            .run(jobId);

        // Unrelated worker tries to submit FOUND
        const res = await request(app).post('/api/v1/submit')
            .send({ name: 'workerB', job_id: jobId, status: 'FOUND', findings: [{ found_key: 'deadbeef'.padStart(64, '0'), found_address: '1Test' }] })
            .expect(200);
        expect(res.body.accepted).toBe(false);

        const finding = db.prepare("SELECT * FROM findings WHERE chunk_id = ?").get(jobId);
        expect(finding).toBeUndefined();
    });
});

// ─── Admin: ADMIN_TOKEN auth ──────────────────────────────────────────────────

describe('ADMIN_TOKEN middleware', () => {
    beforeEach(() => {
        process.env.ADMIN_TOKEN = 'testsecret';
        app = createApp(db); // re-create with token set
    });

    afterEach(() => {
        delete process.env.ADMIN_TOKEN;
    });

    test('returns 401 without token', async () => {
        await request(app).get('/api/v1/admin/puzzles').expect(401);
    });

    test('returns 200 with correct token', async () => {
        await request(app)
            .get('/api/v1/admin/puzzles')
            .set('X-Admin-Token', 'testsecret')
            .expect(200);
    });

    test('returns 401 with wrong token', async () => {
        await request(app)
            .get('/api/v1/admin/puzzles')
            .set('X-Admin-Token', 'wrongtoken')
            .expect(401);
    });
});

// ─── Global Block Allocator ───────────────────────────────────────────────────

describe('buildDeterministicPermutation', () => {
    test('produces a full permutation with no duplicates', () => {
        const n = 500;
        const perm = buildDeterministicPermutation(n, 'test-seed-abc');
        expect(perm).toHaveLength(n);
        const set = new Set(perm);
        expect(set.size).toBe(n);
        for (const v of perm) expect(v >= 0 && v < n).toBe(true);
    });

    test('is deterministic: same seed produces identical output', () => {
        const p1 = buildDeterministicPermutation(200, 'fixed-seed');
        const p2 = buildDeterministicPermutation(200, 'fixed-seed');
        expect(p1).toEqual(p2);
    });

    test('different seeds produce different orderings', () => {
        const p1 = buildDeterministicPermutation(200, 'seed-A');
        const p2 = buildDeterministicPermutation(200, 'seed-B');
        expect(p1).not.toEqual(p2);
    });

    test('handles edge cases: count=0 and count=1', () => {
        expect(buildDeterministicPermutation(0, 'seed')).toEqual([]);
        expect(buildDeterministicPermutation(1, 'seed')).toEqual([0]);
    });
});

describe('Global Block Allocator — seeding', () => {
    test('seedGlobalBlocks creates correct number of contiguous blocks', () => {
        const end = (1000n).toString(16).padStart(64, '0');
        const puzzle = seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        const blocks = db.prepare(
            "SELECT * FROM alloc_blocks WHERE puzzle_id = ? ORDER BY block_index ASC"
        ).all(puzzle.id);

        expect(blocks).toHaveLength(10);
        for (let i = 0; i < 10; i++) {
            const expectedStart = (BigInt(i) * 100n).toString(16).padStart(64, '0');
            const expectedEnd   = (BigInt(i + 1) * 100n).toString(16).padStart(64, '0');
            expect(blocks[i].start_hex).toBe(expectedStart);
            expect(blocks[i].end_hex).toBe(expectedEnd);
            expect(blocks[i].current_hex).toBe(expectedStart);
            expect(blocks[i].status).toBe('open');
        }
    });

    test('alloc_order is a full permutation of all block indices', () => {
        const end = (500n).toString(16).padStart(64, '0');
        const puzzle = seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        const rows = db.prepare(
            "SELECT block_index FROM alloc_order WHERE puzzle_id = ? ORDER BY order_index"
        ).all(puzzle.id);

        expect(rows).toHaveLength(5);
        const sorted = rows.map(r => r.block_index).sort((a, b) => a - b);
        expect(sorted).toEqual([0, 1, 2, 3, 4]);
    });

    test('allocation order is deterministic: same puzzle parameters and seed produce same order', () => {
        const start = '0'.repeat(64);
        const end   = (1000n).toString(16).padStart(64, '0');
        const seed  = 'determinism-test-seed';

        const p1 = seedPuzzle(db, { name: 'P1', start_hex: start, end_hex: end, block_size_keys: 100, seed });
        db.prepare("UPDATE puzzles SET active = 0").run();
        const p2 = seedPuzzle(db, { name: 'P2', start_hex: start, end_hex: end, block_size_keys: 100, seed });

        const order1 = db.prepare(
            "SELECT block_index FROM alloc_order WHERE puzzle_id = ? ORDER BY order_index"
        ).all(p1.id).map(r => r.block_index);

        const order2 = db.prepare(
            "SELECT block_index FROM alloc_order WHERE puzzle_id = ? ORDER BY order_index"
        ).all(p2.id).map(r => r.block_index);

        expect(order1).toEqual(order2);
    });

    test('puzzle range smaller than block size produces exactly one block', () => {
        const end = (500n).toString(16).padStart(64, '0');
        const puzzle = seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end });

        const blocks = db.prepare("SELECT * FROM alloc_blocks WHERE puzzle_id = ?").all(puzzle.id);
        expect(blocks).toHaveLength(1);
        expect(blocks[0].start_hex).toBe('0'.repeat(64));
        expect(BigInt('0x' + blocks[0].end_hex)).toBe(500n);
    });

    test('seedGlobalBlocks is idempotent — calling twice replaces state cleanly', () => {
        const end = (1000n).toString(16).padStart(64, '0');
        const puzzle = seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        // Simulate partial state: delete only alloc_order (as if a boot was interrupted)
        db.prepare("DELETE FROM alloc_order WHERE puzzle_id = ?").run(puzzle.id);

        // Should not throw — seedGlobalBlocks clears both tables before inserting
        expect(() => {
            seedGlobalBlocks(db, puzzle.id, '0'.repeat(64), end,
                defaultAllocSeedForPuzzle(puzzle), 100n);
        }).not.toThrow();

        const blockCount = db.prepare("SELECT COUNT(*) AS c FROM alloc_blocks WHERE puzzle_id = ?").get(puzzle.id).c;
        const orderCount = db.prepare("SELECT COUNT(*) AS c FROM alloc_order WHERE puzzle_id = ?").get(puzzle.id).c;
        expect(blockCount).toBe(10);
        expect(orderCount).toBe(10);
    });
});

describe('Global Block Allocator — fresh allocation', () => {
    test('bootstrap: first chunk targets midpoint block by block_index', async () => {
        // 10 blocks of 100 keys; midpoint = block_index 5, starts at key 500
        const end = (1000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        const r = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1 });
        expect(r.status).toBe(200);
        expect(BigInt('0x' + r.body.start_key)).toBe(500n);
    });

    test('no overlap between consecutive fresh allocations', async () => {
        const end = (2000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 200 });

        const chunks = [];
        for (let i = 0; i < 5; i++) {
            const r = await request(app).post('/api/v1/work').send({ name: `w${i}`, hashrate: 1 });
            expect(r.status).toBe(200);
            chunks.push({ s: BigInt('0x' + r.body.start_key), e: BigInt('0x' + r.body.end_key) });
        }

        for (let i = 0; i < chunks.length; i++) {
            for (let j = i + 1; j < chunks.length; j++) {
                expect(chunks[i].s < chunks[j].e && chunks[j].s < chunks[i].e).toBe(false);
            }
        }
    });

    test('full allocation exhausts puzzle keyspace exactly and then returns 503', async () => {
        const end = (1000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        const chunks = [];
        for (let i = 0; i < 10; i++) {
            const r = await request(app).post('/api/v1/work').send({ name: `w${i}`, hashrate: 1 });
            expect(r.status).toBe(200);
            chunks.push({ s: BigInt('0x' + r.body.start_key), e: BigInt('0x' + r.body.end_key) });
        }

        // No pairwise overlaps
        for (let i = 0; i < chunks.length; i++) {
            for (let j = i + 1; j < chunks.length; j++) {
                expect(chunks[i].s < chunks[j].e && chunks[j].s < chunks[i].e).toBe(false);
            }
        }

        // Union equals full keyspace
        const sorted = [...chunks].sort((a, b) => (a.s < b.s ? -1 : 1));
        const totalCovered = sorted.reduce((acc, c) => acc + (c.e - c.s), 0n);
        expect(totalCovered).toBe(1000n);

        // 11th request: exhausted
        const last = await request(app).post('/api/v1/work').send({ name: 'w_last', hashrate: 1 });
        expect(last.status).toBe(503);
    }, 30000);

    test('alloc_cursor reaches blockCount after all blocks exhausted', async () => {
        const end = (300n).toString(16).padStart(64, '0');
        const puzzle = seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        for (let i = 0; i < 3; i++) {
            await request(app).post('/api/v1/work').send({ name: `w${i}`, hashrate: 1 });
        }

        const p = db.prepare("SELECT alloc_cursor, alloc_block_count FROM puzzles WHERE id = ?").get(puzzle.id);
        expect(p.alloc_cursor).toBe(p.alloc_block_count);
    });

    test('block frontier is not rolled back when chunk is reclaimed', async () => {
        const end = (1000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        const r = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1 });
        const chunk = db.prepare("SELECT alloc_block_id FROM chunks WHERE id = ?").get(r.body.job_id);

        const beforeHex = db.prepare("SELECT current_hex FROM alloc_blocks WHERE id = ?")
            .get(chunk.alloc_block_id).current_hex;

        db.prepare(
            "UPDATE chunks SET status='reclaimed', prev_worker_name=worker_name, worker_name=NULL WHERE id=?"
        ).run(r.body.job_id);

        const afterHex = db.prepare("SELECT current_hex FROM alloc_blocks WHERE id = ?")
            .get(chunk.alloc_block_id).current_hex;

        expect(afterHex).toBe(beforeHex);
    });

    test('reclaimed chunk reissued before fresh allocation, no overlap with subsequent fresh chunk', async () => {
        const end = (1000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        const r1 = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1 });
        db.prepare(
            "UPDATE chunks SET status='reclaimed', prev_worker_name=worker_name, worker_name=NULL WHERE id=?"
        ).run(r1.body.job_id);

        // w2 should receive the reclaimed chunk
        const r2 = await request(app).post('/api/v1/work').send({ name: 'w2', hashrate: 1 });
        expect(r2.body.job_id).toBe(r1.body.job_id);

        // w3 gets a fresh chunk with no overlap against the reclaimed one
        const r3 = await request(app).post('/api/v1/work').send({ name: 'w3', hashrate: 1 });
        expect(r3.status).toBe(200);
        expect(r3.body.job_id).not.toBe(r1.body.job_id);

        const aS = BigInt('0x' + r2.body.start_key), aE = BigInt('0x' + r2.body.end_key);
        const bS = BigInt('0x' + r3.body.start_key), bE = BigInt('0x' + r3.body.end_key);
        expect(aS < bE && bS < aE).toBe(false);
    });
});

describe('Global Block Allocator — stats and API', () => {
    test('stats puzzle object includes allocator diagnostic fields', async () => {
        seedPuzzle(db);
        const res = await request(app).get('/api/v1/stats').expect(200);
        const p = res.body.puzzle;
        expect(p.alloc_strategy).toBe(ALLOC_STRATEGY_GLOBAL);
        expect(p.alloc_block_count).toBeGreaterThan(0);
        expect(typeof p.alloc_cursor).toBe('number');
        expect(p.alloc_blocks_completed).toBe(0);
    });

    test('shards.total reflects alloc_block_count for global strategy', async () => {
        const end = (1000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });
        const res = await request(app).get('/api/v1/stats').expect(200);
        expect(res.body.shards.total).toBe(10);
        expect(res.body.shards.completed).toBe(0);
    });

    test('shards.completed increments as blocks are exhausted', async () => {
        const end = (300n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        let prevCompleted = 0;
        for (let i = 0; i < 3; i++) {
            await request(app).post('/api/v1/work').send({ name: `w${i}`, hashrate: 1 });
            const res = await request(app).get('/api/v1/stats');
            expect(res.body.shards.completed).toBeGreaterThanOrEqual(prevCompleted);
            prevCompleted = res.body.shards.completed;
        }
        expect(prevCompleted).toBe(3);
    });

    test('set-puzzle creates global block puzzle by default', async () => {
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'NewPuzzle', start_hex: '0x0', end_hex: '0x3e8' })
            .expect(200);

        const p = db.prepare("SELECT * FROM puzzles WHERE active = 1").get();
        expect(p.alloc_strategy).toBe(ALLOC_STRATEGY_GLOBAL);
        const blockCount = db.prepare("SELECT COUNT(*) AS c FROM alloc_blocks WHERE puzzle_id = ?").get(p.id).c;
        expect(blockCount).toBeGreaterThan(0);
    });

    test('set-puzzle accepts explicit alloc_strategy=legacy and creates sectors not blocks', async () => {
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'Legacy', start_hex: '0x100', end_hex: '0x200', alloc_strategy: ALLOC_STRATEGY_LEGACY })
            .expect(200);

        const p = db.prepare("SELECT * FROM puzzles WHERE active = 1").get();
        expect(p.alloc_strategy).toBe(ALLOC_STRATEGY_LEGACY);
        const sectors = db.prepare("SELECT COUNT(*) AS c FROM sectors WHERE puzzle_id = ?").get(p.id).c;
        expect(sectors).toBeGreaterThan(0);
        const blocks = db.prepare("SELECT COUNT(*) AS c FROM alloc_blocks WHERE puzzle_id = ?").get(p.id).c;
        expect(blocks).toBe(0);
    });

    test('set-puzzle accepts custom alloc_block_size_keys and populates correct block count', async () => {
        // 0x3e8 = 1000 keys / 100 per block = 10 blocks
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'Custom', start_hex: '0x0', end_hex: '0x3e8', alloc_block_size_keys: '100' })
            .expect(200);

        const p = db.prepare("SELECT alloc_block_count FROM puzzles WHERE active = 1").get();
        expect(p.alloc_block_count).toBe(10);
    });

    test('set-puzzle rejects unknown alloc_strategy', async () => {
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'Bad', start_hex: '0x100', end_hex: '0x200', alloc_strategy: 'bogus_strategy' })
            .expect(400);
    });

    test('set-puzzle creates new puzzle row when block size differs from existing', async () => {
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x0', end_hex: '0x3e8', alloc_block_size_keys: '100' })
            .expect(200);
        const first = db.prepare("SELECT id FROM puzzles WHERE active = 1").get();

        // Same name/range/strategy, different block size — should create a new row
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x0', end_hex: '0x3e8', alloc_block_size_keys: '200' })
            .expect(200);
        const second = db.prepare("SELECT id FROM puzzles WHERE active = 1").get();

        expect(second.id).not.toBe(first.id);
        const newPuzzle = db.prepare("SELECT alloc_block_count FROM puzzles WHERE id = ?").get(second.id);
        expect(newPuzzle.alloc_block_count).toBe(5); // 1000 / 200 = 5 blocks
    });

    test('set-puzzle reuses existing puzzle when block size is unchanged', async () => {
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x0', end_hex: '0x3e8', alloc_block_size_keys: '100' })
            .expect(200);
        const first = db.prepare("SELECT id FROM puzzles WHERE active = 1").get();

        // Same everything — should reuse
        await request(app)
            .post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x0', end_hex: '0x3e8', alloc_block_size_keys: '100' })
            .expect(200);
        const second = db.prepare("SELECT id FROM puzzles WHERE active = 1").get();

        expect(second.id).toBe(first.id);
    });

    test('current_shard and finders[].shard both use physical block_index (consistency)', async () => {
        // 10 blocks of 100 keys; bootstrap goes to block_index 5 (midpoint)
        const end = (1000n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        const r = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1 });
        const job_id = r.body.job_id;

        // Worker is still assigned — check current_shard shows physical block_index
        const statsBeforeSubmit = await request(app).get('/api/v1/stats');
        const worker = statsBeforeSubmit.body.workers.find(w => w.name === 'w1');
        expect(worker.current_shard).toBe(5); // physical block_index 5 = midpoint of 10 blocks

        // Submit FOUND so the chunk appears in finders
        await request(app).post('/api/v1/submit')
            .send({ name: 'w1', job_id, status: 'FOUND', findings: [{ found_key: '0'.repeat(64) }] });

        const statsAfterSubmit = await request(app).get('/api/v1/stats');
        const finder = statsAfterSubmit.body.finders[0];
        expect(finder.shard).toBe(5); // same physical block_index
    });

    test('non-bootstrap fresh chunk shows correct physical block_index in current_shard', async () => {
        // 3 blocks of 100 keys; bootstrap consumes block_index 1 (midpoint of 3)
        const end = (300n).toString(16).padStart(64, '0');
        seedPuzzle(db, { start_hex: '0'.repeat(64), end_hex: end, block_size_keys: 100 });

        // w1 gets bootstrap chunk (block_index 1)
        await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1 });
        // w2 gets next chunk from permutation (some other physical block)
        await request(app).post('/api/v1/work').send({ name: 'w2', hashrate: 1 });

        const stats = await request(app).get('/api/v1/stats');
        const w1 = stats.body.workers.find(w => w.name === 'w1');
        const w2 = stats.body.workers.find(w => w.name === 'w2');

        // Both shards must be valid physical block indices (0, 1, or 2)
        expect(w1.current_shard).toBeGreaterThanOrEqual(0);
        expect(w1.current_shard).toBeLessThan(3);
        expect(w2.current_shard).toBeGreaterThanOrEqual(0);
        expect(w2.current_shard).toBeLessThan(3);
        // And they must be in different blocks
        expect(w1.current_shard).not.toBe(w2.current_shard);
    });
});
