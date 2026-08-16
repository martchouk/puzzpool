# Security

## Threat Model

puzzpool is a **public coordination service** for an open Bitcoin puzzle. The keyspace
and progress are intentionally public. There are no user accounts, no passwords, and no
financial transactions in the application layer. The primary security concerns are:

1. Preventing an attacker from disrupting the pool (false submissions, puzzle hijacking)
2. Protecting the admin API from unauthorised use
3. Authenticating operators individually and revocably
4. Keeping the server OS and process safe

---

## Admin Route Protection

**Risk:** The six admin endpoints (`/api/v1/admin/*`) can change the active puzzle,
inject test chunks, import ranges, force reclaims, and read all puzzle state. If left
unprotected, any internet user could redirect the pool to a different puzzle.

### The guard is default-deny

Every admin route goes through **one** authorization decision
(`authorizeAdminRequest()` in `src/auth.cpp`), wired to the routes by `main.cpp`.
Route handlers never reimplement or combine authentication mechanisms themselves.

If **neither** `ADMIN_TOKEN` nor `ADMIN_GITHUB_USERS` is configured, every admin
route returns `401`. There is no configuration in which a missing or blank setting
makes an admin mutation public.

> #### ⚠️ Breaking change from earlier releases
>
> A blank or unset `ADMIN_TOKEN` used to mean *"authentication disabled"*, and the
> guard let every admin request through. That default was fail-open, and it mattered
> in practice: `deploy/nginx.conf` gives `/api/v1/admin/activate-puzzle` its own
> `location` block **without** an `allow`/`deny` list, deliberately, so the browser
> dashboard can reach it. On a deployment following the old "Option A only"
> recommendation, that one route was therefore reachable by anyone on the internet
> with no credential at all.
>
> Set `ADMIN_TOKEN` and/or `ADMIN_GITHUB_USERS` before upgrading. The server prints
> a warning naming the missing variables at startup.

### Mechanism A — `X-Admin-Token` header

Set `ADMIN_TOKEN`. Admin requests are then authorized by:

```
X-Admin-Token: <your-secret-token>
```

```bash
# In .env or systemd unit:
ADMIN_TOKEN=$(openssl rand -hex 32)
```

The comparison is constant-time. A header credential is not sent ambiently by a
browser, so requests authorized this way need no CSRF proof.

### Mechanism B — GitHub sign-in with an allow-list

Set `ADMIN_GITHUB_USERS` to a comma-separated list of GitHub logins, plus
`SESSION_SIGNING_SECRET` and this stage's OAuth credentials. Matching is
case-insensitive and whitespace-tolerant; an empty list grants nobody access.

The list is re-read from configuration on **every** admin request, so removing a
login revokes access immediately — no cookie invalidation or restart is needed.

Signing in is not the same as being an admin. Any GitHub user can complete the
OAuth flow and receive a session cookie; the cookie grants nothing until the login
appears on the allow-list.

### Mechanism C — Nginx IP restriction (defence in depth)

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

This is a *complement* to the application guard, never a substitute — the
`activate-puzzle` route is intentionally not covered by it. `/api/v1/auth/*` is
public by design and is likewise not IP-restricted.

---

## GitHub OAuth Sign-In

### Flow

1. `GET /api/v1/auth/github/login` mints a 256-bit `state` from `/dev/urandom`,
   binds it to the browser by storing it in a signed, short-lived `HttpOnly`
   `pp_oauth_state` cookie, and redirects to GitHub with **no scopes requested**.
2. `GET /api/v1/auth/github/callback` verifies the returned `state` against that
   cookie in constant time, then clears the cookie — on every outcome, so a `state`
   value can never be replayed.
3. The authorization code is exchanged for an access token, and the token is used
   once to read the public identity (`login`, numeric `id`).
4. A signed session cookie is issued.

### Session cookie

