#include <puzzpool/auth_service.hpp>

#include <puzzpool/base64.hpp>
#include <puzzpool/session.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace puzzpool;

namespace {

constexpr int64_t kNow = 1'770'000'000;
constexpr const char* kSecret = "signing-secret";
constexpr const char* kNonce = "the-nonce";

Config oauthCfg() {
    Config cfg;
    cfg.sessionSigningSecret = kSecret;
    cfg.adminGithubUsers = {"alice"};
    cfg.publicBaseUrl = "https://puzzle.b58.de";
    cfg.githubOauthClientId = "client-id";
    cfg.githubOauthClientSecret = "client-secret";
    cfg.sessionTtlMinutes = 720;
    return cfg;
}

struct FakeGitHub {
    std::string lastPostUrl, lastPostBody, lastGetUrl;
    std::vector<std::string> lastPostHeaders, lastGetHeaders;
    int postCalls = 0;
    int getCalls  = 0;
    HttpResult tokenResponse{200, R"({"access_token":"gho_fake","token_type":"bearer"})", false};
    HttpResult userResponse{200, R"({"login":"alice","id":4242})", false};

    GitHubHttpClient client() {
        return {
            [this](const std::string& url, const std::string& body,
                   const std::vector<std::string>& h) {
                lastPostUrl = url; lastPostBody = body; lastPostHeaders = h;
                ++postCalls;
                return tokenResponse;
            },
            [this](const std::string& url, const std::vector<std::string>& h) {
                lastGetUrl = url; lastGetHeaders = h;
                ++getCalls;
                return userResponse;
            },
        };
    }
};

// A deterministic nonce source. secureRandomBytes is exercised in test_session.
std::string fixedNonce(std::size_t n) { return std::string(n, 'N'); }

AuthService serviceWith(const Config& cfg, FakeGitHub& gh) {
    return AuthService{cfg, gh.client(), [] { return kNow; }, fixedNonce};
}

// Returns the first Set-Cookie header whose cookie name is `name`, or "" when absent.
//
// Deliberately not crow::response::get_header_value(): that returns only the FIRST
// match, and the callback emits two Set-Cookie headers — one clearing
// pp_oauth_state, one issuing pp_session. Using it would make about half of the
// assertions below pass or fail for the wrong reason. crow::response::headers is a
// ci_map, i.e. an unordered_multimap, so scan equal_range.
std::string setCookieNamed(crow::response& r, const std::string& name) {
    const std::string prefix = name + "=";
    const auto range = r.headers.equal_range("Set-Cookie");
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.rfind(prefix, 0) == 0) return it->second;
    }
    return "";
}

/// The cookie's value, i.e. everything between '=' and the first ';'.
std::string cookieTokenOf(const std::string& setCookie) {
    const auto eq = setCookie.find('=');
    if (eq == std::string::npos) return "";
    const auto semi = setCookie.find(';', eq + 1);
    return setCookie.substr(eq + 1, semi == std::string::npos ? std::string::npos
                                                             : semi - eq - 1);
}

/// The `state` query parameter of a redirect Location.
std::string stateParamOf(const std::string& location) {
    const auto pos = location.find("state=");
    if (pos == std::string::npos) return "";
    const auto start = pos + 6;
    const auto amp = location.find('&', start);
    return location.substr(start, amp == std::string::npos ? std::string::npos
                                                           : amp - start);
}

std::string validStateBlob(int64_t expiresAt = kNow + 600) {
    return issueStateToken(kSecret, kNonce, expiresAt);
}

crow::request callbackRequest(const std::string& code, const std::string& stateParam,
                              const std::string& stateCookie = validStateBlob()) {
    crow::request req;
    req.method = crow::HTTPMethod::Get;
    req.url_params = crow::query_string("?code=" + code + "&state=" + stateParam);
    if (!stateCookie.empty()) {
        req.add_header("Cookie", "pp_oauth_state=" + stateCookie);
    }
    return req;
}

