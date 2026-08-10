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
                    ├─ /api/v1/auth/*   (public; OAuth + session cookie)
                    └─ /api/v1/admin/*  (central default-deny guard; IP-restricted
                                         at Nginx as defence in depth)

  Admin sign-in
  ─────────────
  Browser ──GET /auth/github/login──▶ AuthService ──302──▶ github.com
          ◀──302 + pp_oauth_state───                          │
          ────GET /auth/github/callback?code&state───────────┘
                        │
                        ├─ libcurl (injectable seam) ──▶ github.com/login/oauth/access_token
                        ├─ libcurl (injectable seam) ──▶ api.github.com/user
                        └─ 302 "/" + signed pp_session cookie

  Every /api/v1/admin/* request ──▶ authorizeAdminRequest(cfg, view, now)
                                     ├─ X-Admin-Token   (constant-time)
                                     └─ pp_session      (HMAC + expiry + allow-list
                                                         + CSRF proof on POST)
```

## Tech Stack

| Layer | Technology | Notes |
|-------|-----------|-------|
| HTTP server | C++20, [Crow](https://crowcpp.org/) | Header-only, async I/O, built-in JSON |
| Database | SQLite 3 via [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | Synchronous; WAL mode for concurrent reads |
| Permutations | Boost Multiprecision (`cpp_int`) | 256-bit arithmetic for Feistel/affine chunk ordering |
| Outbound HTTP | libcurl | GitHub OAuth calls, behind an injectable `HttpClient` seam |
| Frontend source | TypeScript (strict), Vite, vite-plugin-singlefile | `frontend/` directory |
| Frontend output | `public/index.html` | Single self-contained file; built by `update.sh`; generated and untracked |
| Reverse proxy | Nginx | TLS termination, admin-route IP restriction |
| Process manager | systemd | Auto-restart on failure |

## C++ Server Modules

| File | Responsibility |
|------|---------------|
| `src/main.cpp` | Route wiring (Crow), startup diagnostics, reclaimer thread |
| `src/auth.cpp` | Session/state signing, allow-list, and the central admin authorization decision — deliberately Crow-free so every allow and deny path is unit-testable |
| `src/auth_service.cpp` | `AuthService` — Crow adapters for `/api/v1/auth/*` and for the admin guard |
| `src/http_client.cpp` | libcurl-backed `HttpClient` seam used for GitHub provider calls |
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
| `src/hash_utils.cpp` | SHA-256, the frozen `keyedDigestHex`, and RFC 2104 HMAC-SHA-256 (Apple CommonCrypto or OpenSSL) |
| `src/env.cpp` | dotenv loader, `getEnvOr` / `getEnvInt` helpers |

Headers live under `include/puzzpool/`. Dependency direction (no cycles):

```
main → service      → {work_service, submission_service} → allocator → db → config → env
     → auth_service → {auth, http_client}                                  → config → env
```

`auth` depends on `config` and `hash_utils` only. It has no Crow, no database, and
no I/O, so `authorizeAdminRequest()` is a pure function of configuration plus a
small `AdminRequestView`. `AuthService` holds no lock and no database handle, so
its blocking provider calls never run under the `PoolService` mutex.

## Frontend Modules

The dashboard is compiled from `frontend/src/` by Vite into generated `public/index.html`:

| Module | Responsibility |
|--------|---------------|
| `types.ts` | Typed API interfaces; numeric representation policy |
| `api.ts` | `fetchStats()`, `activatePuzzle()` — typed fetch wrappers |
| `format.ts` | Pure formatting helpers (BigInt, hashrate, ETA, progress, allocator) |
| `canvas.ts` | Canvas rendering (1D bar, heatmap, Hilbert, allocator diagnostics) |
| `dashboard.ts` | Entry point: state, DOM wiring, event handlers, 5-second poll loop |

The build step (`npm run build --prefix frontend`) compiles TypeScript, bundles all modules,
and inlines everything into `public/index.html`. `frontend/` is the single source of truth;
the generated file is served by the C++ process but is not tracked in git. Node.js is not
needed at runtime.

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
  Advance alloc_cursor_hex through permutation, assign virtual chunk run  ──▶  return {job_id}

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
cycle-walking Feistel cipher derived from a SHA-256 seed. The `alloc_cursor_hex` in the
`puzzles` table advances with each assignment. Every virtual chunk is visited exactly
once before any chunk is repeated, giving deterministic full coverage without storing
the full permutation in memory.

**Service layer separation** — `WorkService` and `SubmissionService` contain all domain
logic with no HTTP or Crow dependencies, making them directly testable with in-memory
SQLite. `PoolService` is a thin HTTP adapter: parse → lock mutex → delegate → serialise.

**Worker identity by name** — No registration or authentication. Workers are identified
only by the `name` string they send with each request. Chunk ownership is enforced by
`WHERE worker_name = ?` in all UPDATE statements.

**One default-deny admin guard** — Authorization is a single pure function in
`puzzpool_core`, not a lambda in `main.cpp`, so every allow and deny path — missing
configuration, wrong token, bad signature, expired cookie, revoked login, failed
CSRF check — is covered by direct unit tests. Two independent mechanisms may each
authorize a request (`X-Admin-Token`, or a signed GitHub session whose login is on
the allow-list); neither configured means nobody is authorized. See
[security.md](security.md) and ADR-5/ADR-6 in
[architecture-review.md](architecture-review.md).

**Stateless sessions** — The session is an HMAC-signed cookie carrying the login,
the numeric GitHub id, and an absolute expiry. Nothing is stored server-side, so
there is no session table to migrate, expire, or replicate. The cost is that a
cookie cannot be revoked individually before it expires; revocation works through
the allow-list instead, which is consulted on every request.

**Single-file frontend** — `public/index.html` has no runtime dependencies. The C++ server
serves that generated file with a single static-file route. TypeScript strict mode and the
Vite build catch type errors before the file is regenerated.
