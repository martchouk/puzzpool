# Slice A — GitHub OAuth Backend and Fail-Closed Admin Authorization

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Issue:** #153 (slice A of story #149) · **Base:** `f1db2bf` on `dev` · **Branch:** `feature/153-github-oauth-backend`

**Goal:** Ship the independently shippable backend authentication core — a real HMAC-SHA-256 primitive, a signed stateless session cookie, GitHub OAuth over an injectable libcurl seam, a case-insensitive `ADMIN_GITHUB_USERS` allow-list, and a central admin guard extracted into `puzzpool_core` that fails closed — while `X-Admin-Token` keeps working and the existing dashboard is untouched.

**Architecture:** Auth is layered so that every decision is testable without Crow and without a network. Three crow-free domain units (`hash_utils`, `session`, `admin_auth`) hold the primitives, the cookie format, and the authorization decision. Two thin Crow adapters (`admin_guard`, `AuthService`) translate `crow::request`/`crow::response` and own no decisions. `GitHubHttpClient` is a struct of `std::function`s — the libcurl implementation lives behind it, tests inject a fake — matching the existing `PoolService::AddressStatusFetcher` seam precedent. `AuthService` is deliberately a sibling of `PoolService`, not a member: it touches no database and no service mutex, so folding it into `PoolService` would widen that class's lock scope and dependencies for nothing.

New dependency direction (no cycles, extends `docs/architecture.md:72-73`):
`main → {service, auth_service, admin_guard} → {admin_auth, session, github_client} → {hash_utils, base64, secure_random, config} → env`

**Tech Stack:** C++20, Crow, libcurl (new), Boost Multiprecision, nlohmann/json, SQLiteCpp, Catch2, CTest

---

## Scope correction after final PO handoff (artifact 105)

The final PO handoff for #153 (attempt 230, artifact 105, SHA-256
`160b27565fe3ab0e275df27e5e5381e070110fdb32b443a72bd1726ee12eb70a`)
identified one unnecessary risk in the approved plan: Task 1 extracted the
existing `sha256Hex` formatter into a shared public `toHex` helper. Slice A does
not require that refactor, and a padding error there would change every Feistel
round digest and reorder allocation for every existing puzzle.

This revision supersedes only those Task 1 mechanics:

- the old `hmacSha256Hex` receives the mechanical name `keyedDigestHex`, with
  its `sha256Hex(key + "\x1f" + msg)` bytes unchanged;
- the existing `sha256Hex` implementation and its formatter remain byte-for-byte
  untouched;
- the real HMAC uses a new file-private raw SHA-256 helper and a separate
  file-private hex formatter;
- Step 0 still commits pre-change keyed-digest and permutation goldens before
  the rename, and Step 7a proves those controls fail by changing the keyed
  digest delimiter temporarily.

The prior Task 1 Step 1 `toHex` tests, Step 4 public `sha256Raw`/`toHex`
declarations, Step 5 `sha256Hex` refactor, and Step 7a padding mutation are
superseded. The accepted M2–M3, S1–S7, and optional review corrections remain
unchanged.

This correction must be reviewed and merged into PR #154 before Task 1 begins;
no security-sensitive source implementation may proceed against the superseded
plan.

---

## Plan review round 1 — findings resolved

Review of `2e1aa20` requested changes with three blocking findings and four important ones. Every finding is accepted; none is rejected. Artifact 105 later supersedes only the risky shared-formatter mechanics originally chosen for M1, while preserving the finding's pre-change golden-vector requirement. Each finding is resolved at the location named below, and the reviewer's IDs are preserved.

| ID | Finding | Disposition | Where resolved |
|----|---------|-------------|----------------|
| **M1** | AC4's frozen-digest assertion computes its expected value from `sha256Hex`, which the earlier plan refactored, and `tests/test_permutation.cpp` has no cross-version golden vector — so a formatting regression could reorder every puzzle's allocation with a green suite | **Accepted; implementation remedy narrowed by artifact 105** | Task 1 Step 0 (capture and commit goldens from the pre-change build **before** editing), Step 1 (literal frozen `keyedDigestHex` and published `sha256Hex` vectors), Step 5 (leave `sha256Hex` byte-for-byte untouched), Step 5a (golden `permuteIndexFeistel` vector), Step 7a (direct keyed-digest sensitivity mutation); matrix row AC4 |
| **M2** | Nothing proves all six admin routes are guarded — the AC8 matrix row was vacuous, `main.cpp` is outside `puzzpool_core`, and the smoke test covered one route | **Accepted** | Task 4 Step 6 (all six routes enumerated with their exact methods), Task 4 Step 6a (new committed regression script `tests/test_admin_routes_guarded.sh` looping over all six, denied-without-auth and admitted-with-token), Task 7 Step 7 (documented in `docs/testing.md`), Task 8 Step 4; matrix row AC8 |
| **M3** | ADR-6 and `docs/security.md` were specified to tell operators that allow-list removal revokes access with no restart, but `src/main.cpp:16` loads `Config` once per process | **Accepted** | Task 2 Step 5 note, Task 4 Step 1 test comment, Task 7 Step 4, ADR-6 in Task 7 Step 6 — all restated as *remove the login, then restart; the change applies to the next request and needs no cookie reissue and no session-store purge* |
| **S1** | `pp_session` and `pp_oauth_state` share one format and one key, so either verifies as the other | **Accepted** | D-A3 (purpose inside the signed input), Task 3 Steps 1/4 (`TokenPurpose`, `SessionError::WrongPurpose`, one cross-type rejection test per direction) |
| **S2** | `crow::response::get_header_value` is non-const, so several ready-to-paste snippets do not compile; and it returns only the first match, which is wrong for the two-`Set-Cookie` callback | **Accepted** | Task 6 Step 2 (`const` dropped from every response local; `setCookieNamed` spelled out as an `equal_range` scan over `r.headers`) |
| **S3** | The state-cookie clearing assertion passes even when `Path` does not match the issuing header, so the clear would silently not clear | **Accepted** | Task 6 Step 2 (clearing assertions check `Path` and `HttpOnly`), Task 6 Step 4 (`cookieHeader()` helper emits issuing and clearing headers from one attribute set) |
| **S4** | The callback is an unauthenticated, network-egress-triggering, unbounded route, and any GitHub account can obtain a `pp_session` — both correct, both undocumented | **Accepted** | Task 7 Step 4 — both stated in `docs/security.md` as accepted decisions, with the `limit_req` note handed to slice C |
| **O1** | `"\x00\xff\x10binary"` is ill-formed — `\x10b` is one maximal-munch escape | **Accepted** | Task 3 Step 1 — adjacent string literals |
| **O2** | `hasSameOriginProof` let an `Origin` override an explicit `Sec-Fetch-Site: cross-site` | **Accepted** | Task 4 Step 4 — an explicit non-same-origin `Sec-Fetch-Site` is now a definitive rejection |
| **O3** | `docs/security.md:16-17` says "four admin endpoints"; there are six | **Accepted** | Task 7 Step 4 |
| **O4** | `sessionTtlMinutes` was clamped below but not above | **Accepted** | Task 2 Step 4 — clamped to `[1, 43200]` (30 days) |
| **O5** | `handleLogout` returning 503 contradicts "a stale cookie can always be shed" | **Accepted** | Task 6 Step 4 — the clearing header is emitted unconditionally |

Two of these change what the implementer does before writing any code: **M1's Step 0 must run against the unmodified tree**, and **M2 adds a committed script rather than a checklist item**. Both are sequenced explicitly below.

## Plan review round 2 — findings resolved

Re-review of `d60184a` approved the plan for implementation and raised three further findings, none blocking. All three are accepted; each was independently re-derived against the tree at `f1db2bf` before being applied.

| ID | Finding | Verified how | Where resolved |
|----|---------|--------------|----------------|
| **S5** | The login test's state-binding assertion cannot pass: the cookie carries `base64UrlEncode(nonce)` while the URL carries the bare nonce | Task 6 Step 4 mints the nonce as `base64UrlEncode(nonce_(32))` and puts the bare value in the URL; `signingInput` (Task 3 Step 4) encodes the subject a second time. Base64 of ASCII does not contain its input as a substring, and the base64url alphabet is URL-safe so percent-encoding changes nothing | Task 6 Step 2 — replaced with a `verifyStateToken`-based subject comparison, **not** deleted: it is AC1's only login-side proof of browser binding |
| **S6** | The six-route guard script is registered in neither CTest nor CI | `.github/workflows/ci.yml:58` already runs `bash tests/test_check_node_version_age.sh`, so the precedent exists; the script appears in no workflow step | Task 7 Step 7a — added to the `build` job; companion issue #155 / PR #157 supplies `dev` trigger coverage and must be present in the eventual `dev` base |
| **S7** | Phase 2 of the guard script passes vacuously on a dead server, because a `curl` connection error yields no status, which is "not 401" | Phase 2's assertion is "not 401"; the empty string satisfies it. Phase 1 is unaffected — it requires the literal `401` | Task 4 Step 6a — explicit transport-error and empty-status failures, plus a readiness poll on the unauthenticated `/api/v1/stats` |

The five optional items from the round-2 report remain prescribed: `hasSameOriginProof` in the header snippet, the state blob's empty `<b64url(id)>` field requiring `split` to preserve empty tokens, `serviceWith(cfg)` in the placeholder scan, the two missing includes in `tests/test_permutation.cpp`, and an explicit `char` → `unsigned char` conversion under `-Wconversion`. The last item now applies to private `hmacDigestToHex` rather than the removed public `toHex`; the safety correction is preserved even though the helper name changed.

## Task 1 Step 0 remains the pre-change gate

Step 0 captures golden literals by building and running the pre-change tree.
The execution profile has now been repaired and the unchanged `f1db2bf` source
baseline was verified while resolving #155 (9/9 C++ tests and 40/40 frontend
tests). The implementation attempt must still execute Step 0 and commit its
goldens before any source edit; prior baseline verification cannot substitute
for recording the exact allocation vectors on the implementation branch.

**This is a hard prerequisite, not bookkeeping.** The mechanical rename should
not change bytes, and the committed goldens are what make that claim observable
across versions. Do not start source edits if the Step 0 commands cannot run.

---

## Design decisions taken in this plan

These are settled inputs for implementation, not open questions. Each is justified against a codebase fact.

- **D-A1 — The real HMAC takes the `hmacSha256Hex` name; the old helper becomes `keyedDigestHex`.**
  `src/hash_utils.cpp:36-38` is `sha256Hex(key + "\x1f" + msg)` — a secret-prefix construction that is length-extension forgeable, not a MAC. Its only caller is the Feistel round function at `src/permutation.cpp:27`, where its output bytes must not change or every existing puzzle's allocation order shifts (ADR-4, `docs/architecture-review.md:94`). So the bytes stay frozen under the new honest name, and the new RFC 2104 implementation takes over the name that implies a MAC. Locked by a byte-stability vector plus a test asserting the two functions disagree.
- **D-A2 — HMAC is implemented once over file-private raw-digest and hex helpers, not per platform and not by refactoring `sha256Hex`.**
  `hash_utils.cpp` already branches CommonCrypto/OpenSSL for the digest itself. A new private `sha256Raw()` plus a separate private HMAC hex formatter builds RFC 2104 (block 64, ipad/opad) in one code path, so a cookie signed on macOS verifies identically on Linux. The existing public `sha256Hex` body and formatter remain byte-for-byte untouched. Validated against RFC 4231 test vectors.
- **D-A3 — The session cookie carries the numeric GitHub id, the MAC covers it, and the token's purpose is inside the MAC.**
  This adopts the PO's D18. AC26 derives the avatar URL from the numeric id, and AC7 requires re-evaluating the allow-list from the cookie on every admin request — so re-calling GitHub or storing server state are both excluded. AC3's "login and absolute expiry" is a minimum, not an exclusive list. Token format:
  `v1.<purpose>.<b64url(subject)>.<b64url(id)>.<expiryUnix>.<hmacHex>` where `<purpose>` is the literal `session` or `state`, MAC over everything before the final dot. Base64url on the two variable fields makes the delimiter unambiguous, so no login can inject a field boundary.
  The `<purpose>` label is domain separation (review finding S1). Both blob types are signed with `cfg.sessionSigningSecret`, and without the label they would be byte-compatible: a `pp_oauth_state` blob would verify as a `pp_session` and vice versa. No live escalation follows from that today — a state blob presented as a session yields a random-looking "login" that no allow-list contains — but it becomes live the moment a third blob type is added. `verifySessionToken` therefore takes the expected purpose and returns `SessionError::WrongPurpose` on a mismatch, tested in both directions. One string, one check, and the class of bug is closed.
- **D-A4 — Cookie-authorized admin `POST`s require an affirmative same-origin signal (AC14).**
  Accept `Sec-Fetch-Site: same-origin` or `none`, or — when `Sec-Fetch-Site` is absent — an `Origin` that equals the configured `PUBLIC_BASE_URL` origin. **Absence of both is rejected**, otherwise the defense is void against any client that simply omits the headers. A `Sec-Fetch-Site` that is present and is neither `same-origin` nor `none` is a definitive rejection and is *not* overridable by `Origin` (review finding O2): browsers set the two consistently, so the only thing that override buys is a weaker rule for a request that already told us it was cross-site. This is deliberate: a script or `curl` cannot drive admin `POST`s with a session cookie, and should use `X-Admin-Token`, which is not cookie-authorized and therefore skips the CSRF check entirely (AC12). `SameSite=Lax` already blocks the cross-site case; this is the second layer AC14 asks for.
- **D-A5 — `PUBLIC_BASE_URL` is a new required setting for the OAuth path.**
  The callback URL must be absolute and per-deployment, and the CSRF check needs an expected origin. The story does not name it, but AC20 forbids selecting secrets with a `Config::stage` branch — a per-stage env var is exactly how that is satisfied. When it is unset, `/api/v1/auth/github/*` returns 503 the same way a missing signing secret does.
- **D-A6 — OAuth flow failures redirect; configuration failures return 503.**
  The callback is always a top-level browser navigation, so answering it with raw JSON strands the operator on a JSON page. Flow failures (`invalid_state`, `exchange_failed`, `provider_error`) redirect to `/?auth=error&reason=<fixed-code>` with fixed non-secret codes; missing configuration returns 503 JSON per AC13/AC26. Both are asserted by tests.
- **D-A7 — `/api/v1/auth/me` also returns `is_admin`.**
  Slice B (AC15) must distinguish "signed in" from "signed in and allowed to act". Recomputing the allow-list client-side is impossible, and a second round-trip is waste. The field is a boolean derived from the same allow-list check the guard uses.
- **D-A8 — Startup diagnostics are a pure function, not `std::cout` in `main()`.**
  AC10 and AC13 both require a startup diagnostic. `main.cpp` is outside `puzzpool_core` and untestable (`CMakeLists.txt:41-72`, `tests/CMakeLists.txt:5`), so `startupAuthDiagnostics(const Config&)` returns the lines and `main()` only prints them.

## Risks and limitations to confirm at plan review