std::string validSessionToken(const std::string& login = "alice",
                              int64_t expiresAt = kNow + 600) {
    return issueSessionToken(kSecret, {login, "4242"}, expiresAt);
}

crow::request getWithRawCookie(const std::string& cookie) {
    crow::request req;
    req.method = crow::HTTPMethod::Get;
    if (!cookie.empty()) req.add_header("Cookie", cookie);
    return req;
}

crow::request sameOriginPost(const std::string& cookie) {
    crow::request req;
    req.method = crow::HTTPMethod::Post;
    if (!cookie.empty()) req.add_header("Cookie", cookie);
    req.add_header("Sec-Fetch-Site", "same-origin");
    return req;
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) { out += s; out += "\n"; }
    return out;
}

} // namespace

// ── AC13 / AC26: unconfigured means 503, never a silently unsigned cookie ────

TEST_CASE("the auth routes return 503 without SESSION_SIGNING_SECRET", "[auth][config]") {
    Config cfg = oauthCfg();
    cfg.sessionSigningSecret.clear();
    FakeGitHub gh;
    auto svc = serviceWith(cfg, gh);

    CHECK(svc.handleGithubLogin(crow::request{}).code == 503);
    CHECK(svc.handleGithubCallback(crow::request{}).code == 503);
    CHECK(svc.handleAuthMe(crow::request{}).code == 503);
    // handleLogout is deliberately excluded: clearing a cookie needs no signing key,
    // and 503 here would strand a stale cookie exactly when the secret was rotated
    // away. It still enforces same-origin, so a bare request is 403, never 503.
    CHECK(svc.handleLogout(crow::request{}).code == 403);
}

// Sensitivity for the three 503s above: the same probes must not return 503 once
// the secret is present, so the assertion detects configuration rather than a route
// that is 503 unconditionally.
TEST_CASE("the same probes leave 503 behind once the secret is set", "[auth][config]") {
    FakeGitHub gh;
    auto svc = serviceWith(oauthCfg(), gh);
    CHECK(svc.handleGithubLogin(crow::request{}).code != 503);
    CHECK(svc.handleGithubCallback(crow::request{}).code != 503);
    CHECK(svc.handleAuthMe(crow::request{}).code != 503);
}

TEST_CASE("login returns 503 when the OAuth app is not configured", "[auth][config]") {
    for (int missing = 0; missing < 3; ++missing) {
        Config cfg = oauthCfg();
        if (missing == 0) cfg.githubOauthClientId.clear();
        if (missing == 1) cfg.githubOauthClientSecret.clear();
        if (missing == 2) cfg.publicBaseUrl.clear();
        FakeGitHub gh;
        INFO("missing field " << missing);
        CHECK(serviceWith(cfg, gh).handleGithubLogin(crow::request{}).code == 503);
    }
}

// A 503 must never be answered with a session cookie.
TEST_CASE("an unconfigured callback issues no cookie", "[auth][config][security]") {
    Config cfg = oauthCfg();
    cfg.sessionSigningSecret.clear();
    FakeGitHub gh;
    auto svc = serviceWith(cfg, gh);
    auto r = svc.handleGithubCallback(callbackRequest("the-code", kNonce));
    CHECK(r.code == 503);
    CHECK(setCookieNamed(r, "pp_session").empty());
    CHECK(gh.postCalls == 0);   // and never reaches the provider
}

// ── AC1: unguessable, browser-bound, single-use state ────────────────────────

