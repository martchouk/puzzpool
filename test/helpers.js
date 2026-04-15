'use strict';

const Database = require('better-sqlite3');

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
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE chunks (
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
        CREATE TABLE findings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            chunk_id INTEGER NOT NULL,
            worker_name TEXT NOT NULL,
            found_key TEXT NOT NULL,
            found_address TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    `);

    return db;
}

/**
 * Seed an active puzzle and return its row.
 */
function seedPuzzle(db, opts = {}) {
    const name  = opts.name      || 'Test Puzzle';
    const start = opts.start_hex || '0000000000000000000000000000000000000000000000000400000000000000';
    const end   = opts.end_hex   || '0000000000000000000000000000000000000000000000007fffffffffffffff';
    db.prepare("INSERT INTO puzzles (name, start_hex, end_hex, active) VALUES (?, ?, ?, 1)")
        .run(name, start, end);
    return db.prepare("SELECT * FROM puzzles WHERE active = 1 LIMIT 1").get();
}

module.exports = { createTestDb, seedPuzzle };