1. **libcurl is a new build dependency.** `.github/workflows/ci.yml:21` installs `libssl-dev` but no libcurl; `deps.txt` does not list it. Task 5 adds it to CI, `deps.txt`, and the README prerequisites. Deployment hosts need `libcurl4-openssl-dev` before the next deploy — called out for slice C.
2. **`Secure` cookies make the OAuth path unusable over plain `http://localhost`.** AC3 mandates `Secure` unconditionally and this plan does not weaken it. Local development keeps using `X-Admin-Token`; documented in `README.md` and `docs/security.md`.
3. **AC9 is a breaking change for the currently recommended posture.** `docs/security.md:22-35` presents blank `ADMIN_TOKEN` + Nginx IP restriction as a valid option, and `deploy/nginx.conf:38-46` deliberately exposes `/api/v1/admin/activate-puzzle` to the internet with only the server-side token behind it. After this change that route returns 401 until one mechanism is configured. Handled by the README upgrade warning, the rewritten `docs/security.md`, and the startup diagnostic (D12) — but it is a real operator-visible break and reviewers should confirm they want it in slice A.
4. **Published vectors and branch-local goldens serve different purposes.** The RFC 4231 HMAC and NIST SHA-256 values are external standards; confirm them against the published documents before treating a mismatch as an implementation bug. The keyed-digest and permutation values in Task 1 Step 0 are outputs of the current binary and must be captured and committed on the implementation branch before any source edit. They prove the mechanical rename and allocation order, while leaving `sha256Hex` untouched removes the formatter-refactor risk entirely.

   The Crow API calls used throughout (`add_header`, `redirect`, `url_params.get`, `set_header`) *were* verified against the bundled submodule at `third_party/crow` (commit `7ecd59c`), including this revision's finding that `crow::response::get_header_value` is non-const (`http_response.h:76`) while `crow::request`'s is const (`http_request.h:81`), and that `crow::response::headers` is a `std::unordered_multimap` (`ci_map.h:42`) so `equal_range` is the correct way to read repeated `Set-Cookie` headers.
5. **`src/service_puzzle_status.cpp:55-57` still shells out to `curl` via `popen`.** Migrating it to the new libcurl seam is explicitly out of scope for this story; it is not touched.

---

## File Structure

**New — crow-free domain**

- Add: `include/puzzpool/base64.hpp`, `src/base64.cpp`
  Base64url encode/decode without padding, for cookie fields and random tokens.
- Add: `include/puzzpool/secure_random.hpp`, `src/secure_random.cpp`
  `secureRandomBytes(n)` from `arc4random_buf` (Apple) / `getrandom()` (Linux) with a `/dev/urandom` fallback; throws rather than degrading to a PRNG.
- Add: `include/puzzpool/session.hpp`, `src/session.cpp`
  One signed-blob format with a `TokenPurpose` label, session/state issue and verify wrappers, the `SessionError` taxonomy, and `Cookie:` header parsing.
- Add: `include/puzzpool/admin_auth.hpp`, `src/admin_auth.cpp`
  `authorizeAdmin()` — the whole authorization and CSRF decision over a plain request view, plus `isAllowedAdminLogin()` and `startupAuthDiagnostics()`.

**New — Crow / network adapters**

- Add: `include/puzzpool/github_client.hpp`, `src/github_client.cpp`
  `GitHubHttpClient` seam and `makeLibcurlGitHubClient()`.
- Add: `include/puzzpool/auth_service.hpp`, `src/auth_service.cpp`
  `AuthService` — the four `/api/v1/auth/*` handlers, injectable client, clock, and nonce source.
- Add: `include/puzzpool/admin_guard.hpp`, `src/admin_guard.cpp`
  `adminGuard(cfg, req)` — maps `authorizeAdmin()` onto `std::optional<crow::response>`.

**Modified**

- Modify: `include/puzzpool/hash_utils.hpp`, `src/hash_utils.cpp`
  Mechanically rename the old helper to `keyedDigestHex`; add real `hmacSha256Hex`, `constantTimeEquals`, and file-private raw-digest/HMAC-hex helpers. Leave the existing `sha256Hex` implementation and formatter byte-for-byte untouched.
- Modify: `src/permutation.cpp`
  Mechanical rename of the single call site at line 27.
- Modify: `include/puzzpool/config.hpp`, `src/config.cpp`
  Five new settings plus the parsed allow-list.
- Modify: `src/main.cpp`
  Wire four auth routes, replace the inline lambda guard with `adminGuard`, print the diagnostics.
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`
  New sources, `find_package(CURL REQUIRED)`, four new test targets.
- Modify: `.github/workflows/ci.yml`, `deps.txt`
  Install libcurl (Task 5), and run `tests/test_admin_routes_guarded.sh` in the `build`
  job (Task 7 Step 7a).
- Modify: `tests/test_config.cpp`
  Allow-list parsing, TTL clamping, and stage-independence coverage.
- Modify: `tests/test_permutation.cpp`
  Golden `permuteIndexFeistel` vector captured from the pre-change build — the cross-version proof of ADR-4 that the existing self-consistency case cannot supply.

**New tests**

- Add: `tests/test_hash_utils.cpp`, `tests/test_session.cpp`, `tests/test_admin_auth.cpp`, `tests/test_auth_service.cpp`
- Add: `tests/test_admin_routes_guarded.sh`
  End-to-end guard regression over all six admin routes, because `src/main.cpp` is outside every Catch2 target. Run explicitly, like `tests/test_check_node_version_age.sh`.

**Docs**

- Modify: `docs/api.md`, `docs/security.md`, `docs/architecture.md`, `docs/architecture-review.md` (ADR-5, ADR-6), `docs/testing.md`, `README.md`, `.env.example`, `deploy/nginx.conf` (comment only).

---

### Task 1: Real HMAC beside the frozen permutation digest

**Files:**
- Add: `tests/test_hash_utils.cpp`
- Modify: `tests/CMakeLists.txt`, `include/puzzpool/hash_utils.hpp`, `src/hash_utils.cpp`, `src/permutation.cpp`, `tests/test_permutation.cpp`

> **Ordering is load-bearing (review finding M1 and artifact 105).** Step 0 captures golden literals from the **unmodified** tree. Nothing under `src/` or `include/` may be edited until Step 0's values are recorded and committed. Capturing them afterwards would freeze whatever the rename produced. The earlier shared-`toHex` refactor is removed: `sha256Hex` stays byte-for-byte untouched, while the goldens prove that renaming the legacy keyed digest and its permutation call site did not change allocation.

- [ ] **Step 0: Capture the golden literals from the pre-change build**

Adding test files does not change any behavior, so goldens captured with these new test files present are still baseline goldens — provided no source under `src/` or `include/` has been touched yet. That is the whole discipline of this step.

1. Create `tests/test_hash_utils.cpp` and register it (Step 2's CMake line), containing only the keyed-digest capture case below with a deliberately wrong placeholder.
2. Append the permutation capture case below to `tests/test_permutation.cpp`, also with a placeholder.
3. Build and run:
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUZZPOOL_BUILD_TESTS=ON
   cmake --build build --parallel "${BUILD_JOBS:-1}"
   ctest --test-dir build --output-on-failure --tests-regex "hash_utils|permutation"
   ```
4. Each case fails and Catch2 prints `with expansion: "<actual>" == "CAPTURE_ME"`. Substitute the printed `<actual>` values into the literals, re-run until green, and commit that state **before** editing any source file.

```cpp
// CAPTURE (pre-change): the frozen Feistel round digest. Note the PRE-RENAME name —
// Step 6 renames this call site to keyedDigestHex and the literal must not move.
TEST_CASE("CAPTURE frozen round digest", "[capture]") {
    CHECK(hmacSha256Hex("round-key-0", "12345") == "CAPTURE_ME");
}

```

```cpp
// CAPTURE (pre-change), appended to tests/test_permutation.cpp: this is the assertion
// that actually speaks for ADR-4. Existing coverage only asserts self-consistency
// (test_permutation.cpp:25 calls the function twice and compares), which cannot detect
// a change in the round function's output bytes.
TEST_CASE("CAPTURE feistel golden", "[capture]") {
    const cpp_int n("999983");
    const std::string key = "golden_seed_v1";
    for (int i : {0, 1, 42, 4242, 999982}) {
        INFO("index " << i);
        CHECK(permuteIndexFeistel(cpp_int(i), n, key).str() == "CAPTURE_ME");
    }
}
```

Expected after substitution: both files green against the untouched `src/`. Record the five permutation values and the keyed-digest value in the implementation report — they are the branch-local evidence AC4 rests on. The published NIST values are added independently in Step 1; they are not capture outputs.

- [ ] **Step 1: Write the failing primitive tests first**

`keyedDigestHex` and `constantTimeEquals` do not exist yet and `hmacSha256Hex` is not an HMAC, so the new target fails for the right reasons. The keyed-digest capture from Step 0 is rewritten here into its permanent form — same literal, honest name. `sha256Hex` receives only published-vector tests; its implementation is not edited.

```cpp
#include <puzzpool/hash_utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace puzzpool;

// RFC 4231 test case 2 — key "Jefe", data "what do ya want for nothing?"
TEST_CASE("hmacSha256Hex matches RFC 4231 case 2", "[hash][hmac]") {
    CHECK(hmacSha256Hex("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// RFC 4231 test case 1 — 20 bytes of 0x0b, data "Hi There"
TEST_CASE("hmacSha256Hex matches RFC 4231 case 1", "[hash][hmac]") {
    CHECK(hmacSha256Hex(std::string(20, '\x0b'), "Hi There") ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// RFC 4231 test case 3 — 20 bytes of 0xaa, 50 bytes of 0xdd
TEST_CASE("hmacSha256Hex matches RFC 4231 case 3", "[hash][hmac]") {
    CHECK(hmacSha256Hex(std::string(20, '\xaa'), std::string(50, '\xdd')) ==
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

// RFC 4231 test case 6 — key longer than the 64-byte block must be hashed first.
TEST_CASE("hmacSha256Hex hashes over-long keys per RFC 2104", "[hash][hmac]") {
    CHECK(hmacSha256Hex(std::string(131, '\xaa'),
                        "Test Using Larger Than Block-Size Key - Hash Key First") ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// AC4: the permutation digest is frozen. This literal was captured from the pre-change
// binary in Step 0 and must never change — ADR-4 allocation determinism depends on it.
// It is deliberately NOT written as `== sha256Hex(key + "\x1f" + msg)`: that expression
// would derive the expected value from the same implementation and could not detect
// a changed byte stream.
TEST_CASE("keyedDigestHex output is byte-for-byte frozen", "[hash][permutation]") {
    CHECK(keyedDigestHex("round-key-0", "12345") == "<Step 0 capture>");
}

// Published NIST FIPS 180-4 vectors. These pin the existing sha256Hex behavior to
// values outside this repository. Its production implementation remains untouched.
TEST_CASE("sha256Hex matches the published NIST vectors", "[hash]") {
    CHECK(sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// Sensitivity: proves the rename did not alias the two, i.e. that the HMAC probe
// above would actually detect a regression back to the secret-prefix construction.
TEST_CASE("keyedDigestHex and hmacSha256Hex are different primitives", "[hash][hmac]") {
    CHECK(keyedDigestHex("k", "m") != hmacSha256Hex("k", "m"));
}

TEST_CASE("constantTimeEquals matches string equality", "[hash][compare]") {
    CHECK(constantTimeEquals("abc", "abc"));
    CHECK_FALSE(constantTimeEquals("abc", "abd"));
    CHECK_FALSE(constantTimeEquals("abc", "abcd"));
    CHECK_FALSE(constantTimeEquals("", "a"));
    CHECK(constantTimeEquals("", ""));
}
```

- [ ] **Step 2: Register the new test target**

```cmake
add_puzzpool_test(test_hash_utils)
```

- [ ] **Step 3: Confirm the new tests fail before implementing**

Run: `ctest --test-dir build --output-on-failure --tests-regex hash_utils`

Expected: compilation fails on the missing `keyedDigestHex` / `constantTimeEquals`, and once declared, the RFC 4231 cases fail against the secret-prefix construction.

- [ ] **Step 4: Extend the header with the honest names**

```cpp
#pragma once

#include <string>
#include <string_view>

namespace puzzpool {

std::string sha256Hex(const std::string& input);

/// Keyed digest: sha256(key || 0x1f || msg).
///
/// WARNING: this is deliberately NOT an HMAC. It is a secret-prefix construction and
/// is vulnerable to length-extension. It exists only as the Feistel round function in
/// src/permutation.cpp, where its output bytes are frozen because changing them would
/// reorder allocation for every existing puzzle (ADR-4). Never use it as a MAC — use
/// hmacSha256Hex() for anything that must resist forgery.
std::string keyedDigestHex(const std::string& key, const std::string& msg);

/// RFC 2104 HMAC-SHA-256, lowercase hex. The MAC for session cookies and OAuth state.
std::string hmacSha256Hex(const std::string& key, const std::string& msg);

/// Length-independent-in-content comparison. Length is not secret and short-circuits.
bool constantTimeEquals(std::string_view a, std::string_view b);

} // namespace puzzpool
```

- [ ] **Step 5: Implement HMAC with private raw-digest and hex helpers**

Leave the complete existing `sha256Hex()` function byte-for-byte unchanged,
including its local `digest` buffer, CommonCrypto/OpenSSL call, stream setup,
loop, padding, and return. Do not extract or reuse its formatter.

Append file-private helpers for the new HMAC path. The duplication in
`hmacDigestToHex` is intentional: it prevents slice A from modifying the
allocation digest's formatting path.

```cpp
namespace {

std::string sha256Raw(std::string_view input) {
    unsigned char digest[32];
#if defined(__APPLE__)
    CC_SHA256(reinterpret_cast<const unsigned char*>(input.data()),
              static_cast<CC_LONG>(input.size()), digest);
#else
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
#endif
    return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

std::string hmacDigestToHex(std::string_view bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        oss << std::setw(2) << static_cast<unsigned>(b);
    }
    return oss.str();
}

} // namespace

std::string keyedDigestHex(const std::string& key, const std::string& msg) {
    // Mechanical rename only: this is the exact pre-change hmacSha256Hex body.
    return sha256Hex(key + "\x1f" + msg);
}

std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    constexpr std::size_t kBlock = 64;

    std::string block = key.size() > kBlock ? sha256Raw(key) : key;
    block.resize(kBlock, '\0');

    std::string inner(kBlock, '\0');
    std::string outer(kBlock, '\0');
    for (std::size_t i = 0; i < kBlock; ++i) {
        const auto b = static_cast<unsigned char>(block[i]);
        inner[i] = static_cast<char>(b ^ 0x36u);
        outer[i] = static_cast<char>(b ^ 0x5cu);
    }

    const std::string innerDigest = sha256Raw(inner + msg);
    return hmacDigestToHex(sha256Raw(outer + innerDigest));
}

bool constantTimeEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}
```

Keep `-Wconversion` clean — every `unsigned char` narrowing above is explicit.
The private raw helper duplicates only the platform digest call; the private hex
helper formats only the new HMAC result. Neither is declared in the public header.

- [ ] **Step 5a: Promote the captured permutation vector to its permanent form**

Rewrite the Step 0 capture case in `tests/test_permutation.cpp` with its real literals and an honest name. This is the assertion that speaks for ADR-4 across versions; the existing determinism case at `tests/test_permutation.cpp:25` only compares two calls in the same binary and cannot detect a changed round function.

```cpp
// AC4 / ADR-4: allocation order for every existing puzzle depends on these exact
// outputs. Captured from the pre-change build; a change here is a data migration,
// not a test update.
TEST_CASE("permuteIndexFeistel matches its frozen golden vector", "[permutation][feistel][golden]") {
    const cpp_int n("999983");
    const std::string key = "golden_seed_v1";
    const std::vector<std::pair<int, std::string>> golden{
        {0, "<Step 0 capture>"},      {1, "<Step 0 capture>"},
        {42, "<Step 0 capture>"},     {4242, "<Step 0 capture>"},
        {999982, "<Step 0 capture>"},
    };
    for (const auto& [index, expected] : golden) {
        INFO("index " << index);
        CHECK(permuteIndexFeistel(cpp_int(index), n, key).str() == expected);
    }
}
```

