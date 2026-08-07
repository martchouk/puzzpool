# Slice A — GitHub OAuth Backend and Fail-Closed Admin Authorization

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Issue:** #153 (slice A of story #149) · **Base:** `f1db2bf` on `dev` · **Branch:** `feature/153-github-oauth-backend`

**Goal:** Ship the independently shippable backend authentication core — a real HMAC-SHA-256 primitive, a signed stateless session cookie, GitHub OAuth over an injectable libcurl seam, a case-insensitive `ADMIN_GITHUB_USERS` allow-list, and a central admin guard extracted into `puzzpool_core` that fails closed — while `X-Admin-Token` keeps working and the existing dashboard is untouched.

**Architecture:** Auth is layered so that every decision is testable without Crow and without a network. Three crow-free domain units (`hash_utils`, `session`, `admin_auth`) hold the primitives, the cookie format, and the authorization decision. Two thin Crow adapters (`admin_guard`, `AuthService`) translate `crow::request`/`crow::response` and own no decisions. `GitHubHttpClient` is a struct of `std::function`s — the libcurl implementation lives behind it, tests inject a fake — matching the existing `PoolService::AddressStatusFetcher` seam precedent. `AuthService` is deliberately a sibling of `PoolService`, not a member: it touches no database and no service mutex, so folding it into `PoolService` would widen that class's lock scope and dependencies for nothing.

New dependency direction (no cycles, extends `docs/architecture.md:72-73`):
`main → {service, auth_service, admin_guard} → {admin_auth, session, github_client} → {hash_utils, base64, secure_random, config} → env`

**Tech Stack:** C++20, Crow, libcurl (new), Boost Multiprecision, nlohmann/json, SQLiteCpp, Catch2, CTest

---

## Design decisions taken in this plan

These are settled inputs for implementation, not open questions. Each is justified against a codebase fact.

- **D-A1 — The real HMAC takes the `hmacSha256Hex` name; the old helper becomes `keyedDigestHex`.**
  `src/hash_utils.cpp:36-38` is `sha256Hex(key + "\x1f" + msg)` — a secret-prefix construction that is length-extension forgeable, not a MAC. Its only caller is the Feistel round function at `src/permutation.cpp:27`, where its output bytes must not change or every existing puzzle's allocation order shifts (ADR-4, `docs/architecture-review.md:94`). So the bytes stay frozen under the new honest name, and the new RFC 2104 implementation takes over the name that implies a MAC. Locked by a byte-stability vector plus a test asserting the two functions disagree.
- **D-A2 — HMAC is implemented once over a raw-digest primitive, not per platform.**
  `hash_utils.cpp` already branches CommonCrypto/OpenSSL for the digest itself. Adding `sha256Raw()` and building RFC 2104 on top of it (block 64, ipad/opad) keeps one code path, so a cookie signed on macOS verifies identically on the Linux host. Validated against RFC 4231 test vectors.
- **D-A3 — The session cookie carries the numeric GitHub id, and the MAC covers it.**
  This adopts the PO's D18. AC26 derives the avatar URL from the numeric id, and AC7 requires re-evaluating the allow-list from the cookie on every admin request — so re-calling GitHub or storing server state are both excluded. AC3's "login and absolute expiry" is a minimum, not an exclusive list. Token format:
  `v1.<b64url(login)>.<b64url(id)>.<expiryUnix>.<hmacHex>`, MAC over everything before the final dot. Base64url on the two variable fields makes the delimiter unambiguous, so no login can inject a field boundary.
- **D-A4 — Cookie-authorized admin `POST`s require an affirmative same-origin signal (AC14).**
  Accept `Sec-Fetch-Site: same-origin` or `none`, or an `Origin` that equals the configured `PUBLIC_BASE_URL` origin. **Absence of both is rejected**, otherwise the defense is void against any client that simply omits the headers. This is deliberate: a script or `curl` cannot drive admin `POST`s with a session cookie, and should use `X-Admin-Token`, which is not cookie-authorized and therefore skips the CSRF check entirely (AC12). `SameSite=Lax` already blocks the cross-site case; this is the second layer AC14 asks for.
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
4. **No build or test execution was possible in the planning attempt.** The planning sandbox denied `cmake`, `ctest`, `npm`, and interpreter invocations. Every "Expected:" line below is a specification for the implementer to verify, not an observed result. In particular the RFC 4231 digests in Task 1 are transcribed from the RFC, not machine-checked here — the implementer must confirm them against the published vectors before treating a mismatch as an implementation bug. The Crow API calls used throughout (`add_header`, `redirect`, `url_params.get`, `set_header`) *were* verified against the bundled submodule at `third_party/crow` (commit `7ecd59c`).
5. **`src/service_puzzle_status.cpp:55-57` still shells out to `curl` via `popen`.** Migrating it to the new libcurl seam is explicitly out of scope for this story; it is not touched.

