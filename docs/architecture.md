# Architecture

## Overview

puzzpool is a minimal distributed keyspace search coordinator. Workers request chunks
of a Bitcoin puzzle's keyspace, scan them, and report results. The pool tracks progress
and visualises it on a live dashboard.

## Component Diagram

```
  Workers (scanners)           puzzpool server              Browser
  ─────────────────            ──────────────              ───────
  benchmark --pool    ──POST /work──▶   C++ / Crow    ◀──GET /stats── Dashboard
                      ◀──{job_id}──     (port 8888)           │
                                              │                │ 5-second poll
  scan keyspace       ──POST /heartbeat──▶    │           public/index.html
                                              │          (TypeScript, canvas charts)
  found / done        ──POST /submit──▶       │
                                              ▼
                                         SQLiteCpp
                                          (pool.db)
                                              │
                                        ┌─────────────┐
                                        │  puzzles    │
                                        │  workers    │
                                        │  chunks     │
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
| HTTP server | C++20, [Crow](https://crowcpp.org/) | Header-only, async I/O, built-in JSON |
| Database | SQLite 3 via [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | Synchronous; WAL mode for concurrent reads |
| Permutations | Boost Multiprecision (`cpp_int`) | 256-bit arithmetic for Feistel/affine chunk ordering |
| Frontend source | TypeScript (strict), Vite, vite-plugin-singlefile | `frontend/` directory |
| Frontend output | `public/index.html` | Single self-contained file; built by `update.sh` |
| Reverse proxy | Nginx | TLS termination, admin-route IP restriction |
| Process manager | systemd | Auto-restart on failure |

## C++ Server Modules

| File | Responsibility |
|------|---------------|
| `src/main.cpp` | Route wiring (Crow), admin guard, reclaimer thread |
| `src/service.cpp` | `PoolService` — holds mutex, delegates to sub-services |
| `src/service_work.cpp` | HTTP adapter for `/work` and `/heartbeat` |
| `src/service_submit.cpp` | HTTP adapter for `/submit` |
| `src/service_admin.cpp` | HTTP adapters for admin routes |
| `src/service_bootstrap.cpp` | Bootstrap allocation (midpoint / begin / end anchoring) |
| `src/service_serialization.cpp` | JSON serialisation of the `/stats` response |
| `src/service_stats.cpp` | Stats queries and aggregation |
| `src/work_service.cpp` | `WorkService` — domain logic: assign, heartbeat, reclaim |
| `src/submission_service.cpp` | `SubmissionService` — domain logic: submit done / FOUND |
| `src/allocator.cpp` | Virtual chunk + legacy sector allocation algorithms |
| `src/db.cpp` | SQLite schema migration and query helpers |
| `src/config.cpp` | `loadConfigFromEnv()` — reads `.env` + process env |
| `src/permutation.cpp` | Feistel and affine permutation for chunk ordering |
| `src/hex_bigint.cpp` | Hex ↔ bigint conversion utilities |
| `src/hash_utils.cpp` | SHA-256 / HMAC-SHA-256 (Apple CommonCrypto or OpenSSL) |
| `src/env.cpp` | dotenv loader, `getEnvOr` / `getEnvInt` helpers |

Headers live under `include/puzzpool/`. Dependency direction (no cycles):
`main → service → {work_service, submission_service} → allocator → db → config → env`

## Frontend Modules

The dashboard is compiled from `frontend/src/` by Vite into `public/index.html`:

| Module | Responsibility |
|--------|---------------|
| `types.ts` | Typed API interfaces; numeric representation policy |
| `api.ts` | `fetchStats()`, `activatePuzzle()` — typed fetch wrappers |
| `format.ts` | Pure formatting helpers (BigInt, hashrate, ETA, progress, allocator) |
| `canvas.ts` | Canvas rendering (1D bar, heatmap, Hilbert, allocator diagnostics) |
| `dashboard.ts` | Entry point: state, DOM wiring, event handlers, 5-second poll loop |

The build step (`npm run build --prefix frontend`) compiles TypeScript, bundles all modules,
and inlines everything into `public/index.html`. The server process only serves this static
file — Node.js is not needed at runtime.

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
  Bootstrap stage < 3?   ──yes──▶  assign anchor chunk  ──▶  return {job_id}
       │ no
       ▼
  Advance alloc_cursor through permutation, assign virtual chunk run  ──▶  return {job_id}

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

**Synchronous SQLite** — SQLiteCpp blocks on DB calls. All chunk-assignment logic runs
under a single `std::mutex` in `PoolService`, so there are no async race conditions.
For a single-server pool with ≤100 workers this is simpler and more correct than
async drivers.

**Feistel permutation** — The `virtual_random_chunks_v1` allocator divides the keyspace
into fixed-size virtual chunks and visits them in a pseudo-random order determined by a
cycle-walking Feistel cipher derived from a SHA-256 seed. The `alloc_cursor` in the
`puzzles` table advances with each assignment. Every virtual chunk is visited exactly
once before any chunk is repeated, giving deterministic full coverage without storing
the full permutation in memory.

**Service layer separation** — `WorkService` and `SubmissionService` contain all domain
logic with no HTTP or Crow dependencies, making them directly testable with in-memory
SQLite. `PoolService` is a thin HTTP adapter: parse → lock mutex → delegate → serialise.

**Worker identity by name** — No registration or authentication. Workers are identified
only by the `name` string they send with each request. Chunk ownership is enforced by
`WHERE worker_name = ?` in all UPDATE statements.

**Single-file frontend** — `public/index.html` has no runtime dependencies. Deployment
only requires copying one file; the C++ server serves it with a single static-file route.
TypeScript strict mode and the Vite build catch type errors before the file is regenerated.