- [ ] **Step 6: Rename the single permutation call site**

```cpp
return hexToInt(keyedDigestHex(roundKey, bigToDec(right))) & mask;
```

- [ ] **Step 7: Verify the primitives and the frozen permutation together**

Run: `ctest --test-dir build --output-on-failure --tests-regex "hash_utils|permutation"`

Expected: the RFC 4231 vectors pass, the unchanged `sha256Hex` still matches the NIST vectors, the frozen `keyedDigestHex` literal and the golden `permuteIndexFeistel` vector both still match the Step 0 captures, and the pre-existing `test_permutation` suite is unchanged and green. The captured keyed digest and permutation vector are the proof that AC4 held; the rest of `test_permutation` cannot supply it.

- [ ] **Step 7a: Prove the golden vector can fail (sensitivity)**

An assertion that guards against a regression is worth nothing until it has been seen to fail. Temporarily change the delimiter in `keyedDigestHex` from `"\x1f"` to `"\x1e"` and re-run Step 7. Do not touch `sha256Hex`; the mutation is confined to the mechanically renamed wrapper whose bytes AC4 freezes.

Expected: **`test_hash_utils` and `test_permutation` both fail**, with the frozen `keyedDigestHex` literal and the golden Feistel vector reporting mismatches. The RFC 4231 HMAC and NIST `sha256Hex` cases should remain green, proving the probe is scoped to the allocation digest rather than the new primitive. If the golden vector still passes, it is not covering the round function and Step 0 must be redone. Revert the deliberate break and confirm green before committing; record both runs in the implementation report.

- [ ] **Step 8: Commit**

```bash
git add include/puzzpool/hash_utils.hpp src/hash_utils.cpp src/permutation.cpp \
        tests/test_hash_utils.cpp tests/test_permutation.cpp tests/CMakeLists.txt
git commit -m "feat: add real HMAC-SHA-256 and freeze the permutation keyed digest"
```

---

### Task 2: Configuration, allow-list, and startup diagnostics

**Files:**
- Modify: `include/puzzpool/config.hpp`, `src/config.cpp`, `tests/test_config.cpp`
- Add: `include/puzzpool/admin_auth.hpp`, `src/admin_auth.cpp` (allow-list + diagnostics only in this task)

- [ ] **Step 1: Write the failing configuration tests**

Follow the `setEnvVar`/`unsetEnvVar` idiom already at `tests/test_config.cpp:17-40`.

```cpp
TEST_CASE("ADMIN_GITHUB_USERS parses, trims, and lowercases", "[config][auth]") {
    setEnvVar("ADMIN_GITHUB_USERS", "  Alice , BOB ,,carol  ", true);
    Config cfg = loadConfigFromEnv();
    CHECK(cfg.adminGithubUsers == std::vector<std::string>{"alice", "bob", "carol"});
    unsetEnvVar("ADMIN_GITHUB_USERS");
}

TEST_CASE("an empty ADMIN_GITHUB_USERS grants nobody access", "[config][auth]") {
    setEnvVar("ADMIN_GITHUB_USERS", "   ,  , ", true);
    Config cfg = loadConfigFromEnv();
    CHECK(cfg.adminGithubUsers.empty());
    CHECK_FALSE(isAllowedAdminLogin(cfg, "alice"));
    unsetEnvVar("ADMIN_GITHUB_USERS");
}

TEST_CASE("allow-list matching is case-insensitive (AC6)", "[config][auth]") {
    setEnvVar("ADMIN_GITHUB_USERS", "Alice", true);
    Config cfg = loadConfigFromEnv();
    CHECK(isAllowedAdminLogin(cfg, "ALICE"));
    CHECK(isAllowedAdminLogin(cfg, "alice"));
    CHECK_FALSE(isAllowedAdminLogin(cfg, "alicee"));
    CHECK_FALSE(isAllowedAdminLogin(cfg, ""));
    unsetEnvVar("ADMIN_GITHUB_USERS");
}

// AC20: credentials come from the stage's own environment; no Config::stage branch.
TEST_CASE("OAuth credentials do not vary with STAGE", "[config][auth]") {
    setEnvVar("GITHUB_OAUTH_CLIENT_ID", "id-from-env", true);
    setEnvVar("GITHUB_OAUTH_CLIENT_SECRET", "secret-from-env", true);
    setEnvVar("STAGE", "TEST", true);
    Config testStage = loadConfigFromEnv();
    setEnvVar("STAGE", "PROD", true);
    Config prodStage = loadConfigFromEnv();

    CHECK(testStage.githubOauthClientId == prodStage.githubOauthClientId);
    CHECK(testStage.githubOauthClientSecret == prodStage.githubOauthClientSecret);
    CHECK(prodStage.githubOauthClientId == "id-from-env");

    unsetEnvVar("GITHUB_OAUTH_CLIENT_ID");
    unsetEnvVar("GITHUB_OAUTH_CLIENT_SECRET");
    unsetEnvVar("STAGE");
}

// AC10 + AC13: actionable, non-secret startup diagnostics.
TEST_CASE("startup diagnostics warn when no admin mechanism is configured", "[config][auth]") {
    Config cfg;  // no adminToken, no allow-list, no signing secret
    const auto lines = startupAuthDiagnostics(cfg);
    REQUIRE(lines.size() >= 2);
    const std::string joined = join(lines, "\n");
    CHECK(joined.find("ADMIN_TOKEN") != std::string::npos);
    CHECK(joined.find("ADMIN_GITHUB_USERS") != std::string::npos);
    CHECK(joined.find("SESSION_SIGNING_SECRET") != std::string::npos);
}

// Sensitivity: the diagnostic must disappear once a mechanism is configured, so the
// assertion above is detecting configuration state and not a hard-coded banner.
TEST_CASE("startup diagnostics stay quiet when auth is configured", "[config][auth]") {
    Config cfg;
    cfg.adminToken = "t";
    cfg.adminGithubUsers = {"alice"};
    cfg.sessionSigningSecret = "s";
    cfg.publicBaseUrl = "https://puzzle.b58.de";
    cfg.githubOauthClientId = "id";
    cfg.githubOauthClientSecret = "secret";
    CHECK(startupAuthDiagnostics(cfg).empty());
}

// AC22: a diagnostic must never carry a secret value.
TEST_CASE("startup diagnostics never contain secret values", "[config][auth][security]") {
    Config cfg;
    cfg.adminToken = "super-secret-token";
    const auto lines = startupAuthDiagnostics(cfg);
    for (const auto& line : lines) {
        CHECK(line.find("super-secret-token") == std::string::npos);
    }
}
```

- [ ] **Step 2: Confirm they fail**

Run: `ctest --test-dir build --output-on-failure --tests-regex config`

Expected: compilation fails on the missing `Config` members and `admin_auth.hpp` declarations.

- [ ] **Step 3: Extend `Config`**

```cpp
    std::string adminToken;
    std::string sessionSigningSecret;
    std::string githubOauthClientId;
    std::string githubOauthClientSecret;
    std::string publicBaseUrl;
    int         sessionTtlMinutes = 720;
    std::vector<std::string> adminGithubUsers;  // normalised: trimmed, lowercased, non-empty
```

- [ ] **Step 4: Read and normalise them in `loadConfigFromEnv()`**

```cpp
    cfg.sessionSigningSecret    = getEnvOr("SESSION_SIGNING_SECRET", "");
    cfg.githubOauthClientId     = getEnvOr("GITHUB_OAUTH_CLIENT_ID", "");
    cfg.githubOauthClientSecret = getEnvOr("GITHUB_OAUTH_CLIENT_SECRET", "");
    cfg.publicBaseUrl           = trimTrailingSlash(getEnvOr("PUBLIC_BASE_URL", ""));
    cfg.sessionTtlMinutes       = std::clamp(getEnvInt("SESSION_TTL_MINUTES", 720), 1, 43'200);
    cfg.adminGithubUsers        = parseAdminGithubUsers(getEnvOr("ADMIN_GITHUB_USERS", ""));
```

The upper clamp of 43 200 minutes (30 days) is deliberate (review finding O4): sessions are stateless and cannot be revoked individually, so a typo like `SESSION_TTL_MINUTES=7200000` would otherwise mint multi-year credentials whose only revocation lever is a secret rotation. Add a `test_config` case asserting that an absurd value clamps to 43 200 and that a negative one clamps to 1.

`parseAdminGithubUsers()` splits on `,`, applies the existing `trim()` from `env.hpp`, lowercases with an explicit `static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))`, and drops empties. Place it in `admin_auth.cpp` and declare it in `admin_auth.hpp` so `config.cpp` does not grow auth logic.

- [ ] **Step 5: Implement the allow-list check and diagnostics**

```cpp
bool isAllowedAdminLogin(const Config& cfg, std::string_view login) {
    if (login.empty() || cfg.adminGithubUsers.empty()) return false;
    const std::string needle = toLower(login);
    return std::find(cfg.adminGithubUsers.begin(), cfg.adminGithubUsers.end(), needle)
           != cfg.adminGithubUsers.end();
}

std::vector<std::string> startupAuthDiagnostics(const Config& cfg) {
    std::vector<std::string> out;
    if (cfg.adminToken.empty() && cfg.adminGithubUsers.empty()) {
        out.push_back("WARNING: neither ADMIN_TOKEN nor ADMIN_GITHUB_USERS is configured; "
                      "every /api/v1/admin/* route returns 401 until one is set");
    }
    if (cfg.sessionSigningSecret.empty()) {
        out.push_back("GitHub sign-in disabled: SESSION_SIGNING_SECRET is unset; "
                      "/api/v1/auth/* returns 503 and no session cookie is issued or accepted");
    } else if (cfg.githubOauthClientId.empty() || cfg.githubOauthClientSecret.empty()
               || cfg.publicBaseUrl.empty()) {
        out.push_back("GitHub sign-in disabled: GITHUB_OAUTH_CLIENT_ID, "
                      "GITHUB_OAUTH_CLIENT_SECRET, and PUBLIC_BASE_URL must all be set");
    }
    return out;
}
```

The allow-list is read from `cfg` on each call and never cached, which is what makes AC7's "removal takes effect immediately" true — but be precise about what "immediately" means here (review finding M3). `src/main.cpp:16` calls `loadConfigFromEnv()` exactly once per process and nothing re-reads the environment afterwards, so **editing `ADMIN_GITHUB_USERS` has no effect until the service restarts.** What the design does give you, and it is the part that matters operationally, is that no authorization decision is cached anywhere: once the process holds the new configuration the change applies on the very next request, with no cookie reissue, no session store to purge, and no waiting for outstanding sessions to expire. The revocation procedure is therefore *remove the login, then restart* — and every place this plan documents it (the Task 4 test comment, `docs/security.md`, ADR-6) must say so. Making the original "no restart" claim true would take a `Config` reload path; that is new scope and does not belong in slice A.

- [ ] **Step 6: Register the sources and re-run**

Add `src/admin_auth.cpp` to `puzzpool_core` in `CMakeLists.txt:41-61`.

Run: `ctest --test-dir build --output-on-failure --tests-regex config`

Expected: all new configuration and diagnostic cases pass; existing keyspace/puzzle-target cases stay green.

- [ ] **Step 7: Commit**

```bash
git add include/puzzpool/config.hpp src/config.cpp include/puzzpool/admin_auth.hpp \
        src/admin_auth.cpp tests/test_config.cpp CMakeLists.txt
git commit -m "feat: add GitHub auth configuration and admin allow-list parsing"
```

---

### Task 3: Signed session cookie and OAuth state

**Files:**
- Add: `include/puzzpool/base64.hpp`, `src/base64.cpp`, `include/puzzpool/secure_random.hpp`, `src/secure_random.cpp`, `include/puzzpool/session.hpp`, `src/session.cpp`, `tests/test_session.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing session tests**

```cpp
#include <puzzpool/session.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace puzzpool;

namespace {
constexpr const char* kSecret = "test-signing-secret";
constexpr int64_t kNow = 1'770'000'000;
}

TEST_CASE("a session token round-trips login and id", "[session]") {
    const auto token = issueSessionToken(kSecret, {"Alice", "4242"}, kNow + 600);
    const auto v = verifySessionToken(kSecret, token, kNow);
    REQUIRE(v.error == SessionError::None);
    CHECK(v.identity.login == "Alice");
    CHECK(v.identity.githubId == "4242");
    CHECK(v.expiresAt == kNow + 600);
}

TEST_CASE("a tampered login is rejected (AC3)", "[session][security]") {
    auto token = issueSessionToken(kSecret, {"alice", "1"}, kNow + 600);
    const auto forged = issueSessionToken("attacker-secret", {"root", "1"}, kNow + 600);
    // Swap in the attacker's payload but keep the legitimate signature.
    const auto payloadEnd = forged.rfind('.');
    const auto sigStart   = token.rfind('.');
    const std::string spliced = forged.substr(0, payloadEnd) + token.substr(sigStart);
    CHECK(verifySessionToken(kSecret, spliced, kNow).error == SessionError::BadSignature);
}

TEST_CASE("a flipped signature byte is rejected", "[session][security]") {
    auto token = issueSessionToken(kSecret, {"alice", "1"}, kNow + 600);
    token.back() = (token.back() == 'a') ? 'b' : 'a';
    CHECK(verifySessionToken(kSecret, token, kNow).error == SessionError::BadSignature);
}

TEST_CASE("a token signed with a different secret is rejected", "[session][security]") {
    const auto token = issueSessionToken("other-secret", {"alice", "1"}, kNow + 600);
    CHECK(verifySessionToken(kSecret, token, kNow).error == SessionError::BadSignature);
}

TEST_CASE("expiry is absolute and checked against the supplied clock", "[session]") {
    const auto token = issueSessionToken(kSecret, {"alice", "1"}, kNow + 60);
    CHECK(verifySessionToken(kSecret, token, kNow + 59).error == SessionError::None);
    CHECK(verifySessionToken(kSecret, token, kNow + 61).error == SessionError::Expired);
}

TEST_CASE("malformed tokens are rejected without throwing", "[session][security]") {
    for (const std::string bad : {"", "v1", "v1.session.a.b.c", "v1.session.a.b.c.d.e",
                                  "v2.session.YWxpY2U.MQ.1770000600.deadbeef",
                                  "v1.session.!!!.MQ.1770000600.deadbeef",
                                  "v1.session.YWxpY2U.MQ.not-a-number.deadbeef"}) {
        INFO(bad);
        CHECK(verifySessionToken(kSecret, bad, kNow).error == SessionError::Malformed);
    }
}

// S1 — domain separation. Both blob types are signed with the same key, so without the
// purpose label inside the signed input they would be byte-compatible and either would
// verify as the other. Both directions, because one-directional separation is not
// separation.
TEST_CASE("a state token never verifies as a session token", "[session][security]") {
    const auto state = issueStateToken(kSecret, "the-nonce", kNow + 600);
    CHECK(verifySessionToken(kSecret, state, kNow).error == SessionError::WrongPurpose);
}

TEST_CASE("a session token never verifies as a state token", "[session][security]") {
    const auto session = issueSessionToken(kSecret, {"alice", "1"}, kNow + 600);
    CHECK(verifyStateToken(kSecret, session, kNow).error == SessionError::WrongPurpose);
}

// Sensitivity for the two cases above: each blob still verifies under its own purpose,
// so WrongPurpose is detecting the label and not a blanket rejection.
TEST_CASE("each token still verifies under its own purpose", "[session]") {
    const auto state = issueStateToken(kSecret, "the-nonce", kNow + 600);
    const auto verified = verifyStateToken(kSecret, state, kNow);
    REQUIRE(verified.error == SessionError::None);
    CHECK(verified.identity.login == "the-nonce");
    CHECK(verifySessionToken(kSecret, issueSessionToken(kSecret, {"alice", "1"}, kNow + 600),
                             kNow).error == SessionError::None);
}

