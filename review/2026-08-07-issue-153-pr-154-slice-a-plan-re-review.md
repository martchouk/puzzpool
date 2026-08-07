# Plan re-review — issue #153, PR #154 (round 2)

**Issue:** [#153](https://github.com/martchouk/puzzpool/issues/153) · **PR:** [#154](https://github.com/martchouk/puzzpool/pull/154) (draft, base `dev`) · **Branch:** `feature/153-github-oauth-backend`
**Round 1 head:** `2e1aa2008001781d48e144aca596f149f4c8894d` → **Round 2 head:** `d60184a240981f29d825f1bc245a0487a791dcc4`
**Base:** `f1db2bf102848fce6da3086af3370826d082d6b3`
**Revision diff:** one commit, one file, +568/-98 — `docs/superpowers/plans/2026-08-07-issue-153-slice-a-github-oauth-backend.md`. Still no source file is touched.
**Pinned project policy:** `puzzpool` @ `f1db2bf`, profile `standard`.
**Reviewer:** mud-rev (same reviewer as round 1; finding IDs preserved)

**Verdict: APPROVED for implementation.** All three blocking findings are resolved, and two of them are resolved better than the remedy I asked for. Three new non-blocking findings and five optional items are recorded below; none of them changes the plan's architecture or blocks Task 1 from starting.

---

## Round-1 finding resolutions

| ID | Severity | Status | Evidence in the revision |
|----|----------|--------|--------------------------|
| M1 | MUST FIX | **Resolved — improved** | Task 1 Step 0, Step 1, Step 4, Step 5, Step 5a, Step 7a; matrix row AC4 |
| M2 | MUST FIX | **Resolved** | Task 4 Step 6 (six-route table), Step 6a (`tests/test_admin_routes_guarded.sh`), Step 7a; Task 7 Step 7; Task 8 Step 4; matrix row AC8 |
| M3 | MUST FIX | **Resolved** | Task 2 Step 5, Task 4 Step 1 test comment, Task 7 Step 4, ADR-6 |
| S1 | SHOULD FIX | **Resolved** | D-A3, Task 3 Step 1 + Step 4 |
| S2 | SHOULD FIX | **Resolved** | Task 6 Step 2 |
| S3 | SHOULD FIX | **Resolved — improved** | Task 6 Step 2 + Step 4 (`cookieHeader()`) |
| S4 | SHOULD FIX | **Resolved** | Task 7 Step 4 |
| O1–O5 | Optional | **All applied** | Task 3 Step 1, Task 4 Step 4, Task 7 Step 4, Task 2 Step 4, Task 6 Step 4 |

### M1 — resolved, and the remedy is stronger than the one I proposed

I asked for three literals. The revision supplies four things, and the fourth is the one that matters most.

Promoting `toHex` from file-local to the public header (Task 1 Step 4) and testing it directly is strictly better than my suggestion of hunting for a digest that happens to contain a leading-zero byte. `toHex(std::string("\x00\x01\x0f", 3)) == "00010f"` and `toHex(std::string(32, '\0')).size() == 64` pin the padding against expected values that require no digest computation at all. I checked the escape sequences: every one terminates on a non-hex character (`\x0f` before `"`, `\x10` before `"`), so none of them repeats the O1 maximal-munch bug.

More important, the **NIST FIPS 180-4 vectors make the whole remedy robust against its own procedure failing.** Step 0's capture discipline — record goldens against the untouched tree, before any `src/` edit — is correct, but a capture taken late would freeze whatever the refactor produced, and Step 7a's deliberate `setw(1)` break would not detect that. `sha256Hex("abc") == "ba7816bf…"` and `sha256Hex("") == "e3b0c442…"` are external literals that no capture order can corrupt, and since `keyedDigestHex(key, msg)` is exactly `sha256Hex(key + "\x1f" + msg)`, freezing `sha256Hex` freezes the entire Feistel round digest path. The captured goldens are now belt-and-braces rather than the sole guarantee. That closes the failure mode I filed, including the one I did not name.

I re-verified both NIST digests and all four RFC 4231 vectors (cases 1, 2, 3, 6) as transcribed at plan lines 199–239 — all correct.

Step 0's capture case correctly calls the **pre-rename** `hmacSha256Hex`, and both `sha256Hex` and `hmacSha256Hex` are already public at `include/puzzpool/hash_utils.hpp:7-8`, so the capture file compiles against the untouched tree. The golden `permuteIndexFeistel` snippet matches the real signature used at `tests/test_permutation.cpp:19,28`, and index `999982 < n = 999983`.

I confirmed the gap M1 was about is real and still unguarded at `f1db2bf`: `tests/test_permutation.cpp:25-31` calls the function twice with identical arguments and compares — self-consistency, not cross-version stability — and no literal vector exists anywhere in the file.

Step 7a's break-and-observe requirement, including the explicit instruction that a still-passing golden vector means Step 0 must be redone, is the right shape for a sensitivity run.

### M2 — resolved

The six-route table at Task 4 Step 6 matches `src/main.cpp` exactly. I checked every row:

| Route | Method | Claimed lines | Actual | Handler |
|---|---|---|---|---|
| `activate-puzzle` | POST | 84-88 | ✓ | `handleActivatePuzzle(req)` ✓ |
| `set-puzzle` | POST | 90-94 | ✓ | `handleSetPuzzle(req)` ✓ |
| `set-test-chunk` | POST | 96-100 | ✓ | `handleSetTestChunk(req)` ✓ |
| `puzzles` | GET | 102-106 | ✓ | `handleAdminPuzzles()` ✓ |
| `reclaim` | POST | 108-112 | ✓ | `handleAdminReclaim()` ✓ |
| `import-ranges` | POST | 114-118 | ✓ | `handleImportRanges(req)` ✓ |

The vacuous AC8 matrix row is gone, replaced by a citation of the committed script with the split stated plainly: `test_admin_auth` proves the decision, only the script proves the wiring. Choosing `/reclaim` for the Step 7a sensitivity break — deliberately not the route the old one-route smoke check happened to cover — is the right instinct.

The script is buildable as specified: `PORT` and `DB_PATH` are real env settings (`src/config.cpp:20-21`), the server binds `127.0.0.1` (`src/main.cpp:142`), and `tests/test_check_node_version_age.sh` is a genuine in-repo precedent for a bash test under `tests/`. Two weaknesses in the script's design are recorded as S6 and S7 below; neither undermines phase 1, which is the assertion that actually carries AC8.

### M3 — resolved

Confirmed again at `src/main.cpp:16`: `loadConfigFromEnv()` is called once and nothing re-reads the environment. All four sites now say remove-then-restart, and the property that *is* real — no cached authorization decision, so the change applies on the very next request with no cookie reissue and no session store to purge — is stated rather than dropped. `docs/security.md`'s ready-to-paste block carries an explicit "do not write *no restart*" note beside it, which is the right place for it: that line is read under time pressure. Rotating `SESSION_SIGNING_SECRET` is covered the same way. Declining to add a `Config` reload path is the correct scope call.

### S1 — resolved

The purpose label is inside the signed input, not compared afterwards, and the verification order (shape → signature → purpose → expiry) is right for the reason the plan gives. The `"the purpose label is inside the MAC"` case is the one that proves it: I traced the offset arithmetic — `state.find(".state.")` returns 2, `+6` lands on index 8, which is the `.` before the subject field, so `"v1.session" + state.substr(8)` produces a correctly-shaped blob with a forged label. It fails as `BadSignature`, which is the assertion. Both cross-type directions are covered, and the "each token still verifies under its own purpose" case is a real control against a blanket rejection.

Splitting into `issueSessionToken`/`issueStateToken`/`verifySessionToken`/`verifyStateToken` over one private pair is better than threading a purpose parameter through call sites, and the callback correctly uses `verifyStateToken`. The failure-path table gains a "session blob as state" case, which is the end-to-end form.

### S2 — resolved

`crow::response::get_header_value` is non-const at `third_party/crow/include/crow/http_response.h:76` (vs `http_request.h:81`, which is const), and `crow::response::headers` is a `ci_map`, i.e. `std::unordered_multimap`, at `ci_map.h:42` — so `equal_range("Set-Cookie")` is the correct scan and is case-insensitive through `ci_key_eq`. Every response local is now a named non-const `auto`, the three temporaries that were being passed to `setCookieNamed` are named locals, and `setCookieNamed` is spelled out in full with a comment explaining why the obvious implementation is wrong. The prefix match cannot confuse `pp_session=` with `pp_oauth_state=`.

### S3 — resolved, and the remedy is stronger than the one I proposed

Rather than only strengthening the assertions, Task 6 Step 4 removes the way for the two headers to drift: one `CookieSpec` per cookie and one `cookieHeader(spec, value, maxAge)`, so issuing and clearing are the same call with a different value and age. The assertions then check the full attribute set on both clears. I checked the substring choices — `find("Path=/;")` for the session cookie correctly avoids matching `Path=/api/v1/auth`, and `Max-Age=0` cannot be a false positive against `Max-Age=600` or `Max-Age=43200`. The new `setCookieNamed(...).empty()` success-path control and the failure-path state-clear assertion are both genuine additions.

### S4 — resolved

Both consequences are recorded in `docs/security.md` as accepted decisions, in the terms I raised them: any GitHub account can obtain a valid `pp_session` from a public deployment because the callback issues before any allow-list check, and authorization rather than authentication is what protects the admin routes; and each `/login` + `/callback` pair costs one synchronous 10-second outbound call from a Crow worker thread, drivable anonymously. Routing the `limit_req` zone to slice C is correct — it is a deployment concern by nature.

### Optional findings O1–O5

All five applied, and all five checked:

- **O1** — adjacent literals with a maximal-munch comment. Correct.
- **O2** — an explicit `Sec-Fetch-Site` is now definitive in both directions, with two new denial cases (`cross-site` plus a *matching* `Origin`, and `same-site`). D-A4's prose and the implementation snippet now agree.
- **O3** — the "four admin endpoints" count at `docs/security.md:16-17` is confirmed present at `f1db2bf` and scheduled for correction to six.
- **O4** — `std::clamp(..., 1, 43'200)`; `<algorithm>` is already included at `src/config.cpp:5`, so no new include is needed. The rationale (a stateless session that cannot be individually revoked should not be mintable for years by a typo) is right.
- **O5** — `handleLogout` loses its 503 gate but keeps the same-origin check, so the old `handleLogout(crow::request{}) == 503` assertion correctly becomes `== 403`, and the 503 test gains a not-503-when-configured control. This is a deliberate deviation from AC13's literal text, and the matrix row records it as such rather than hiding it. I agree with the deviation: clearing a cookie needs no signing key, and 503 there would strand a stale cookie exactly when the secret was rotated away.

---

## New findings

None is blocking.

### S5 — the login test's state-binding assertion cannot pass as written

**Where:** plan line 1517, in `TEST_CASE("login redirects to GitHub and binds the state to a cookie")`.

```cpp
CHECK(stateCookie.find(stateParamOf(location)) != std::string::npos);
```

Trace what the two sides actually hold. Task 6 Step 4 specifies the nonce as `base64UrlEncode(nonce_(32))` — so `nonce` is already base64url *text*. The URL's `state` parameter is "the bare nonce". The cookie holds `issueStateToken(secret, nonce, now + 600)`, and `signingInput` unconditionally applies `base64UrlEncode` to the subject field (D-A3, plan line 806-810). So the cookie's subject field is `base64UrlEncode(nonce)` — base64url of a base64url string.

Base64-encoding ASCII text does not leave the original as a substring (`base64("abc") == "YWJj"`). The assertion therefore fails, and no base64url percent-encoding difference rescues it: the base64url alphabet is entirely URL-safe, so `stateParamOf(location)` returns the nonce unchanged.

This is a pre-existing defect — it is unchanged from `2e1aa20` and I missed it in round 1. I am filing it now rather than letting it stand.

It is **not blocking** because it fails loudly at implementation time rather than passing vacuously, which is the opposite of the M1/M2 failure mode: nothing green hides a bug. But the remedy matters, because the obvious reaction to a failing assertion whose comment says "must be bound to, and not equal to" is to delete it — and then AC1's browser-binding has no login-side proof at all. The callback tests build `validState()` as a hand-made consistent pair, so they cannot detect a login handler that puts the wrong value in either place.

**Required remedy:** replace it, do not delete it. Assert the binding through verification:

```cpp
const auto v = verifyStateToken(oauthCfg().sessionSigningSecret,
                                cookieTokenOf(stateCookie), kNow);
REQUIRE(v.error == SessionError::None);
CHECK(v.identity.login == stateParamOf(location));   // the actual binding
```

Keep the neighbouring `CHECK(cookieTokenOf(stateCookie) != stateParamOf(location))` — it is a good addition, and it is what would catch a handler that leaked the signed blob into the redirect URL, where it would reach GitHub's logs and any `Referer`.

### S6 — the six-route guard script is not wired into CI

**Where:** Task 4 Step 6a, Task 7 Step 7, Task 8 Step 4.

M2's stated purpose was a regression "that survives as a regression rather than living in a checklist". The script is committed under `tests/`, which is most of the way there, but it is registered nowhere: not in CTest (deliberately, and I agree — it needs a running server), and not in `.github/workflows/ci.yml`. It runs only when someone remembers to run it.

The project already has the precedent: `.github/workflows/ci.yml:58` runs `bash tests/test_check_node_version_age.sh`. Adding one step after the existing `ctest` line costs three lines and turns a documented habit into an enforced one.

One caveat worth stating rather than leaving implicit: `ci.yml:4-7` triggers only on `push`/`pull_request` against `main`, so **no CI runs on this PR at all** (`gh pr checks 154` → "no checks reported"). That is a pre-existing project gap, not something this issue introduced, and wiring the script in would still cover the `dev` → `main` promotion, which is where an unguarded admin route would reach production.

**Suggested remedy:** add the script to the backend job in `.github/workflows/ci.yml` alongside the existing `ctest` step.

### S7 — phase 2 of the guard script can pass on a dead server

**Where:** Task 4 Step 6a.

Phase 2 asserts each route returns "something other than `401`". The plan is explicit and right that this is the control which makes phase 1 meaningful — without it, a server that 401s unconditionally would pass. But the control has the same weakness one layer down: if the phase-2 server fails to start or the restart races the port becoming free, `curl` returns no status (`000` with `-w '%{http_code}'`), which is *not* `401`, and all six phase-2 checks pass vacuously.

Phase 1 is unaffected — it requires exactly `401`, so an unreachable server fails it — and Task 4 Step 7a's guard-removal run is a stronger sensitivity proof than phase 2 is. So this weakens a control, not the primary assertion.

**Suggested remedy:** in phase 2, fail on a `curl` connection error explicitly — check `curl`'s exit status, or assert the status is a real HTTP code (`[0-9]{3}` and not `000`) before applying the not-401 rule. Poll for readiness after each server start rather than sleeping a fixed interval.

---

## Optional

- **Plan line 1116** — the `admin_auth.hpp` snippet declares `AdminRequestView` and `authorizeAdmin` but not `hasSameOriginProof`, which Task 6 Step 4 reuses from `auth_service.cpp` — a different translation unit. Add the declaration to the header snippet.
- **Plan line 856** — `issueStateToken` passes `{nonce, ""}`, so the state blob's `<b64url(id)>` field is empty and the token contains `..`. That only works if `split(token, '.')` preserves empty tokens (`parts.size() == 6`) and `base64UrlDecode("")` returns an engaged optional. Both are unstated requirements of otherwise-unspecified helpers, and getting either wrong makes *every* state token `Malformed`. One sentence in Task 3 Step 4 would remove the trap.
- **Plan line 232** (`toHex` body) — `for (unsigned char b : bytes)` over a `std::string_view` is an implicit `char` → `unsigned char` conversion, which `-Wconversion` (enabled at `CMakeLists.txt:90`) will flag on sign change. Harmless today because `PUZZPOOL_WARNINGS_AS_ERRORS` defaults `OFF` and CI does not set it, but the step claims "-Wconversion clean". Use `for (char b : bytes)` with an explicit `static_cast<unsigned char>(b)` at the point of use.
- **Plan line 1691** — the new O5 test uses `serviceWith(cfg)`, which is not in the Self-Review's list of local fixtures (`stateParamOf`, `cookieTokenOf`, `validState`, `expiredStateRequest`, `tamperedStateRequest`, `sessionBlobAsStateRequest`). Trivially mechanical, but the placeholder scan is supposed to be exhaustive.
- **Plan Step 5a** — the golden-vector snippet uses `std::vector<std::pair<...>>`, and `tests/test_permutation.cpp` currently includes only `<set>` and `<string>`. Add `<vector>` and `<utility>`.

---

## Documentation status

| Surface | Planned | Assessment |
|---|---|---|
| `docs/api.md` | Task 7 Step 3 | Adequate — all four routes, `Set-Cookie` attributes now including `Path`, the 503 rule with the logout exemption stated, and `403 csrf_check_failed` on the admin preamble |
| `docs/security.md` | Task 7 Step 4 | **M3 resolved**; S1 and S4 both landed; O3's count correction scheduled. The revocation procedure is ready-to-paste and carries its own do-not-write warning |
| `docs/architecture.md` | Task 7 Step 5 | Adequate — module rows, corrected `main.cpp`/`hash_utils.cpp` rows, extended dependency line; matches the direction the project policy requires |
| `docs/architecture-review.md` | Task 7 Step 6 | **ADR-6 corrected**; it now states the restart requirement, the 30-day cap rationale, the six-route script's role, and the purpose-label separation |
| `docs/testing.md` | Task 7 Step 7 | Adequate — four new binaries plus a dedicated section for the guard script explaining why no Catch2 target can see the wiring |
| `README.md`, `.env.example` | Task 7 Steps 1-2 | Good; the upgrade warning is the right prominence for an operator-visible break |
| `deploy/nginx.conf` | Task 7 Step 7 | Comment only — AC23 holds |
| `docs/database.md` | not planned | Correct — no schema change |

## Test-sensitivity assessment

Round 1's central objection was that the plan's two *strongest* claims — AC4's frozen digest and AC8's route wiring — were the two without working controls, while the weaker claims were well guarded. That asymmetry is inverted:

- **AC4** is anchored to two published NIST literals and five direct `toHex` padding assertions that no refactor and no capture-ordering mistake can move, plus captured goldens, plus a required break-and-observe run.
- **AC8** is anchored to a committed six-route script plus a required guard-removal run.
- Three controls are added beyond what I asked for: 503-when-unconfigured paired with not-503-when-configured, the `setCookieNamed(...).empty()` probes paired with a success-path case that finds a cookie, and the `WrongPurpose` rejections paired with each blob verifying under its own purpose.

The pre-existing controls stand. The remaining weaknesses are S7 (a control that can pass on a dead server) and S5 (an assertion that cannot pass at all) — in both cases the primary assertion is intact.

## Security, persistence, performance

- **Security.** S1's domain separation is correctly placed inside the MAC with the right verification order. O2 makes an explicit cross-site signal unoverridable, which is the correct reading of a header the request already controls. S3's single-helper cookie construction removes a class of silent-clear bug. S4's two accepted decisions are documented. No identity is read from an unverified token; uniform `401 {"error":"unauthorized"}` across every denial cause; `constantTimeEquals` correctly implemented and correctly documented as length-leaking; `secureRandomBytes` throws rather than degrading. The libcurl requirements are still the right list.
- **Persistence.** Unchanged — no schema change, no migration, no SQLite access. Consistent with ADR-6.
- **Performance.** Unchanged. The one new cost remains the synchronous outbound call in the callback, now documented per S4.
- **Public API compatibility.** Four new public routes; no existing route contract changes. The one behavioral break is AC9's fail-closed guard, which is the intent of the issue and is handled with an upgrade warning, a rewritten `docs/security.md`, and a startup diagnostic.

## Verification — what I could and could not run

**No build or test ran in this attempt.** `cmake`, `ctest`, `python3` and `shasum` are present on `PATH` in this workspace but every invocation was refused by the attempt's permission layer. I did not work around it. That is the same wall the two prior attempts hit, and the predecessor's Risk 4 remains an accurate statement of the consequence.

What that leaves unverified: I have not confirmed the `f1db2bf` baseline green, and I could not machine-check the NIST or RFC 4231 digests — those are transcription checks against published values I know, not computed results. Everything else in this report is a direct read of the tree at the reviewed revision.

What I did do beyond round 1: I initialized all three submodules (`Catch2` `11a96e18`, `SQLiteCpp` `2824eb49`, `crow` `7ecd59c`, all at their pinned commits) and re-derived every codebase claim the revision's new remedies rest on:

| Claim | Verification |
|---|---|
| `sha256Hex`/`hmacSha256Hex` are public, so Step 0's capture file compiles pre-change | `include/puzzpool/hash_utils.hpp:7-8` |
| The existing hex loop is correct and is what `toHex` must preserve | `src/hash_utils.cpp:28-33` — `setfill('0')` once, `setw(2)` per iteration |
| `keyedDigestHex` is exactly `sha256Hex(key + "\x1f" + msg)`, so NIST vectors freeze the whole path | `src/hash_utils.cpp:36-38` |
| No golden/literal permutation vector exists today | `tests/test_permutation.cpp:25-31` is a two-call self-comparison |
| `permuteIndexFeistel` signature matches the golden snippet | `tests/test_permutation.cpp:19,28` |
| `Config` is loaded once per process | `src/main.cpp:16` |
| The six admin routes, their methods, line ranges and handlers | `src/main.cpp:84-118` — all six rows of the plan's table correct |
| `main.cpp` is untestable from Catch2 | `CMakeLists.txt:41-61,71`; `tests/CMakeLists.txt:5` links `puzzpool_core` only |
| `PORT` / `DB_PATH` exist, so the guard script is configurable | `src/config.cpp:20-21`; server binds `127.0.0.1` at `src/main.cpp:142` |
| A bash test under `tests/` has precedent, and CI already runs one | `tests/test_check_node_version_age.sh`; `.github/workflows/ci.yml:58` |
| `docs/security.md` says "four admin endpoints" | `docs/security.md:16-17` |
| `std::clamp` needs no new include | `src/config.cpp:5` |
| `-Wconversion` is on, `-Werror` is not | `CMakeLists.txt:90,99-101`; `ci.yml:25-27` sets no `PUZZPOOL_WARNINGS_AS_ERRORS` |
| `crow::response::get_header_value` is non-const | `third_party/crow/include/crow/http_response.h:76` vs `http_request.h:81` |
| Response headers support `equal_range` case-insensitively | `third_party/crow/include/crow/ci_map.h:42` |
| No CI runs on this PR | `ci.yml:4-7` triggers on `main` only; `gh pr checks 154` reports none |

The diff is a single documentation file adding no source, so there is no changed code path whose tests I am failing to run.

## PR verdict

I did **not** submit a formal `--approve` on PR #154, and that is deliberate rather than a mechanical limitation. PR #154 is an open draft that currently contains only the plan; per Task 8 Step 7 it becomes the implementation PR once Task 1 begins. Attaching an approval now would approve a pull request whose entire implementation content does not yet exist. The correct time for a formal PR verdict is the implementation review.

This report is the binding record, and it is a full verdict in the same words a PR approval would carry.

## Transition

`status:ready-for-development`. Every blocking finding from round 1 is resolved in the artifact under review, two of them more thoroughly than the remedy I specified. S5, S6 and S7 are important but non-blocking: none of them can produce a green suite hiding a defect, which was the criterion that made M1 and M2 blocking. They are recorded here for the implementer to fold in as they reach the relevant task — S5 in Task 6 Step 2, S6 and S7 in Task 4 Step 6a.

`status:dev-planning` would be wrong — sending this back for another planning round would buy nothing the implementer cannot do in place. `status:blocked` would be wrong — nothing is waiting on an outside decision; the denied build tooling limited what evidence I could produce, not whether the plan could be assessed.
