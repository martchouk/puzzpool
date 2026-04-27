'use strict';

const Database = require('better-sqlite3');
const {
    seedVirtualChunks,
    seedSectors,
    defaultAllocSeedForPuzzle,
    chooseDefaultVirtualChunkSize,
    ALLOC_STRATEGY_LEGACY,
    ALLOC_STRATEGY_VCHUNKS,
} = require('../server');

function createTestDb() {
    const db = new Database(':memory:');
    db.pragma('journal_mode = WAL');

    db.exec(`
        CREATE TABLE puzzles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            start_hex TEXT NOT NULL,
            end_hex TEXT NOT NULL,
            active INTEGER NOT NULL DEFAULT 0,
            test_start_hex TEXT,
            test_end_hex TEXT,
            alloc_strategy TEXT,
            alloc_seed TEXT,
            alloc_cursor INTEGER NOT NULL DEFAULT 0,
            virtual_chunk_size_keys TEXT,
            virtual_chunk_count INTEGER,
            bootstrap_stage INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE workers (
            name TEXT PRIMARY KEY,
            hashrate REAL,
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP,
            version TEXT,
            min_chunk_keys TEXT,
            chunk_quantum_keys TEXT
        );
        CREATE TABLE chunks (
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
            sector_id INTEGER,
            alloc_block_id INTEGER,
            vchunk_start INTEGER,
            vchunk_end INTEGER
        );
        CREATE TABLE sectors (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            puzzle_id   INTEGER NOT NULL,
            start_hex   TEXT NOT NULL,
            end_hex     TEXT NOT NULL,
            current_hex TEXT NOT NULL,
            status      TEXT NOT NULL DEFAULT 'open'
        );
        CREATE UNIQUE INDEX idx_sectors_unique_span ON sectors (puzzle_id, start_hex, end_hex);
        CREATE TABLE alloc_order_vchunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            puzzle_id INTEGER NOT NULL,
            order_index INTEGER NOT NULL,
            chunk_index INTEGER NOT NULL
        );
        CREATE UNIQUE INDEX idx_alloc_order_vchunks_order ON alloc_order_vchunks (puzzle_id, order_index);
        CREATE UNIQUE INDEX idx_alloc_order_vchunks_chunk ON alloc_order_vchunks (puzzle_id, chunk_index);
        CREATE TABLE findings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            chunk_id INTEGER NOT NULL,
            worker_name TEXT NOT NULL,
            found_key TEXT NOT NULL,
            found_address TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE UNIQUE INDEX idx_findings_dedup ON findings (chunk_id, worker_name, found_key);
    `);

    return db;
}

/**
 * Seed an active puzzle and return its row.
 *
 * opts.strategy defaults to ALLOC_STRATEGY_VCHUNKS (new allocator).
 * Pass { strategy: ALLOC_STRATEGY_LEGACY } to use the legacy sector allocator.
 *
 * For ALLOC_STRATEGY_VCHUNKS, opts.virtual_chunk_size_keys (number or bigint)
 * overrides the default chunk size — useful for small test puzzles with multiple chunks.
 *
 * Default range [0, 1,000,000,000) with default chunk size 30,000,000
 * produces exactly one virtual chunk (30M > 1B → clamped to range).
 */
function seedPuzzle(db, opts = {}) {
    const name     = opts.name      || 'Test Puzzle';
    const start    = opts.start_hex || '0'.repeat(64);
    const end      = opts.end_hex   || '000000000000000000000000000000000000000000000000000000003b9aca00';
    const strategy = opts.strategy  || ALLOC_STRATEGY_VCHUNKS;
    const seed = opts.seed || defaultAllocSeedForPuzzle({ name, start_hex: start, end_hex: end }, strategy);

    let virtualChunkSizeKeys = null;
    if (strategy === ALLOC_STRATEGY_VCHUNKS) {
        const range = BigInt('0x' + end) - BigInt('0x' + start);
        if (opts.virtual_chunk_size_keys !== undefined) {
            virtualChunkSizeKeys = BigInt(opts.virtual_chunk_size_keys);
            if (virtualChunkSizeKeys > range) virtualChunkSizeKeys = range;
        } else {
            virtualChunkSizeKeys = chooseDefaultVirtualChunkSize(range);
        }
    }

    const info = db.prepare(`
        INSERT INTO puzzles (
            name, start_hex, end_hex, active,
            alloc_strategy, alloc_seed, alloc_cursor,
            virtual_chunk_size_keys, bootstrap_stage
        )
        VALUES (?, ?, ?, 1, ?, ?, 0, ?, 0)
    `).run(
        name, start, end, strategy, seed,
        virtualChunkSizeKeys ? virtualChunkSizeKeys.toString() : null
    );

    const id = info.lastInsertRowid;

    if (strategy === ALLOC_STRATEGY_VCHUNKS) {
        seedVirtualChunks(db, id, start, end, seed, virtualChunkSizeKeys);
    } else {
        seedSectors(db, id, start, end);
    }

    return db.prepare("SELECT * FROM puzzles WHERE id = ?").get(id);
}

module.exports = { createTestDb, seedPuzzle };