// The purpose is covered by the MAC, not merely compared after the fact: rewriting the
// label in a valid state blob must fail on the signature, before any purpose check.
TEST_CASE("the purpose label is inside the MAC", "[session][security]") {
    auto state = issueStateToken(kSecret, "the-nonce", kNow + 600);
    const auto relabelled = "v1.session" + state.substr(state.find(".state.") + 6);
    CHECK(verifySessionToken(kSecret, relabelled, kNow).error == SessionError::BadSignature);
}

TEST_CASE("an empty signing secret never verifies anything (AC13)", "[session][security]") {
    const auto token = issueSessionToken(kSecret, {"alice", "1"}, kNow + 600);
    CHECK(verifySessionToken("", token, kNow).error == SessionError::BadSignature);
}

TEST_CASE("a login containing the delimiter cannot forge a field boundary", "[session][security]") {
    const auto token = issueSessionToken(kSecret, {"ali.ce.9999999999", "1"}, kNow + 600);
    const auto v = verifySessionToken(kSecret, token, kNow);
    REQUIRE(v.error == SessionError::None);
    CHECK(v.identity.login == "ali.ce.9999999999");
    CHECK(v.identity.githubId == "1");
}

TEST_CASE("cookieValue extracts the right cookie", "[session][cookie]") {
    CHECK(cookieValue("pp_session=abc", "pp_session") == "abc");
    CHECK(cookieValue("a=1; pp_session=abc; b=2", "pp_session") == "abc");
    CHECK(cookieValue("a=1;pp_session=abc", "pp_session") == "abc");
    CHECK_FALSE(cookieValue("xpp_session=abc", "pp_session").has_value());
    CHECK_FALSE(cookieValue("pp_session_extra=abc", "pp_session").has_value());
    CHECK_FALSE(cookieValue("", "pp_session").has_value());
    CHECK(cookieValue("pp_session=", "pp_session") == "");
}

TEST_CASE("secureRandomBytes returns distinct full-length buffers", "[session][random]") {
    const auto a = secureRandomBytes(32);
    const auto b = secureRandomBytes(32);
    CHECK(a.size() == 32);
    CHECK(b.size() == 32);
    CHECK(a != b);
}

TEST_CASE("base64url round-trips binary and omits padding", "[session][base64]") {
    // Adjacent literals, not "\x00\xff\x10binary": C++ hex escapes are maximal-munch,
    // so \x10b would parse as one escape with value 0x10b and not fit in a char.
    const std::string raw("\x00\xff\x10" "binary", 9);
    const auto encoded = base64UrlEncode(raw);
    CHECK(encoded.find('=') == std::string::npos);
    CHECK(encoded.find('+') == std::string::npos);
    CHECK(encoded.find('/') == std::string::npos);
    CHECK(base64UrlDecode(encoded) == raw);
    CHECK_FALSE(base64UrlDecode("!!!").has_value());
}
```

- [ ] **Step 2: Register the target and confirm failure**

```cmake
add_puzzpool_test(test_session)
```

Run: `ctest --test-dir build --output-on-failure --tests-regex session`

Expected: fails to compile — the headers do not exist yet.

- [ ] **Step 3: Implement base64url and the CSPRNG**

`base64UrlEncode(std::string_view) -> std::string` and `base64UrlDecode(std::string_view) -> std::optional<std::string>`, alphabet `A-Za-z0-9-_`, no padding, decode rejecting any out-of-alphabet character and any length ≡ 1 (mod 4).

```cpp
std::string secureRandomBytes(std::size_t n) {
    std::string out(n, '\0');
#if defined(__APPLE__)
    arc4random_buf(out.data(), n);
    return out;
#elif defined(__linux__)
    std::size_t filled = 0;
    while (filled < n) {
        const ssize_t got = ::getrandom(out.data() + filled, n - filled, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;  // fall through to /dev/urandom
        }
        filled += static_cast<std::size_t>(got);
    }
    if (filled == n) return out;
#endif
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom || !urandom.read(out.data(), static_cast<std::streamsize>(n))) {
        throw std::runtime_error("secureRandomBytes: no cryptographic entropy source");
    }
    return out;
}
```

There is deliberately no `std::mt19937` or `std::random_device` fallback: failing to start is correct when no entropy source exists.

- [ ] **Step 4: Implement the session token**

One signed-blob implementation, two purposes, and two named wrappers so a call site cannot forget which it is minting.

```cpp
// session.hpp
enum class TokenPurpose { Session, State };
enum class SessionError { None, Malformed, WrongPurpose, BadSignature, Expired };
```

```cpp
namespace {

constexpr std::string_view purposeLabel(TokenPurpose p) {
    return p == TokenPurpose::Session ? "session" : "state";
}

std::string signingInput(TokenPurpose purpose, const SessionIdentity& id, int64_t expiresAt) {
    return std::string("v1.") + std::string(purposeLabel(purpose)) + "."
         + base64UrlEncode(id.login) + "." + base64UrlEncode(id.githubId) + "."
         + std::to_string(expiresAt);
}

std::string issueSignedBlob(const std::string& secret, TokenPurpose purpose,
                            const SessionIdentity& id, int64_t expiresAtUnix) {
    const std::string payload = signingInput(purpose, id, expiresAtUnix);
    return payload + "." + hmacSha256Hex(secret, payload);
}

SessionVerification verifySignedBlob(const std::string& secret, TokenPurpose expected,
                                     std::string_view token, int64_t nowUnix) {
    SessionVerification out;
    const auto parts = split(token, '.');
    if (parts.size() != 6 || parts[0] != "v1") { out.error = SessionError::Malformed; return out; }

    const auto subject = base64UrlDecode(parts[2]);
    const auto id      = base64UrlDecode(parts[3]);
    int64_t expiresAt = 0;
    if (!subject || !id || !parseInt64(parts[4], expiresAt)) {
        out.error = SessionError::Malformed;
        return out;
    }

    const std::string payload = concatFirstFive(parts);
    if (secret.empty() ||
        !constantTimeEquals(parts[5], hmacSha256Hex(secret, payload))) {
        out.error = SessionError::BadSignature;
        return out;
    }
    if (parts[1] != purposeLabel(expected)) { out.error = SessionError::WrongPurpose; return out; }
    if (nowUnix > expiresAt) { out.error = SessionError::Expired; return out; }

    out.error = SessionError::None;
    out.identity = {*subject, *id};
    out.expiresAt = expiresAt;
    return out;
}

} // namespace

std::string issueSessionToken(const std::string& secret, const SessionIdentity& id,
                              int64_t expiresAtUnix) {
    return issueSignedBlob(secret, TokenPurpose::Session, id, expiresAtUnix);
}

std::string issueStateToken(const std::string& secret, const std::string& nonce,
                            int64_t expiresAtUnix) {
    return issueSignedBlob(secret, TokenPurpose::State, {nonce, ""}, expiresAtUnix);
}

SessionVerification verifySessionToken(const std::string& secret, std::string_view token,
                                       int64_t nowUnix) {
    return verifySignedBlob(secret, TokenPurpose::Session, token, nowUnix);
}

SessionVerification verifyStateToken(const std::string& secret, std::string_view token,
                                     int64_t nowUnix) {
    return verifySignedBlob(secret, TokenPurpose::State, token, nowUnix);
}
```

Order matters: shape → signature → purpose → expiry. Never read the identity out of an unverified token, and never let the purpose check stand in for the signature — checking the label first would answer "is this the right kind of blob?" about bytes nobody has authenticated yet. Verifying the signature first also means a forged label fails as `BadSignature`, which is what the "purpose is inside the MAC" test above asserts.

The state blob reuses the same format with the nonce in the subject field and an empty id (review finding S1): one signed-blob format, one place to get it right, and a label that keeps the two from being interchangeable.

- [ ] **Step 5: Implement `cookieValue()`**

Split the `Cookie:` header on `;`, trim, split each pair on the first `=`, and compare the name exactly — no `rfind(name, 0) == 0` prefix matching, which is what the `xpp_session=` and `pp_session_extra=` cases above are guarding.

- [ ] **Step 6: Verify**

Run: `ctest --test-dir build --output-on-failure --tests-regex session`

Expected: every case passes, including the splice-forgery and delimiter-injection cases.

- [ ] **Step 7: Commit**

```bash
git add include/puzzpool/base64.hpp src/base64.cpp \
        include/puzzpool/secure_random.hpp src/secure_random.cpp \
        include/puzzpool/session.hpp src/session.cpp \
        tests/test_session.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add signed stateless session tokens and cookie parsing"
```

---

### Task 4: Extract the admin guard and make it fail closed

**Files:**
- Add: `tests/test_admin_auth.cpp`, `include/puzzpool/admin_guard.hpp`, `src/admin_guard.cpp`
- Modify: `include/puzzpool/admin_auth.hpp`, `src/admin_auth.cpp`, `src/main.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

This is the task that closes the fail-open hole at `src/main.cpp:73-82`, where an empty `cfg.adminToken` returns `std::nullopt` and admits everyone.

- [ ] **Step 1: Write the failing authorization matrix**

```cpp
#include <puzzpool/admin_auth.hpp>
#include <puzzpool/session.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace puzzpool;

namespace {
constexpr int64_t kNow = 1'770'000'000;

Config authCfg() {
    Config cfg;
    cfg.sessionSigningSecret = "signing-secret";
    cfg.adminGithubUsers = {"alice"};
    cfg.publicBaseUrl = "https://puzzle.b58.de";
    return cfg;
}

AdminRequestView cookieGet(const std::string& token) {
    AdminRequestView v;
    v.isPost = false;
    v.cookieHeader = "pp_session=" + token;
    return v;
}

AdminRequestView cookiePost(const std::string& token) {
    AdminRequestView v = cookieGet(token);
    v.isPost = true;
    v.secFetchSite = "same-origin";
    return v;
}

std::string tokenFor(const Config& cfg, const std::string& login, int64_t exp = kNow + 600) {
    return issueSessionToken(cfg.sessionSigningSecret, {login, "42"}, exp);
}
}

// AC9 — the headline regression. Under the pre-change guard this returned nullopt.
TEST_CASE("with no mechanism configured every admin request is denied", "[admin][authz]") {
    Config cfg;  // no token, no allow-list
    AdminRequestView v;
    const auto d = authorizeAdmin(cfg, v, kNow);
    CHECK_FALSE(d.allowed);
    CHECK(d.statusCode == 401);
}

// Sensitivity for the assertion above: the same probe must return allowed==true when a
// mechanism IS configured, proving it detects authorization rather than always denying.
TEST_CASE("a configured token authorizes the same probe", "[admin][authz]") {
    Config cfg;
    cfg.adminToken = "secret-token";
    AdminRequestView v;
    v.adminTokenHeader = "secret-token";
    const auto d = authorizeAdmin(cfg, v, kNow);
    CHECK(d.allowed);
    CHECK(d.mechanism == AdminAuthMechanism::Token);
}

TEST_CASE("a wrong or absent admin token is denied (AC12)", "[admin][authz]") {
    Config cfg;
    cfg.adminToken = "secret-token";
    AdminRequestView wrong;  wrong.adminTokenHeader = "nope";
    AdminRequestView absent;
    CHECK_FALSE(authorizeAdmin(cfg, wrong, kNow).allowed);
    CHECK_FALSE(authorizeAdmin(cfg, absent, kNow).allowed);
    CHECK(authorizeAdmin(cfg, absent, kNow).statusCode == 401);
}

TEST_CASE("an allow-listed login in a valid cookie authorizes (AC7)", "[admin][authz]") {
    const Config cfg = authCfg();
    const auto d = authorizeAdmin(cfg, cookieGet(tokenFor(cfg, "alice")), kNow);
    CHECK(d.allowed);
    CHECK(d.mechanism == AdminAuthMechanism::Cookie);
}

TEST_CASE("allow-list matching from the cookie is case-insensitive (AC6)", "[admin][authz]") {
    const Config cfg = authCfg();
    CHECK(authorizeAdmin(cfg, cookieGet(tokenFor(cfg, "ALICE")), kNow).allowed);
}

TEST_CASE("a valid cookie for a non-allow-listed login is denied", "[admin][authz]") {
    const Config cfg = authCfg();
    const auto d = authorizeAdmin(cfg, cookieGet(tokenFor(cfg, "mallory")), kNow);
    CHECK_FALSE(d.allowed);
    CHECK(d.statusCode == 401);
}

// AC7 — no authorization decision is cached: once the process holds the new allow-list,
// an already-issued cookie stops working on the very next request, with no cookie
// reissue and no session store to purge. Note what this does NOT say: main.cpp:16 loads
// Config once per process, so editing ADMIN_GITHUB_USERS still requires a restart.
TEST_CASE("removing a login from the allow-list denies the existing cookie", "[admin][authz]") {
    Config cfg = authCfg();
    const auto token = tokenFor(cfg, "alice");
    REQUIRE(authorizeAdmin(cfg, cookieGet(token), kNow).allowed);
    cfg.adminGithubUsers.clear();
    CHECK_FALSE(authorizeAdmin(cfg, cookieGet(token), kNow).allowed);
}

TEST_CASE("expired and tampered cookies are denied", "[admin][authz][security]") {
    const Config cfg = authCfg();
    CHECK_FALSE(authorizeAdmin(cfg, cookieGet(tokenFor(cfg, "alice", kNow - 1)), kNow).allowed);

    auto tampered = tokenFor(cfg, "alice");
    tampered.back() = (tampered.back() == 'a') ? 'b' : 'a';
    CHECK_FALSE(authorizeAdmin(cfg, cookieGet(tampered), kNow).allowed);

    CHECK_FALSE(authorizeAdmin(cfg, cookieGet("not-a-token"), kNow).allowed);
    CHECK_FALSE(authorizeAdmin(cfg, AdminRequestView{}, kNow).allowed);
}

// AC13 — no signing secret means no cookie is ever accepted, allow-list or not.
TEST_CASE("an unset signing secret disables cookie authorization", "[admin][authz][security]") {
    Config signing = authCfg();
    const auto token = tokenFor(signing, "alice");
    Config cfg = signing;
    cfg.sessionSigningSecret.clear();
    CHECK_FALSE(authorizeAdmin(cfg, cookieGet(token), kNow).allowed);
}

// AC14 — CSRF.
TEST_CASE("a cookie-authorized POST needs an affirmative same-origin signal", "[admin][csrf]") {
    const Config cfg = authCfg();
    const auto token = tokenFor(cfg, "alice");

    AdminRequestView bare = cookieGet(token);  bare.isPost = true;   // no Origin, no Sec-Fetch-Site
    const auto denied = authorizeAdmin(cfg, bare, kNow);
    CHECK_FALSE(denied.allowed);
    CHECK(denied.statusCode == 403);

    AdminRequestView cross = bare;  cross.origin = "https://evil.example";
    CHECK_FALSE(authorizeAdmin(cfg, cross, kNow).allowed);

    AdminRequestView crossFetch = bare;  crossFetch.secFetchSite = "cross-site";
    CHECK_FALSE(authorizeAdmin(cfg, crossFetch, kNow).allowed);

    // O2: an explicit cross-site signal is not overridable by a matching Origin.
    AdminRequestView crossFetchWithOrigin = crossFetch;
    crossFetchWithOrigin.origin = "https://puzzle.b58.de";
    CHECK_FALSE(authorizeAdmin(cfg, crossFetchWithOrigin, kNow).allowed);

    AdminRequestView sameSite = bare;  sameSite.secFetchSite = "same-site";
    CHECK_FALSE(authorizeAdmin(cfg, sameSite, kNow).allowed);

    // Positive controls — the check accepts genuine same-origin requests.
    CHECK(authorizeAdmin(cfg, cookiePost(token), kNow).allowed);
    AdminRequestView sameOrigin = bare;  sameOrigin.origin = "https://puzzle.b58.de";
    CHECK(authorizeAdmin(cfg, sameOrigin, kNow).allowed);
}

TEST_CASE("a GET with a valid cookie needs no CSRF proof", "[admin][csrf]") {
    const Config cfg = authCfg();
    CHECK(authorizeAdmin(cfg, cookieGet(tokenFor(cfg, "alice")), kNow).allowed);
}

// AC12/AC14 boundary: token auth is not cookie-authorized, so CSRF does not apply.
TEST_CASE("a token-authorized POST is not subject to the CSRF check", "[admin][csrf]") {
    Config cfg = authCfg();
    cfg.adminToken = "secret-token";
    AdminRequestView v;
    v.isPost = true;
    v.adminTokenHeader = "secret-token";  // no Origin, no Sec-Fetch-Site
    CHECK(authorizeAdmin(cfg, v, kNow).allowed);
}

TEST_CASE("a denial never echoes the token or cookie (AC22)", "[admin][security]") {
    Config cfg = authCfg();
    cfg.adminToken = "secret-token";
    AdminRequestView v;
    v.adminTokenHeader = "wrong-but-secret";
    v.cookieHeader = "pp_session=" + tokenFor(cfg, "mallory");
    const auto d = authorizeAdmin(cfg, v, kNow);
    CHECK(d.error.find("wrong-but-secret") == std::string::npos);
    CHECK(d.error.find("pp_session") == std::string::npos);
    CHECK(d.error.find(cfg.sessionSigningSecret) == std::string::npos);
}
```

