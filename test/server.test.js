'use strict';

const request = require('supertest');
const { createApp } = require('../server');
const { createTestDb, seedPuzzle } = require('./helpers');

let db, app;

beforeEach(() => {
    db  = createTestDb();
    app = createApp(db);
});

afterEach(() => {
    db.close();
});

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
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id: jobId, status: 'done' })
            .expect(200, { accepted: true });
        const chunk = db.prepare("SELECT status FROM chunks WHERE id=?").get(jobId);
        expect(chunk.status).toBe('completed');
    });

    test('marks chunk FOUND and inserts findings row', async () => {
        await request(app)
            .post('/api/v1/submit')
            .send({
                name: 'w1', job_id: jobId, status: 'FOUND',
                found_key: '0'.repeat(64),
                found_address: '1TestAddress',
            })
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
            .send({ name: 'w_other', job_id: jobId, status: 'done' });
        const chunk = db.prepare("SELECT status FROM chunks WHERE id=?").get(jobId);
        expect(chunk.status).toBe('assigned'); // unchanged
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

    test('finders entry includes chunk_id and shard', async () => {
        seedPuzzle(db);
        const r = await request(app).post('/api/v1/work').send({ name: 'w1', hashrate: 1000000 });
        const { job_id } = r.body;
        await request(app).post('/api/v1/submit')
            .send({ name: 'w1', job_id, status: 'FOUND', found_key: '0'.repeat(64), found_address: '1Test' });
        const stats = await request(app).get('/api/v1/stats');
        const finder = stats.body.finders[0];
        expect(typeof finder.chunk_id).toBe('number');
        expect(typeof finder.shard).toBe('number');
        expect(finder.chunk_id).toBe(job_id);
    });

    test('total_keys_completed reflects submitted chunks', async () => {
        seedPuzzle(db);
        const r = await request(app)
            .post('/api/v1/work')
            .send({ name: 'w1', hashrate: 1000000 });
        const { job_id, start_key, end_key } = r.body;
        await request(app)
            .post('/api/v1/submit')
            .send({ name: 'w1', job_id, status: 'done' });

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
        seedPuzzle(db, { name: 'Large', end_hex: end });

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

        // Sector frontier must not have moved — reclaim took priority
        const sector = db.prepare("SELECT * FROM sectors WHERE puzzle_id = ?").get(puzzle.id);
        expect(sector.current_hex).toBe(sector.start_hex);
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
        const p1Sectors = db.prepare("SELECT COUNT(*) as c FROM sectors WHERE puzzle_id=?").get(p1.id).c;
        expect(p1Sectors).toBe(1);

        // Change range → new puzzle row
        await request(app).post('/api/v1/admin/set-puzzle')
            .send({ name: 'P1', start_hex: '0x300', end_hex: '0x400' });
        const p2 = db.prepare("SELECT * FROM puzzles WHERE active=1").get();

        expect(p2.id).not.toBe(p1.id);
        expect(p2.start_hex).toContain('3');   // normalized 0x300
        const p2Sectors = db.prepare("SELECT COUNT(*) as c FROM sectors WHERE puzzle_id=?").get(p2.id).c;
        expect(p2Sectors).toBe(1);
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
        await request(app).post('/api/v1/submit').send({ name: 'w1', job_id: r.body.job_id, status: 'done' });

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
            .send({ name: 'tester', job_id: work.body.job_id, status: 'done' })
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
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', found_key: 'deadbeef'.padStart(64, '0'), found_address: '1Test' })
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
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', found_key: 'aabb'.padStart(64, '0'), found_address: '1A' })
            .expect(200);
        expect(late.body.accepted).toBe(true);

        // Chunk must be finalized — Worker B cannot now submit completed or FOUND
        const chunk = db.prepare("SELECT status FROM chunks WHERE id = ?").get(jobId);
        expect(chunk.status).toBe('FOUND');

        const bSubmit = await request(app).post('/api/v1/submit')
            .send({ name: 'workerB', job_id: jobId, status: 'done' })
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
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', found_key: key, found_address: '1A' })
            .expect(200);
        expect(r1.body.accepted).toBe(true);

        // Retry — same request
        const r2 = await request(app).post('/api/v1/submit')
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', found_key: key, found_address: '1A' })
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
            .send({ name: 'workerA', job_id: jobId, status: 'FOUND', found_key: 'beef'.padStart(64, '0'), found_address: '1B' })
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
            .send({ name: 'workerB', job_id: jobId, status: 'FOUND', found_key: 'deadbeef'.padStart(64, '0'), found_address: '1Test' })
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