`pp_session` is stateless and carries `Path=/; HttpOnly; Secure; SameSite=Lax` plus
an absolute `Max-Age`. Its value is
`v1.<base64url(payload)>.<base64url(HMAC-SHA-256)>` over a domain-separated signing
input, so a token minted for the OAuth state cannot be replayed as a session and
vice versa. Signature comparison is constant-time. Bad signatures, tampered
payloads, and expired cookies are all rejected.

### Fail-closed signing

`SESSION_SIGNING_SECRET` has **no default**. When it is unset or empty:

- `/api/v1/auth/*` returns `503`;
- no session cookie is issued;
- no presented session cookie is accepted, including one signed earlier under a
  secret that has since been removed;
- attempting to sign a value throws rather than falling back to an empty key.

The signing key must be distinct from `ADMIN_TOKEN` and from any allocator seed.

### CSRF defence

`SameSite=Lax` alone does not cover every cross-site `POST` vector, so a
cookie-authorized admin `POST` must additionally present same-origin evidence:
`Sec-Fetch-Site: same-origin`, or an `Origin` whose authority equals the request's
`Host`. A request carrying neither is rejected with `403`
`{"error":"csrf_check_failed"}`.

`POST /api/v1/auth/logout` carries the same requirement. It is the one
state-changing route that acts *without* needing a cookie, so `SameSite=Lax`
withholds nothing from it and a cross-site top-level form `POST` would reach it.
The impact would be availability only — a forced sign-out — but the guard already
had the primitive, so logout uses it. A rejected logout does not clear the cookie.

### Request logging

GitHub returns the authorization code and the OAuth state nonce in the callback's
**query string**. That is the provider's contract, not a choice this API makes, so
both values have to be removed on the way to the log rather than kept out of the
URL in the first place.

An unredeemed authorization code is a live credential: within its validity window
anyone holding it and the client secret can redeem it, and it is most exposed
precisely on the paths where the exchange failed and it was never consumed. The
state nonce is what binds a flow to one browser, so disclosing it removes that
binding while the `pp_oauth_state` cookie is still live.

Two logs would otherwise record them:

| Log | Why it would record them | Mitigation |
|-----|--------------------------|------------|
| The process log (`stderr` → journald under systemd) | Crow logs the raw request target for every response at its default `Info` level | `main.cpp` installs a `crow::ILogHandler` that runs every line through `redactQueryStrings()` (`src/log_redaction.cpp`) |
| The Nginx access log | The default `combined` format's `$request` contains the query string | `access_log off;` on `location /api/v1/auth/` in `deploy/nginx.conf` |

Redaction is preferred over raising `CROW_LOG_LEVEL`, which would silence request
logging altogether. Everything an operator reads a request log for — method, path,
status, timing — survives; only the run of characters from `?` to the next
whitespace is replaced with `?<redacted>`. The rule is deliberately blunt so that
a parameter name nobody anticipated cannot slip through.

`tests/test_log_redaction.cpp` pins the transform, and
`tests/test_admin_routes_smoke.sh` drives a real callback with sentinel `code` and
`state` values and asserts neither reaches `server.log` — alongside a positive
control asserting the request line *is* still logged, so the check proves
redaction rather than an empty log.

If you need these requests in the Nginx log, define a `log_format` that uses
`$uri` (path only) instead of `$request`; `deploy/nginx.conf` carries a worked
example in the comment above the `access_log` line.

### Secret handling

OAuth client secrets, authorization codes, access tokens, and session cookie values
are never placed in a URL, a shell command, or process arguments. The code exchange
is performed by libcurl through an injectable client seam (`src/http_client.cpp`),
with the secret in the request body over TLS, certificate verification enforced,
redirects disabled, `https` the only permitted protocol, and the response body
capped. Startup diagnostics and error bodies name environment *variables*, never
their values.

Provider calls are made by `AuthService`, which holds no database handle and takes
no lock, so blocking network I/O never runs under the `PoolService` mutex.