- [ ] **Step 2: Register and confirm failure**

```cmake
add_puzzpool_test(test_admin_auth)
```

Run: `ctest --test-dir build --output-on-failure --tests-regex admin_auth`

Expected: fails to compile on the missing `AdminRequestView` / `authorizeAdmin`.

- [ ] **Step 3: Declare the decision type**

```cpp
enum class AdminAuthMechanism { None, Token, Cookie };

struct AdminRequestView {
    bool        isPost = false;
    std::string adminTokenHeader;   // X-Admin-Token
    std::string cookieHeader;       // raw Cookie header
    std::string origin;             // Origin
    std::string secFetchSite;       // Sec-Fetch-Site
};

struct AdminAuthDecision {
    bool                allowed    = false;
    int                 statusCode = 401;
    std::string         error      = "unauthorized";  // fixed, non-secret codes only
    AdminAuthMechanism  mechanism  = AdminAuthMechanism::None;
    std::string         login;                        // set only when allowed by cookie
};

AdminAuthDecision authorizeAdmin(const Config&, const AdminRequestView&, int64_t nowUnix);
```

- [ ] **Step 4: Implement the decision**

```cpp
AdminAuthDecision authorizeAdmin(const Config& cfg, const AdminRequestView& req, int64_t nowUnix) {
    AdminAuthDecision d;

    if (!cfg.adminToken.empty() &&
        constantTimeEquals(req.adminTokenHeader, cfg.adminToken)) {
        d.allowed = true;
        d.mechanism = AdminAuthMechanism::Token;
        return d;                       // AC12, AC11; not cookie-authorized, so no CSRF gate
    }

    if (cfg.sessionSigningSecret.empty() || cfg.adminGithubUsers.empty()) {
        return d;                       // AC9/AC13 — fail closed
    }

    const auto raw = cookieValue(req.cookieHeader, kSessionCookieName);
    if (!raw) return d;

    const auto session = verifySessionToken(cfg.sessionSigningSecret, *raw, nowUnix);
    if (session.error != SessionError::None) return d;
    if (!isAllowedAdminLogin(cfg, session.identity.login)) return d;

    if (req.isPost && !hasSameOriginProof(cfg, req)) {   // AC14
        d.statusCode = 403;
        d.error = "csrf_check_failed";
        return d;
    }

    d.allowed = true;
    d.mechanism = AdminAuthMechanism::Cookie;
    d.login = session.identity.login;
    return d;
}

bool hasSameOriginProof(const Config& cfg, const AdminRequestView& req) {
    if (!req.secFetchSite.empty()) {
        // An explicit signal is definitive in both directions (review finding O2): a
        // request that already told us it is cross-site does not get to overrule itself
        // with an Origin header it also controls.
        return req.secFetchSite == "same-origin" || req.secFetchSite == "none";
    }
    if (!req.origin.empty() && !cfg.publicBaseUrl.empty()) {
        return originOf(req.origin) == originOf(cfg.publicBaseUrl);
    }
    return false;   // absence is not proof
}
```

Every rejection returns the same `401 {"error":"unauthorized"}` regardless of cause, so the guard does not tell a prober whether a login exists, is allow-listed, or merely expired. The distinct 403 is limited to the CSRF case, which is not an identity oracle.

- [ ] **Step 5: Write the Crow adapter**

```cpp
// include/puzzpool/admin_guard.hpp
std::optional<crow::response> adminGuard(const Config& cfg, const crow::request& req);
```

```cpp
std::optional<crow::response> adminGuard(const Config& cfg, const crow::request& req) {
    AdminRequestView view;
    view.isPost           = (req.method == crow::HTTPMethod::Post);
    view.adminTokenHeader = req.get_header_value("X-Admin-Token");
    view.cookieHeader     = req.get_header_value("Cookie");
    view.origin           = req.get_header_value("Origin");
    view.secFetchSite     = req.get_header_value("Sec-Fetch-Site");

    const auto decision = authorizeAdmin(cfg, view, nowUnixSeconds());
    if (decision.allowed) return std::nullopt;

    crow::response r;
    r.code = decision.statusCode;
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    r.body = nlohmann::json({{"error", decision.error}}).dump();
    return r;
}
```

Add two adapter-level cases to `tests/test_admin_auth.cpp` — one denied (asserting 401, the JSON body, and that the body contains neither header value) and one allowed (`std::nullopt`) — built with `crow::request::add_header()`.

- [ ] **Step 6: Rewire `main.cpp` — all six routes, named**

Delete the lambda at `src/main.cpp:73-82` and call the shared guard. This is the one part of slice A that is structurally untestable from Catch2: `main.cpp` is outside `puzzpool_core` (`CMakeLists.txt:41-61`, `tests/CMakeLists.txt:6` links `puzzpool_core` only), and the six route bodies are near-identical copies, so a missed edit leaves a route unguarded while every test binary stays green. `test_handler_validation` will not catch it either — it calls `PoolService` methods directly, below the guard. Hence the explicit table, and hence Step 6a.

| Route | Method | `src/main.cpp` | Handler |
|---|---|---|---|
| `/api/v1/admin/activate-puzzle` | POST | `:84-88` | `handleActivatePuzzle(req)` |
| `/api/v1/admin/set-puzzle` | POST | `:90-94` | `handleSetPuzzle(req)` |
| `/api/v1/admin/set-test-chunk` | POST | `:96-100` | `handleSetTestChunk(req)` |
| `/api/v1/admin/puzzles` | GET | `:102-106` | `handleAdminPuzzles()` |
| `/api/v1/admin/reclaim` | POST | `:108-112` | `handleAdminReclaim()` |
| `/api/v1/admin/import-ranges` | POST | `:114-118` | `handleImportRanges(req)` |

Every one of the six gets the same first line; nothing else in the bodies changes:

```cpp
#include <puzzpool/admin_guard.hpp>

        CROW_ROUTE(app, "/api/v1/admin/activate-puzzle").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(cfg, req)) return std::move(*denied);
            return service.handleActivatePuzzle(req);
        });
```

Before moving on, confirm mechanically that no route kept the old lambda and that the lambda is gone:

```bash
grep -c 'adminGuard(cfg, req)' src/main.cpp     # expect 6
grep -n 'cfg.adminToken.empty()' src/main.cpp   # expect no output
```

`/api/v1/admin/activate-puzzle` deserves particular care: `deploy/nginx.conf:37-46` deliberately publishes it to the internet with no IP restriction, so a miss there is a publicly reachable unauthenticated mutation — precisely the hole this issue exists to close.

- [ ] **Step 6a: Commit a six-route regression script (review finding M2)**

The grep above is a one-time check; this is the regression that survives. Add `tests/test_admin_routes_guarded.sh`, following the shape of the existing `tests/test_check_node_version_age.sh` (plain bash, `PASS`/`FAIL` counters, non-zero exit on failure, no CTest registration — it is run explicitly like the node-age guard is).

The script builds the server, then runs the same loop twice over all six routes:

```bash
ADMIN_ROUTES=(
    "POST /api/v1/admin/activate-puzzle"
    "POST /api/v1/admin/set-puzzle"
    "POST /api/v1/admin/set-test-chunk"
    "GET  /api/v1/admin/puzzles"
    "POST /api/v1/admin/reclaim"
    "POST /api/v1/admin/import-ranges"
)
```

- **Phase 1 — fail closed (AC9).** Start the server with `ADMIN_TOKEN`, `ADMIN_GITHUB_USERS`, and `SESSION_SIGNING_SECRET` all unset. Every route in the list must answer `401`. A route that answers anything else — including `400` for a missing body, which would mean the request reached the handler — is a failure.
- **Phase 2 — sensitivity, and AC12.** Restart with `ADMIN_TOKEN` set to a throwaway value and repeat with `-H "X-Admin-Token: $ADMIN_TOKEN"`. Every route must answer something **other than `401`**. This is the control that makes phase 1 meaningful: without it, a server that 401s unconditionally — or that failed to start at all — would pass phase 1. Assert on "not 401" rather than on `200`, because most of these routes legitimately return `400` for an empty body; the guard, not the handler, is what is under test.

**Phase 2 must fail explicitly when the server is not answering (review finding S7).** "Not 401" is satisfied by the empty string, and a `curl` that cannot connect prints exactly that — so a phase-2 server that never came up turns all six checks green and silently retires the only control phase 1 has. Phase 1 is not exposed to this (it requires the literal `401`), which is precisely why the weakness is easy to miss. Two rules close it:

```bash
# Poll for readiness; never `sleep N` and hope.
wait_for_server() {
    for _ in $(seq 1 50); do
        # /api/v1/stats (src/main.cpp:38) is unauthenticated, so readiness never
        # depends on the guard under test. There is no /health route in this repo.
        if curl -fsS -o /dev/null "http://127.0.0.1:${PORT}/api/v1/stats"; then return 0; fi
        sleep 0.2
    done
    echo "FAIL: server did not become ready on port ${PORT}"
    exit 1
}

# A transport failure is a script failure, never a passing "not 401".
status="$(curl -s -o /dev/null -w '%{http_code}' -X "${method}" "${url}" ...)" || {
    echo "FAIL: ${method} ${path} — curl transport error"
    FAILURES=$((FAILURES + 1))
    continue
}
case "${status}" in
    ''|000) echo "FAIL: ${method} ${path} — no HTTP status (server down?)"
            FAILURES=$((FAILURES + 1)) ;;
    401)    echo "FAIL: ${method} ${path} — still 401 with a valid X-Admin-Token"
            FAILURES=$((FAILURES + 1)) ;;
    *)      echo "PASS: ${method} ${path} — ${status}" ;;
esac
```

Apply the same readiness poll before phase 1, and use an existing unauthenticated route for the probe so readiness never depends on the guard under test.

Use a throwaway `ADMIN_TOKEN` value generated in the script, a temporary `DB_PATH` under `mktemp -d`, and a non-default port. Never a real credential, and remove the temporary database on exit via `trap`.

This is what replaces the previous AC8 matrix row, which claimed only that "the guard tests exist at all" — true, and silent about whether `main.cpp` calls the guard on each route.

And print the diagnostics beside the existing startup lines at `src/main.cpp:138-140`:

```cpp
        for (const auto& line : puzzpool::startupAuthDiagnostics(cfg)) {
            std::cerr << "[puzzpool-cpp] " << line << "\n";
        }
```

- [ ] **Step 7: Verify the guard and the untouched handlers**

Run: `ctest --test-dir build --output-on-failure --tests-regex "admin_auth|handler"`

Expected: the full matrix passes and `test_handler_validation`'s admin-handler cases are unchanged — they call `PoolService` methods directly, below the guard, which is why extraction does not disturb them.

Run: `bash tests/test_admin_routes_guarded.sh`

Expected: 12 checks pass — six routes denied with no mechanism configured, six admitted past the guard with `X-Admin-Token`.

- [ ] **Step 7a: Prove the six-route script can fail (sensitivity)**

Temporarily delete the `adminGuard` line from **one** route body — use `/api/v1/admin/reclaim`, not the one the old smoke test happened to cover — rebuild, and re-run the script.

Expected: **phase 1 fails on exactly that route** and the script exits non-zero. If it still passes, the loop is not reaching that route and the script is not evidence for AC8. Restore the line, rebuild, confirm green, and record both runs in the implementation report.

- [ ] **Step 8: Commit**

```bash
git add include/puzzpool/admin_auth.hpp src/admin_auth.cpp \
        include/puzzpool/admin_guard.hpp src/admin_guard.cpp \
        src/main.cpp tests/test_admin_auth.cpp tests/test_admin_routes_guarded.sh \
        tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: extract a fail-closed admin guard into puzzpool_core"
```

---

### Task 5: libcurl GitHub client seam

**Files:**
- Add: `include/puzzpool/github_client.hpp`, `src/github_client.cpp`
- Modify: `CMakeLists.txt`, `.github/workflows/ci.yml`, `deps.txt`

- [ ] **Step 1: Declare the seam**

```cpp
struct HttpResult {
    long        status = 0;      // 0 means the request never completed
    std::string body;
    bool        transportError = false;
};

/// Injectable HTTP seam. Tests supply fakes; production supplies libcurl.
struct GitHubHttpClient {
    std::function<HttpResult(const std::string& url,
                             const std::string& formBody,
                             const std::vector<std::string>& headers)> postForm;
    std::function<HttpResult(const std::string& url,
                             const std::vector<std::string>& headers)> get;

    bool valid() const { return static_cast<bool>(postForm) && static_cast<bool>(get); }
};

GitHubHttpClient makeLibcurlGitHubClient(int timeoutSeconds = 10);
```

This mirrors `PoolService::AddressStatusFetcher` (`include/puzzpool/service.hpp:27-28`), so the codebase keeps one seam idiom rather than two.

- [ ] **Step 2: Implement it over libcurl**

Requirements the implementation must satisfy, each of them the reason the story asked for this seam:

- `curl_global_init(CURL_GLOBAL_DEFAULT)` exactly once behind `std::call_once`.
- The client secret and the OAuth code go in `CURLOPT_POSTFIELDS` — never in the URL, never in a header, and never through `popen`/`system`. This is what makes AC2's "never appear in a shell command or process arguments" structurally true rather than a convention.
- `CURLOPT_SSL_VERIFYPEER = 1`, `CURLOPT_SSL_VERIFYHOST = 2`, `CURLOPT_FOLLOWLOCATION = 0`, `CURLOPT_TIMEOUT = timeoutSeconds`, `CURLOPT_NOSIGNAL = 1`.
- No `CURLOPT_VERBOSE`, no logging of URL, headers, request body, or response body — a verbose handle would print the bearer token.
- Free the `curl_slist` and the easy handle on every path, including error paths.
- Return `{0, "", true}` on any `CURLcode != CURLE_OK`; the caller distinguishes "provider said no" from "we never reached the provider".

