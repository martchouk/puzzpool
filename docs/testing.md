# Testing

## CI Branch Coverage Contract

```bash
bash tests/test_ci_branch_coverage.sh
```

The dependency-free checker verifies that `.github/workflows/ci.yml` runs for
pushes to `main` and `dev` and for pull requests targeting either branch. The
test mutates each event independently to prove it fails when `dev` coverage is
removed.

## C++ Unit Tests (Catch2 + CTest)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUZZPOOL_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Each test binary links against the `puzzpool_core` static library (all sources except
`main.cpp`).

`ctest` also runs `tests/test_check_node_version_age.sh`, the unit tests for the
Node.js supply-chain guard (`scripts/check-node-version-age.sh`). It mocks
`node --version` and `curl`, so it makes no network calls. It is registered only when
both `bash` and `node` are found at configure time; otherwise CMake prints a warning
and skips it, so a missing interpreter never masquerades as a pass.

Run that guard's tests on their own with
`ctest --test-dir build -R "^test_check_node_version_age$" --output-on-failure`
rather than invoking the script directly, so a sandbox that denies a bare shell
command can still run it.

`tests/test_admin_routes_smoke.sh` is registered the same way (it needs `bash` and
`curl`). It starts the real `puzzpool` binary on `127.0.0.1:18899` with a temporary
database, so it runs `RUN_SERIAL`. Pass a different port as its second argument if
that one is taken.

### Test coverage

| File | What is verified |
|------|-----------------|
| `tests/test_hex_bigint.cpp` | `isValidHex`, `hexToInt`, `intToHex`, `normalizeHex`, `ceilDiv`, `minBig`/`maxBig`, `bigToDec`, `normalizedRange`, `bitLength` |
| `tests/test_permutation.cpp` | Feistel: determinism, bounds `[0,n)`, 100k-sample injectivity, edge keyspace sizes; Affine: bounded, deterministic, 10k injectivity |
| `tests/test_submission.cpp` | `submitDone` (exact/overscan accepted, underscan rejected+reclaimed, wrong worker, missing fields, negative); `submitFound` (valid, deduplication, empty array, invalid hex); `clearTestChunkIfNeeded` |
| `tests/test_allocator.cpp` | `upsertWorker` (new/fresh), `assignWork` (valid chunk, idempotent, two-worker non-overlap), `reclaimChunk`, `existingAssignedChunk` (found/nullopt), `reclaimTimedOutChunks` (backdated/fresh) |
| `tests/test_hash_utils.cpp` | `keyedDigestHex` golden vectors and the frozen Feistel allocation order (ADR-4/ADR-5); `hmacSha256Hex` against the RFC 4231 vectors; proof that the two helpers are not aliased |
| `tests/test_auth.cpp` | base64url round-trip and rejection; constant-time comparison; fail-closed signing with no secret; session encode/decode, expiry, tampering, wrong key, purpose confusion; allow-list parsing and matching; cookie parsing; CSRF proof; the full admin-guard allow/deny matrix; startup diagnostics |
| `tests/test_auth_routes.cpp` | `/api/v1/auth/*` through `AuthService` with a stubbed `HttpClient`: 503 when unconfigured, redirect and state cookie shape, state mismatch/tampering/expiry/replay, provider transport and payload failures, session cookie attributes, `/auth/me` signed-in and signed-out bodies, logout including its cross-site rejection, and the Crow guard adapter with an explicit evaluation instant |
| `tests/test_http_client.cpp` | `urlEncode` against the RFC 3986 unreserved set, the delimiters that could split a parameter, multi-byte UTF-8, and an injected `&client_secret=`; the response-size cap at and past its boundary, including a chunk large enough to wrap a naive sum; the curl client's protocol restriction and its transport-error path, with no request body echoed into the error |
| `tests/test_log_redaction.cpp` | `redactQueryStrings` — the OAuth callback line, lines with no query string, several per line, one at end-of-line, tab and newline delimiters, and preservation of path and status |
| `tests/test_project_instructions.cpp` | `agent-instructions/PROJECT_INSTRUCTIONS.md` — WorkPackage status/role stage authority and the non-authoritative issue/comment/document sources, the single final issue comment and truthful transition reporting, every named read-only assessment role and clean-status probe restoration, the first push/first-PR publication subjects, every gate prerequisite, and the gate's explicit supersession of `GIT_HYGIENE.md`, checkout-free report publication including adapter unavailability and every forbidden answer to a refusal, both draft-location fallback bans, Developer-owned backend/frontend evidence reported with its 40-hex head, every complete-suite repeat condition, the headless background/detachment/retry prohibitions and foreground durable capture, and the CTest node-age route; every pin has a direct sensitivity proof, reviewer-identified semantic carriers are deleted individually (including each publication subject and both together), and one compatible mutation from every obligation group is replayed all at once |
| `tests/test_check_node_version_age.sh` | `scripts/check-node-version-age.sh` — threshold enforcement, disable switch, network and lookup failures (mocked `node` and `curl`) |
| `tests/test_admin_routes_smoke.sh` | Route wiring against a real running server: all six admin routes denied when nothing is configured and accepted with a valid `X-Admin-Token`, `/api/v1/auth/*` reachable and 503 without a signing secret, cookie attributes on the redirect, logout's same-origin requirement, and no secret, authorization code, or state nonce in the redirect or the server log — with a positive control asserting the callback *is* logged, so the absence checks prove redaction |