### Per-stage OAuth applications

PROD and TEST register **separate** GitHub OAuth applications with separate callback
URLs. Each deployment supplies only its own `GITHUB_OAUTH_CLIENT_ID` and
`GITHUB_OAUTH_CLIENT_SECRET`. The application never loads both stages' credentials
and never selects between secrets with a `Config::stage` branch.

---

## Hashing and Message Authentication

The codebase has two similar-looking helpers in `include/puzzpool/hash_utils.hpp`,
and the distinction is load-bearing:

| Helper | Construction | Use |
|--------|--------------|-----|
| `keyedDigestHex(key, msg)` | `sha256(key ‖ 0x1f ‖ msg)` | Allocator permutation only |
| `hmacSha256Hex(key, msg)` | RFC 2104 HMAC-SHA-256 | All authentication |

`keyedDigestHex` is **not** a MAC. A secret-prefix construction over a
Merkle–Damgård hash is length-extension forgeable, so it must never authenticate a
cookie, token, or any other attacker-influenced value.

Its output is nonetheless frozen: it is the Feistel round function behind
`virtual_random_chunks_v1` (ADR-4), so changing a single output byte would reorder
allocation for every puzzle that has already issued work. `tests/test_hash_utils.cpp`
pins both the digest bytes and the resulting allocation order with golden vectors
captured from the pre-rename tree.

`hmacSha256Hex` is verified against the RFC 4231 test vectors and is the only helper
used for signing.

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

## Node.js Supply-Chain Protection

A compromised or malicious Node.js release can introduce vulnerabilities at build time.
To reduce this risk, a minimum release age is enforced before Node.js is used during the
frontend build step.

### How it works

The script `scripts/check-node-version-age.sh`:
1. Reads the installed Node.js version (`node --version`).
2. Fetches the official Node.js release index from `https://nodejs.org/dist/index.json`.
3. Calculates how many minutes ago the installed version was published.
4. Rejects the build if the version is younger than `MINIMUM_NODE_RELEASE_AGE` minutes
   (default **1440 minutes = 24 hours**), logging the release date and how long until the
   threshold will be met.
5. Exits non-zero so the build/CI pipeline fails visibly rather than silently proceeding.

This matches the supply-chain mitigation commonly known as the **minimum release age**
pattern (also used by Renovate's `minimumReleaseAge` setting).

### Configuration

| Variable | Default | Description |
|----------|---------|-------------|
| `MINIMUM_NODE_RELEASE_AGE` | `1440` | Minimum age in minutes a Node.js release must have before use. Set to `0` to disable (not recommended in production). |

### Recommended values by risk profile

| Environment | Recommended value | Rationale |
|-------------|------------------|-----------|
| Production | `1440` (24 h) | Allows time for community vetting before adoption |
| Staging / QA | `1440` | Mirror production to catch regressions early |
| Local development | `0` (disabled) | Developers may intentionally use cutting-edge releases |
| High-security | `10080` (7 days) | Extended hold for maximum vetting time |

### What to do when a build is rejected

If CI or a local build fails because the Node.js version is too new:
1. Check the log for the release date and the minutes remaining until it passes.
2. Wait until the threshold is met — the version will be accepted automatically.
3. To unblock immediately in a non-production context, set `MINIMUM_NODE_RELEASE_AGE=0` in
   your environment (do **not** use this in production).

---

## Recommendations for Production

- Configure at least one admin mechanism (`ADMIN_TOKEN` or `ADMIN_GITHUB_USERS`);
  the guard denies everything otherwise, and the startup log says so
- Prefer `ADMIN_GITHUB_USERS` for multi-operator pools: access is attributable and
  revocable per person without rotating a shared secret
- Generate `SESSION_SIGNING_SECRET` with `openssl rand -hex 32`, keep it distinct
  from `ADMIN_TOKEN`, and rotate it to invalidate all outstanding sessions
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
