# API Reference

Base URL: `https://puzzle.b58.de` (production) or `http://127.0.0.1:8888` (local)

All request and response bodies are JSON. All hex strings are lowercase, 64 characters,
zero-padded (256-bit representation).

## Large integer transport

Values that may exceed JavaScript's `Number.MAX_SAFE_INTEGER` (2^53−1) are transported as
**decimal strings**, not JSON numbers. Clients must parse these with `BigInt()` or an
equivalent arbitrary-precision library — do not coerce them with `Number()` or `parseFloat()`.

| Field | Type | Notes |
|-------|------|-------|
| `total_keys_completed` | decimal string | Total keys scanned across all completed chunks |
| `puzzle.total_keys` | decimal string | Total keys in the puzzle keyspace |
| `puzzle.virtual_chunk_size_keys` | decimal string \| null | Size of one virtual chunk in keys |
| `puzzle.virtual_chunk_count` | decimal string \| null | Total number of virtual chunks (may exceed 2^63) |
| `puzzle.alloc_cursor` | decimal string | Current allocator position in permutation order |
| `worker.current_job_keys` | decimal string \| null | Key count of the worker's current job |
| `worker.current_vchunk_run_start` | decimal string \| null | First virtual chunk index in the worker's current run |
| `worker.current_vchunk_run_end` | decimal string \| null | Exclusive end virtual chunk index (last + 1) |
| `finder.vchunk_start` | decimal string \| null | First virtual chunk index of the chunk where the key was found |
| `finder.vchunk_end` | decimal string \| null | Exclusive end virtual chunk index |
| `virtual_chunks.total` | decimal string \| 0 | Total virtual chunk count |
| `virtual_chunks.started_vchunks` | decimal string \| 0 | Sum of virtual chunk index spans for started jobs |
| `virtual_chunks.completed_vchunks` | decimal string \| 0 | Sum of virtual chunk index spans for completed jobs |
| `score.total_keys` | decimal string | Total keys attributed to a worker |

All hex fields (`start_hex`, `end_hex`, etc.) are fixed-width 64-character strings.
Regular counts (`active_workers_count`, `completed_chunks`, etc.) are safe JSON integers.

---

## Worker API

### POST /api/v1/work

Request the next keyspace chunk to scan.

