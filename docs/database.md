# Database

puzzpool uses **SQLite 3** via the `better-sqlite3` synchronous driver.
The database file defaults to `pool.db` in the working directory (override with `DB_PATH`).
WAL (Write-Ahead Logging) mode is enabled for concurrent read access from the dashboard.

## Schema

```
┌─────────────────────────────────────────────────────────────────────────┐
│ puzzles                                                                  │
├──────────────────────┬─────────────┬────────────────────────────────────┤
│ id                   │ INTEGER PK  │ Auto-increment                     │
│ name                 │ TEXT        │ Human-readable label               │
│ start_hex            │ TEXT        │ Lower bound, 64-char hex           │
│ end_hex              │ TEXT        │ Upper bound, 64-char hex           │
│ active               │ INTEGER     │ 1 = currently active, 0 = historical│
│ test_start_hex       │ TEXT NULL   │ Test chunk start (nullable)        │
│ test_end_hex         │ TEXT NULL   │ Test chunk end   (nullable)        │
│ alloc_strategy       │ TEXT NULL   │ virtual_random_chunks_v1 or legacy │
│ alloc_seed           │ TEXT NULL   │ Hex seed for deterministic perm.   │
│ alloc_cursor         │ INTEGER     │ Next permutation index to allocate │
│ virtual_chunk_size_keys│ TEXT NULL │ Chunk size in keys (decimal string)│
│ virtual_chunk_count  │ INTEGER NULL│ Total virtual chunks in puzzle     │
│ bootstrap_stage      │ INTEGER     │ 0–3: bootstrap progress (see below)│
└──────────────────────┴─────────────┴────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│ workers                                                                  │
├──────────────────┬─────────────┬────────────────────────────────────────┤
│ name             │ TEXT PK     │ Worker identifier (hostname)           │
│ hashrate         │ REAL        │ Last reported keys/s                   │
│ last_seen        │ DATETIME    │ Updated on every /work and /heartbeat  │
│ version          │ TEXT NULL   │ Client version string from APP_VERSION │
│ min_chunk_keys   │ TEXT NULL   │ Minimum efficient job size (decimal)   │
│ chunk_quantum_keys│ TEXT NULL  │ Job size must be multiple of this      │
└──────────────────┴─────────────┴────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│ chunks                                                                   │
├──────────────────┬─────────────┬────────────────────────────────────────┤
│ id               │ INTEGER PK  │ Auto-increment; returned as job_id     │
│ puzzle_id        │ INTEGER     │ FK → puzzles.id                        │
│ start_hex        │ TEXT        │ Chunk lower bound (64-char hex)        │
│ end_hex          │ TEXT        │ Chunk upper bound (64-char hex)        │
│ status           │ TEXT        │ See lifecycle below                    │
│ worker_name      │ TEXT NULL   │ Current assignee (NULL when reclaimed) │
│ prev_worker_name │ TEXT NULL   │ Previous assignee (set on reclaim)     │
│ assigned_at      │ DATETIME    │ Timestamp of (last) assignment         │
│ heartbeat_at     │ DATETIME    │ Timestamp of last heartbeat; used for  │
│                  │             │ reclaim timeout (falls back to         │
│                  │             │ assigned_at if NULL)                   │
│ found_key        │ TEXT NULL   │ Private key (populated on FOUND)       │
│ found_address    │ TEXT NULL   │ Bitcoin address (populated on FOUND)   │
│ is_test          │ INTEGER     │ 1 = test chunk, excluded from stats    │
│ sector_id        │ INTEGER NULL│ FK → sectors.id (legacy allocator)     │
│ alloc_block_id   │ INTEGER NULL│ (unused, legacy)                       │
│ vchunk_start     │ INTEGER NULL│ First virtual chunk index in this job  │
│ vchunk_end       │ INTEGER NULL│ Last virtual chunk index + 1 (exclusive)│
│ alloc_generation │ TEXT NULL   │ Permutation mode when assigned:        │
│                  │             │ "feistel", "affine", "legacy", "test"  │
└──────────────────┴─────────────┴────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│ sectors                                                                  │
├──────────────────┬─────────────┬────────────────────────────────────────┤
│ id               │ INTEGER PK  │ Auto-increment                         │
│ puzzle_id        │ INTEGER     │ FK → puzzles.id                        │
│ start_hex        │ TEXT        │ Sector lower bound                     │
│ end_hex          │ TEXT        │ Sector upper bound                     │
│ current_hex      │ TEXT        │ Next key to allocate within sector     │
│ status           │ TEXT        │ open | exhausted                       │
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
                ├─ worker calls /heartbeat  →  heartbeat_at reset (stay 'assigned')
                │
                ├─ background job (every 60s)
                │    COALESCE(heartbeat_at, assigned_at) < NOW() - TIMEOUT_MINUTES
                │                                          →  'reclaimed'
                │       │
                │       └─ next /work call re-assigns it   →  'assigned'
                │
                ├─ worker calls /submit status='done'      →  'completed'
                │
                └─ worker calls /submit status='FOUND'     →  'FOUND'

  Late FOUND (worker submits after its chunk was reclaimed):
    prev_worker_name is saved on reclaim
    server accepts FOUND if submitter == prev_worker_name
    chunk transitions to 'FOUND' regardless of current assignee
```

