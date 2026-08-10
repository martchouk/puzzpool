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

**Risk:** The six admin endpoints (`/api/v1/admin/*`) can change the active puzzle,
inject test chunks, import ranges, reclaim chunks, and read all puzzle state. If left
unprotected, any internet user could redirect the pool to a different puzzle.

### The guard fails closed

All six routes go through a single authorization guard in `puzzpool_core`
(`src/admin_auth.cpp`, adapted to Crow by `src/admin_guard.cpp`). It is **default-deny**:

> If neither `ADMIN_TOKEN` nor `ADMIN_GITHUB_USERS` is configured, every admin route
> returns `401`. A blank configuration closes admin access; it does not open it.

This is a deliberate change from earlier releases, which treated a blank `ADMIN_TOKEN`
as "authentication disabled" and served admin routes to anyone who could reach the
port. The server prints a startup warning naming both variables when neither is set.

Two mechanisms can authorize a request; either is sufficient, and route handlers never
reimplement or combine them.

### Mechanism 1 — `X-Admin-Token` header

Set `ADMIN_TOKEN`; admin requests then carry `X-Admin-Token: <value>`.

```bash
# In .env or the systemd unit:
ADMIN_TOKEN=$(openssl rand -hex 32)
```

The comparison is constant-time (`constantTimeEquals`), so a wrong token leaks no
information about how much of it was right. Token-authorized requests are **not**
cookie-authorized and are therefore exempt from the CSRF check below: no browser
attaches this header automatically, so it cannot be driven cross-site. This remains
the right mechanism for scripts, automation, and local development.

### Mechanism 2 — GitHub sign-in

A user signs in through GitHub OAuth and receives a signed session cookie. Requires
`SESSION_SIGNING_SECRET`, `GITHUB_OAUTH_CLIENT_ID`, `GITHUB_OAUTH_CLIENT_SECRET`, and
`PUBLIC_BASE_URL`.

**Authorization is the allow-list, not the sign-in.** `ADMIN_GITHUB_USERS` is matched
case-insensitively against the login in the cookie, re-read from configuration on
*every* request and never cached.

The session cookie (`pp_session`):

| Attribute | Why |
|-----------|-----|
| `HttpOnly` | JavaScript cannot read it, so an XSS bug cannot exfiltrate the session |
| `Secure` | never sent over plain HTTP — which is why sign-in requires HTTPS |
| `SameSite=Lax` | the browser will not attach it to cross-site `POST`s |
| `Path=/` | matched by the clearing header that logout emits |
| signed with HMAC-SHA-256 | the payload cannot be forged or edited |
| absolute expiry inside the signed payload | expiry cannot be extended by the client |

The OAuth `state` is a signed, expiring, single-use blob held in a separate
`pp_oauth_state` cookie (`Path=/api/v1/auth`), with only the bare nonce in the redirect
URL. The callback compares the two, so the sign-in cannot be driven by a third party.
State and session blobs share one format and one key, so the token's purpose is part
of the signed input — presenting one where the other is expected fails as
`WrongPurpose`.

### CSRF defence for cookie-authorized `POST`s

`SameSite=Lax` already blocks the cross-site case; this is the second layer. A
cookie-authorized `POST` must carry affirmative proof of same-origin: either
`Sec-Fetch-Site: same-origin` or `none`, or — when `Sec-Fetch-Site` is absent — an
`Origin` matching `PUBLIC_BASE_URL`. Otherwise it is rejected with `403
csrf_check_failed`.

**Absence of both signals is rejected**, because a defence that a client can disable
by omitting a header is not a defence. A `Sec-Fetch-Site` that is present and is
neither `same-origin` nor `none` is definitive and is *not* overridable by `Origin`:
browsers set the two consistently, so allowing the override would only weaken the rule
for a request that already declared itself cross-site.

### Revoking access

To revoke an admin's access, remove their login from `ADMIN_GITHUB_USERS` and restart
the service. The change applies to the very next request: no authorization decision is
cached, so there is no cookie to reissue, no session store to purge, and no wait for
outstanding sessions to expire. **The restart is required because configuration is read
once at startup** (`src/main.cpp` calls `loadConfigFromEnv()` exactly once per process).

