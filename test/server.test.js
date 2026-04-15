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