- [ ] **Step 3: Wire the dependency**

```cmake
find_package(CURL REQUIRED)
target_link_libraries(puzzpool_core PUBLIC CURL::libcurl)
```

`.github/workflows/ci.yml:21` — append `libcurl4-openssl-dev` to the `apt-get install` list.
`deps.txt` — add `libcurl4-openssl-dev` to the Linux line and `curl` to the brew line.

- [ ] **Step 4: Confirm the project still configures and builds**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUZZPOOL_BUILD_TESTS=ON`

Expected: `find_package(CURL)` resolves; the summary block still prints.

Run: `cmake --build build --parallel "${BUILD_JOBS:-1}"`

Expected: `puzzpool_core` links against libcurl with no `-Wconversion`/`-Wshadow` warnings from the new file.

- [ ] **Step 5: Commit**

```bash
git add include/puzzpool/github_client.hpp src/github_client.cpp \
        CMakeLists.txt .github/workflows/ci.yml deps.txt
git commit -m "feat: add an injectable libcurl-backed GitHub HTTP client"
```

---

### Task 6: The four `/api/v1/auth/*` routes

**Files:**
- Add: `include/puzzpool/auth_service.hpp`, `src/auth_service.cpp`, `tests/test_auth_service.cpp`
- Modify: `src/main.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Declare `AuthService` with every non-determinism injected**

```cpp
class AuthService {
public:
    using Clock       = std::function<int64_t()>;                 // unix seconds
    using NonceSource = std::function<std::string(std::size_t)>;  // raw random bytes

    explicit AuthService(const Config& cfg,
                         GitHubHttpClient client = {},
                         Clock clock = {},
                         NonceSource nonce = {});

    crow::response handleGithubLogin(const crow::request& req);
    crow::response handleGithubCallback(const crow::request& req);
    crow::response handleLogout(const crow::request& req);
    crow::response handleAuthMe(const crow::request& req);

private:
    const Config      cfg_;
    GitHubHttpClient  http_;
    Clock             clock_;
    NonceSource       nonce_;
};
```

Defaults: `makeLibcurlGitHubClient()`, `std::chrono::system_clock` seconds, `secureRandomBytes`. Every test injects all three, so no test touches the network or the wall clock.

- [ ] **Step 2: Write the failing route tests**

```cpp
namespace {

struct FakeGitHub {
    std::string lastPostUrl, lastPostBody, lastGetUrl;
    std::vector<std::string> lastPostHeaders, lastGetHeaders;
    HttpResult tokenResponse{200, R"({"access_token":"gho_fake","token_type":"bearer"})", false};
    HttpResult userResponse{200, R"({"login":"alice","id":4242})", false};

    GitHubHttpClient client() {
        return {
            [this](const std::string& url, const std::string& body,
                   const std::vector<std::string>& h) {
                lastPostUrl = url; lastPostBody = body; lastPostHeaders = h;
                return tokenResponse;
            },
            [this](const std::string& url, const std::vector<std::string>& h) {
                lastGetUrl = url; lastGetHeaders = h;
                return userResponse;
            },
        };
    }
};

Config oauthCfg() {
    Config cfg = /* authCfg() from test_admin_auth, duplicated locally */;
    cfg.githubOauthClientId = "client-id";
    cfg.githubOauthClientSecret = "client-secret";
    cfg.publicBaseUrl = "https://puzzle.b58.de";
    return cfg;
}

// Returns the first Set-Cookie header whose cookie name is `name`, or "" when absent.
//
// This body is NOT mechanical and must not use crow::response::get_header_value()
// (review finding S2): that returns only the FIRST match, and the callback deliberately
// emits two Set-Cookie headers — one clearing pp_oauth_state, one issuing pp_session.
// Using it would make roughly half of the assertions below pass or fail for the wrong
// reason. crow::response::headers is a ci_map, i.e. std::unordered_multimap
// (third_party/crow/include/crow/ci_map.h:42), so scan equal_range.
std::string setCookieNamed(crow::response& r, const std::string& name) {
    const std::string prefix = name + "=";
    const auto range = r.headers.equal_range("Set-Cookie");
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.rfind(prefix, 0) == 0) return it->second;
    }
    return "";
}

} // namespace

// ── AC13 / AC26: unconfigured means 503, never a silently-unsigned cookie ──

TEST_CASE("the auth routes return 503 without SESSION_SIGNING_SECRET", "[auth][config]") {
    Config cfg = oauthCfg();
    cfg.sessionSigningSecret.clear();
    FakeGitHub gh;
    AuthService svc{cfg, gh.client(), [] { return kNow; }, fixedNonce};

    CHECK(svc.handleGithubLogin(crow::request{}).code == 503);
    CHECK(svc.handleGithubCallback(crow::request{}).code == 503);
    CHECK(svc.handleAuthMe(crow::request{}).code == 503);
    // handleLogout is deliberately excluded (review finding O5): clearing a cookie needs
    // no signing key, and 503 here would strand a stale cookie in the browser exactly
    // when the secret has been rotated away. It still enforces same-origin, so a bare
    // request with no origin proof is 403, never 503 — covered below.
    CHECK(svc.handleLogout(crow::request{}).code == 403);
}

// Sensitivity for the three 503s above: the same probes must not return 503 once the
// secret is present, so the assertion is detecting configuration and not a route that
// is 503 unconditionally.
TEST_CASE("the same probes leave 503 behind once the secret is set", "[auth][config]") {
    FakeGitHub gh;
    AuthService svc{oauthCfg(), gh.client(), [] { return kNow; }, fixedNonce};
    CHECK(svc.handleGithubLogin(crow::request{}).code != 503);
    CHECK(svc.handleGithubCallback(crow::request{}).code != 503);
    CHECK(svc.handleAuthMe(crow::request{}).code != 503);
}

TEST_CASE("login returns 503 when the OAuth app is not configured", "[auth][config]") {
    Config cfg = oauthCfg();
    cfg.githubOauthClientId.clear();
    FakeGitHub gh;
    AuthService svc{cfg, gh.client(), [] { return kNow; }, fixedNonce};
    CHECK(svc.handleGithubLogin(crow::request{}).code == 503);
}

// ── AC1: unguessable, browser-bound, single-use state ──

// NOTE on `auto r` rather than `const auto r` throughout this file (review finding S2):
// crow::response::get_header_value is NOT const (third_party/crow/include/crow/
// http_response.h:76, unlike crow::request's at http_request.h:81), and setCookieNamed
// above takes a non-const reference. A `const auto r` local does not compile here, and
// a temporary cannot bind to setCookieNamed's parameter — so every response under test
// is a named non-const local.

TEST_CASE("login redirects to GitHub and binds the state to a cookie", "[auth][oauth]") {
    auto r = loginService().handleGithubLogin(crow::request{});
    REQUIRE(r.code == 302);
    const std::string location = r.get_header_value("Location");
    CHECK(location.rfind("https://github.com/login/oauth/authorize?", 0) == 0);
    CHECK(location.find("client_id=client-id") != std::string::npos);
    CHECK(location.find("state=") != std::string::npos);

    const std::string stateCookie = setCookieNamed(r, "pp_oauth_state");
    REQUIRE_FALSE(stateCookie.empty());
    CHECK(stateCookie.find("HttpOnly") != std::string::npos);
    CHECK(stateCookie.find("Secure") != std::string::npos);
    CHECK(stateCookie.find("SameSite=Lax") != std::string::npos);
    // Path is asserted here as well as on the clearing header (S3), because the two
    // must match for the clear to have any effect at all.
    CHECK(stateCookie.find("Path=/api/v1/auth") != std::string::npos);
    // The state in the URL must be bound to, and not equal to, the signed cookie blob.
    //
    // Bound: verify the cookie blob and compare its SUBJECT to the URL parameter — do
    // NOT substring-search the cookie for the parameter (review finding S5). Step 4 mints
    // the nonce as base64UrlEncode(nonce_(32)) and puts the BARE nonce in the URL, while
    // signingInput base64url-encodes the subject field a second time. Base64-encoding
    // ASCII does not leave the input as a substring of its output, and the base64url
    // alphabet is already URL-safe so no percent-encoding brings the two back into
    // alignment — a `find` here would fail against a perfectly correct handler.
    //
    // This assertion is AC1's only login-side proof of browser binding. The callback
    // cases cannot supply it: validState() builds the cookie/parameter pair by hand as a
    // consistent pair, so it cannot detect a handler that writes the wrong value into
    // either side. Do not delete this check if it fails — fix the handler.
    const auto issuedState = verifyStateToken(oauthCfg().sessionSigningSecret,
                                              cookieTokenOf(stateCookie), kNow);
    REQUIRE(issuedState.error == SessionError::None);
    CHECK(issuedState.identity.login == stateParamOf(location));
    // Not equal: the signed blob must stay in the cookie. Leaking it into the redirect
    // URL would hand it to GitHub's logs and to any Referer on the way.
    CHECK(cookieTokenOf(stateCookie) != stateParamOf(location));
}

TEST_CASE("the login URL never carries the client secret (AC22)", "[auth][security]") {
    auto r = loginService().handleGithubLogin(crow::request{});
    CHECK(r.get_header_value("Location").find("client-secret") == std::string::npos);
    CHECK(r.body.find("client-secret") == std::string::npos);
}

TEST_CASE("two logins produce different state values", "[auth][oauth]") {
    auto svc = loginServiceWithRealRandom();
    CHECK(stateParamOf(svc.handleGithubLogin(crow::request{}).get_header_value("Location")) !=
          stateParamOf(svc.handleGithubLogin(crow::request{}).get_header_value("Location")));
}

// ── AC2 / AC3: the callback ──

TEST_CASE("a valid callback issues a hardened session cookie", "[auth][oauth]") {
    FakeGitHub gh;
    auto svc = callbackService(gh);
    auto r = svc.handleGithubCallback(callbackRequest("the-code", validState()));

    REQUIRE(r.code == 302);
    CHECK(r.get_header_value("Location") == "/");

    const std::string cookie = setCookieNamed(r, "pp_session");
    REQUIRE_FALSE(cookie.empty());
    CHECK(cookie.find("HttpOnly") != std::string::npos);
    CHECK(cookie.find("Secure") != std::string::npos);
    CHECK(cookie.find("SameSite=Lax") != std::string::npos);
    CHECK(cookie.find("Path=/") != std::string::npos);

    const auto session = verifySessionToken(oauthCfg().sessionSigningSecret,
                                            cookieTokenOf(cookie), kNow);
    REQUIRE(session.error == SessionError::None);
    CHECK(session.identity.login == "alice");
    CHECK(session.identity.githubId == "4242");   // AC26/D18 — the id is in the payload
}

TEST_CASE("the code and secret travel in the POST body, not the URL (AC2)", "[auth][security]") {
    FakeGitHub gh;
    callbackService(gh).handleGithubCallback(callbackRequest("the-code", validState()));

    CHECK(gh.lastPostUrl == "https://github.com/login/oauth/access_token");
    CHECK(gh.lastPostUrl.find("client-secret") == std::string::npos);
    CHECK(gh.lastPostUrl.find("the-code") == std::string::npos);
    CHECK(gh.lastPostBody.find("client_secret=client-secret") != std::string::npos);
    CHECK(gh.lastPostBody.find("code=the-code") != std::string::npos);
    CHECK(gh.lastGetUrl == "https://api.github.com/user");
    CHECK(joined(gh.lastGetHeaders).find("Authorization: Bearer gho_fake") != std::string::npos);
}

// AC1 single-use. A clearing Set-Cookie removes nothing unless its name, Path and Domain
// all match the issuing header (RFC 6265) — so `Max-Age=0` alone is not evidence that
// the cookie is gone (review finding S3). The state cookie is issued with
// Path=/api/v1/auth, so a clearing header emitted with Path=/ or with no Path at all
// leaves the original in the browser and the state blob stays replayable for its full
// 600-second window, with this assertion still green. Assert the whole attribute set.
TEST_CASE("the state cookie is cleared on every callback, making it single-use", "[auth][oauth]") {
    FakeGitHub gh;
    auto r = callbackService(gh).handleGithubCallback(callbackRequest("c", validState()));
    const std::string cleared = setCookieNamed(r, "pp_oauth_state");
    REQUIRE_FALSE(cleared.empty());
    CHECK(cleared.find("Max-Age=0") != std::string::npos);
    CHECK(cleared.find("Path=/api/v1/auth") != std::string::npos);
    CHECK(cleared.find("HttpOnly") != std::string::npos);
    CHECK(cleared.find("Secure") != std::string::npos);
    CHECK(cookieTokenOf(cleared).empty());
}

TEST_CASE("callbacks that fail state validation issue no session", "[auth][oauth][security]") {
    struct Case { std::string name; crow::request req; };
    FakeGitHub gh;
    for (auto& c : std::vector<Case>{
             {"no state cookie",   callbackRequestNoCookie("c", validState())},
             {"state mismatch",    callbackRequest("c", "some-other-nonce")},
             {"missing state param", callbackRequest("c", "")},
             {"missing code",      callbackRequest("", validState())},
             {"expired state",     expiredStateRequest()},
             {"tampered state cookie", tamperedStateRequest()},
             {"session blob as state", sessionBlobAsStateRequest()},   // S1
         }) {
        INFO(c.name);
        auto r = callbackService(gh).handleGithubCallback(c.req);
        CHECK(setCookieNamed(r, "pp_session").empty());
        CHECK(r.get_header_value("Location").find("auth=error") != std::string::npos);
        // The state cookie is cleared even on the failure paths — that is what stops a
        // failed attempt from leaving a reusable state blob behind.
        CHECK(setCookieNamed(r, "pp_oauth_state").find("Path=/api/v1/auth") != std::string::npos);
    }
}

TEST_CASE("provider failures issue no session", "[auth][oauth]") {
    for (auto response : {HttpResult{401, R"({"error":"bad_verification_code"})", false},
                          HttpResult{200, R"({"error":"bad_verification_code"})", false},
                          HttpResult{200, "not json", false},
                          HttpResult{0, "", true}}) {
        FakeGitHub gh;
        gh.tokenResponse = response;
        auto r = callbackService(gh).handleGithubCallback(
            callbackRequest("c", validState()));
        CHECK(setCookieNamed(r, "pp_session").empty());
    }

    FakeGitHub badUser;
    badUser.userResponse = {403, R"({"message":"Forbidden"})", false};
    auto forbidden = callbackService(badUser).handleGithubCallback(
        callbackRequest("c", validState()));
    CHECK(setCookieNamed(forbidden, "pp_session").empty());

    FakeGitHub noLogin;
    noLogin.userResponse = {200, R"({"id":1})", false};   // login missing
    auto missingLogin = callbackService(noLogin).handleGithubCallback(
        callbackRequest("c", validState()));
    CHECK(setCookieNamed(missingLogin, "pp_session").empty());
}

// Sensitivity for every `setCookieNamed(r, "pp_session").empty()` above: the same probe
// must find a cookie on the success path, or it is asserting that the helper is broken.
TEST_CASE("the no-session probe finds a session on the success path", "[auth][oauth]") {
    FakeGitHub gh;
    auto r = callbackService(gh).handleGithubCallback(callbackRequest("c", validState()));
    CHECK_FALSE(setCookieNamed(r, "pp_session").empty());
}

TEST_CASE("a failed callback never leaks the code, token, or secret (AC22)", "[auth][security]") {
    FakeGitHub gh;
    gh.tokenResponse = {401, R"({"error":"bad_verification_code","hint":"client-secret"})", false};
    auto r = callbackService(gh).handleGithubCallback(callbackRequest("the-code", validState()));
    for (const std::string secret : {"client-secret", "the-code", "gho_fake"}) {
        CHECK(r.body.find(secret) == std::string::npos);
        CHECK(r.get_header_value("Location").find(secret) == std::string::npos);
    }
}