---

## File Structure

**New — crow-free domain**

- Add: `include/puzzpool/base64.hpp`, `src/base64.cpp`
  Base64url encode/decode without padding, for cookie fields and random tokens.
- Add: `include/puzzpool/secure_random.hpp`, `src/secure_random.cpp`
  `secureRandomBytes(n)` from `arc4random_buf` (Apple) / `getrandom()` (Linux) with a `/dev/urandom` fallback; throws rather than degrading to a PRNG.
- Add: `include/puzzpool/session.hpp`, `src/session.cpp`
  Session-token issue/verify, the `SessionError` taxonomy, and `Cookie:` header parsing.
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
  Add `sha256Raw`, real `hmacSha256Hex`, `constantTimeEquals`; rename the old helper to `keyedDigestHex` with a non-MAC warning comment.
- Modify: `src/permutation.cpp`
  Mechanical rename of the single call site at line 27.
- Modify: `include/puzzpool/config.hpp`, `src/config.cpp`
  Five new settings plus the parsed allow-list.
- Modify: `src/main.cpp`
  Wire four auth routes, replace the inline lambda guard with `adminGuard`, print the diagnostics.
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`
  New sources, `find_package(CURL REQUIRED)`, four new test targets.
- Modify: `.github/workflows/ci.yml`, `deps.txt`
  Install libcurl.
- Modify: `tests/test_config.cpp`
  Allow-list parsing and stage-independence coverage.

**New tests**

- Add: `tests/test_hash_utils.cpp`, `tests/test_session.cpp`, `tests/test_admin_auth.cpp`, `tests/test_auth_service.cpp`

**Docs**

- Modify: `docs/api.md`, `docs/security.md`, `docs/architecture.md`, `docs/architecture-review.md` (ADR-5, ADR-6), `docs/testing.md`, `README.md`, `.env.example`, `deploy/nginx.conf` (comment only).

---

### Task 1: Real HMAC beside the frozen permutation digest

**Files:**
- Add: `tests/test_hash_utils.cpp`
- Modify: `tests/CMakeLists.txt`, `include/puzzpool/hash_utils.hpp`, `src/hash_utils.cpp`, `src/permutation.cpp`

- [ ] **Step 1: Write the failing primitive tests first**

`keyedDigestHex` does not exist yet and `hmacSha256Hex` is not an HMAC, so both halves fail for the right reason.

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

// AC4: the permutation digest is frozen. This value is the current output of the
// pre-rename helper and must never change — ADR-4 allocation determinism depends on it.
TEST_CASE("keyedDigestHex output is byte-for-byte frozen", "[hash][permutation]") {
    CHECK(keyedDigestHex("round-key-0", "12345") ==
          sha256Hex(std::string("round-key-0") + "\x1f" + "12345"));
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

/// Raw 32-byte SHA-256 digest. Used to build HMAC without a second platform branch.
std::string sha256Raw(std::string_view input);

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

- [ ] **Step 5: Implement over a shared raw digest**

```cpp
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