## Bootstrap Stage (virtual_random_chunks_v1)

The first three chunk assignments for a new puzzle anchor coverage at known positions
before random permutation begins:

| Stage | Assignment | Purpose |
|-------|-----------|---------|
| 0 | Midpoint of keyspace | Balance: covers the middle first |
| 1 | Beginning of keyspace | Ensures full coverage starts early |
| 2 | End of keyspace | Ensures full coverage starts early |
| ≥ 3 | Affine permutation order | Normal random allocation |

## Migrations

All schema additions run as idempotent `ALTER TABLE` statements at startup.
The `try/catch` ensures existing databases with those columns are unaffected:

```js
// puzzles
try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_start_hex TEXT").run(); }        catch (_) {}
try { db.prepare("ALTER TABLE puzzles ADD COLUMN test_end_hex TEXT").run(); }          catch (_) {}
try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_strategy TEXT").run(); }        catch (_) {}
try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_seed TEXT").run(); }            catch (_) {}
try { db.prepare("ALTER TABLE puzzles ADD COLUMN alloc_cursor INTEGER ...").run(); }   catch (_) {}
try { db.prepare("ALTER TABLE puzzles ADD COLUMN virtual_chunk_size_keys TEXT").run(); } catch (_) {}
try { db.prepare("ALTER TABLE puzzles ADD COLUMN virtual_chunk_count INTEGER").run(); } catch (_) {}
try { db.prepare("ALTER TABLE puzzles ADD COLUMN bootstrap_stage INTEGER ...").run(); } catch (_) {}

// workers
try { db.prepare("ALTER TABLE workers ADD COLUMN version TEXT").run(); }               catch (_) {}
try { db.prepare("ALTER TABLE workers ADD COLUMN min_chunk_keys TEXT").run(); }        catch (_) {}
try { db.prepare("ALTER TABLE workers ADD COLUMN chunk_quantum_keys TEXT").run(); }    catch (_) {}

// chunks
try { db.prepare("ALTER TABLE chunks ADD COLUMN is_test INTEGER ...").run(); }         catch (_) {}
try { db.prepare("ALTER TABLE chunks ADD COLUMN prev_worker_name TEXT").run(); }       catch (_) {}
try { db.prepare("ALTER TABLE chunks ADD COLUMN alloc_block_id INTEGER").run(); }      catch (_) {}
try { db.prepare("ALTER TABLE chunks ADD COLUMN vchunk_start INTEGER").run(); }        catch (_) {}
try { db.prepare("ALTER TABLE chunks ADD COLUMN vchunk_end INTEGER").run(); }          catch (_) {}
try { db.prepare("ALTER TABLE chunks ADD COLUMN heartbeat_at DATETIME").run(); }       catch (_) {}
try { db.prepare("ALTER TABLE chunks ADD COLUMN alloc_generation TEXT").run(); }       catch (_) {}
// Backfill: carry assigned_at into heartbeat_at for old rows
UPDATE chunks SET heartbeat_at = assigned_at WHERE heartbeat_at IS NULL AND assigned_at IS NOT NULL
```

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
sudo systemctl start puzzpool
```

## Inspecting the Database

```bash
sqlite3 pool.db

-- Active puzzle
SELECT name, start_hex, end_hex, alloc_strategy, virtual_chunk_count FROM puzzles WHERE active = 1;

-- Chunk counts by status
SELECT status, COUNT(*) FROM chunks WHERE is_test = 0 GROUP BY status;

-- Leaderboard
SELECT worker_name, COUNT(*) chunks,
       SUM(vchunk_end - vchunk_start) vchunks_done
FROM chunks WHERE status IN ('completed','FOUND') AND is_test = 0
GROUP BY worker_name ORDER BY vchunks_done DESC;

-- All findings
SELECT * FROM findings;
```
