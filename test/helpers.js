'use strict';

const Database = require('better-sqlite3');
const { seedSectors } = require('../server');

/**
 * Create an in-memory SQLite database with the full puzzpool schema.
 * Returns the db instance.  Caller is responsible for db.close().
 */
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
            test_end_hex TEXT
        );
        CREATE TABLE workers (
            name TEXT PRIMARY KEY,
            hashrate REAL,
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP,
            version TEXT
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
            sector_id INTEGER
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
 * Seed an active puzzle with sectors and return its row.
 * Uses a range < MIN_SECTOR_SIZE (1B keys) so exactly 1 sector is created — fast for tests.
 */
function seedPuzzle(db, opts = {}) {
    const name  = opts.name      || 'Test Puzzle';
    const start = opts.start_hex || '0'.repeat(64);
    // 0x3b9aca00 = 1,000,000,000 → range / MIN_SECTOR_SIZE = 1 sector
    const end   = opts.end_hex   || '000000000000000000000000000000000000000000000000000000003b9aca00';
    const info  = db.prepare("INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 1)")
        .run(name, start, end);
    const puzzle = db.prepare("SELECT * FROM puzzles WHERE id = ?").get(info.lastInsertRowid);
    seedSectors(db, puzzle.id, start, end);
    return puzzle;
}

module.exports = { createTestDb, seedPuzzle };