**Request**
```json
{ "name": "worker-hostname", "hashrate": 8000000, "version": "1.2.1", "min_chunk_keys": "17045651456", "chunk_quantum_keys": "4261412864" }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Unique worker identifier (hostname recommended) |
| `hashrate` | number | no | Current scan speed in keys/s. Used to size chunks. Defaults to last known hashrate or 1,000,000. |
| `version` | string | no | Client version string (e.g. from `APP_VERSION`). Displayed in the Active Workers dashboard. |
| `min_chunk_keys` | string | no | Minimum efficient job size for this worker. Prevents pathologically small jobs for GPUs. Stored as a decimal integer string. |
| `chunk_quantum_keys` | string | no | Smallest meaningful increment for this worker. The job size is rounded up to a multiple of this value. Stored as a decimal integer string. |

**Job size calculation**

1. `target_keys = hashrate × TARGET_MINUTES × 60`
2. `requested_keys = max(target_keys, min_chunk_keys)` (if `min_chunk_keys` is provided)
3. `requested_keys = round_up(requested_keys, chunk_quantum_keys)` (if `chunk_quantum_keys` is provided)
4. `requested_virtual_chunks = ceil(requested_keys / virtual_chunk_size_keys)`, minimum 1

**Response 200**
```json
{ "job_id": 42, "start_key": "000...0600000000000000000", "end_key": "000...06000002bf20000000" }
```

| Field | Type | Description |
|-------|------|-------------|
| `job_id` | number | Chunk identifier — required for `/submit` and `/heartbeat` |
| `start_key` | string | First key to scan (inclusive) |
| `end_key` | string | Scan boundary (exclusive) — scan the half-open range `[start_key, end_key)` |

**Response 503** — no active puzzle configured
```json
{ "error": "No active puzzle configured" }
```

---

### POST /api/v1/submit

Report completion of a chunk, or a key discovery.

**Request — chunk completed**
```json
{ "name": "worker-hostname", "job_id": 42, "status": "done", "keys_scanned": 500000000 }
{ "name": "worker-hostname", "job_id": 42, "status": "done", "keys_scanned": "99999999999999999999" }
```

**Request — key found**
```json
{
  "name": "worker-hostname",
  "job_id": 42,
  "status": "FOUND",
  "findings": [
    { "found_key": "0000...0600000000000000001", "found_address": "1A1zP1eP5QGefi2DMPTfTL5SLmv7Divf" }
  ]
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Must match the worker name used in `/work` |
| `job_id` | number | yes | Job ID returned by `/work` |
| `status` | string | yes | `"done"` or `"FOUND"` |
| `keys_scanned` | number or string | yes (when done) | Keys actually scanned (only used with `status: "done"`). Accepts a JSON integer or a decimal string (e.g. `"99999999999999999999"`) for chunks larger than `int64_t`. Must be non-negative. If less than the chunk size, the chunk is reclaimed instead of completed. Values equal to or greater than the chunk size are accepted (clients may overshoot due to fixed batch granularity). |
| `findings` | array | when FOUND | Non-empty array of found key objects. Each object must include `found_key` (hex string, `0x` prefix optional) and may optionally include `found_address` (Bitcoin address or 40-char hash160 hex). All keys found in the chunk go here. |

**Response 200**
```json
{ "accepted": true }
```

**Response 400** — incomplete scan (`keys_scanned` provided but less than chunk size)
```json
{ "accepted": false, "error": "chunk #42 not accepted, reported size: 123456, expected size: 500000000. Chunk reclaimed." }
```

> **Note:** Ownership is enforced — submitting `job_id` for a chunk assigned to a different
> worker silently has no effect (the UPDATE matches on both `id` and `worker_name`).

---

### POST /api/v1/heartbeat

Keep a long-running job alive. Resets the 15-minute reclaim timer.
Call every 60–120 seconds while scanning a large chunk.

**Request**
```json
{ "name": "worker-hostname", "job_id": 42 }
```

**Response 200**
```json
{ "ok": true }
```

**Response 400** — missing fields
```json
{ "error": "Missing name or job_id" }
```

---

### GET /api/v1/stats

Dashboard data — polled every 5 seconds by the dashboard.

**Query parameters**

| Parameter | Type | Description |
|-----------|------|-------------|
| `puzzle_id` | number | *(optional)* Return stats for this puzzle ID instead of the pool-active one. Used by the dashboard when the user views a non-active tab. |

**Response 200** (abbreviated)
```json
{
  "puzzles": [
    { "id": 1, "name": "Puzzle #71", "active": 1 },
    { "id": 2, "name": "ALL BTC",    "active": 0 }
  ],
  "puzzle": {
    "id": 1,
    "name": "Puzzle #71",
    "start_hex": "000...0400000000000000000",
    "end_hex":   "000...07fffffffffffffffff",
    "total_keys": "2361183241434822606848",
    "test_chunk": null,
    "alloc_strategy": "virtual_random_chunks_v1",
    "alloc_cursor": "10452",
    "virtual_chunk_size_keys": "30000000",
    "virtual_chunk_count": "78706",
    "bootstrap_stage": 3
  },
  "stage": "PROD",
  "target_minutes": 10,
  "timeout_minutes": 15,
  "active_minutes": 1.167,
  "active_workers_count": 3,
  "inactive_workers_count": 1,
  "total_hashrate": 24500000,
  "completed_chunks": 187,
  "reclaimed_chunks": 3,
  "total_keys_completed": "12345678901234567890",
  "virtual_chunks": {
    "total": "78706",
    "started_vchunks": "200",
    "completed_vchunks": "187",
    "virtual_chunk_size_keys": "1000000"
  },
  "workers": [
    {
      "name": "rig1", "hashrate": 8000000, "last_seen": "2024-01-15 12:34:56",
      "version": "1.2.1", "active": true,
      "current_chunk": 42, "current_vchunk_run": "223735..223744",
      "current_vchunk_run_start": "223735", "current_vchunk_run_end": "223745",
      "assigned_at": "2024-01-15 12:30:00", "heartbeat_at": "2024-01-15 12:34:30",
      "current_job_start_hex": "000...06000000000", "current_job_end_hex": "000...060002bf200",
      "current_job_keys": "900000000",
      "current_job_elapsed_seconds": 270, "current_job_progress_percent": 24.0,
      "min_chunk_keys": "17045651456", "chunk_quantum_keys": "4261412864"
    }
  ],
  "scores": [
    { "worker_name": "rig1", "completed_chunks": 95, "total_keys": "6300000000000" }
  ],
  "finders": [
    { "worker_name": "rig1", "found_key": "000...001", "found_address": "1ABC...", "created_at": "2024-01-15 12:34:56", "chunk_global": 42, "vchunk_start": "223735", "vchunk_end": "223745" }
  ],
  "chunks_vis": [
    { "id": 1, "st": "completed", "w": "rig1", "s": 0.0, "e": 0.004 }
  ]
}
```

**`workers[]` fields**

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Worker identifier |
| `hashrate` | number | Last reported scan speed in keys/s |
| `version` | string\|null | Client version string sent via `/work`; null if not reported |
| `last_seen` | string | UTC timestamp of last `/work` or `/heartbeat` call |
| `active` | boolean | `true` if last seen within `ACTIVE_MINUTES` AND holds an assigned chunk in this puzzle (green dot); `false` if within `TIMEOUT_MINUTES` grace period but stale or unassigned (gray dot) |
| `current_chunk` | number\|null | ID of the currently assigned chunk; null if none |
| `current_vchunk_run` | string\|null | Virtual chunk range as `"start..end-1"` (e.g. `"223735..223744"`); null if no chunk assigned |
| `current_vchunk_run_start` | number\|null | First virtual chunk index of the current job; null if no chunk assigned |
| `current_vchunk_run_end` | number\|null | Exclusive end virtual chunk index of the current job; null if no chunk assigned |
| `assigned_at` | string\|null | UTC timestamp when the current chunk was assigned; null if no chunk |
| `heartbeat_at` | string\|null | UTC timestamp of the last heartbeat for the current chunk; null if no chunk |
| `current_job_start_hex` | string\|null | `start_hex` of the currently assigned chunk; null if no chunk |
| `current_job_end_hex` | string\|null | `end_hex` of the currently assigned chunk; null if no chunk |
| `current_job_keys` | string\|null | Size of the current job in keys (decimal integer string); null if no chunk |
| `min_chunk_keys` | string\|null | Worker's reported minimum job size (decimal integer string); null if not reported |
| `chunk_quantum_keys` | string\|null | Worker's reported job size quantum (decimal integer string); null if not reported |
| `fresh` | boolean | `true` if this worker was reactivated (transitioned from inactive to active) in the current stats response |
| `assigned_here` | boolean | `true` if the worker's current chunk is assigned to the puzzle currently being viewed |

**Top-level stats fields**

| Field | Type | Description |
|-------|------|-------------|
| `stage` | string | Deployment stage: `"PROD"` or `"TEST"` (from `STAGE` env var) |
| `target_minutes` | number | Configured chunk target duration in minutes (`TARGET_MINUTES`) |
| `timeout_minutes` | number | Configured reclaim timeout in minutes (`TIMEOUT_MINUTES`) |
| `active_minutes` | number | Configured active-worker threshold in minutes (`ACTIVE_MINUTES`) |
| `active_workers_count` | number | Workers currently active (green) |
| `inactive_workers_count` | number | Workers visible but not active (gray) |
| `total_hashrate` | number | Sum of hashrates of active workers |
| `completed_chunks` | number | Chunks with `status='completed'` or `'FOUND'` (excluding test chunks) |
| `reclaimed_chunks` | number | Chunks with `status='reclaimed'` |
| `total_keys_completed` | string | Total keys covered by completed/FOUND chunks (decimal string) |
| `alloc_generations` | object | Per-generation chunk counts: `{ "legacy": N, "affine": N, "feistel": N }` — tracks how many chunks were issued under each permutation algorithm |

**`virtual_chunks` field**

```json
{ "virtual_chunks": { "total": "78706", "started_vchunks": "200", "completed_vchunks": "187", "virtual_chunk_size_keys": "1000000" } }
```

All four fields are decimal strings (may exceed 2^53 for large keyspaces).

For `virtual_random_chunks_v1`: `total` is the total number of virtual chunks in the puzzle. `started_vchunks` is the sum of virtual chunk index spans covered by any live or completed job. `completed_vchunks` is the same sum for completed or FOUND jobs only. To derive keys: `started_vchunks × virtual_chunk_size_keys`.

For `legacy_random_shards_v1`: counts sectors as before (in both `virtual_chunks` and the backward-compatible `shards` alias). `virtual_chunk_size_keys` is `null` for the legacy strategy.

**`puzzle` allocator fields** (present when a puzzle is active)

| Field | Type | Description |
|-------|------|-------------|
| `alloc_strategy` | string | Active allocator: `"virtual_random_chunks_v1"` or `"legacy_random_shards_v1"` |
| `alloc_cursor` | string | Next position in the randomized permutation to allocate from |
| `virtual_chunk_size_keys` | string\|null | Size of each virtual chunk in keys (decimal integer string); `virtual_random_chunks_v1` only |
| `virtual_chunk_count` | string\|null | Total number of virtual chunks; `virtual_random_chunks_v1` only |
| `bootstrap_stage` | number | Bootstrap phase: 0=not started, 1=midpoint assigned, 2=begin assigned, 3=end assigned, ≥3=normal allocation |

`chunks_vis[].s` and `.e` are fractional positions within the puzzle range (0.0–1.0),
used by the canvas visualisations. `chunks_vis[].g` is the `alloc_generation` value
(`"feistel"`, `"affine"`, `"legacy"`, `"test"`, or `null` for old rows) — used by the
Allocator Diagnostics view to filter chunks by generation.

---

## Admin API

> Admin routes are IP-restricted at the Nginx level (see `deploy/nginx.conf`).
> If `ADMIN_TOKEN` is set, they additionally require the header `X-Admin-Token: <token>`.

### POST /api/v1/admin/set-puzzle

Create or activate a puzzle. Deactivates any currently active puzzle.

**Request**
```json
{
  "name": "Puzzle #71",
  "start_hex": "0x400000000000000000",
  "end_hex": "0x7FFFFFFFFFFFFFFFFF",
  "alloc_strategy": "virtual_random_chunks_v1",
  "virtual_chunk_size_keys": "30000000",
  "alloc_seed": "optional-hex-seed"
}
```

Hex strings may include an optional `0x` prefix; they are normalised server-side.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Puzzle name |
| `start_hex` | string | yes | Keyspace start (inclusive) |
| `end_hex` | string | yes | Keyspace end (exclusive) |
| `alloc_strategy` | string | no | `"virtual_random_chunks_v1"` (default) or `"legacy_random_shards_v1"` |
| `virtual_chunk_size_keys` | string | no | Virtual chunk size in keys as a decimal integer string. Defaults to `30000000`. Ignored for legacy strategy. |
| `alloc_seed` | string | no | Override the deterministic allocation seed. Immutable after first fresh allocation. |

If a puzzle with the same `name`, `start_hex`, `end_hex`, `alloc_strategy`, and `virtual_chunk_size_keys` already exists, it is reactivated without creating a new row.

**Response 200**
```json
{ "ok": true, "puzzle": { "id": 1, "name": "Puzzle #71", ... } }
```

---

### POST /api/v1/admin/set-test-chunk

Set a known-good test chunk so new workers can verify their scanner is working correctly.
The test chunk is offered to the **first** worker that calls `/work`.

**Set:**
```json
{ "start_hex": "0x5fffffffffff000000", "end_hex": "0x5fffffffffff100000" }
```

**Clear:**
```json
{ "start_hex": null }
```

**Response 200**
```json
{ "ok": true, "test_chunk": { "start_hex": "...", "end_hex": "..." } }
```

---

### GET /api/v1/admin/puzzles

List all puzzles (active and historical).

**Response 200**
```json
{ "puzzles": [ { "id": 1, "name": "Puzzle #71", "active": 1, ... } ] }
```

---

### POST /api/v1/admin/reclaim

Force-reclaim all timed-out chunks immediately, without waiting for the background
60-second reclaimer thread. Useful after a sudden loss of workers.

**Request** — empty body or `{}`

**Response 200**
```json
{ "ok": true, "reclaimed": 3 }
```

`reclaimed` is the number of chunks whose status was changed to `'reclaimed'`.

---

### POST /api/v1/admin/import-ranges

Import externally-searched key ranges and mark the corresponding virtual chunks as
blocked. Blocked virtual chunks are never assigned to workers.

The conversion formula maps each integer `range_id` to a contiguous key range:
- `key_start = base + range_id * step`
- `key_end   = key_start + step`

These key bounds are converted to virtual chunk indices using the puzzle's
`virtual_chunk_size_keys` and stored in `blocked_vchunk_ranges`. The endpoint is
idempotent: importing the same `(puzzle_id, computed vchunk range, source)` tuple
twice is safe.

**Request**
```json
{
  "puzzle_id":  1,
  "source":     "btcpuzzle.info",
  "base_hex":   "0000000000000000000000000000000000000000000000000000000000000001",
  "step":       "33554432",
  "range_ids":  ["0", "1", "42", "99"]
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `puzzle_id` | integer | yes | ID of the target puzzle (must use `virtual_random_chunks_v1`) |
| `source` | string | yes | Label identifying the origin of the data (e.g. `"btcpuzzle.info"`) |
| `base_hex` | hex string | yes | Key value corresponding to `range_id = 0` |
| `step` | decimal string or integer | yes | Number of keys per range unit |
| `range_ids` | array of decimal strings or integers | yes | Range identifiers to block |

**Response 200**
```json
{
  "ok": true,
  "inserted_ranges": 3,
  "already_blocked": 1,
  "invalid": 0,
  "errors": []
}
```

| Field | Description |
|-------|-------------|
| `inserted_ranges` | Number of new rows written to `blocked_vchunk_ranges` |
| `already_blocked` | Number of range_ids that were already present (idempotent duplicates) |
| `invalid` | Number of range_ids skipped (out of puzzle bounds, unparseable, etc.) |
| `errors` | Array of error strings from any DB-level failures |

**Response 400** — invalid request
```json
{ "error": "Puzzle does not use virtual_random_chunks_v1 strategy" }
```

**Response 404** — puzzle not found
```json
{ "error": "Puzzle not found" }
```

---

### POST /api/v1/admin/activate-puzzle

Switch the active puzzle. The previously active puzzle is deactivated.
Workers already scanning chunks from the old puzzle can still submit their results.

**Request**
```json
{ "id": 2 }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | number | yes | ID of the puzzle to activate |

**Response 200**
```json
{ "ok": true, "puzzle": { "id": 2, "name": "ALL BTC", "active": 1, ... } }
```

**Response 400** — missing id
```json
{ "error": "Missing id" }
```

**Response 404** — id not found
```json
{ "error": "Puzzle not found" }
```