TEST_CASE("login redirects to GitHub and binds the state to a cookie", "[auth][oauth]") {
    FakeGitHub gh;
    auto svc = serviceWith(oauthCfg(), gh);
    auto r = svc.handleGithubLogin(crow::request{});

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
    // Path is asserted here as well as on the clearing header, because the two must
    // match for the clear to have any effect at all.
    CHECK(stateCookie.find("Path=/api/v1/auth") != std::string::npos);

    // Bound, but not equal. Verify the cookie blob and compare its SUBJECT to the
    // URL parameter rather than substring-searching the cookie for the parameter:
    // signingInput base64url-encodes the subject a second time, so a `find` would
    // fail against a perfectly correct handler. This is AC1's only login-side proof
    // of browser binding — the callback cases build the cookie/parameter pair by
    // hand and so cannot detect a handler that writes the wrong value into either.
    const auto issuedState = verifyStateToken(kSecret, cookieTokenOf(stateCookie), kNow);
    REQUIRE(issuedState.error == SessionError::None);
    CHECK(issuedState.identity.login == stateParamOf(location));
    // The signed blob must stay in the cookie: leaking it into the redirect URL
    // would hand it to GitHub's logs and to any Referer on the way.
    CHECK(cookieTokenOf(stateCookie) != stateParamOf(location));
}

TEST_CASE("the login URL never carries the client secret (AC22)", "[auth][security]") {
    FakeGitHub gh;
    auto r = serviceWith(oauthCfg(), gh).handleGithubLogin(crow::request{});
    CHECK(r.get_header_value("Location").find("client-secret") == std::string::npos);
    CHECK(r.body.find("client-secret") == std::string::npos);
}

TEST_CASE("two logins produce different state values", "[auth][oauth]") {
    FakeGitHub gh;
    // Real entropy here, not fixedNonce — this is the case that would catch a
    // handler that minted a constant state.
    AuthService svc{oauthCfg(), gh.client(), [] { return kNow; }, {}};
    auto first  = svc.handleGithubLogin(crow::request{});
    auto second = svc.handleGithubLogin(crow::request{});
    CHECK(stateParamOf(first.get_header_value("Location")) !=
          stateParamOf(second.get_header_value("Location")));
}

// ── AC2 / AC3: the callback ──────────────────────────────────────────────────

TEST_CASE("a valid callback issues a hardened session cookie", "[auth][oauth]") {
    FakeGitHub gh;
    auto r = serviceWith(oauthCfg(), gh).handleGithubCallback(
        callbackRequest("the-code", kNonce));

    REQUIRE(r.code == 302);
    CHECK(r.get_header_value("Location") == "/");

    const std::string cookie = setCookieNamed(r, "pp_session");
    REQUIRE_FALSE(cookie.empty());
    CHECK(cookie.find("HttpOnly") != std::string::npos);
    CHECK(cookie.find("Secure") != std::string::npos);
    CHECK(cookie.find("SameSite=Lax") != std::string::npos);
    CHECK(cookie.find("Path=/;") != std::string::npos);

    const auto session = verifySessionToken(kSecret, cookieTokenOf(cookie), kNow);
    REQUIRE(session.error == SessionError::None);
    CHECK(session.identity.login == "alice");
    CHECK(session.identity.githubId == "4242");   // AC26 — the id is in the payload
    CHECK(session.expiresAt == kNow + 720 * 60);
}

TEST_CASE("the code and secret travel in the POST body, not the URL (AC2)",
          "[auth][security]") {
    FakeGitHub gh;
    serviceWith(oauthCfg(), gh).handleGithubCallback(callbackRequest("the-code", kNonce));

    CHECK(gh.lastPostUrl == "https://github.com/login/oauth/access_token");
    CHECK(gh.lastPostUrl.find("client-secret") == std::string::npos);
    CHECK(gh.lastPostUrl.find("the-code") == std::string::npos);
    CHECK(gh.lastPostBody.find("client_secret=client-secret") != std::string::npos);
    CHECK(gh.lastPostBody.find("code=the-code") != std::string::npos);
    CHECK(gh.lastGetUrl == "https://api.github.com/user");
    CHECK(joined(gh.lastGetHeaders).find("Authorization: Bearer gho_fake")
          != std::string::npos);
    // The access token must not travel to GitHub's token endpoint as a header, and
    // the secret must never appear in any header on either call.
    CHECK(joined(gh.lastPostHeaders).find("client-secret") == std::string::npos);
    CHECK(joined(gh.lastGetHeaders).find("client-secret") == std::string::npos);
}

