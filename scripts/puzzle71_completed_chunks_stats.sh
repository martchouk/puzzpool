#!/usr/bin/env bash
set -euo pipefail

OLD_DB="${OLD_DB:-/home/jan/git/puzzpool/pool.db}"
NEW_DB="${NEW_DB:-/home/jan/git/puzzpool.test/pool.db}"
PUZZLE_START_HEX="${PUZZLE_START_HEX:-0000000000000000000000000000000000000000000000400000000000000000}"
PUZZLE_END_HEX="${PUZZLE_END_HEX:-00000000000000000000000000000000000000000000007fffffffffffffffff}"

if [[ ! -f "$OLD_DB" ]]; then
    printf 'old db not found: %s\n' "$OLD_DB" >&2
    exit 1
fi

if [[ ! -f "$NEW_DB" ]]; then
    printf 'new db not found: %s\n' "$NEW_DB" >&2
    exit 1
fi

sqlite3 "$NEW_DB" <<SQL
.headers on
.mode box
.bail on

ATTACH DATABASE '$OLD_DB' AS olddb;

CREATE TEMP TABLE source_puzzle AS
SELECT id AS old_puzzle_id, name, start_hex, end_hex, active, alloc_strategy,
       alloc_seed, alloc_cursor, virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
FROM olddb.puzzles
WHERE start_hex = '$PUZZLE_START_HEX'
  AND end_hex = '$PUZZLE_END_HEX'
ORDER BY id ASC
LIMIT 1;

CREATE TEMP TABLE dest_puzzle AS
SELECT id AS puzzle_id, name, start_hex, end_hex, active, alloc_strategy,
       alloc_seed, alloc_cursor_hex, virtual_chunk_size_keys, virtual_chunk_count_hex, bootstrap_stage
FROM puzzles
WHERE start_hex = (SELECT start_hex FROM source_puzzle)
  AND end_hex = (SELECT end_hex FROM source_puzzle)
ORDER BY id ASC
LIMIT 1;

SELECT 'source_puzzle_matches' AS metric, COUNT(*) AS value
FROM olddb.puzzles
WHERE start_hex = '$PUZZLE_START_HEX'
  AND end_hex = '$PUZZLE_END_HEX';

SELECT 'destination_puzzle_matches' AS metric, COUNT(*) AS value
FROM puzzles
WHERE start_hex = (SELECT start_hex FROM source_puzzle)
  AND end_hex = (SELECT end_hex FROM source_puzzle);

SELECT 'source_puzzle' AS section, *
FROM source_puzzle;

SELECT 'destination_puzzle' AS section, *
FROM dest_puzzle;

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

CREATE TEMP TABLE migratable_chunks AS
SELECT s.*
FROM source_chunks s
WHERE NOT EXISTS (
    SELECT 1
    FROM chunks n
    WHERE n.puzzle_id = (SELECT puzzle_id FROM dest_puzzle)
      AND n.start_hex = s.start_hex
      AND n.end_hex = s.end_hex
      AND n.status = s.status
      AND COALESCE(n.worker_name, '') = COALESCE(s.worker_name, '')
      AND COALESCE(n.assigned_at, '') = COALESCE(s.assigned_at, '')
      AND COALESCE(n.heartbeat_at, '') = COALESCE(s.heartbeat_at, '')
);

SELECT 'source_completed_chunks' AS metric, COUNT(*) AS value FROM source_chunks;
SELECT 'source_found_chunks' AS metric, COUNT(*) AS value FROM source_chunks WHERE status = 'FOUND';
SELECT 'source_chunks_with_vchunk_span' AS metric, COUNT(*) AS value
FROM source_chunks
WHERE vchunk_start IS NOT NULL AND vchunk_end IS NOT NULL;
SELECT 'migratable_chunks' AS metric, COUNT(*) AS value FROM migratable_chunks;
SELECT 'already_present_chunks' AS metric, (SELECT COUNT(*) FROM source_chunks) - (SELECT COUNT(*) FROM migratable_chunks) AS value;
SELECT 'migratable_findings' AS metric, COUNT(*) AS value
FROM olddb.findings f
JOIN migratable_chunks s ON s.old_chunk_id = f.chunk_id;
SELECT 'distinct_workers' AS metric, COUNT(DISTINCT worker_name) AS value
FROM migratable_chunks
WHERE worker_name IS NOT NULL;

SELECT 'migratable_chunk_samples' AS section,
       old_chunk_id,
       status,
       worker_name,
       start_hex,
       end_hex,
       vchunk_start,
       vchunk_end
FROM migratable_chunks
ORDER BY old_chunk_id
LIMIT 10;
SQL