std::string keyedDigestHex(const std::string& key, const std::string& msg) {
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
    return toHex(sha256Raw(outer + innerDigest));
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

Factor the existing hex loop in `sha256Hex()` into a file-local `toHex(std::string_view)` and have `sha256Hex()` return `toHex(sha256Raw(input))`, so there is one hex formatter. Keep `-Wconversion` clean — every `unsigned char` narrowing above is explicit.

- [ ] **Step 6: Rename the single permutation call site**

```cpp
return hexToInt(keyedDigestHex(roundKey, bigToDec(right))) & mask;
```

- [ ] **Step 7: Verify the primitives and the frozen permutation together**

Run: `ctest --test-dir build --output-on-failure --tests-regex "hash_utils|permutation"`

Expected: the RFC 4231 vectors pass, the frozen-digest and difference checks pass, and the pre-existing `test_permutation` determinism suite is unchanged and green — the proof that AC4 held.

- [ ] **Step 8: Commit**

```bash
git add include/puzzpool/hash_utils.hpp src/hash_utils.cpp src/permutation.cpp \
        tests/test_hash_utils.cpp tests/CMakeLists.txt
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
    cfg.sessionTtlMinutes       = std::max(1, getEnvInt("SESSION_TTL_MINUTES", 720));
    cfg.adminGithubUsers        = parseAdminGithubUsers(getEnvOr("ADMIN_GITHUB_USERS", ""));
```

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

The allow-list is read from `cfg` on each call and never cached, which is what makes AC7's "removal takes effect immediately" true by construction.

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
    for (const std::string bad : {"", "v1", "v1.a.b.c", "v1.a.b.c.d.e",
                                  "v2.YWxpY2U.MQ.1770000600.deadbeef",
                                  "v1.!!!.MQ.1770000600.deadbeef",
                                  "v1.YWxpY2U.MQ.not-a-number.deadbeef"}) {
        CHECK(verifySessionToken(kSecret, bad, kNow).error == SessionError::Malformed);
    }
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
    const std::string raw("\x00\xff\x10binary", 9);
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

```cpp
namespace {
std::string signingInput(const SessionIdentity& id, int64_t expiresAt) {
    return "v1." + base64UrlEncode(id.login) + "." + base64UrlEncode(id.githubId) + "."
         + std::to_string(expiresAt);
}
}

std::string issueSessionToken(const std::string& secret,
                              const SessionIdentity& id,
                              int64_t expiresAtUnix) {
    const std::string payload = signingInput(id, expiresAtUnix);
    return payload + "." + hmacSha256Hex(secret, payload);
}

SessionVerification verifySessionToken(const std::string& secret,
                                       std::string_view token,
                                       int64_t nowUnix) {
    SessionVerification out;
    const auto parts = split(token, '.');
    if (parts.size() != 5 || parts[0] != "v1") { out.error = SessionError::Malformed; return out; }

    const auto login = base64UrlDecode(parts[1]);
    const auto id    = base64UrlDecode(parts[2]);
    int64_t expiresAt = 0;
    if (!login || !id || !parseInt64(parts[3], expiresAt)) {
        out.error = SessionError::Malformed;
        return out;
    }

    const std::string payload = concatFirstFour(parts);
    if (secret.empty() ||
        !constantTimeEquals(parts[4], hmacSha256Hex(secret, payload))) {
        out.error = SessionError::BadSignature;
        return out;
    }
    if (nowUnix > expiresAt) { out.error = SessionError::Expired; return out; }

    out.error = SessionError::None;
    out.identity = {*login, *id};
    out.expiresAt = expiresAt;
    return out;
}
```

Order matters: shape → signature → expiry. Never read the identity out of an unverified token. The same `issueSessionToken`/`verifySessionToken` pair signs the OAuth state nonce, with the nonce in the `login` field and an empty id — one signed-blob format, one place to get it right.

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

// AC7 — removal takes effect immediately, with no cookie reissue and no restart.
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
    if (req.secFetchSite == "same-origin" || req.secFetchSite == "none") return true;
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

- [ ] **Step 6: Rewire `main.cpp`**

Delete the lambda at `src/main.cpp:73-82` and call the shared guard, leaving all six route bodies otherwise identical:

```cpp
#include <puzzpool/admin_guard.hpp>

        CROW_ROUTE(app, "/api/v1/admin/activate-puzzle").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(cfg, req)) return std::move(*denied);
            return service.handleActivatePuzzle(req);
        });
```

And print the diagnostics beside the existing startup lines at `src/main.cpp:138-140`:

```cpp
        for (const auto& line : puzzpool::startupAuthDiagnostics(cfg)) {
            std::cerr << "[puzzpool-cpp] " << line << "\n";
        }
```

- [ ] **Step 7: Verify the guard and the untouched handlers**

Run: `ctest --test-dir build --output-on-failure --tests-regex "admin_auth|handler"`

Expected: the full matrix passes and `test_handler_validation`'s admin-handler cases are unchanged — they call `PoolService` methods directly, below the guard, which is why extraction does not disturb them.

- [ ] **Step 8: Commit**

```bash
git add include/puzzpool/admin_auth.hpp src/admin_auth.cpp \
        include/puzzpool/admin_guard.hpp src/admin_guard.cpp \
        src/main.cpp tests/test_admin_auth.cpp tests/CMakeLists.txt CMakeLists.txt
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

std::string setCookieNamed(const crow::response& r, const std::string& name);  // "" when absent

} // namespace

// ── AC13 / AC26: unconfigured means 503, never a silently-unsigned cookie ──

TEST_CASE("every auth route returns 503 without SESSION_SIGNING_SECRET", "[auth][config]") {
    Config cfg = oauthCfg();
    cfg.sessionSigningSecret.clear();
    FakeGitHub gh;
    AuthService svc{cfg, gh.client(), [] { return kNow; }, fixedNonce};

    CHECK(svc.handleGithubLogin(crow::request{}).code == 503);
    CHECK(svc.handleGithubCallback(crow::request{}).code == 503);
    CHECK(svc.handleLogout(crow::request{}).code == 503);
    CHECK(svc.handleAuthMe(crow::request{}).code == 503);
}

TEST_CASE("login returns 503 when the OAuth app is not configured", "[auth][config]") {
    Config cfg = oauthCfg();
    cfg.githubOauthClientId.clear();
    FakeGitHub gh;
    AuthService svc{cfg, gh.client(), [] { return kNow; }, fixedNonce};
    CHECK(svc.handleGithubLogin(crow::request{}).code == 503);
}

// ── AC1: unguessable, browser-bound, single-use state ──

TEST_CASE("login redirects to GitHub and binds the state to a cookie", "[auth][oauth]") {
    const auto r = loginService().handleGithubLogin(crow::request{});
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
    // The state in the URL must be bound to, and not equal to, the signed cookie blob.
    CHECK(stateCookie.find(stateParamOf(location)) != std::string::npos);
}

TEST_CASE("the login URL never carries the client secret (AC22)", "[auth][security]") {
    const auto r = loginService().handleGithubLogin(crow::request{});
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
    const auto r = svc.handleGithubCallback(callbackRequest("the-code", validState()));

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

TEST_CASE("the state cookie is cleared on every callback, making it single-use", "[auth][oauth]") {
    FakeGitHub gh;
    const auto r = callbackService(gh).handleGithubCallback(callbackRequest("c", validState()));
    CHECK(setCookieNamed(r, "pp_oauth_state").find("Max-Age=0") != std::string::npos);
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
         }) {
        INFO(c.name);
        const auto r = callbackService(gh).handleGithubCallback(c.req);
        CHECK(setCookieNamed(r, "pp_session").empty());
        CHECK(r.get_header_value("Location").find("auth=error") != std::string::npos);
    }
}

TEST_CASE("provider failures issue no session", "[auth][oauth]") {
    for (auto response : {HttpResult{401, R"({"error":"bad_verification_code"})", false},
                          HttpResult{200, R"({"error":"bad_verification_code"})", false},
                          HttpResult{200, "not json", false},
                          HttpResult{0, "", true}}) {
        FakeGitHub gh;
        gh.tokenResponse = response;
        const auto r = callbackService(gh).handleGithubCallback(
            callbackRequest("c", validState()));
        CHECK(setCookieNamed(r, "pp_session").empty());
    }

    FakeGitHub badUser;
    badUser.userResponse = {403, R"({"message":"Forbidden"})", false};
    CHECK(setCookieNamed(callbackService(badUser).handleGithubCallback(
              callbackRequest("c", validState())), "pp_session").empty());

    FakeGitHub noLogin;
    noLogin.userResponse = {200, R"({"id":1})", false};   // login missing
    CHECK(setCookieNamed(callbackService(noLogin).handleGithubCallback(
              callbackRequest("c", validState())), "pp_session").empty());
}

TEST_CASE("a failed callback never leaks the code, token, or secret (AC22)", "[auth][security]") {
    FakeGitHub gh;
    gh.tokenResponse = {401, R"({"error":"bad_verification_code","hint":"client-secret"})", false};
    const auto r = callbackService(gh).handleGithubCallback(callbackRequest("the-code", validState()));
    for (const std::string secret : {"client-secret", "the-code", "gho_fake"}) {
        CHECK(r.body.find(secret) == std::string::npos);
        CHECK(r.get_header_value("Location").find(secret) == std::string::npos);
    }
}

// ── AC5: logout ──

TEST_CASE("logout expires the session cookie", "[auth][logout]") {
    const auto r = authedService().handleLogout(sameOriginPost(validSessionCookie()));
    CHECK(r.code == 200);
    const std::string cookie = setCookieNamed(r, "pp_session");
    CHECK(cookie.find("Max-Age=0") != std::string::npos);
    CHECK(cookie.find("HttpOnly") != std::string::npos);
}

TEST_CASE("a cross-site logout POST is rejected", "[auth][logout][csrf]") {
    crow::request req = sameOriginPost(validSessionCookie());
    req.headers.clear();
    req.add_header("Cookie", "pp_session=" + validSessionToken());
    req.add_header("Origin", "https://evil.example");
    CHECK(authedService().handleLogout(req).code == 403);
}

// ── AC26: /auth/me ──

TEST_CASE("me returns the identity and an id-derived avatar for a valid cookie", "[auth][me]") {
    const auto r = authedService().handleAuthMe(getWithCookie(validSessionCookie()));
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
        const auto r = authedService().handleAuthMe(getWithRawCookie(cookie));
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
    const auto r = authedService().handleAuthMe(getWithCookie(validSessionCookie()));
    CHECK(r.body.find(validSessionToken()) == std::string::npos);
    CHECK(r.body.find(oauthCfg().sessionSigningSecret) == std::string::npos);
}
```

Build `crow::request` fixtures with `req.add_header("Cookie", ...)` and
`req.url_params = crow::query_string("?code=...&state=...")` — both verified against the
bundled Crow (`third_party/crow/include/crow/http_request.h:74-85`,
`query_string.h:345,381`).

- [ ] **Step 3: Register and confirm failure**

```cmake
add_puzzpool_test(test_auth_service)
```

Run: `ctest --test-dir build --output-on-failure --tests-regex auth_service`

Expected: fails to compile — `AuthService` does not exist.

- [ ] **Step 4: Implement the handlers**

Shape each one as: configuration gate → parse → verify → act → serialise. Notes that matter:

- **login** — 503 if `sessionSigningSecret`, `githubOauthClientId`, `githubOauthClientSecret`, or `publicBaseUrl` is empty. Nonce is `base64UrlEncode(nonce_(32))`. The `pp_oauth_state` cookie holds `issueSessionToken(secret, {nonce, ""}, now + 600)`; the URL's `state` parameter is the bare nonce. The signature makes the cookie unforgeable, the cookie makes the state browser-bound, and comparing the two in the callback is what closes the login-CSRF hole. Scope is empty (`scope=`) per AC1's "no-scope". URL-encode every parameter.
- **callback** — 503 on missing configuration. Then: read `code`/`state` via `req.url_params.get()` (returns `char*`, so null-check); read `pp_oauth_state`; `verifySessionToken` it; `constantTimeEquals(nonceFromCookie, stateParam)`. Always emit the state-clearing `Set-Cookie`, on success and failure alike — that is what makes it single-use. Exchange with `Accept: application/json` and body `client_id=…&client_secret=…&code=…&redirect_uri=…`, all URL-encoded. Treat a 200 that contains `error` and no `access_token` as failure (GitHub does this). Then `GET https://api.github.com/user` with `Authorization: Bearer …`, `Accept: application/vnd.github+json`, `User-Agent: puzzpool`. Require a non-empty string `login` and an integer `id`. Issue `pp_session` with `now + sessionTtlMinutes * 60`. `nlohmann::json::parse` must use the non-throwing overload or be wrapped — a malformed provider body must not 500.
- **logout** — 503 when unconfigured; reject without same-origin proof (403, reusing `hasSameOriginProof`); otherwise emit the `Max-Age=0` clearing cookie and `200 {"authenticated":false}`. Clearing does not require a valid session, so a stale cookie can always be shed.
- **me** — 503 when unconfigured; otherwise always 200. `{"authenticated":false}` for every failure mode, with no `reason` field: distinguishing "expired" from "forged" tells an attacker which half of the token to work on.

Cookie attribute string, one shared helper:
`pp_session=<token>; Path=/; Max-Age=<ttl>; HttpOnly; Secure; SameSite=Lax`
and for the state cookie `Path=/api/v1/auth; Max-Age=600`. Use `res.add_header("Set-Cookie", …)` — not `set_header` — because the callback emits two (`http_response.h:60,69`).

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
- Modify: `.env.example`, `README.md`, `docs/api.md`, `docs/security.md`, `docs/architecture.md`, `docs/architecture-review.md`, `docs/testing.md`, `deploy/nginx.conf`

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

Before `## Admin API` (`docs/api.md:422`), document all four routes: method, path, parameters, redirect behavior, `Set-Cookie` attributes (without any example token value), the `/auth/me` response shape for both states, and the 503-when-unconfigured rule. Then extend the Admin API preamble with the two accepted mechanisms, the `401` fail-closed rule, and the `403 csrf_check_failed` response for cookie-authorized `POST`s without same-origin proof.

- [ ] **Step 4: `docs/security.md` — rewrite Admin Route Protection**

`docs/security.md:16-50` currently presents "Option A — Nginx IP restriction" as sufficient with a blank token, which this change makes false. Rewrite to: mechanisms (token, GitHub sign-in), the fail-closed rule, the allow-list and its immediate-revocation property, cookie properties and why each is set, the CSRF defense and why absence of a signal is rejected, constant-time comparison, `SESSION_SIGNING_SECRET` handling and rotation (rotation invalidates all sessions — that is the revocation lever), the deliberate non-MAC status of `keyedDigestHex`, and the Nginx layer as remaining defense in depth.

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
write path and a retention obligation for no gain. The trade-off is accepted deliberately —
an individual session cannot be revoked before expiry; revocation is by allow-list removal,
which takes effect on the next request because the guard re-reads the list from `Config`
every time, or by rotating `SESSION_SIGNING_SECRET`, which invalidates all sessions at once.
The guard moved from a lambda in `main.cpp` into `puzzpool_core` so the unconfigured case is
directly testable, and now denies by default instead of returning `nullopt`.
```

- [ ] **Step 7: `docs/testing.md` and `deploy/nginx.conf`**

List the four new test binaries and what each covers. In `deploy/nginx.conf`, add a comment above `location /` recording that `/api/v1/auth/*` is intentionally public (AC23) and that `X-Forwarded-Proto` must stay set so the deployment terminates TLS in front of the `Secure` cookie. No `location` blocks change.

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

- [ ] **Step 4: Local-server smoke test — route wiring and startup changed, so this is required**

Per `docs/testing.md`, start the server and check:
- with no auth configured: startup prints the warning naming `ADMIN_TOKEN` and `ADMIN_GITHUB_USERS`, and `GET /api/v1/admin/puzzles` returns `401` (the fail-closed behavior, end to end);
- with `ADMIN_TOKEN` set: the same route with `X-Admin-Token` returns `200` (AC12 intact);
- with no `SESSION_SIGNING_SECRET`: `GET /api/v1/auth/me` returns `503`;
- with the full OAuth configuration but no cookie: `GET /api/v1/auth/me` returns `200 {"authenticated":false}`.

Use a throwaway token value; never a real credential. Delete `pool.db` afterwards.

- [ ] **Step 5: Self-review the diff against AC22 before pushing**

```bash
git diff dev...HEAD | grep -nEi 'client_secret=[^&"]|gho_|ADMIN_TOKEN=[^[:space:]]|SESSION_SIGNING_SECRET=[^[:space:]]'
```

Expected: no hits outside `.env.example` placeholders and the documented `openssl rand` recipes. Confirm no `pool.db`, `.env`, WAL/SHM file, or build artifact is staged.

- [ ] **Step 6: Push and open the implementation PR**

```bash
git push -u origin feature/153-github-oauth-backend
gh pr create --draft --base dev --title "feat: GitHub OAuth backend and fail-closed admin authorization" \
             --body "Implements slice A of #149. Closes #153."
```

---

## Requirement → design → test matrix

| AC | Where it is implemented | Where it is proven |
|----|------------------------|--------------------|
| AC1 state | `auth_service.cpp` login/callback, signed `pp_oauth_state` | `test_auth_service` — bound, distinct, cleared, mismatch/expired/tampered rejected |
| AC2 libcurl seam | `github_client.cpp`, `auth_service.cpp` exchange | `test_auth_service` — secret/code asserted in POST body, absent from URL |
| AC3 signed cookie | `session.cpp`, cookie helper in `auth_service.cpp` | `test_session` (sign/verify/tamper/expire), `test_auth_service` (attributes) |
| AC4 frozen digest | `hash_utils.cpp` `keyedDigestHex`, `permutation.cpp:27` | `test_hash_utils` byte-stability + difference; existing `test_permutation` |
| AC5 logout | `auth_service.cpp` `handleLogout` | `test_auth_service` — `Max-Age=0`, cross-site rejected |
| AC6 allow-list parsing | `admin_auth.cpp` `parseAdminGithubUsers`, `isAllowedAdminLogin` | `test_config` — trim, case, empties, empty-denies |
| AC7 re-evaluated per request | `authorizeAdmin` reads `cfg` every call | `test_admin_auth` — removal denies an already-issued cookie |
| AC8 extracted guard | `admin_auth.cpp` + `admin_guard.cpp`, `main.cpp` wiring only | the guard tests exist at all — they link `puzzpool_core` |
| AC9 fail closed | `authorizeAdmin` default-deny | `test_admin_auth` unconfigured→401, with a configured-mechanism sensitivity control |
| AC10 upgrade note + diagnostic | `.env.example`, `README.md`, `startupAuthDiagnostics` | `test_config` diagnostics, incl. quiet-when-configured and no-secret checks |
| AC11 constant time | `constantTimeEquals` for token and MAC | `test_hash_utils` truth table; used in `authorizeAdmin`/`verifySessionToken` |
| AC12 token still works | first branch of `authorizeAdmin` | `test_admin_auth` token accept/reject, and token POST skips CSRF |
| AC13 503 without secret | config gate in all four handlers; cookie branch refuses empty secret | `test_auth_service` 503×4; `test_session`/`test_admin_auth` empty-secret |
| AC14 CSRF | `hasSameOriginProof`, applied to cookie-authorized POSTs | `test_admin_auth` — bare/cross-site/cross-fetch denied, two positive controls |
| AC20 per-stage credentials | plain env reads, no `Config::stage` branch | `test_config` — values identical across `STAGE` |
| AC21 documented env | `.env.example`, `README.md` | reviewed in Task 7; no real values |
| AC22 no leaks | fixed error codes; no verbose curl; no secret in bodies | leak assertions in `test_admin_auth` and `test_auth_service` |
| AC23 Nginx unchanged | comment only in `deploy/nginx.conf` | diff review — no `location` block changes |
| AC24 docs with behavior | Task 7 | the same PR as the behavior |
| AC25 both suites | Task 8 | recorded commands and counts |
| AC26 `/auth/me` | `auth_service.cpp` `handleAuthMe` | `test_auth_service` — signed-in, signed-out, tampered, expired, avatar from id |

AC15–AC19 and AC27 are slice B and are deliberately untouched here.

## Self-Review

- **Spec coverage:** every AC in slice A's scope appears in the matrix with a named implementation site and a named test. Nothing is deferred to "a follow-up".
- **Sensitivity:** the three assertions that could pass vacuously each carry a control — the unconfigured-401 case is paired with a configured-allow case, the CSRF denials are paired with two positive same-origin controls, and the quiet-diagnostics case pairs with the warning case. The frozen-digest check is paired with a test that the two primitives differ, so a rename that aliased them would fail.
- **Placeholder scan:** no `TODO`, `TBD`, or "tests later" remains. The four helper functions referenced but not spelled out in test snippets (`setCookieNamed`, `stateParamOf`, `cookieTokenOf`, `validState`) are local test fixtures whose bodies are mechanical.
- **Type consistency:** the GitHub id is a string end to end (`SessionIdentity::githubId`, cookie field, avatar URL) and is converted once at the callback boundary, where GitHub sends it as a JSON integer.
- **Scope:** no frontend file, no database schema, no allocator or permutation output, and no `service_*.cpp` file is modified. The only change to existing behavior is that unconfigured admin routes now deny, which is the point of AC9.