### In-memory isolation

All component tests use `:memory:` as the SQLite path (via `memConfig()` in
`tests/test_helpers.hpp`). No files are read or written; tests are fully reproducible
and safe to run in parallel.

### Performance benchmarks

Permutation benchmarks are tagged `[.benchmark]` and skipped by CTest. Run them explicitly:

```bash
./build/tests/test_permutation '[.benchmark]' --benchmark-no-analysis
```

---

## TypeScript Type Check

```bash
npm run build --prefix frontend
```

This runs `tsc --noEmit` (strict type check) followed by `vite build` (bundle to
`public/index.html`). A clean exit means zero type errors and a valid build output.

`public/index.html` is generated and untracked. Always edit files under `frontend/`.

---

## Frontend Unit Tests (Vitest)

```bash
npm test --prefix frontend
```

This runs the Vitest suite (`vitest run`) over `frontend/src/*.test.ts`. All tests must
pass before a frontend change is merged.

### Test coverage

| File | What is verified |
|------|-----------------|
| `frontend/src/format.test.ts` | Number/hex/duration formatting helpers |
| `frontend/src/performance.test.ts` | Performance-sensitive rendering helpers |
| `frontend/src/auth.test.ts` | `/api/v1/auth/me` body normalization, avatar-URL validation, activation-hint and refused-activation-hint text |
| `frontend/src/api.test.ts` | Auth and admin transports against a stubbed network: request shape, status mapping, network errors |
| `frontend/src/accessibility.test.ts` | Accessibility regressions in the built dashboard markup |

### Authentication state tests

`auth.ts` is pure, so `auth.test.ts` exercises the real decision logic rather than
a stand-in: the signed-in and signed-out bodies, an `authenticated: true` body
that cannot name the account, truthy-but-not-`true` flags, a non-object body, the
`503` returned when `SESSION_SIGNING_SECRET` is unset, a `javascript:`/`data:`/
`http:` avatar URL, and a maximum-length GitHub login. The DOM wiring on top of it
is pinned by the source-level assertions in `accessibility.test.ts`.

`refusedActivationHint()` is covered against every state `/api/v1/auth/me` can
report *after* an activation `401`, because a `401` does not imply the session
ended: the allow-list is consulted per request, so an admin removed mid-session
is refused while still authenticated. Each case gets its own wording — sign in
again, name the account that is not allow-listed, or explain the refusal when the
refreshed state looks authorized again. That last branch is why the function is
total: `activationHint()` returns `null` there, which would close the overlay and
say nothing at all.

### Transport tests

`api.ts` depends on the global `fetch` and on no DOM, so `api.test.ts` runs the real
transport code against a stubbed network rather than a wrapper: the request URL,
method, `credentials: 'same-origin'`, and JSON body; the `401` → `unauthorized`
mapping and the `403` that must **not** be treated as an expired session; an
unparsable error body falling back to the status code; and a dropped connection on
each of `/auth/me`, `/auth/logout` and `/admin/activate-puzzle`.

All three transports are required never to reject. That is what keeps a network
failure mid-activation from becoming an unhandled rejection that strands the
confirmation overlay with no message (AC4). Each test awaits the returned promise,
so the guarantee is self-sensitive: removing a `try`/`catch` from `api.ts` makes the
corresponding case fail with the rejection itself rather than pass quietly.

### Accessibility regression tests

`accessibility.test.ts` asserts against `frontend/index.html` to lock in accessible
markup. Covered guarantees include:

