#include <puzzpool/admin_auth.hpp>
#include <puzzpool/admin_guard.hpp>
#include <puzzpool/session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

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

} // namespace

// ── AC9: default deny ────────────────────────────────────────────────────────

// The headline regression. Under the pre-change guard in main.cpp this returned
// nullopt and admitted everyone.
TEST_CASE("with no mechanism configured every admin request is denied", "[admin][authz]") {
    Config cfg;  // no token, no allow-list
    AdminRequestView v;
    const auto d = authorizeAdmin(cfg, v, kNow);
    CHECK_FALSE(d.allowed);
    CHECK(d.statusCode == 401);
}

// Sensitivity for the assertion above: the same probe must return allowed==true when
// a mechanism IS configured, proving it detects authorization rather than always
// denying.
TEST_CASE("a configured token authorizes the same probe", "[admin][authz]") {
    Config cfg;
    cfg.adminToken = "secret-token";
    AdminRequestView v;
    v.adminTokenHeader = "secret-token";
    const auto d = authorizeAdmin(cfg, v, kNow);
    CHECK(d.allowed);
    CHECK(d.mechanism == AdminAuthMechanism::Token);
}

// ── AC12: the configured token keeps working ─────────────────────────────────

TEST_CASE("a wrong or absent admin token is denied (AC12)", "[admin][authz]") {
    Config cfg;
    cfg.adminToken = "secret-token";
    AdminRequestView wrong;  wrong.adminTokenHeader = "nope";
    AdminRequestView absent;
    // A prefix of the real token must not authorize — constantTimeEquals compares
    // lengths, so this would only pass under a starts-with comparison.
    AdminRequestView prefix;  prefix.adminTokenHeader = "secret";
    CHECK_FALSE(authorizeAdmin(cfg, wrong, kNow).allowed);
    CHECK_FALSE(authorizeAdmin(cfg, absent, kNow).allowed);
    CHECK_FALSE(authorizeAdmin(cfg, prefix, kNow).allowed);
    CHECK(authorizeAdmin(cfg, absent, kNow).statusCode == 401);
}

// An empty X-Admin-Token must never match an empty configured token: the empty
// configuration is what AC9 makes fail closed.
TEST_CASE("an empty token header cannot match an unset ADMIN_TOKEN", "[admin][authz]") {
    Config cfg;
    AdminRequestView v;
    v.adminTokenHeader = "";
    CHECK_FALSE(authorizeAdmin(cfg, v, kNow).allowed);
}

// ── AC6/AC7: the cookie mechanism ────────────────────────────────────────────

TEST_CASE("an allow-listed login in a valid cookie authorizes (AC7)", "[admin][authz]") {
    const Config cfg = authCfg();
    const auto d = authorizeAdmin(cfg, cookieGet(tokenFor(cfg, "alice")), kNow);
    CHECK(d.allowed);
    CHECK(d.mechanism == AdminAuthMechanism::Cookie);
    CHECK(d.login == "alice");
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

// AC7 — no authorization decision is cached: once the process holds the new
// allow-list, an already-issued cookie stops working on the very next request, with
// no cookie reissue and no session store to purge. Note what this does NOT say:
// main.cpp loads Config once per process, so editing ADMIN_GITHUB_USERS still
// requires a restart.
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
    CHECK_FALSE(authorizeAdmin(cfg, cookieGet(""), kNow).allowed);
    CHECK_FALSE(authorizeAdmin(cfg, AdminRequestView{}, kNow).allowed);
}

// A state blob is signed with the same key as a session blob; the purpose label is
// what stops it from authorizing an admin request.
TEST_CASE("an OAuth state blob does not authorize as a session", "[admin][authz][security]") {
    const Config cfg = authCfg();
    AdminRequestView v;
    v.cookieHeader = "pp_session=" + issueStateToken(cfg.sessionSigningSecret,
                                                     "alice", kNow + 600);
    CHECK_FALSE(authorizeAdmin(cfg, v, kNow).allowed);
}

// AC13 — no signing secret means no cookie is ever accepted, allow-list or not.
TEST_CASE("an unset signing secret disables cookie authorization", "[admin][authz][security]") {
    const Config signing = authCfg();
    const auto token = tokenFor(signing, "alice");
    Config cfg = signing;
    cfg.sessionSigningSecret.clear();
    CHECK_FALSE(authorizeAdmin(cfg, cookieGet(token), kNow).allowed);
}

// ── AC14: CSRF ───────────────────────────────────────────────────────────────

