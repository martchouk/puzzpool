# Code review — issue #162 / PR #163

GitHub OAuth backend and fail-closed admin authorization (story #149, delivery slice A)

| | |
|---|---|
| Reviewer | `mud-rev` (role: reviewer) |
| Issue | [#162](https://github.com/martchouk/puzzpool/issues/162) |
| Pull request | [#163](https://github.com/martchouk/puzzpool/pull/163) — open, mergeable, `Closes #162` |
| Branch | `feature/162-github-oauth-backend` → `dev` |
| Base revision | `946634c96908d1a4f8ea5df591a67e8c558a5e75` |
| Head / candidate revision | `0c4efa5567f8a38ff192fbcc1b02d280b1818fa1` |
| Diff | 29 files, +3396 / −63 |
| `sha256(git diff --binary <base> HEAD --)` | `88e2b19bb2758a970acade1fabc2374852866f8f80cc30ae62c1f7c2a2cae512` |
| Pinned project policy | `puzzpool` @ `946634c9`, profile `standard`, binding digest `4be834ab7872aa6b64d2a319631955e0996a03d3d202df6e8b25d3b34c630d91` |
| Companion PRs | none |

I reviewed this blind: I read the diff, built it, and ran the suites before reading the
implementation report, then reconciled the two. The report's factual claims that I could
check independently all held, including the diff hash above, which I recomputed myself.

---

## Verdict — APPROVE

No blocking finding. The slice-A scope is implemented, correct, secure, documented, and
covered by tests that would fail if the behaviour regressed. One important non-blocking
finding (`SF-1`) is a dated failure in the test suite that I reproduced experimentally;
it does not affect production behaviour or today's coverage.

---

## Findings

### MUST FIX

None.

### SHOULD FIX

**`SF-1` — `tests/test_auth_routes.cpp:588` will start failing on 2027-01-15T09:00:00Z.**

`adminGuard()` (`src/auth_service.cpp:52-54`) reads the real system clock; it has no
injectable `Clock`, unlike `AuthService`. The test at `tests/test_auth_routes.cpp:588`
builds its session cookie through `issuedSessionCookie()`, which uses
`fixedClock(kNow)` with `kNow = 1'800'000'000` (2027-01-15T08:00:00Z) and
`sessionTtlMinutes = 60`. The cookie therefore expires at `1'800'003'600` =
**2027-01-15T09:00:00Z**. Until that instant the cookie is unexpired against wall-clock
time and the test passes; after it, `decodeSessionToken` returns `Expired`, the guard
returns `401 unauthorized` instead of `403 csrf_check_failed`, and the whole `ctest`
suite goes red for reasons unrelated to any change.

I confirmed this rather than inferring it: setting `kNow = 1'700'000'000` locally and
rerunning produced exactly that failure and no other —

```
tests/test_auth_routes.cpp:598: FAILED: CHECK( response->code == 403 )
with expansion: 401 (0x191) == 403 (0x193)
tests/test_auth_routes.cpp:599: FAILED: ... "unauthorized" == "csrf_check_failed"
tests/test_auth_routes.cpp:606: FAILED: CHECK_FALSE( adminGuard(cfg, proven).has_value() )
test cases: 20 | 19 passed | 1 failed
```

I reverted that local edit; the reviewed tree is unmodified.

*Remedy (either is sufficient):* give `adminGuard` an optional `nowUnix` parameter
defaulting to the system clock, and pass `kNow` from this test; or mint the cookie in
this one test with an expiry derived from `std::chrono::system_clock::now()` rather than
from `kNow`. The first is preferable — it removes the last wall-clock dependency from
the auth tests.

*Why this is not blocking:* the behaviour under test is separately and deterministically
covered by `tests/test_auth.cpp:466-502`, which drives `authorizeAdminRequest()` directly
with an explicit `nowUnix` and asserts the same 403/CSRF contract plus its same-origin
sensitivity twin. Coverage of the changed path is adequate today; what rots is one
adapter-level assertion, on a known date, in a way that is loud rather than silent.

### Optional

- **`OP-1` — `POST /api/v1/auth/logout` requires no same-origin proof**
  (`src/auth_service.cpp:225-232`). A cross-site page can force-log-out a signed-in
  operator. The impact is nuisance-level (no state changes beyond clearing the client's
  own cookie), and AC14 scopes the CSRF requirement to admin `POST`s, so this is out of
  the letter of the spec. Applying `hasSameSiteProof` here anyway would cost two lines.
- **`OP-2` — OAuth `state` is single-use by browser cooperation, not server enforcement**
  (`src/auth_service.cpp:124-130`). The callback clears the state cookie on every path,
  which is the right stateless design, but a client that ignores `Set-Cookie` can present
  the same signed state twice within its 600 s TTL. The authorization code itself is
  single-use at GitHub, so there is no exploitable replay. `docs/security.md` describes
  the mechanism accurately; only the word "single-use" in the report is slightly stronger
  than the mechanism.
- **`OP-3` — `tests/test_admin_routes_smoke.sh` binds a fixed port (18899).** `RUN_SERIAL`
  serialises it within one `ctest` run but not against anything else on the host; a
  collision fails the readiness loop with a startup error rather than a clear diagnostic.
  A `PORT=0`-style probe or a retry across a small port range would make it robust.
- **`OP-4` — startup diagnostic wording.** With `ADMIN_TOKEN` blank, `ADMIN_GITHUB_USERS`
  set, and `SESSION_SIGNING_SECRET` unset, every admin route returns 401 but the message
  that literally says "401" is suppressed, because `startupAuthDiagnostics`
  (`src/auth.cpp:392`) gates it on `adminGithubUsers.empty()`. The third message does
  state the allow-list cannot authorize anyone, so the operator is not misled — this is a
  wording refinement, not a gap.

---

## Verification I ran myself

All on candidate revision `0c4efa5`, from a fresh submodule checkout and an empty CMake
cache.

| Command | Result |
|---|---|
| `git submodule update --init --recursive` | exit 0 |
| `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUZZPOOL_BUILD_TESTS=ON` | exit 0, no skip warnings |
| `cmake --build build --parallel 8` | exit 0, no compiler warnings |
| `ctest --test-dir build --output-on-failure` | exit 0 — **14/14 passed**, 8.10 s |
| `npm ci --prefix frontend` | exit 0 |
| `npm test --prefix frontend` | exit 0 — **40/40 passed** (3 files) |
| `npm run build --prefix frontend` | exit 0 — `tsc --noEmit` clean, Vite build clean |
| `git diff --binary <base> HEAD --` → SHA-256 | matches the declared `88e2b19b…` |

The **Node.js release-age guard genuinely executed**: `test_check_node_version_age` is
CTest test 13 and passed in 1.90 s. The CMake configure emitted no "Skipping
test_check_node_version_age" warning, which is the signal that would have distinguished a
skipped test from a passing one. `test_admin_routes_smoke` is test 14 and passed. Neither
is declared as an unexecuted gap, satisfying the issue's explicit requirement.

`git status` is clean apart from the untracked runtime directory: the Vite build did not
dirty the tree, confirming `public/index.html` stays generated and untracked.

### Changed test files reviewed

| File | Assessment |
|---|---|
| `tests/test_auth.cpp` (+558) | The core of the coverage. Reviewed every case. |
| `tests/test_auth_routes.cpp` (+629) | Reviewed every case; carries `SF-1`. |
| `tests/test_hash_utils.cpp` (+118) | Golden vectors + RFC 4231 + anti-aliasing guard. |
| `tests/test_config.cpp` (+73) | Configuration shape and fail-closed defaults. |
| `tests/test_admin_routes_smoke.sh` (+227, new) | Covers `main.cpp` wiring, which no unit test can reach. |
| `tests/CMakeLists.txt` (+51) | Registration; correctly warns instead of silently skipping when an interpreter is absent. |

### Coverage and test sensitivity

Covered, with a failing-direction probe present for each absence/guard assertion:

- signature verify / tamper / wrong key / purpose confusion — and the rejected OAuth-state
  token *does* verify under its own purpose (`test_auth.cpp:240-241`), so the rejection is
  domain separation and not a broken token;
- expiry — rejected at exactly `exp`, accepted at `exp − 1` (`test_auth.cpp:167-170`);
- CSRF — the identical cookie POST is authorized once `Sec-Fetch-Site: same-origin` or a
  matching `Origin`/`Host` is added (`test_auth.cpp:486-495`);
- state mismatch / tamper / expiry / absent cookie — and the matching pair *does* reach the
  provider with exactly two outbound calls (`test_auth_routes.cpp:248-253`);
- twelve provider-failure scenarios (transport, 5xx, `error` body, missing/blank
  `access_token`, non-JSON, and five identity-shape failures), each asserting 502, no
  secret in the body, and no session cookie issued;
- signed-out `/auth/me` — five bad-cookie forms produce a byte-identical body, paired with
  a valid cookie that authenticates (`test_auth_routes.cpp:560-564`);
- blank allow-list — paired with a real entry in the same slot (`test_config.cpp`);
- `keyedDigestHex` vs `hmacSha256Hex` — asserted *unequal*, so a future "cleanup" that
  aliases them fails the build (`test_hash_utils.cpp:99-103`).

I found no vacuous assertion, no test that cannot fail, and no disabled or skipped test.

Uncovered, and correctly disclosed rather than papered over:

- the live GitHub provider is never contacted (stub seam only) — a real end-to-end sign-in
  needs a registered OAuth app and belongs to slice C;
- browser cookie enforcement (`Secure` over TLS, `SameSite=Lax` on the callback
  navigation) is asserted at the header level, not observed in a browser;
- no GitHub Actions check runs on PRs into `dev` (`.github/workflows/ci.yml` triggers on
  `main`); I confirmed `statusCheckRollup` is empty on PR #163. Filed as a `github_checks`
  gap by the developer; I re-ran the equivalent commands locally, above. Widening the
  trigger is out of this issue's scope, but it is worth a follow-up issue.

---

## Correctness assessment

I read `src/auth.cpp`, `src/auth_service.cpp`, `src/http_client.cpp`, `src/hash_utils.cpp`,
`src/config.cpp` and `src/main.cpp` line by line.

- **`hmacSha256Raw`** is a correct RFC 2104 construction — long keys hashed first, short
  keys zero-padded to the 64-byte block, `ipad`/`opad` applied per byte. It reproduces five
  RFC 4231 vectors including both over-length-key cases.
- **`base64UrlEncode`/`Decode`** round-trip all 256 byte values, emit no padding, and reject
  impossible lengths, standard-alphabet characters, padding, and non-zero trailing bits.
- **`verifySignedValue`** checks the MAC *before* parsing the payload, so no attacker-shaped
  JSON reaches the parser unauthenticated, and the comparison is `constantTimeEquals`.
  Domain separation is baked into the signing input
  (`purpose ‖ "." ‖ version ‖ "." ‖ payload`), which is why an OAuth-state token cannot be
  replayed as a session.
- **`authorizeAdminRequest`** is genuinely default-deny. `tokenConfigured` and
  `cookieConfigured` are computed first, and `cookieConfigured` correctly requires *both* a
  signing secret and a non-empty allow-list. A wrong `X-Admin-Token` falls through to the
  cookie mechanism rather than short-circuiting. There is no path on which an unset or
  blank setting yields `authorized = true`.
- **CSRF scope is right.** Five of the six admin routes are `POST` and get the same-origin
  requirement; the sixth, `GET /api/v1/admin/puzzles`, is read-only and returns JSON with
  no CORS headers, so a cross-site read cannot observe the body. The `Origin`-vs-`Host`
  fallback is sound: a cross-site attacker controls neither header, and `deploy/nginx.conf`
  forwards `Host $host`, so the comparison holds behind the proxy.
- **Frozen allocator bytes.** `src/permutation.cpp` differs from base by one identifier and
  a comment. The golden 16-element allocation order and the three `keyedDigestHex` literals
  pin the observable behaviour, not just the formula, so ADR-4 determinism is preserved.
- **AC7 re-evaluation is real**: `main.cpp:100` captures `cfg` by reference and the guard
  reads `cfg.adminGithubUsers` per call; nothing is memoised.

## Security assessment

- **Fail-closed signing** — `signValue` throws rather than signing with an empty key; every
  `/api/v1/auth/*` handler returns 503 as its first statement when the secret is absent;
  a token minted under a since-removed secret is rejected as `NotConfigured`. No default or
  predictable fallback key exists anywhere in the diff.
- **Secret handling** — the client secret and code travel in the libcurl request body; I
  verified no secret reaches a URL, `argv`, or a shell. `curl_easy_strerror` is the only
  transport text surfaced and cannot echo a body. Error bodies and startup diagnostics name
  variables, never values, and the tests assert exactly that with deliberately conspicuous
  placeholder values. Nothing secret is committed.
- **HTTP client hardening** — TLS peer and host verification on, redirects off, `https` the
  only permitted protocol (with a correct pre-7.85 fallback), a 256 KB response cap that
  aborts the transfer, 15 s connect and total timeouts, `CURLOPT_NOSIGNAL`, and
  `CURLOPT_POSTFIELDSIZE` set before `COPYPOSTFIELDS` so a body with an embedded NUL is not
  truncated by `strlen`. `curl_global_init` is guarded by `std::call_once`, which matters
  because these run on Crow worker threads.
- **Randomness** — 256 bits from `/dev/urandom`, throwing rather than degrading if the
  source is unavailable.
- **Session model** — `HttpOnly; Secure; SameSite=Lax; Path=/` with an absolute `Max-Age`
  and a signed absolute `exp`. Revocation is via the allow-list, re-read per request; the
  inability to revoke one cookie early is recorded in ADR-6.
- **Nginx** — `/api/v1/auth/` is a new public prefix location. Nginx prefix matching is
  longest-match, not order-dependent, so it does not shadow or get shadowed by
  `/api/v1/admin/`. The admin IP restriction stays as defence in depth, and the docs are
  candid that `activate-puzzle` is deliberately outside it — which is exactly why the old
  fail-open guard was a live hole.
- **Breaking change is disclosed, not buried.** A blank `ADMIN_TOKEN` now denies instead of
  allowing. `README.md` and `docs/security.md` both carry an upgrade warning that names the
  previously-exposed route, and the server warns at startup. This is the right call and the
  right disclosure.

## Persistence, concurrency and performance

No schema change, no migration, no SQL, no `PoolDb` touch — correctly, since nothing in
this slice needs persistence. `AuthService` holds no database handle and takes no lock, so
its blocking libcurl calls never run under the `PoolService` mutex; its members are a const
`Config` copy and two `std::function`s, none mutated per request, so concurrent handler
invocation is safe. The admin guard adds one HMAC and a short header scan per admin
request — negligible, and off the worker hot path entirely.

## Documentation status

| Surface | Status |
|---|---|
| `docs/api.md` | **Complete.** Full Auth API section, the two-mechanism admin table, CSRF rule, and every status/body pair. I checked each documented code against the handler; all match. |
| `docs/security.md` | **Complete.** Default-deny guard, breaking-change notice, OAuth flow, cookie, fail-closed signing, CSRF, secret handling, per-stage apps, and the `keyedDigestHex` vs `hmacSha256Hex` table. |
| `docs/architecture.md` | **Complete.** Auth flow diagram, new files, dependency direction, concurrency note, libcurl in the stack table. |
| `docs/architecture-review.md` | **Complete.** ADR-5 and ADR-6 state context, decision, and consequences honestly, including the trade-offs. |
| `docs/testing.md` | **Complete.** New suites, both CTest-registered shell tests, manual scenarios. |
| `README.md` | **Complete.** Upgrade warning, env table, `libcurl` prerequisite, corrected `ctest` invocation. |
| `.env.example` | **Complete.** The "leave blank to disable auth" guidance is gone and replaced with the fail-closed statement; new variables documented with no values. |
| `deploy/nginx.conf`, `deps.txt`, `.github/workflows/ci.yml` | **Complete** and consistent with the code. |
| `docs/database.md` | Correctly untouched — no schema change. |

## Acceptance criteria

AC1–AC14 and AC26 are each implemented with direct coverage; I verified the code-to-test
mapping independently rather than accepting the report's matrix. AC20–AC24 hold where this
slice touches configuration, routes, secrets and documentation — in particular AC20: no
credential is selected by `Config::stage` anywhere in the diff. AC25 holds: both suites
pass. AC15–AC19 and AC27 are slice B and correctly absent — `frontend/` is untouched, so
slice A is independently shippable with the dashboard still on `X-Admin-Token`.

## Prior findings

None. This is the first review of PR #163; the typed handoff carries no reviewer findings
to resolve. Attempts 240/241 were rejected at evidence handoff, not on code.

## Typed exceptions

None applied. No policy exception was requested or needed.

## Positive observations

- The `keyedDigestHex` rename is handled with unusual care: the frozen bytes are pinned by
  golden literals *captured from the pre-rename tree*, and the resulting 16-element
  allocation order is pinned separately, so the guard protects the observable behaviour
  rather than the helper's implementation.
- `hmacSha256Hex("key","message") != keyedDigestHex("key","message")` is a genuinely good
  test: it makes a plausible future "these two look redundant" cleanup fail loudly.
- The smoke test enumerates the admin surface as data, so a seventh admin route added
  without the guard shows up as a 200 where a 401 was expected.
- The report's Known Limitations section is accurate and does not overclaim; the CI gap is
  filed as a gap rather than glossed.

## Required next steps

1. Address `SF-1` before it fires (any time in the next five months; a one-line parameter
   on `adminGuard` is the clean fix).
2. Consider `OP-1` through `OP-4` at the team's discretion — none gates this merge.
3. Open a follow-up issue to run CI on pull requests targeting `dev`, so future PRs carry a
   remote check instead of a locally-attested one.

## Transition

`status:po-approval`. The slice-A scope is complete, correct and verified on this exact
revision; no blocking finding remains, so `status:in-development` would send finished work
backwards, and `status:blocked` does not apply — the one thing unobtainable here, a remote
CI check, is a repository configuration fact, not an obstacle.