- Filter buttons manage `aria-pressed` state and the dashboard logic keeps it in sync.
- Each visualization filter set is exposed as a **named group** in the accessibility
  tree: the Night Sky Heatmap layer filter (`#hm-layer-filter`), the Allocator
  Diagnostics generation filter (`#alloc-generation-filter`), and the Hilbert Curve
  Mapping layer filter (`#hil-layer-filter`) each carry `role="group"` and a unique
  `aria-label` naming the visualization and filter dimension.
- The auth controls: `#auth-signin-btn` has a visible accessible name;
  `#auth-identity` is a `role="group"` with an `aria-label`, holding the decorative
  avatar, the login text, and a labelled `#auth-signout-btn`; the two controls are
  mutually exclusive; **neither carries `aria-pressed`**, which stays reserved for
  the `.alloc-filter-btn` toggle groups.
- The activation hint `#ks-auth-hint` stays an **always-rendered** live region:
  `role="status" aria-live="polite"`, no `hidden` attribute, no `display` rule that
  removes it, and dashboard helpers that swap only its text. A `role="status"`
  element has to be in the accessibility tree before its content changes for the
  change to be announced, and this hint is the only feedback a signed-out visitor
  gets when activation is refused (AC17), so populating it while hidden and
  revealing it afterwards would leave a screen-reader user with silence.
- The header wraps and the auth cluster stacks at the existing `768px` breakpoint,
  where `#stage-label` rejoins the flow so it cannot overlap `h1` or the stacked
  controls, and a long GitHub login ellipsizes instead of clipping the header.
- The admin-`401` branch re-reads `/api/v1/auth/me` **before** wording the hint,
  so the message can never contradict the identity the refresh restores to the
  header. The ordering is only visible in the source, so it is pinned here; the
  resulting text is unit-tested in `auth.test.ts`.
- The removals: no admin-token field in the activation modal, no
  `sessionStorage`/`localStorage` admin-token path, and no `X-Admin-Token` header
  sent from the browser.

Every assertion that something is **absent** is paired with a sensitivity check in
the same test: the identical probe is run over a copy of the real source with the
prohibited attribute, field, or storage call put back, and must report it. An
absence assertion whose probe cannot detect the behaviour proves nothing.

These are source-level checks. They pin markup conventions and do **not** prove
runtime keyboard focus, focus restoration, or focus visibility — that needs a
DOM-capable harness or a browser test, tracked separately.

When adding or renaming a filter group, an auth control, or a header breakpoint
rule in `frontend/index.html`, update these assertions so the accessible-name
contract stays enforced.

---

## Smoke Test (local server)

```bash
./update.sh

./build/bin/puzzpool &
SERVER_PID=$!
sleep 1
curl -sf http://127.0.0.1:8888/api/v1/stats | python3 -m json.tool
kill $SERVER_PID 2>/dev/null
rm -f pool.db
echo '[OK] smoke test passed'
```

---

## Manual API Tests

**Scenario 1 — Normal worker lifecycle**
```bash
BASE_URL=http://127.0.0.1:8888

# 1. Request work
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","hashrate":1000000}' | python3 -m json.tool

# 2. Send heartbeat (keep job alive)
curl -s -X POST $BASE_URL/api/v1/heartbeat \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","job_id":1}' | python3 -m json.tool

# 3. Submit completion (keys_scanned required)
curl -s -X POST $BASE_URL/api/v1/submit \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","job_id":1,"status":"done","keys_scanned":500000000}' | python3 -m json.tool

# 4. Verify stats updated
curl -s $BASE_URL/api/v1/stats | python3 -c "
import sys, json; d = json.load(sys.stdin)
print('completed_chunks:', d['completed_chunks'])
print('total_keys_completed:', d['total_keys_completed'])
"
```

**Scenario 2 — Test chunk verification**
```bash
# 1. Admin: set a test chunk with known keys
curl -s -X POST $BASE_URL/api/v1/admin/set-test-chunk \
  -H 'Content-Type: application/json' \
  -d '{"start_hex":"0x5fffffffffff000000","end_hex":"0x5fffffffffff100000"}' | python3 -m json.tool

# 2. Worker requests work — should receive the test chunk
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker2","hashrate":500000}' | python3 -m json.tool
# Verify start_key matches test chunk start

# 3. Second worker should NOT receive the test chunk
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker3","hashrate":500000}' | python3 -m json.tool
```