TEST_CASE("a cookie-authorized POST needs an affirmative same-origin signal", "[admin][csrf]") {
    const Config cfg = authCfg();
    const auto token = tokenFor(cfg, "alice");

    AdminRequestView bare = cookieGet(token);  bare.isPost = true;  // no Origin, no Sec-Fetch-Site
    const auto denied = authorizeAdmin(cfg, bare, kNow);
    CHECK_FALSE(denied.allowed);
    CHECK(denied.statusCode == 403);

    AdminRequestView cross = bare;  cross.origin = "https://evil.example";
    CHECK_FALSE(authorizeAdmin(cfg, cross, kNow).allowed);

    AdminRequestView crossFetch = bare;  crossFetch.secFetchSite = "cross-site";
    CHECK_FALSE(authorizeAdmin(cfg, crossFetch, kNow).allowed);

    // An explicit cross-site signal is not overridable by a matching Origin.
    AdminRequestView crossFetchWithOrigin = crossFetch;
    crossFetchWithOrigin.origin = "https://puzzle.b58.de";
    CHECK_FALSE(authorizeAdmin(cfg, crossFetchWithOrigin, kNow).allowed);

    AdminRequestView sameSite = bare;  sameSite.secFetchSite = "same-site";
    CHECK_FALSE(authorizeAdmin(cfg, sameSite, kNow).allowed);

    // Positive controls — the check accepts genuine same-origin requests.
    CHECK(authorizeAdmin(cfg, cookiePost(token), kNow).allowed);
    AdminRequestView sameOrigin = bare;  sameOrigin.origin = "https://puzzle.b58.de";
    CHECK(authorizeAdmin(cfg, sameOrigin, kNow).allowed);
    AdminRequestView noneFetch = bare;  noneFetch.secFetchSite = "none";
    CHECK(authorizeAdmin(cfg, noneFetch, kNow).allowed);
}

// A same-host Origin on a different scheme or port is a different origin.
TEST_CASE("the Origin comparison covers scheme and port", "[admin][csrf]") {
    const Config cfg = authCfg();
    const auto token = tokenFor(cfg, "alice");
    AdminRequestView base = cookieGet(token);  base.isPost = true;

    for (const std::string bad : {std::string("http://puzzle.b58.de"),
                                  std::string("https://puzzle.b58.de:8443"),
                                  std::string("https://evil.puzzle.b58.de")}) {
        INFO(bad);
        AdminRequestView v = base;  v.origin = bad;
        CHECK_FALSE(authorizeAdmin(cfg, v, kNow).allowed);
    }
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

// ── AC22: denials leak nothing ───────────────────────────────────────────────

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

// Every rejection cause returns the same 401 body, so the guard is not an oracle for
// whether a login exists, is allow-listed, or has merely expired.
TEST_CASE("all identity-related denials are indistinguishable", "[admin][security]") {
    const Config cfg = authCfg();
    for (const auto& v : {cookieGet(tokenFor(cfg, "mallory")),
                          cookieGet(tokenFor(cfg, "alice", kNow - 1)),
                          cookieGet("not-a-token"),
                          AdminRequestView{}}) {
        const auto d = authorizeAdmin(cfg, v, kNow);
        CHECK_FALSE(d.allowed);
        CHECK(d.statusCode == 401);
        CHECK(d.error == "unauthorized");
    }
}

// ── The Crow adapter ─────────────────────────────────────────────────────────

TEST_CASE("adminGuard denies with JSON and leaks no header value", "[admin][guard]") {
    Config cfg = authCfg();
    cfg.adminToken = "secret-token";
    crow::request req;
    req.method = crow::HTTPMethod::Post;
    req.add_header("X-Admin-Token", "wrong-but-secret");

    auto denied = adminGuard(cfg, req);
    REQUIRE(denied.has_value());
    CHECK(denied->code == 401);
    CHECK(denied->body == R"({"error":"unauthorized"})");
    CHECK(denied->body.find("wrong-but-secret") == std::string::npos);
}

TEST_CASE("adminGuard admits a valid token and returns nullopt", "[admin][guard]") {
    Config cfg = authCfg();
    cfg.adminToken = "secret-token";
    crow::request req;
    req.method = crow::HTTPMethod::Post;
    req.add_header("X-Admin-Token", "secret-token");
    CHECK_FALSE(adminGuard(cfg, req).has_value());
}

TEST_CASE("adminGuard denies when nothing is configured", "[admin][guard]") {
    const Config cfg;   // AC9
    crow::request req;
    req.method = crow::HTTPMethod::Get;
    auto denied = adminGuard(cfg, req);
    REQUIRE(denied.has_value());
    CHECK(denied->code == 401);
}