Rotating `SESSION_SIGNING_SECRET` invalidates every issued session at once and likewise
takes effect at the next restart. An individual session cannot be revoked before its
absolute expiry — that is the accepted trade-off of stateless sessions (ADR-6), and it
is why `SESSION_TTL_MINUTES` is capped at 30 days.

### Accepted properties, recorded rather than left to be discovered

- **Any GitHub account can obtain a valid `pp_session` cookie** from a public
  deployment. The callback issues the cookie before any allow-list check; only the
  admin guard and `/api/v1/auth/me`'s `is_admin` consult the list. This is deliberate —
  the dashboard needs to distinguish "signed in" from "signed in and allowed to act" —
  but it means the signed-cookie surface is open to the internet, not to admins.
  *Authorization, not authentication, is what protects the admin routes.*
- **Each `/login` + `/callback` pair costs one synchronous outbound HTTPS call to
  GitHub** from a Crow worker thread with a 10-second timeout. An anonymous client can
  drive that loop with a garbage `code`: the state check passes, because they obtained
  a real state cookie from `/login`, and the exchange runs before GitHub rejects it.
  With enough concurrency this parks worker threads on a network wait. A bounding
  `limit_req` zone belongs with the deployment configuration.

### Defence in depth — Nginx IP restriction

The provided `deploy/nginx.conf` additionally restricts admin routes to `127.0.0.1` and
any explicitly listed management IPs:

```nginx
location /api/v1/admin/ {
    allow 127.0.0.1;
    # allow 203.0.113.42;   # your management IP
    deny all;
    proxy_pass http://127.0.0.1:8888;
}
```

This is a **complement to** the server-side guard, not a substitute for it: it is not
applied uniformly. `deploy/nginx.conf` deliberately publishes
`/api/v1/admin/activate-puzzle` to the internet, so that route's only protection is the
guard described above.

`/api/v1/auth/*` is intentionally public — those routes are how an anonymous browser
signs in — and the deployment must keep `X-Forwarded-Proto` set, because it terminates
TLS in front of a `Secure` cookie.

---

## Cryptographic Primitives

`src/hash_utils.cpp` provides three distinct functions, and the distinction is
load-bearing:

| Function | What it is | Use |
|----------|-----------|-----|
| `sha256Hex` | plain SHA-256, hex | general hashing |
| `keyedDigestHex` | `sha256(key ‖ 0x1f ‖ msg)` | **the allocator's Feistel round function only** |
| `hmacSha256Hex` | RFC 2104 HMAC-SHA-256 | session cookies and OAuth state |

**`keyedDigestHex` is not a MAC.** It is a secret-prefix construction and is vulnerable
to length extension. It carries that name — rather than the `hmacSha256Hex` name it
used to occupy — precisely so it cannot be mistaken for one. Its output bytes are
frozen: changing them would reorder allocation for every existing puzzle (ADR-4), so
`tests/test_hash_utils.cpp` pins it to a literal captured before the rename and
`tests/test_permutation.cpp` pins the resulting permutation to a golden vector.

Authentication uses the real HMAC, validated against the RFC 4231 test vectors, with a
signing key (`SESSION_SIGNING_SECRET`) distinct from every other secret.

**Missing signing configuration fails closed.** When `SESSION_SIGNING_SECRET` is unset
or empty, `/api/v1/auth/*` returns `503` and no session cookie is issued or accepted.
There is no built-in or default fallback key. `POST /api/v1/auth/logout` is the one
exception: it always clears the cookie, so a stale session can still be shed after the
secret has been rotated away.

### Handling OAuth secrets

Client secrets, authorization codes, and access tokens travel in libcurl request bodies
and headers — never in a URL, a shell command, or a process argument. The GitHub client
(`src/github_client.cpp`) never enables `CURLOPT_VERBOSE` and logs no URL, header, or
body, because a verbose handle would print the bearer token into the service log. TLS
peer and host verification are on, redirects are off, and response bodies are bounded.
Authorization failures return fixed non-secret codes and never echo the presented
credential.

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