**Scenario 3 — Key found**
```bash
curl -s -X POST $BASE_URL/api/v1/submit \
  -H 'Content-Type: application/json' \
  -d '{
    "name":"testworker",
    "job_id":1,
    "status":"FOUND",
    "findings": [
      {
        "found_key":"0000000000000000000000000000000000000000000000000000000000000042",
        "found_address":"1A1zP1eP5QGefi2DMPTfTL5SLmv7Divf"
      }
    ]
  }' | python3 -m json.tool

# Verify it appears in stats
curl -s $BASE_URL/api/v1/stats | python3 -c "
import sys, json; d = json.load(sys.stdin)
print('finders:', d['finders'])
"
```

**Scenario 4 — Admin token**
```bash
# Without a credential — always 401, including when ADMIN_TOKEN is blank
curl -s -X GET $BASE_URL/api/v1/admin/puzzles
# → {"error":"unauthorized"}

# With correct token
curl -s -X GET $BASE_URL/api/v1/admin/puzzles \
  -H "X-Admin-Token: $ADMIN_TOKEN" | python3 -m json.tool
```

**Scenario 5 — Fail-closed admin guard**
```bash
# Start the server with neither mechanism configured.
env -u ADMIN_TOKEN -u ADMIN_GITHUB_USERS ./build/bin/puzzpool
# Startup log names the missing variables:
#   [puzzpool-cpp] WARNING: neither ADMIN_TOKEN nor ADMIN_GITHUB_USERS is configured …

curl -s -o /dev/null -w '%{http_code}\n' $BASE_URL/api/v1/admin/puzzles
# → 401   (previously this returned 200)
```

**Scenario 6 — GitHub sign-in**

Requires `SESSION_SIGNING_SECRET`, `GITHUB_OAUTH_CLIENT_ID`,
`GITHUB_OAUTH_CLIENT_SECRET`, and your login in `ADMIN_GITHUB_USERS`.

```bash
# Fail-closed behaviour with no signing secret
env -u SESSION_SIGNING_SECRET ./build/bin/puzzpool &
curl -s $BASE_URL/api/v1/auth/me
# → {"error":"auth_unavailable","reason":"session_signing_secret_missing"}  (503)

# Signed-out identity with signing configured
curl -s $BASE_URL/api/v1/auth/me
# → {"authenticated":false,"is_admin":false}

# Redirect and state cookie
curl -s -D - -o /dev/null $BASE_URL/api/v1/auth/github/login
# → 302 to github.com/login/oauth/authorize?client_id=…&state=…
# → Set-Cookie: pp_oauth_state=…; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=600

# Complete the flow in a browser, then check the session cookie attributes in
# DevTools → Application → Cookies: pp_session must be HttpOnly + Secure + Lax.
curl -s --cookie "pp_session=$COOKIE" $BASE_URL/api/v1/auth/me
# → {"authenticated":true,"login":"…","avatar_url":"…","is_admin":true}

# CSRF: a cookie-authorized POST without same-origin proof is refused
curl -s -X POST --cookie "pp_session=$COOKIE" $BASE_URL/api/v1/admin/reclaim
# → {"error":"csrf_check_failed"}  (403)

curl -s -X POST --cookie "pp_session=$COOKIE" \
  -H "Sec-Fetch-Site: same-origin" $BASE_URL/api/v1/admin/reclaim
# → 200

# Sign out — logout needs same-origin proof too, because it acts without a cookie
curl -s -o /dev/null -w '%{http_code}\n' -X POST $BASE_URL/api/v1/auth/logout
# → 403   {"error":"csrf_check_failed"}, and the cookie is NOT cleared

curl -s -X POST --cookie "pp_session=$COOKIE" \
  -H "Sec-Fetch-Site: same-origin" $BASE_URL/api/v1/auth/logout
# → {"ok":true}   with Set-Cookie: pp_session=; … Max-Age=0

# Log redaction: the authorization code and state must not reach the log.
# Run the server with its stderr captured, then:
curl -s -o /dev/null "$BASE_URL/api/v1/auth/github/callback?code=SENTINEL&state=SENTINEL2"
grep -c SENTINEL server.log        # → 0
grep -c 'auth/github/callback' server.log   # → ≥1, so the 0 above is redaction,
                                            #   not an empty or unwritten log
```

Never paste a real `SESSION_SIGNING_SECRET`, OAuth client secret, access token, or
session cookie value into a log, issue, or review artifact.

---

## Database Inspection

```bash
sqlite3 pool.db

SELECT status, COUNT(*) FROM chunks GROUP BY status;
SELECT * FROM findings;
SELECT * FROM workers ORDER BY last_seen DESC;
```