// ── AC5: logout ──

TEST_CASE("logout expires the session cookie", "[auth][logout]") {
    auto r = authedService().handleLogout(sameOriginPost(validSessionCookie()));
    CHECK(r.code == 200);
    const std::string cookie = setCookieNamed(r, "pp_session");
    REQUIRE_FALSE(cookie.empty());
    CHECK(cookie.find("Max-Age=0") != std::string::npos);
    CHECK(cookie.find("HttpOnly") != std::string::npos);
    // S3: the session cookie is issued with Path=/, so the clearing header must carry
    // Path=/ too or the browser keeps the original and logout silently does nothing.
    CHECK(cookie.find("Path=/;") != std::string::npos);
    CHECK(cookie.find("Secure") != std::string::npos);
    CHECK(cookieTokenOf(cookie).empty());
}

TEST_CASE("a cross-site logout POST is rejected", "[auth][logout][csrf]") {
    crow::request req = sameOriginPost(validSessionCookie());
    req.headers.clear();
    req.add_header("Cookie", "pp_session=" + validSessionToken());
    req.add_header("Origin", "https://evil.example");
    CHECK(authedService().handleLogout(req).code == 403);
}

// O5: a stale cookie must always be sheddable. After a secret rotation or an OAuth
// teardown, the one request that clears the stale cookie is the one that would have
// stopped working under the old 503 rule.
TEST_CASE("logout still clears the cookie when auth is unconfigured", "[auth][logout]") {
    Config cfg = oauthCfg();
    cfg.sessionSigningSecret.clear();
    auto r = serviceWith(cfg).handleLogout(sameOriginPost(validSessionCookie()));
    CHECK(r.code == 200);
    CHECK(setCookieNamed(r, "pp_session").find("Max-Age=0") != std::string::npos);
}

// ── AC26: /auth/me ──

TEST_CASE("me returns the identity and an id-derived avatar for a valid cookie", "[auth][me]") {
    auto r = authedService().handleAuthMe(getWithCookie(validSessionCookie()));
    REQUIRE(r.code == 200);
    const auto body = nlohmann::json::parse(r.body);
    CHECK(body["authenticated"] == true);
    CHECK(body["login"] == "alice");
    CHECK(body["avatar_url"] == "https://avatars.githubusercontent.com/u/4242?v=4");
    CHECK(body["is_admin"] == true);
}

TEST_CASE("me reports signed out for every invalid cookie shape", "[auth][me]") {
    for (const std::string cookie : {"", "pp_session=", "pp_session=garbage",
                                     "pp_session=" + tamperedSessionToken(),
                                     "pp_session=" + expiredSessionToken()}) {
        auto r = authedService().handleAuthMe(getWithRawCookie(cookie));
        INFO(cookie);
        CHECK(r.code == 200);
        const auto body = nlohmann::json::parse(r.body);
        CHECK(body["authenticated"] == false);
        CHECK_FALSE(body.contains("login"));
        CHECK_FALSE(body.contains("avatar_url"));
    }
}

TEST_CASE("me distinguishes a signed-in non-admin from an admin", "[auth][me]") {
    Config cfg = oauthCfg();
    cfg.adminGithubUsers = {"someone-else"};
    const auto body = nlohmann::json::parse(
        serviceWith(cfg).handleAuthMe(getWithCookie(validSessionCookie())).body);
    CHECK(body["authenticated"] == true);
    CHECK(body["is_admin"] == false);
}

TEST_CASE("me never echoes the cookie or the signing secret (AC22)", "[auth][me][security]") {
    auto r = authedService().handleAuthMe(getWithCookie(validSessionCookie()));
    CHECK(r.body.find(validSessionToken()) == std::string::npos);
    CHECK(r.body.find(oauthCfg().sessionSigningSecret) == std::string::npos);
}
```

Build `crow::request` fixtures with `req.add_header("Cookie", ...)` and
`req.url_params = crow::query_string("?code=...&state=...")` — both verified against the
bundled Crow (`third_party/crow/include/crow/http_request.h:74-85`,
`query_string.h:345,381`). `sessionBlobAsStateRequest()` puts a valid `pp_session` blob
into the `pp_oauth_state` cookie with a matching `state` parameter; it is the end-to-end
form of the S1 domain-separation cases in `test_session`.

- [ ] **Step 3: Register and confirm failure**

```cmake
add_puzzpool_test(test_auth_service)
```

Run: `ctest --test-dir build --output-on-failure --tests-regex auth_service`

Expected: fails to compile — `AuthService` does not exist.

- [ ] **Step 4: Implement the handlers**

Shape each one as: configuration gate → parse → verify → act → serialise. Notes that matter:

- **login** — 503 if `sessionSigningSecret`, `githubOauthClientId`, `githubOauthClientSecret`, or `publicBaseUrl` is empty. Nonce is `base64UrlEncode(nonce_(32))`. The `pp_oauth_state` cookie holds `issueStateToken(secret, nonce, now + 600)`; the URL's `state` parameter is the bare nonce. The signature makes the cookie unforgeable, the cookie makes the state browser-bound, and comparing the two in the callback is what closes the login-CSRF hole. Scope is empty (`scope=`) per AC1's "no-scope". URL-encode every parameter.
- **callback** — 503 on missing configuration. Then: read `code`/`state` via `req.url_params.get()` (returns `char*`, so null-check); read `pp_oauth_state`; `verifyStateToken` it — not `verifySessionToken`, which is what the `WrongPurpose` separation from D-A3 buys; `constantTimeEquals(nonceFromCookie, stateParam)`. Always emit the state-clearing `Set-Cookie`, on success and failure alike — that is what makes it single-use. Exchange with `Accept: application/json` and body `client_id=…&client_secret=…&code=…&redirect_uri=…`, all URL-encoded. Treat a 200 that contains `error` and no `access_token` as failure (GitHub does this). Then `GET https://api.github.com/user` with `Authorization: Bearer …`, `Accept: application/vnd.github+json`, `User-Agent: puzzpool`. Require a non-empty string `login` and an integer `id`. Issue `pp_session` with `now + sessionTtlMinutes * 60`. `nlohmann::json::parse` must use the non-throwing overload or be wrapped — a malformed provider body must not 500.
- **logout** — **no configuration gate** (review finding O5): clearing a cookie needs no signing key, and returning 503 would strand a stale cookie in the browser at exactly the moment the secret was rotated away or the OAuth app was torn down — the one request that sheds it would be the one that stops working. Reject without same-origin proof (403, reusing `hasSameOriginProof`); otherwise emit the clearing cookie and `200 {"authenticated":false}`, configured or not. Clearing does not require a valid session either.
- **me** — 503 when unconfigured; otherwise always 200. `{"authenticated":false}` for every failure mode, with no `reason` field: distinguishing "expired" from "forged" tells an attacker which half of the token to work on.

**Cookies come from one helper, in both directions (review finding S3).** RFC 6265 removes a cookie only when the clearing `Set-Cookie` matches the original's name, `Path` and `Domain`; a clear emitted with a different `Path` leaves the original in place and looks successful from the server side. Hand-writing the issuing and clearing strings separately is how they drift, so define the attribute set once per cookie and derive both:

```cpp
struct CookieSpec { std::string_view name; std::string_view path; };
constexpr CookieSpec kSessionCookie{"pp_session", "/"};
constexpr CookieSpec kStateCookie{"pp_oauth_state", "/api/v1/auth"};

std::string cookieHeader(const CookieSpec& spec, std::string_view value, long maxAgeSeconds) {
    return std::string(spec.name) + "=" + std::string(value)
         + "; Path=" + std::string(spec.path)
         + "; Max-Age=" + std::to_string(maxAgeSeconds)
         + "; HttpOnly; Secure; SameSite=Lax";
}
```

Issuing is `cookieHeader(kSessionCookie, token, ttlSeconds)` / `cookieHeader(kStateCookie, blob, 600)`; clearing is the same call with an empty value and `0`. Emit with `res.add_header("Set-Cookie", …)` — not `set_header` — because the callback emits two (`http_response.h:60,69`).

- [ ] **Step 5: Wire the routes in `main.cpp`**

```cpp
        puzzpool::AuthService auth(cfg);

        CROW_ROUTE(app, "/api/v1/auth/github/login").methods(crow::HTTPMethod::GET)
        ([&auth](const crow::request& req) { return auth.handleGithubLogin(req); });

        CROW_ROUTE(app, "/api/v1/auth/github/callback").methods(crow::HTTPMethod::GET)
        ([&auth](const crow::request& req) { return auth.handleGithubCallback(req); });

        CROW_ROUTE(app, "/api/v1/auth/logout").methods(crow::HTTPMethod::POST)
        ([&auth](const crow::request& req) { return auth.handleLogout(req); });

        CROW_ROUTE(app, "/api/v1/auth/me").methods(crow::HTTPMethod::GET)
        ([&auth](const crow::request& req) { return auth.handleAuthMe(req); });
```

- [ ] **Step 6: Verify**

Run: `ctest --test-dir build --output-on-failure --tests-regex auth_service`

Expected: every route case passes, including all provider-failure and leak checks.

Run: `ctest --test-dir build --output-on-failure`

Expected: the whole backend suite is green.

- [ ] **Step 7: Commit**

```bash
git add include/puzzpool/auth_service.hpp src/auth_service.cpp src/main.cpp \
        tests/test_auth_service.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add GitHub OAuth login, callback, logout, and identity routes"
```

---

### Task 7: Documentation, configuration surface, and the upgrade warning

**Files:**
- Modify: `.env.example`, `README.md`, `docs/api.md`, `docs/security.md`, `docs/architecture.md`, `docs/architecture-review.md`, `docs/testing.md`, `deploy/nginx.conf`, `.github/workflows/ci.yml` (Step 7a)

- [ ] **Step 1: `.env.example` — remove the fail-open guidance (AC10) and add the new settings (AC21)**

Replace the block at `.env.example:33-37`:

```bash
# Admin authentication. Configure AT LEAST ONE of ADMIN_TOKEN or ADMIN_GITHUB_USERS.
# If both are empty, every /api/v1/admin/* route returns 401 — admin access is closed,
# not open. (This changed in the GitHub sign-in release; see the README upgrade note.)
ADMIN_TOKEN=

# Comma-separated GitHub logins allowed to administer the pool. Case-insensitive.
# An empty list grants nobody access.
ADMIN_GITHUB_USERS=

# Signs the session cookie. Required for GitHub sign-in; without it /api/v1/auth/*
# returns 503 and no session cookie is issued or accepted. Never reuse ADMIN_TOKEN.
#   SESSION_SIGNING_SECRET=$(openssl rand -hex 32)
SESSION_SIGNING_SECRET=

# GitHub OAuth application for THIS stage only. PROD and TEST use separate OAuth apps;
# never place both stages' credentials in one environment.
GITHUB_OAUTH_CLIENT_ID=
GITHUB_OAUTH_CLIENT_SECRET=

# Absolute public base URL of this deployment. Builds the OAuth callback URL and is the
# expected Origin for admin CSRF checks. Must match the OAuth app's callback URL host.
PUBLIC_BASE_URL=https://puzzle.b58.de

# Session lifetime in minutes (absolute expiry; no sliding renewal).
SESSION_TTL_MINUTES=720
```

- [ ] **Step 2: `README.md` — the upgrade warning (AC10) and the env table (AC21)**

Add rows to the table at `README.md:112` for `ADMIN_GITHUB_USERS`, `SESSION_SIGNING_SECRET`, `GITHUB_OAUTH_CLIENT_ID`, `GITHUB_OAUTH_CLIENT_SECRET`, `PUBLIC_BASE_URL`, `SESSION_TTL_MINUTES`, correct the `ADMIN_TOKEN` row, and add a prominent block near the configuration section:

```md
> **⚠️ Upgrade warning — admin routes now fail closed.**
> Earlier releases treated an empty `ADMIN_TOKEN` as "authentication disabled", so admin
> routes were reachable by anyone who could reach the port. They now return `401` unless
> `ADMIN_TOKEN` or `ADMIN_GITHUB_USERS` is configured. Before upgrading, set at least one.
> The server prints a warning naming both variables at startup when neither is set.
>
> This matters most for `/api/v1/admin/activate-puzzle`, which `deploy/nginx.conf`
> deliberately exposes to the internet with only the server-side check behind it.
>
> GitHub sign-in additionally needs `SESSION_SIGNING_SECRET`, `GITHUB_OAUTH_CLIENT_ID`,
> `GITHUB_OAUTH_CLIENT_SECRET`, and `PUBLIC_BASE_URL`. Because the session cookie is
> `Secure`, sign-in works only over HTTPS — use `ADMIN_TOKEN` for local development.
```

Also document creating the two GitHub OAuth apps (PROD and TEST) with callback URL
`<PUBLIC_BASE_URL>/api/v1/auth/github/callback` and no scopes.

- [ ] **Step 3: `docs/api.md` — a new Authentication API section**

Before `## Admin API` (`docs/api.md:422`), document all four routes: method, path, parameters, redirect behavior, `Set-Cookie` attributes including `Path` (without any example token value), the `/auth/me` response shape for both states, and the 503-when-unconfigured rule — stating explicitly that `POST /api/v1/auth/logout` is exempt from it and always clears the cookie, so a stale cookie can be shed after a secret rotation. Then extend the Admin API preamble with the two accepted mechanisms, the `401` fail-closed rule applying to all six admin routes, and the `403 csrf_check_failed` response for cookie-authorized `POST`s without same-origin proof.

- [ ] **Step 4: `docs/security.md` — rewrite Admin Route Protection**

`docs/security.md:16-50` currently presents "Option A — Nginx IP restriction" as sufficient with a blank token, which this change makes false. It also says "The four admin endpoints" at `:16-17`; there are six (review finding O3) — correct the count while rewriting.

Rewrite to cover: the two mechanisms (token, GitHub sign-in), the fail-closed rule, the allow-list, cookie properties and why each is set, the CSRF defense and why absence of a signal is rejected, constant-time comparison, `SESSION_SIGNING_SECRET` handling, the deliberate non-MAC status of `keyedDigestHex`, and the Nginx layer as remaining defense in depth.

**Revocation must be documented exactly as it behaves (review finding M3).** `src/main.cpp:16` loads `Config` once per process. Write the procedure as:

```md
To revoke an admin's access, remove their login from `ADMIN_GITHUB_USERS` and restart
the service. The change applies to the very next request: no authorization decision is
cached, so there is no cookie to reissue, no session store to purge, and no wait for
outstanding sessions to expire. The restart is required because configuration is read
once at startup.

Rotating `SESSION_SIGNING_SECRET` invalidates every issued session at once and likewise
takes effect at the next restart. An individual session cannot be revoked before its
absolute expiry — that is the accepted trade-off of stateless sessions (ADR-6), and it
is why `SESSION_TTL_MINUTES` is capped at 30 days.
```

Do **not** write "takes effect immediately with no restart". An operator reads this line while revoking a compromised admin under time pressure, and acting on it would leave the compromised login working.

Two further properties belong here as accepted decisions rather than being left for a later reader to discover (review finding S4):

