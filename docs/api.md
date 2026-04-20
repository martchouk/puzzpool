# API Reference

Base URL: `https://puzzle.b58.de` (production) or `http://127.0.0.1:8888` (local)

All request and response bodies are JSON. All hex strings are lowercase, 64 characters,
zero-padded (256-bit representation).

---

## Worker API

### POST /api/v1/work

Request the next keyspace chunk to scan.

**Request**
```json
{ "name": "worker-hostname", "hashrate": 8000000 }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Unique worker identifier (hostname recommended) |
| `hashrate` | number | no | Current scan speed in keys/s. Used to size chunks. Defaults to last known hashrate or 1,000,000. |

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
{ "name": "worker-hostname", "job_id": 42, "status": "done" }
```

**Request — key found**
```json
{
  "name": "worker-hostname",
  "job_id": 42,
  "status": "FOUND",
  "found_key": "0000...0600000000000000001",
  "found_address": "1A1zP1eP5QGefi2DMPTfTL5SLmv7Divf"
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Must match the worker name used in `/work` |
| `job_id` | number | yes | Job ID returned by `/work` |
| `status` | string | yes | `"done"` or `"FOUND"` |
| `found_key` | string | when FOUND | Private key (hex) |
| `found_address` | string | no | Bitcoin address or hash160 hex (40-char) if known; clients loading binary `.h160` files may submit the hash160 hex directly |

**Response 200**
```json
{ "accepted": true }
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

Dashboard data — polled every 3 seconds by `index.html`.

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
    "test_chunk": null
  },
  "active_workers_count": 3,
  "total_hashrate": 24500000,
  "completed_chunks": 187,
  "total_keys_completed": "12345678901234567890",
  "workers": [
    { "name": "rig1", "hashrate": 8000000, "last_seen": "2024-01-15 12:34:56", "current_chunk": 42 }
  ],
  "scores": [
    { "worker_name": "rig1", "completed_chunks": 95, "total_keys": "6300000000000" }
  ],
  "finders": [
    { "worker_name": "rig1", "found_key": "000...001", "found_address": "1ABC...", "created_at": "2024-01-15 12:34:56", "chunk_id": 42, "shard": 32768 }
  ],
  "chunks_vis": [
    { "id": 1, "st": "completed", "w": "rig1", "s": 0.0, "e": 0.004 }
  ]
}
```

`chunks_vis[].s` and `.e` are fractional positions within the puzzle range (0.0–1.0),
used by the canvas visualisations.

---

## Admin API

> Admin routes are IP-restricted at the Nginx level (see `deploy/nginx.conf`).
> If `ADMIN_TOKEN` is set, they additionally require the header `X-Admin-Token: <token>`.

### POST /api/v1/admin/set-puzzle

Create or activate a puzzle. Deactivates any currently active puzzle.

**Request**
```json
{ "name": "Puzzle #71", "start_hex": "0x400000000000000000", "end_hex": "0x7FFFFFFFFFFFFFFFFF" }
```

Hex strings may include an optional `0x` prefix; they are normalised server-side.

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
