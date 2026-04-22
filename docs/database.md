# Database

puzzpool uses **SQLite 3** via the `better-sqlite3` synchronous driver.
The database file defaults to `pool.db` in the working directory (override with `DB_PATH`).
WAL (Write-Ahead Logging) mode is enabled for concurrent read access from the dashboard.

## Schema

```
┌─────────────────────────────────────────────────────────────────────────┐
│ puzzles                                                                  │
├──────────────────┬─────────────┬────────────────────────────────────────┤
│ id               │ INTEGER PK  │ Auto-increment                         │
│ name             │ TEXT        │ Human-readable label, e.g. "Puzzle #71"│
│ start_hex        │ TEXT        │ Lower bound, 64-char hex               │
│ end_hex          │ TEXT        │ Upper bound, 64-char hex               │
│ active           │ INTEGER     │ 1 = currently active, 0 = historical   │
│ test_start_hex   │ TEXT NULL   │ Test chunk start (nullable)            │
│ test_end_hex     │ TEXT NULL   │ Test chunk end   (nullable)            │
└──────────────────┴─────────────┴────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│ workers                                                                  │
├──────────────────┬─────────────┬────────────────────────────────────────┤
│ name             │ TEXT PK     │ Worker identifier (hostname)           │
│ hashrate         │ REAL        │ Last reported keys/s                   │
│ last_seen        │ DATETIME    │ Updated on every /work and /heartbeat  │
│ version          │ TEXT NULL   │ Client version string from APP_VERSION │
└──────────────────┴─────────────┴────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│ chunks                                                                   │
├──────────────────┬─────────────┬────────────────────────────────────────┤
│ id               │ INTEGER PK  │ Auto-increment; returned as job_id     │
│ puzzle_id        │ INTEGER     │ FK → puzzles.id                        │
│ start_hex        │ TEXT        │ Chunk lower bound (64-char hex)        │
│ end_hex          │ TEXT        │ Chunk upper bound (64-char hex)        │
│ status           │ TEXT        │ See lifecycle below                    │
│ worker_name      │ TEXT NULL   │ FK → workers.name (NULL when reclaimed)│
│ assigned_at      │ DATETIME    │ Last assignment timestamp              │
│ found_key        │ TEXT NULL   │ Private key (populated on FOUND)       │
│ found_address    │ TEXT NULL   │ Bitcoin address (populated on FOUND)   │
└──────────────────┴─────────────┴────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│ findings                                                                 │
├──────────────────┬─────────────┬────────────────────────────────────────┤
│ id               │ INTEGER PK  │ Auto-increment                         │
│ chunk_id         │ INTEGER     │ FK → chunks.id                         │
│ worker_name      │ TEXT        │ Who found it                           │
│ found_key        │ TEXT        │ Private key (hex)                      │
│ found_address    │ TEXT NULL   │ Bitcoin address                        │
│ created_at       │ DATETIME    │ Insertion timestamp                    │
└──────────────────┴─────────────┴────────────────────────────────────────┘
```

## Chunk Status Lifecycle

```
  INSERT  →  'assigned'
                │
                ├─ worker calls /heartbeat  →  assigned_at reset (stay 'assigned')
                │
                ├─ background job (every 60s, timeout 15min)  →  'reclaimed'
                │       │
                │       └─ next /work call re-assigns it      →  'assigned'
                │
                ├─ worker calls /submit status='done'         →  'completed'
                │
                └─ worker calls /submit status='FOUND'        →  'FOUND'
```

## Migrations

Two idempotent `ALTER TABLE` statements run at startup to add `test_start_hex` and
`test_end_hex` columns to the `puzzles` table (added after initial release):

```js
try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_start_hex TEXT").run(); } catch (_) {}
try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_end_hex   TEXT").run(); } catch (_) {}
```

The `try/catch` approach ensures existing databases with those columns are unaffected.

## WAL Mode

```js
db.pragma('journal_mode = WAL');
```

WAL allows concurrent readers (dashboard polling) while a writer holds the lock.
Without WAL, the dashboard would occasionally see `SQLITE_BUSY` errors during chunk
assignment writes.

## Database Reset

```bash
sudo systemctl stop puzzpool
rm pool.db pool.db-wal pool.db-shm
sudo systemctl start puzzpool   # re-creates and seeds Puzzle #71
```

## Inspecting the Database

```bash
sqlite3 pool.db

-- Active puzzle
SELECT name, start_hex, end_hex FROM puzzles WHERE active = 1;

-- Chunk counts by status
SELECT status, COUNT(*) FROM chunks GROUP BY status;

-- Leaderboard
SELECT worker_name, COUNT(*) chunks, SUM(CAST('0x'||end_hex AS REAL) - CAST('0x'||start_hex AS REAL)) keys
FROM chunks WHERE status IN ('completed','FOUND') GROUP BY worker_name ORDER BY keys DESC;

-- All findings
SELECT * FROM findings;
```