- **Any GitHub account can obtain a valid `pp_session` cookie** from a public deployment. The callback issues the cookie before any allow-list check; only the admin guard and `/auth/me`'s `is_admin` consult the list. This is deliberate — slice B needs to distinguish "signed in" from "signed in and allowed to act" — but it means the signed-cookie surface is open to the internet, not to admins. Authorization, not authentication, is what protects the admin routes.
- **Each `/login` + `/callback` pair costs one synchronous outbound HTTPS call to GitHub** from a Crow worker thread with a 10-second timeout. An anonymous client can drive that loop with a garbage `code`: the state check passes, because they obtained a real state cookie from `/login`, and the exchange runs before GitHub rejects it. With enough concurrency this parks worker threads on a network wait. Not a reason to change slice A's design; record it, and hand the bounding `limit_req` zone to slice C's deployment note.

- [ ] **Step 5: `docs/architecture.md` — modules and dependency direction**

Add the seven new source rows to the table at `docs/architecture.md:52-70`, correct the `src/main.cpp` row (it no longer holds the guard) and the `src/hash_utils.cpp` row (SHA-256, frozen keyed digest, real HMAC), replace the dependency line at `:72-73` with the extended one from this plan's header, and add the auth flow to the component diagram.

- [ ] **Step 6: `docs/architecture-review.md` — two ADRs (AC24)**

```md
### ADR-5: A real HMAC beside the frozen permutation digest

The permutation round function consumed a helper named `hmacSha256Hex` that was actually
`sha256(key || 0x1f || msg)` — length-extension forgeable. Its output cannot change without
reordering allocation for every existing puzzle (ADR-4). Decision: freeze the bytes under
the honest name `keyedDigestHex`, documented as non-MAC, and implement RFC 2104
HMAC-SHA-256 under the `hmacSha256Hex` name for session and state signing. Locked by
RFC 4231 vectors, a byte-stability vector, and a test asserting the two disagree.

### ADR-6: Stateless signed sessions and a fail-closed central guard

Sessions are a signed, absolutely-expiring cookie rather than server-side state: the pool
runs as a single process with one SQLite file, and per-request session rows would add a
write path and a retention obligation for no gain. The trade-off is accepted deliberately:
an individual session cannot be revoked before its absolute expiry.

Revocation is by allow-list removal or by rotating `SESSION_SIGNING_SECRET`. Because the
guard re-reads `ADMIN_GITHUB_USERS` from `Config` on every request and caches no
authorization decision, either change applies to the very next request — no cookie
reissue, no session store to purge. Both still require a service restart, because
`src/main.cpp:16` loads configuration once per process; a live-reload path was considered
and deliberately left out of slice A. `SESSION_TTL_MINUTES` is capped at 30 days so a
misconfiguration cannot mint a credential that outlives any practical rotation cadence.

The guard moved from a lambda in `main.cpp` into `puzzpool_core` so the unconfigured case is
directly testable, and now denies by default instead of returning `nullopt`. Because the
route wiring itself stays in `main.cpp` and is outside every Catch2 target,
`tests/test_admin_routes_guarded.sh` asserts end to end that all six admin routes deny
without configuration and admit with a token — the Catch2 matrix proves the decision, the
script proves it is actually on every route.

Session and OAuth-state blobs share one signed format and one key, so the token's purpose
(`session` / `state`) is part of the signed input. Without that label the two would be
interchangeable; with it, presenting one where the other is expected fails as
`WrongPurpose`.
```

- [ ] **Step 7: `docs/testing.md` and `deploy/nginx.conf`**

Add the four new test binaries to the coverage table with what each covers.

Add `tests/test_admin_routes_guarded.sh` beside the existing `tests/test_check_node_version_age.sh` entry, as a script that is run explicitly rather than through CTest, with its command and what it proves:

```md
### Admin route guard regression

`bash tests/test_admin_routes_guarded.sh` starts the server twice and checks all six
`/api/v1/admin/*` routes end to end: every route returns `401` when neither `ADMIN_TOKEN`
nor `ADMIN_GITHUB_USERS` is configured, and every route is admitted past the guard with a
valid `X-Admin-Token`. Run it whenever an admin route is added, removed, or rewired —
`src/main.cpp` is outside `puzzpool_core`, so no Catch2 target can see the wiring, and a
route that loses its guard leaves the whole suite green.
```

Extend the "Smoke Test (local server)" section at `docs/testing.md:86-98` with the auth checks from Task 8 Step 4.

- [ ] **Step 7a: Wire the guard script into CI (review finding S6)**

M2 asked for a regression that survives, not a checklist item, and a committed script nobody runs is a checklist item. `.github/workflows/ci.yml:58` already establishes the precedent by running `bash tests/test_check_node_version_age.sh`, so add the guard script to the `build` job — it needs the compiled server, which only that job has:

```yaml
      - name: Admin route guard regression
        run: bash tests/test_admin_routes_guarded.sh
```

Place it after the existing `Run unit tests` step (`ci.yml:32-33`) and before `Smoke test`, so a route that loses its guard fails CI rather than waiting for someone to remember the command.

**Track the companion CI fix rather than duplicating it.** Issue #155 / PR #157 adds `dev` to the `push` and `pull_request` triggers and has already demonstrated all three jobs on a `dev`-targeted PR. It remains a separate repository-level fix. Before PR #154 is marked ready for implementation review, bring its branch forward onto a `dev` revision containing #157; then this new guard-script step will execute on PR #154 itself. Do not duplicate #157's trigger edits in this implementation branch. If #157 has not merged, record that dependency explicitly in the implementation report instead of claiming dev-targeted CI coverage.

In `deploy/nginx.conf`, add a comment above `location /` recording that `/api/v1/auth/*` is intentionally public (AC23) and that `X-Forwarded-Proto` must stay set so the deployment terminates TLS in front of the `Secure` cookie. No `location` blocks change.

- [ ] **Step 8: Commit**

```bash
git add .env.example README.md docs/api.md docs/security.md docs/architecture.md \
        docs/architecture-review.md docs/testing.md deploy/nginx.conf
git commit -m "docs: document GitHub sign-in, fail-closed admin auth, and the upgrade"
```

---

### Task 8: Full verification and PR

**Files:** none

- [ ] **Step 1: Full backend suite from a clean configure**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUZZPOOL_BUILD_TESTS=ON
cmake --build build --parallel "${BUILD_JOBS:-1}"
ctest --test-dir build --output-on-failure
```

Expected: 13 test binaries (9 existing + 4 new) all pass. Record the exact pass/fail counts.

- [ ] **Step 2: Frontend regression — slice A changes no frontend file**

Run: `npm test --prefix frontend`
Run: `npm run build --prefix frontend`

Expected: unchanged and green. Any movement here means slice A leaked into slice B's scope.

- [ ] **Step 3: Supply-chain guard**

Run: `bash tests/test_check_node_version_age.sh`

Expected: passes; the guard is untouched.

- [ ] **Step 4: All six admin routes, end to end**

Run: `bash tests/test_admin_routes_guarded.sh`

Expected: 12 checks pass — the six routes of Task 4 Step 6 each denied with `401` when no mechanism is configured, and each admitted past the guard with a valid `X-Admin-Token`. Record the per-route results, not just the exit code.

This is the evidence for AC8 and the end-to-end half of AC9. It is a script rather than a checklist item precisely because `main.cpp` is outside every Catch2 target: the previous draft checked one route by hand, which would not have detected five of the six wiring mistakes it was meant to catch.

- [ ] **Step 5: Local-server smoke test — startup and the auth routes**

Per `docs/testing.md`, start the server and check what the script above does not cover:
- with no auth configured: startup prints the warning naming `ADMIN_TOKEN` and `ADMIN_GITHUB_USERS`;
- with no `SESSION_SIGNING_SECRET`: `GET /api/v1/auth/me` returns `503`;
- with the full OAuth configuration but no cookie: `GET /api/v1/auth/me` returns `200 {"authenticated":false}`;
- `GET /api/v1/auth/github/login` returns `302` to `github.com` with a `pp_oauth_state` cookie, and `POST /api/v1/auth/logout` with an `Origin` matching `PUBLIC_BASE_URL` returns `200` with a `Max-Age=0` cookie — including with `SESSION_SIGNING_SECRET` unset, per O5.

Use a throwaway token value; never a real credential. Delete `pool.db` afterwards.

- [ ] **Step 6: Self-review the diff against AC22 before pushing**

```bash
git diff dev...HEAD | grep -nEi 'client_secret=[^&"]|gho_|ADMIN_TOKEN=[^[:space:]]|SESSION_SIGNING_SECRET=[^[:space:]]'
```

Expected: no hits outside `.env.example` placeholders and the documented `openssl rand` recipes. Confirm no `pool.db`, `.env`, WAL/SHM file, or build artifact is staged.

Also confirm the sensitivity runs from Steps 7a (Task 1) and 7a (Task 4) are recorded and that the deliberate breaks were reverted:

```bash
grep -n 'setw(1)' src/hash_utils.cpp                       # expect no output
grep -c 'adminGuard(cfg, req)' src/main.cpp                # expect 6
```

- [ ] **Step 7: Push and open the implementation PR**

```bash
git push -u origin feature/153-github-oauth-backend
gh pr create --draft --base dev --title "feat: GitHub OAuth backend and fail-closed admin authorization" \
             --body "Implements slice A of #149. Closes #153."
```

---

## Requirement → design → test matrix

| AC | Where it is implemented | Where it is proven |
|----|------------------------|--------------------|
| AC1 state | `auth_service.cpp` login/callback, signed `pp_oauth_state` | `test_auth_service` — bound, distinct, cleared with a matching `Path` (S3), mismatch/expired/tampered/wrong-purpose rejected |
| AC2 libcurl seam | `github_client.cpp`, `auth_service.cpp` exchange | `test_auth_service` — secret/code asserted in POST body, absent from URL |
| AC3 signed cookie | `session.cpp`, `cookieHeader()` in `auth_service.cpp` | `test_session` (sign/verify/tamper/expire/purpose), `test_auth_service` (attributes) |
| AC4 frozen digest | unchanged `sha256Hex`, mechanically renamed `keyedDigestHex`, `permutation.cpp:27` | `test_hash_utils` — frozen `keyedDigestHex` **literal** captured pre-change plus published NIST `sha256Hex` vectors; `test_permutation` golden `permuteIndexFeistel` vector captured pre-change; scoped delimiter-mutation sensitivity run in Task 1 Step 7a |
| AC5 logout | `auth_service.cpp` `handleLogout` | `test_auth_service` — `Max-Age=0` with `Path=/`, cross-site rejected, clears while unconfigured |
| AC6 allow-list parsing | `admin_auth.cpp` `parseAdminGithubUsers`, `isAllowedAdminLogin` | `test_config` — trim, case, empties, empty-denies |
| AC7 re-evaluated per request | `authorizeAdmin` reads `cfg` every call | `test_admin_auth` — removal denies an already-issued cookie (restart required to change `cfg`; documented, not claimed away) |
| AC8 extracted guard | `admin_auth.cpp` + `admin_guard.cpp`, `main.cpp` wiring only | `tests/test_admin_routes_guarded.sh` — all six routes `401` unconfigured and admitted with a token, end to end; sensitivity run in Task 4 Step 7a. `test_admin_auth` proves the decision; only the script proves the wiring |
| AC9 fail closed | `authorizeAdmin` default-deny | `test_admin_auth` unconfigured→401, with a configured-mechanism sensitivity control |
| AC10 upgrade note + diagnostic | `.env.example`, `README.md`, `startupAuthDiagnostics` | `test_config` diagnostics, incl. quiet-when-configured and no-secret checks |
| AC11 constant time | `constantTimeEquals` for token and MAC | `test_hash_utils` truth table; used in `authorizeAdmin`/`verifySessionToken` |
| AC12 token still works | first branch of `authorizeAdmin` | `test_admin_auth` token accept/reject, and token POST skips CSRF |
| AC13 503 without secret | config gate in `login`/`callback`/`me`; cookie branch refuses empty secret. `logout` deliberately exempt (O5) | `test_auth_service` 503×3 with a not-503-when-configured control, plus logout-still-clears; `test_session`/`test_admin_auth` empty-secret |
| AC14 CSRF | `hasSameOriginProof`, applied to cookie-authorized POSTs | `test_admin_auth` — bare/cross-site/same-site/cross-site-with-matching-Origin denied, two positive controls |
| AC20 per-stage credentials | plain env reads, no `Config::stage` branch | `test_config` — values identical across `STAGE` |
| AC21 documented env | `.env.example`, `README.md` | reviewed in Task 7; no real values |
| AC22 no leaks | fixed error codes; no verbose curl; no secret in bodies | leak assertions in `test_admin_auth` and `test_auth_service` |
| AC23 Nginx unchanged | comment only in `deploy/nginx.conf` | diff review — no `location` block changes |
| AC24 docs with behavior | Task 7 | the same PR as the behavior |
| AC25 both suites | Task 8 | recorded commands and counts, including the two sensitivity runs and `tests/test_admin_routes_guarded.sh` |
| AC26 `/auth/me` | `auth_service.cpp` `handleAuthMe` | `test_auth_service` — signed-in, signed-out, tampered, expired, avatar from id |

AC15–AC19 and AC27 are slice B and are deliberately untouched here.

## Self-Review

- **Spec coverage:** every AC in slice A's scope appears in the matrix with a named implementation site and a named test. Nothing is deferred to "a follow-up".
- **Sensitivity:** the reviewer's sharpest observation about the previous draft was that its two *strongest* claims — AC4's frozen digest and AC8's route wiring — were the two without working controls, while the weaker claims were well guarded. That asymmetry is now inverted. AC4 is anchored to keyed-digest and permutation literals captured from the pre-change build plus published NIST vectors, while `sha256Hex` is no longer refactored at all; Task 1 Step 7a requires *observing* both allocation assertions fail under a deliberate keyed-digest delimiter mutation while the independent HMAC/SHA tests stay green. AC8 is anchored to a committed six-route script, and Task 4 Step 7a requires observing it fail with one guard removed. The pre-existing controls stand: unconfigured-401 paired with configured-allow, CSRF denials paired with two positive same-origin controls, quiet-diagnostics paired with the warning case, `keyedDigestHex ≠ hmacSha256Hex` against an aliasing rename; and three more are added — 503-when-unconfigured paired with not-503-when-configured, the `setCookieNamed(…).empty()` probes paired with a success-path case that finds a cookie, and the `WrongPurpose` rejections paired with each blob verifying under its own purpose.
- **Placeholder scan:** no `TODO`, `TBD`, or "tests later" remains. `setCookieNamed` is now spelled out in full, because it is the one helper whose body is *not* mechanical — `crow::response::get_header_value` returns only the first match and the callback emits two `Set-Cookie` headers, so the obvious implementation would make the S3 assertions pass for the wrong reason. `stateParamOf`, `cookieTokenOf` (returns the cookie's value up to the first `;`), `validState`, `expiredStateRequest`, `tamperedStateRequest` and `sessionBlobAsStateRequest` remain local fixtures whose bodies are mechanical.
- **Review findings:** M1–M3, S1–S7, and O1–O5 are accepted and resolved; the tables at the top of this plan map each stable ID to where. Artifact 105 supersedes only M1's risky shared-formatter implementation mechanics, not its golden-vector requirement; the optional explicit-conversion correction is carried into the private HMAC formatter. None was rejected, and none was deferred to slice B or C except the `limit_req` rate-limit zone, which is a deployment concern by nature and which S4 explicitly routes to slice C.
- **Type consistency:** the GitHub id is a string end to end (`SessionIdentity::githubId`, cookie field, avatar URL) and is converted once at the callback boundary, where GitHub sends it as a JSON integer.
- **Scope:** no frontend file, no database schema, no allocator or permutation output, and no `service_*.cpp` file is modified. The only change to existing behavior is that unconfigured admin routes now deny, which is the point of AC9.
