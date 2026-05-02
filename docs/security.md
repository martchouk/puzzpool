# Security

## Threat Model

puzzpool is a **public coordination service** for an open Bitcoin puzzle. The keyspace
and progress are intentionally public. There are no user accounts, no passwords, and no
financial transactions in the application layer. The primary security concerns are:

1. Preventing an attacker from disrupting the pool (false submissions, puzzle hijacking)
2. Protecting the admin API from unauthorised use
3. Keeping the server OS and process safe

---

## Admin Route Protection

**Risk:** The four admin endpoints (`/api/v1/admin/*`) can change the active puzzle,
inject test chunks, and read all puzzle state. If left unprotected, any internet user
could redirect the pool to a different puzzle.

**Mitigations (choose one or both):**

### Option A — Nginx IP restriction (recommended for single-operator pools)

The provided `deploy/nginx.conf` restricts admin routes to `127.0.0.1` and any
explicitly listed management IPs:

```nginx
location /api/v1/admin/ {
    allow 127.0.0.1;
    # allow 203.0.113.42;   # your management IP
    deny all;
    proxy_pass http://127.0.0.1:8888;
}
```

### Option B — ADMIN_TOKEN header (suitable for multi-operator setups)

Set the `ADMIN_TOKEN` environment variable. The server will then require:
```
X-Admin-Token: <your-secret-token>
```
on all admin requests. Returns `401 Unauthorized` if the header is absent or wrong.

```bash
# In .env or systemd unit:
ADMIN_TOKEN=$(openssl rand -hex 32)
```

Both options can be combined for defence in depth.

---

## Worker Identity

Workers are identified by name only — there is no registration, password, or token.

**Risk:** A malicious actor can impersonate a worker by using the same name.

**Mitigations built-in:**
- `UPDATE chunks SET status='completed' WHERE id = ? AND worker_name = ?` — a worker
  cannot complete another worker's job (ownership enforced by the WHERE clause).
- A malicious worker can only cause their own chunks to be mis-reported, not other workers'.

**Remaining risk:** A bad actor using another worker's name can report false completions
for that worker's chunks (marking them done without actually scanning them). For a public
Bitcoin puzzle this is an accepted trade-off — the worst outcome is missed coverage of a
small keyspace region.

---

## Input Validation

| Input | Validation |
|-------|-----------|
| `name` | Presence check; no length limit (cosmetic) |
| `start_hex`, `end_hex` | `isValidHex()` — must match `/^(0x)?[0-9a-fA-F]+$/` |
| `job_id` | Used directly in parameterised SQL — no injection risk |
| `found_key` | Validated as a hex string; rejected if malformed |
| `found_address` | Stored as-is; no format validation (by design) |

All database queries use **parameterised statements** (`db.prepare(...).run(params)`).
SQL injection is not possible.

---

## XSS Prevention

The dashboard uses two rendering paths:

**`textContent`** — used for simple scalar updates (stat card values, puzzle name, frontier range). Safe by construction.

**`innerHTML` with `esc()`** — used for table rows and tooltips where HTML structure (e.g. coloured spans, progress bars) is needed. All untrusted fields from the API are passed through `esc()` in `frontend/src/format.ts` before interpolation. `esc()` escapes `& < > " '`.

Untrusted fields (worker-supplied or stored from unauthenticated input):
- Worker `name`, `version`
- Score and finder `worker_name`
- Finder `found_address`
- Chunk `w` (worker name in canvas tooltips)
- Puzzle `name` (admin-set but stored in DB)

Trusted fields interpolated without escaping (server-formatted or enum-bounded):
- Formatted numbers from `formatIntegerDots`, `formatBigInt`, etc.
- CSS variable strings (`var(--accent-cyan)`)
- `ChunkStatus` enum values mapped through `CHUNK_COLORS`
- `alloc_strategy` (mapped to known display names; unknown values escaped via `allocatorFriendlyName`)

No `eval` or `document.write` calls exist.

---

## CORS

No CORS headers are set. The API is same-origin only (served from the same Nginx vhost).
Cross-origin API calls from other domains will be blocked by the browser.

---

## TLS

TLS is terminated at Nginx. The C++ server only listens on `127.0.0.1:8888` and
is never exposed directly to the internet. Certificates are managed by Let's Encrypt /
Certbot with auto-renewal.

---

## Recommendations for Production

- Keep `ADMIN_TOKEN` set and rotate it periodically
- Enable Nginx rate limiting on `/api/v1/work` to prevent resource exhaustion:
  ```nginx
  limit_req_zone $binary_remote_addr zone=pool:10m rate=10r/s;
  location /api/v1/ { limit_req zone=pool burst=20 nodelay; }
  ```
- Run the server process as a non-root user (the provided systemd unit does this)
- Enable `NoNewPrivileges=true` in the systemd unit (already in `deploy/puzzpool.service`)
- Keep system packages (`libboost`, `libsqlite3`) updated for security patches
- Use GitHub's private vulnerability reporting for security issues (see SECURITY.md template)