// AC1 single-use. A clearing Set-Cookie removes nothing unless its name, Path and
// Domain all match the issuing header (RFC 6265) — so Max-Age=0 alone is not
// evidence that the cookie is gone. The state cookie is issued with
// Path=/api/v1/auth, so a clear emitted with Path=/ or no Path at all leaves the
// original in the browser and the blob stays replayable for its full window.
TEST_CASE("the state cookie is cleared on every callback, making it single-use",
          "[auth][oauth]") {
    FakeGitHub gh;
    auto r = serviceWith(oauthCfg(), gh).handleGithubCallback(callbackRequest("c", kNonce));
    const std::string cleared = setCookieNamed(r, "pp_oauth_state");
    REQUIRE_FALSE(cleared.empty());
    CHECK(cleared.find("Max-Age=0") != std::string::npos);
    CHECK(cleared.find("Path=/api/v1/auth") != std::string::npos);
    CHECK(cleared.find("HttpOnly") != std::string::npos);
    CHECK(cleared.find("Secure") != std::string::npos);
    CHECK(cookieTokenOf(cleared).empty());
}

TEST_CASE("callbacks that fail state validation issue no session",
          "[auth][oauth][security]") {
    struct Case { std::string name; crow::request req; };
    std::vector<Case> cases;
    cases.push_back({"no state cookie", callbackRequest("c", kNonce, "")});
    cases.push_back({"state mismatch", callbackRequest("c", "some-other-nonce")});
    cases.push_back({"missing state param", callbackRequest("c", "")});
    cases.push_back({"missing code", callbackRequest("", kNonce)});
    cases.push_back({"expired state",
                     callbackRequest("c", kNonce, validStateBlob(kNow - 1))});
    {
        std::string tampered = validStateBlob();
        tampered.back() = (tampered.back() == 'a') ? 'b' : 'a';
        cases.push_back({"tampered state cookie", callbackRequest("c", kNonce, tampered)});
    }
    // The end-to-end form of the domain-separation case: a valid session blob
    // presented where a state blob is expected.
    cases.push_back({"session blob as state",
                     callbackRequest("c", kNonce,
                                     issueSessionToken(kSecret, {kNonce, ""}, kNow + 600))});

    for (auto& c : cases) {
        INFO(c.name);
        FakeGitHub gh;
        auto r = serviceWith(oauthCfg(), gh).handleGithubCallback(c.req);
        CHECK(setCookieNamed(r, "pp_session").empty());
        CHECK(r.get_header_value("Location").find("auth=error") != std::string::npos);
        // The state cookie is cleared even on the failure paths — that is what stops
        // a failed attempt from leaving a reusable blob behind.
        CHECK(setCookieNamed(r, "pp_oauth_state").find("Path=/api/v1/auth")
              != std::string::npos);
        // A request that never proved browser binding must not cost an outbound call.
        CHECK(gh.postCalls == 0);
    }
}

TEST_CASE("provider failures issue no session", "[auth][oauth]") {
    for (auto response : {HttpResult{401, R"({"error":"bad_verification_code"})", false},
                          HttpResult{200, R"({"error":"bad_verification_code"})", false},
                          HttpResult{200, "not json", false},
                          HttpResult{200, R"({"access_token":""})", false},
                          HttpResult{0, "", true}}) {
        FakeGitHub gh;
        gh.tokenResponse = response;
        auto r = serviceWith(oauthCfg(), gh).handleGithubCallback(
            callbackRequest("c", kNonce));
        INFO(response.body);
        CHECK(setCookieNamed(r, "pp_session").empty());
        CHECK(r.get_header_value("Location").find("auth=error") != std::string::npos);
    }

    for (auto response : {HttpResult{403, R"({"message":"Forbidden"})", false},
                          HttpResult{200, R"({"id":1})", false},          // login missing
                          HttpResult{200, R"({"login":"alice"})", false}, // id missing
                          HttpResult{200, R"({"login":"","id":1})", false},
                          HttpResult{200, R"({"login":"alice","id":"4242"})", false},
                          HttpResult{0, "", true}}) {
        FakeGitHub gh;
        gh.userResponse = response;
        auto r = serviceWith(oauthCfg(), gh).handleGithubCallback(
            callbackRequest("c", kNonce));
        INFO(response.body);
        CHECK(setCookieNamed(r, "pp_session").empty());
    }
}

