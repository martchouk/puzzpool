-- Copy completed Puzzle 71 chunks from the old pre-C++ database into the new C++ database.
-- Run this against the NEW database file:
--   sqlite3 /home/jan/git/puzzpool.test/pool.db < scripts/puzzle71_completed_chunks_migration.sql
--
-- This script assumes the destination database already contains the Puzzle 71 row.
-- It imports only non-test rows with status 'completed' or 'FOUND', and carries
-- over matching findings rows through a deterministic old-id -> new-id mapping.
-- Canonical Puzzle 71 keyspace in normalized 64-char hex:
--   start = 0000000000000000000000000000000000000000000000400000000000000000
--   end   = 00000000000000000000000000000000000000000000007fffffffffffffffff

ATTACH DATABASE '/home/jan/git/puzzpool/pool.db' AS olddb;

PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;

CREATE TEMP TABLE source_puzzle AS
SELECT id AS old_puzzle_id, name, start_hex, end_hex, active, alloc_strategy,
       alloc_seed, alloc_cursor, virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
FROM olddb.puzzles
WHERE start_hex = '0000000000000000000000000000000000000000000000400000000000000000'
  AND end_hex = '00000000000000000000000000000000000000000000007fffffffffffffffff'
ORDER BY id ASC
LIMIT 1;

CREATE TEMP TABLE dest_puzzle AS
SELECT id AS puzzle_id
FROM puzzles
WHERE start_hex = (SELECT start_hex FROM source_puzzle)
  AND end_hex = (SELECT end_hex FROM source_puzzle)
ORDER BY id ASC
LIMIT 1;

CREATE TEMP TABLE source_chunks AS
SELECT
    c.id AS old_chunk_id,
    c.start_hex,
    c.end_hex,
    c.status,
    c.worker_name,
    c.prev_worker_name,
    c.assigned_at,
    c.heartbeat_at,
    c.found_key,
    c.found_address,
    c.is_test,
    c.sector_id,
    c.vchunk_start,
    c.vchunk_end,
    c.alloc_generation,
    row_number() OVER (ORDER BY c.id) AS rn
FROM olddb.chunks c
JOIN source_puzzle p ON p.old_puzzle_id = c.puzzle_id
WHERE c.status IN ('completed', 'FOUND')
  AND c.is_test = 0;

CREATE TEMP TABLE existing_chunk_map AS
SELECT
    s.old_chunk_id,
    n.id AS new_chunk_id
FROM source_chunks s
JOIN chunks n
  ON n.puzzle_id = (SELECT puzzle_id FROM dest_puzzle)
 AND n.start_hex = s.start_hex
 AND n.end_hex = s.end_hex
 AND n.status = s.status
 AND COALESCE(n.worker_name, '') = COALESCE(s.worker_name, '')
 AND COALESCE(n.assigned_at, '') = COALESCE(s.assigned_at, '')
 AND COALESCE(n.heartbeat_at, '') = COALESCE(s.heartbeat_at, '');

CREATE TEMP TABLE pending_chunks AS
SELECT s.*
FROM source_chunks s
WHERE NOT EXISTS (
    SELECT 1
    FROM existing_chunk_map e
    WHERE e.old_chunk_id = s.old_chunk_id
);

CREATE TEMP TABLE chunk_base AS
SELECT COALESCE(MAX(id), 0) AS base_id
FROM chunks;

CREATE TEMP TABLE inserted_chunk_map AS
SELECT
    old_chunk_id,
    (SELECT base_id FROM chunk_base) + rn AS new_chunk_id
FROM pending_chunks;

CREATE TEMP TABLE chunk_map AS
SELECT old_chunk_id, new_chunk_id FROM existing_chunk_map
UNION ALL
SELECT old_chunk_id, new_chunk_id FROM inserted_chunk_map;

INSERT INTO chunks (
    id,
    puzzle_id,
    start_hex,
    end_hex,
    status,
    worker_name,
    prev_worker_name,
    assigned_at,
    heartbeat_at,
    found_key,
    found_address,
    is_test,
    sector_id,
    vchunk_start_hex,
    vchunk_end_hex,
    alloc_generation
)
SELECT
    m.new_chunk_id,
    (SELECT puzzle_id FROM dest_puzzle),
    s.start_hex,
    s.end_hex,
    s.status,
    s.worker_name,
    s.prev_worker_name,
    s.assigned_at,
    s.heartbeat_at,
    s.found_key,
    s.found_address,
    s.is_test,
    s.sector_id,
    CASE WHEN s.vchunk_start IS NULL THEN NULL ELSE lower(printf('%064x', s.vchunk_start)) END,
    CASE WHEN s.vchunk_end   IS NULL THEN NULL ELSE lower(printf('%064x', s.vchunk_end)) END,
    s.alloc_generation
FROM pending_chunks s
JOIN inserted_chunk_map m ON m.old_chunk_id = s.old_chunk_id
ORDER BY s.old_chunk_id;

UPDATE sqlite_sequence
SET seq = (SELECT COALESCE(MAX(id), 0) FROM chunks)
WHERE name = 'chunks';

INSERT OR IGNORE INTO findings (
    chunk_id,
    worker_name,
    found_key,
    found_address,
    created_at
)
SELECT
    m.new_chunk_id,
    f.worker_name,
    f.found_key,
    f.found_address,
    f.created_at
FROM olddb.findings f
JOIN chunk_map m ON m.old_chunk_id = f.chunk_id
ORDER BY f.id;

COMMIT;
