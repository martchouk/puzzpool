# Architecture

## Overview

puzzpool is a minimal distributed keyspace search coordinator. Workers request chunks
of a Bitcoin puzzle's keyspace, scan them, and report results. The pool tracks progress
and visualises it on a live dashboard.

## Component Diagram

```
  Workers (scanners)           puzzpool server              Browser
  ─────────────────            ──────────────              ───────
  benchmark --pool    ──POST /work──▶   Express.js    ◀──GET /stats── Dashboard
                      ◀──{job_id}──     (port 8888)           │
                                              │                │ 3-second poll
  scan keyspace       ──POST /heartbeat──▶    │           index.html
                                              │          (canvas charts)
  found / done        ──POST /submit──▶       │
                                              ▼
                                         better-sqlite3
                                          (pool.db)
                                              │
                                        ┌─────────────┐
                                        │  puzzles    │
                                        │  workers    │
                                        │  chunks     │
                                        │  sectors    │
                                        │  findings   │
                                        └─────────────┘

  Internet
  ────────
  HTTPS :443  ──▶  Nginx  ──▶  127.0.0.1:8888
                    │
                    └─ /api/v1/admin/*  (IP-restricted or token-gated)
```

## Tech Stack

| Layer | Technology | Notes |
|-------|-----------|-------|
| Runtime | Node.js ≥ 18 | No transpilation, uses native BigInt |
| HTTP framework | Express 4 | Minimal routing, JSON body parsing |
| Database | SQLite 3 via `better-sqlite3` | Synchronous API; WAL mode for concurrent reads |
| Frontend | Vanilla HTML/CSS/JS | No build step; single file `public/index.html` |
| Reverse proxy | Nginx | TLS termination, admin-route IP restriction |
| Process manager | systemd | Auto-restart on failure |

## Data Flow — Chunk Lifecycle

```
  /work request
       │
       ▼
  Test chunk available?  ──yes──▶  assign test chunk  ──▶  return {job_id}
       │ no
       ▼
  Reclaimed chunks?      ──yes──▶  re-assign reclaimed  ──▶  return {job_id}
       │ no
       ▼
  Generate random chunk within puzzle range  ──▶  INSERT chunks  ──▶  return {job_id}

  Background (every 60 s):
    chunks WHERE status='assigned'
      AND COALESCE(heartbeat_at, assigned_at) < NOW()-TIMEOUT_MINUTES  →  status='reclaimed'

  /heartbeat:
    UPDATE chunks SET heartbeat_at = NOW()  (resets reclaim timer)

  /submit status='done':
    UPDATE chunks SET status='completed'

  /submit status='FOUND':
    UPDATE chunks SET status='FOUND', found_key, found_address
    INSERT findings
    append to BINGO_FOUND_KEYS.txt

  Late FOUND (worker submits after its chunk was reclaimed and reassigned):
    prev_worker_name is saved on reclaim
    server accepts FOUND if submitter matches prev_worker_name
    chunk finalized as status='FOUND' regardless of current assignee
```

## Key Design Decisions

**Synchronous SQLite** — `better-sqlite3` blocks on DB calls. For a single-server pool
with ≤100 workers this is simpler and more correct (no async race conditions on chunk
assignment) than an async driver.

**Deterministic affine permutation** — The `virtual_random_chunks_v1` allocator divides
the keyspace into fixed-size virtual chunks and visits them in a pseudo-random order
determined by an affine permutation (`i → (a·i + b) mod n`) derived from a SHA-256 seed.
The `alloc_cursor` in the `puzzles` table advances with each assignment. Every virtual
chunk is visited exactly once before any chunk is repeated, giving deterministic full
coverage. The legacy `legacy_random_shards_v1` allocator (sharded sector frontiers) is
retained for backward compatibility.

**Worker identity by name** — No registration or authentication. Workers are identified
only by the `name` string they send with each request. Chunk ownership is enforced by
`WHERE worker_name = ?` in all UPDATE statements.

**Single-file frontend** — `public/index.html` has no dependencies and no build step.
This makes deployment trivial: copy the file and serve it with Express static middleware.