// Sensitivity for every `setCookieNamed(r, "pp_session").empty()` above: the same
// probe must find a cookie on the success path, or it is asserting that the helper
// is broken rather than that no session was issued.
TEST_CASE("the no-session probe finds a session on the success path", "[auth][oauth]") {
    FakeGitHub gh;
    auto r = serviceWith(oauthCfg(), gh).handleGithubCallback(callbackRequest("c", kNonce));
    CHECK_FALSE(setCookieNamed(r, "pp_session").empty());
}

TEST_CASE("a failed callback never leaks the code, token, or secret (AC22)",
          "[auth][security]") {
    FakeGitHub gh;
    gh.tokenResponse = {401, R"({"error":"bad_verification_code","hint":"client-secret"})",
                        false};
    auto r = serviceWith(oauthCfg(), gh).handleGithubCallback(
        callbackRequest("the-code", kNonce));
    for (const std::string secret : {std::string("client-secret"), std::string("the-code"),
                                     std::string("gho_fake")}) {
        INFO(secret);
        CHECK(r.body.find(secret) == std::string::npos);
        CHECK(r.get_header_value("Location").find(secret) == std::string::npos);
    }
}

// A non-allow-listed GitHub user still gets a session: authentication and
// authorization are separate, and the admin guard is what protects admin routes.
TEST_CASE("any GitHub account receives a session, allow-listed or not", "[auth][oauth]") {
    FakeGitHub gh;
    gh.userResponse = {200, R"({"login":"mallory","id":99})", false};
    auto r = serviceWith(oauthCfg(), gh).handleGithubCallback(callbackRequest("c", kNonce));
    const std::string cookie = setCookieNamed(r, "pp_session");
    REQUIRE_FALSE(cookie.empty());
    CHECK(verifySessionToken(kSecret, cookieTokenOf(cookie), kNow).identity.login
          == "mallory");
}

// ── AC5: logout ──────────────────────────────────────────────────────────────

TEST_CASE("logout expires the session cookie", "[auth][logout]") {
    FakeGitHub gh;
    auto r = serviceWith(oauthCfg(), gh).handleLogout(
        sameOriginPost("pp_session=" + validSessionToken()));
    CHECK(r.code == 200);
    const std::string cookie = setCookieNamed(r, "pp_session");
    REQUIRE_FALSE(cookie.empty());
    CHECK(cookie.find("Max-Age=0") != std::string::npos);
    CHECK(cookie.find("HttpOnly") != std::string::npos);
    // The session cookie is issued with Path=/, so the clearing header must carry
    // Path=/ too or the browser keeps the original and logout silently does nothing.
    CHECK(cookie.find("Path=/;") != std::string::npos);
    CHECK(cookie.find("Secure") != std::string::npos);
    CHECK(cookieTokenOf(cookie).empty());
}

TEST_CASE("a cross-site logout POST is rejected", "[auth][logout][csrf]") {
    FakeGitHub gh;
    crow::request req;
    req.method = crow::HTTPMethod::Post;
    req.add_header("Cookie", "pp_session=" + validSessionToken());
    req.add_header("Origin", "https://evil.example");
    auto r = serviceWith(oauthCfg(), gh).handleLogout(req);
    CHECK(r.code == 403);
    CHECK(setCookieNamed(r, "pp_session").empty());
}

// A stale cookie must always be sheddable. After a secret rotation or an OAuth
// teardown, the one request that clears the stale cookie is the one that would have
// stopped working under a configuration gate.
TEST_CASE("logout still clears the cookie when auth is unconfigured", "[auth][logout]") {
    Config cfg = oauthCfg();
    cfg.sessionSigningSecret.clear();
    FakeGitHub gh;
    auto r = serviceWith(cfg, gh).handleLogout(
        sameOriginPost("pp_session=" + validSessionToken()));
    CHECK(r.code == 200);
    CHECK(setCookieNamed(r, "pp_session").find("Max-Age=0") != std::string::npos);
}

// Clearing does not require a valid session either.
TEST_CASE("logout clears the cookie without a valid session", "[auth][logout]") {
    FakeGitHub gh;
    auto r = serviceWith(oauthCfg(), gh).handleLogout(sameOriginPost("pp_session=garbage"));
    CHECK(r.code == 200);
    CHECK(setCookieNamed(r, "pp_session").find("Max-Age=0") != std::string::npos);
}

// ── AC26: /auth/me ───────────────────────────────────────────────────────────

TEST_CASE("me returns the identity and an id-derived avatar for a valid cookie",
          "[auth][me]") {
    FakeGitHub gh;
    auto r = serviceWith(oauthCfg(), gh).handleAuthMe(
        getWithRawCookie("pp_session=" + validSessionToken()));
    REQUIRE(r.code == 200);
    const auto body = nlohmann::json::parse(r.body);
    CHECK(body["authenticated"] == true);
    CHECK(body["login"] == "alice");
    CHECK(body["avatar_url"] == "https://avatars.githubusercontent.com/u/4242?v=4");
    CHECK(body["is_admin"] == true);
}

TEST_CASE("me reports signed out for every invalid cookie shape", "[auth][me]") {
    std::string tampered = validSessionToken();
    tampered.back() = (tampered.back() == 'a') ? 'b' : 'a';

    for (const std::string cookie : {std::string(""),
                                     std::string("pp_session="),
                                     std::string("pp_session=garbage"),
                                     "pp_session=" + tampered,
                                     "pp_session=" + validSessionToken("alice", kNow - 1),
                                     "pp_session=" + issueStateToken(kSecret, "alice",
                                                                     kNow + 600)}) {
        FakeGitHub gh;
        auto r = serviceWith(oauthCfg(), gh).handleAuthMe(getWithRawCookie(cookie));
        INFO(cookie);
        CHECK(r.code == 200);
        const auto body = nlohmann::json::parse(r.body);
        CHECK(body["authenticated"] == false);
        CHECK_FALSE(body.contains("login"));
        CHECK_FALSE(body.contains("avatar_url"));
        // No `reason`: telling an attacker whether the token expired or was forged
        // tells them which half of it to work on.
        CHECK_FALSE(body.contains("reason"));
    }
}

TEST_CASE("me distinguishes a signed-in non-admin from an admin", "[auth][me]") {
    Config cfg = oauthCfg();
    cfg.adminGithubUsers = {"someone-else"};
    FakeGitHub gh;
    const auto body = nlohmann::json::parse(
        serviceWith(cfg, gh).handleAuthMe(
            getWithRawCookie("pp_session=" + validSessionToken())).body);
    CHECK(body["authenticated"] == true);
    CHECK(body["is_admin"] == false);
}

TEST_CASE("me never echoes the cookie or the signing secret (AC22)",
          "[auth][me][security]") {
    FakeGitHub gh;
    auto r = serviceWith(oauthCfg(), gh).handleAuthMe(
        getWithRawCookie("pp_session=" + validSessionToken()));
    CHECK(r.body.find(validSessionToken()) == std::string::npos);
    CHECK(r.body.find(kSecret) == std::string::npos);
}
