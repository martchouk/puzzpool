#include <puzzpool/auth.hpp>
#include <puzzpool/auth_service.hpp>
#include <puzzpool/config.hpp>

#include <catch2/catch_test_macros.hpp>
#include <crow.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace puzzpool;
using json = nlohmann::json;

namespace {

constexpr std::int64_t kNow = 1'800'000'000;

// Deliberately distinctive so a leak into a URL, header, or body is unmistakable.
constexpr const char* kClientSecret = "CLIENT-SECRET-MUST-NEVER-LEAK";
constexpr const char* kAccessToken  = "ACCESS-TOKEN-MUST-NEVER-LEAK";

Config authConfig() {
    Config cfg;
    cfg.sessionSigningSecret    = "route-test-signing-secret";
    cfg.githubOauthClientId     = "client-id-123";
    cfg.githubOauthClientSecret = kClientSecret;
    cfg.adminGithubUsers        = parseAdminGithubUsers("operator-one");
    cfg.sessionTtlMinutes       = 60;
    return cfg;
}

// Records every outbound request and replays scripted responses in order.
struct RecordingClient {
    std::vector<HttpRequest>  requests;
    std::vector<HttpResponse> responses;

    HttpClient handler() {
        return [this](const HttpRequest& request) -> HttpResponse {
            requests.push_back(request);
            if (requests.size() <= responses.size()) return responses[requests.size() - 1];
            HttpResponse fallback;
            fallback.error = "unexpected request";
            return fallback;
        };
    }
};

HttpResponse ok(const std::string& body) {
    HttpResponse response;
    response.status = 200;
    response.body   = body;
    return response;
}

HttpResponse tokenSuccess() {
    return ok(json{{"access_token", kAccessToken}, {"token_type", "bearer"}}.dump());
}

HttpResponse identitySuccess(const std::string& login = "operator-one") {
    return ok(json{{"login", login}, {"id", 583231}}.dump());
}

AuthService::Clock fixedClock(std::int64_t at = kNow) {
    return [at] { return at; };
}

crow::request getRequest(const std::string& query = "", const std::string& cookie = "") {
    crow::request req;
    req.method = crow::HTTPMethod::GET;
    if (!query.empty()) req.url_params = crow::query_string("?" + query);
    if (!cookie.empty()) req.add_header("Cookie", cookie);
    return req;
}

std::vector<std::string> setCookieHeaders(const crow::response& response) {
    std::vector<std::string> out;
    const auto range = response.headers.equal_range("Set-Cookie");
    for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
    return out;
}

std::string headerValue(const crow::response& response, const std::string& name) {
    const auto it = response.headers.find(name);
    return it == response.headers.end() ? std::string() : it->second;
}

bool anyContains(const std::vector<std::string>& values, const std::string& needle) {
    for (const auto& value : values) {
        if (value.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Completes a full login → callback exchange and returns the issued session cookie.
std::string issuedSessionCookie(const Config& cfg, const std::string& login = "operator-one") {
    RecordingClient client;
    client.responses = {tokenSuccess(), identitySuccess(login)};
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response redirect = service.handleGithubLogin(getRequest());
    const std::string stateCookie = setCookieHeaders(redirect).at(0);
    const std::string stateValue =
        stateCookie.substr(0, stateCookie.find(';'));
    const std::string location = headerValue(redirect, "Location");
    const std::string nonce    = location.substr(location.find("&state=") + 7);

    const crow::response callback =
        service.handleGithubCallback(getRequest("code=abc&state=" + nonce, stateValue));
    REQUIRE(callback.code == 302);

    for (const auto& header : setCookieHeaders(callback)) {
        if (header.rfind(std::string(kSessionCookieName) + "=", 0) == 0 &&
            header.find("Max-Age=0") == std::string::npos) {
            return header.substr(0, header.find(';'));
        }
    }
    FAIL("callback did not issue a session cookie");
    return "";
}

} // namespace

// ── fail closed without a signing secret (AC13, AC26) ────────────────────────

TEST_CASE("every auth route returns 503 without SESSION_SIGNING_SECRET", "[auth][routes]") {
    Config cfg = authConfig();
    cfg.sessionSigningSecret.clear();

    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    for (const crow::response& response : {service.handleGithubLogin(getRequest()),
                                           service.handleGithubCallback(getRequest("code=a&state=b")),
                                           service.handleLogout(getRequest()),
                                           service.handleMe(getRequest())}) {
        CHECK(response.code == 503);
        const auto body = json::parse(response.body);
        CHECK(body["error"] == "auth_unavailable");
        CHECK(body["reason"] == "session_signing_secret_missing");
        // No cookie is issued or cleared while the path is disabled.
        CHECK(setCookieHeaders(response).empty());
    }

    // Nothing reached the provider.
    CHECK(client.requests.empty());
}

TEST_CASE("login and callback return 503 without OAuth credentials", "[auth][routes]") {
    Config cfg = authConfig();
    cfg.githubOauthClientSecret.clear();

    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response login = service.handleGithubLogin(getRequest());
    CHECK(login.code == 503);
    CHECK(json::parse(login.body)["reason"] == "github_oauth_not_configured");

    // /auth/me still answers, because it needs no provider credentials.
    CHECK(service.handleMe(getRequest()).code == 200);
    CHECK(client.requests.empty());
}

// ── login (AC1) ──────────────────────────────────────────────────────────────

TEST_CASE("login redirects to GitHub with an unguessable browser-bound state", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response response = service.handleGithubLogin(getRequest());
    REQUIRE(response.code == 302);

    const std::string location = headerValue(response, "Location");
    CHECK(location.rfind("https://github.com/login/oauth/authorize?", 0) == 0);
    CHECK(location.find("client_id=client-id-123") != std::string::npos);
    CHECK(location.find("&state=") != std::string::npos);
    // No-scope flow: the app asks for no permissions at all.
    CHECK(location.find("scope=") == std::string::npos);
    // The client secret never appears in a redirect URL.
    CHECK(location.find(kClientSecret) == std::string::npos);

    const std::string nonce = location.substr(location.find("&state=") + 7);
    CHECK(nonce.size() == 64); // 256 bits of CSPRNG output, hex-encoded

    const auto cookies = setCookieHeaders(response);
    REQUIRE(cookies.size() == 1);
    CHECK(cookies[0].rfind(std::string(kOauthStateCookieName) + "=", 0) == 0);
    CHECK(cookies[0].find("HttpOnly") != std::string::npos);
    CHECK(cookies[0].find("Secure") != std::string::npos);
    CHECK(cookies[0].find("SameSite=Lax") != std::string::npos);
    // The raw nonce is bound to the browser through the signed cookie, so the
    // state in the URL alone is not enough to complete the flow.
    CHECK(cookies[0].find(nonce) == std::string::npos);
}

TEST_CASE("two logins produce different state values", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    const std::string first  = headerValue(service.handleGithubLogin(getRequest()), "Location");
    const std::string second = headerValue(service.handleGithubLogin(getRequest()), "Location");
    CHECK(first != second);
}

// ── callback: state validation (AC1) ─────────────────────────────────────────

TEST_CASE("callback rejects a missing code or state", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    for (const std::string& query : {"", "code=abc", "state=abc"}) {
        const crow::response response = service.handleGithubCallback(getRequest(query));
        CHECK(response.code == 400);
        CHECK(json::parse(response.body)["error"] == "invalid_request");
    }
    CHECK(client.requests.empty());
}

TEST_CASE("callback rejects a state that does not match the browser cookie", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response login = service.handleGithubLogin(getRequest());
    const std::string stateCookie =
        setCookieHeaders(login).at(0).substr(0, setCookieHeaders(login).at(0).find(';'));
    const std::string location = headerValue(login, "Location");
    const std::string nonce    = location.substr(location.find("&state=") + 7);

    // Attacker-supplied state with the victim's cookie.
    const crow::response mismatched =
        service.handleGithubCallback(getRequest("code=abc&state=" + nonce + "0", stateCookie));
    CHECK(mismatched.code == 400);
    CHECK(json::parse(mismatched.body)["error"] == "invalid_state");

    // Correct state with no cookie: not browser-bound, so it is refused.
    const crow::response unbound =
        service.handleGithubCallback(getRequest("code=abc&state=" + nonce));
    CHECK(unbound.code == 400);
    CHECK(json::parse(unbound.body)["error"] == "invalid_state");

    CHECK(client.requests.empty());

    // Sensitivity: the matching pair does proceed to the provider.
    client.responses = {tokenSuccess(), identitySuccess()};
    const crow::response accepted =
        service.handleGithubCallback(getRequest("code=abc&state=" + nonce, stateCookie));
    CHECK(accepted.code == 302);
    CHECK(client.requests.size() == 2);
}

TEST_CASE("callback rejects a tampered state cookie", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response login = service.handleGithubLogin(getRequest());
    std::string stateCookie =
        setCookieHeaders(login).at(0).substr(0, setCookieHeaders(login).at(0).find(';'));
    const std::string location = headerValue(login, "Location");
    const std::string nonce    = location.substr(location.find("&state=") + 7);

    stateCookie.back() = (stateCookie.back() == 'A') ? 'B' : 'A';
    const crow::response response =
        service.handleGithubCallback(getRequest("code=abc&state=" + nonce, stateCookie));
    CHECK(response.code == 400);
    CHECK(json::parse(response.body)["error"] == "invalid_state");
    CHECK(client.requests.empty());
}

TEST_CASE("callback rejects an expired state", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    client.responses = {tokenSuccess(), identitySuccess()};

    AuthService issuer(cfg, client.handler(), fixedClock(kNow));
    const crow::response login = issuer.handleGithubLogin(getRequest());
    const std::string stateCookie =
        setCookieHeaders(login).at(0).substr(0, setCookieHeaders(login).at(0).find(';'));
    const std::string location = headerValue(login, "Location");
    const std::string nonce    = location.substr(location.find("&state=") + 7);

    AuthService late(cfg, client.handler(), fixedClock(kNow + kOauthStateTtlSeconds + 1));
    const crow::response response =
        late.handleGithubCallback(getRequest("code=abc&state=" + nonce, stateCookie));
    CHECK(response.code == 400);
    CHECK(json::parse(response.body)["error"] == "invalid_state");
    CHECK(client.requests.empty());
}

TEST_CASE("callback clears the state cookie on success and on failure", "[auth][routes]") {
    const Config cfg = authConfig();

    RecordingClient failing;
    AuthService failService(cfg, failing.handler(), fixedClock());
    const crow::response rejected = failService.handleGithubCallback(getRequest("code=a&state=b"));
    CHECK(anyContains(setCookieHeaders(rejected),
                      std::string(kOauthStateCookieName) + "=; Path=/"));
    CHECK(anyContains(setCookieHeaders(rejected), "Max-Age=0"));

    RecordingClient client;
    client.responses = {tokenSuccess(), identitySuccess()};
    AuthService service(cfg, client.handler(), fixedClock());
    const crow::response login = service.handleGithubLogin(getRequest());
    const std::string stateCookie =
        setCookieHeaders(login).at(0).substr(0, setCookieHeaders(login).at(0).find(';'));
    const std::string location = headerValue(login, "Location");
    const std::string nonce    = location.substr(location.find("&state=") + 7);

    const crow::response success =
        service.handleGithubCallback(getRequest("code=abc&state=" + nonce, stateCookie));
    REQUIRE(success.code == 302);
    // The single-use state is destroyed even though the flow succeeded, so the
    // browser cannot replay the same nonce.
    CHECK(anyContains(setCookieHeaders(success),
                      std::string(kOauthStateCookieName) + "=; Path=/"));

    // Replaying the same code and state without the cookie now fails.
    RecordingClient replayClient;
    replayClient.responses = {tokenSuccess(), identitySuccess()};
    AuthService replayService(cfg, replayClient.handler(), fixedClock());
    const crow::response replay =
        replayService.handleGithubCallback(getRequest("code=abc&state=" + nonce));
    CHECK(replay.code == 400);
    CHECK(replayClient.requests.empty());
}

// ── callback: provider exchange (AC2, AC22) ──────────────────────────────────

TEST_CASE("the code exchange sends the secret in the body, never the URL", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    client.responses = {tokenSuccess(), identitySuccess()};
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response login = service.handleGithubLogin(getRequest());
    const std::string stateCookie =
        setCookieHeaders(login).at(0).substr(0, setCookieHeaders(login).at(0).find(';'));
    const std::string location = headerValue(login, "Location");
    const std::string nonce    = location.substr(location.find("&state=") + 7);

    const crow::response callback =
        service.handleGithubCallback(getRequest("code=the-code&state=" + nonce, stateCookie));
    REQUIRE(callback.code == 302);
    REQUIRE(client.requests.size() == 2);

    const HttpRequest& exchange = client.requests[0];
    CHECK(exchange.method == "POST");
    CHECK(exchange.url == "https://github.com/login/oauth/access_token");
    CHECK(exchange.url.rfind("https://", 0) == 0);
    CHECK(exchange.url.find(kClientSecret) == std::string::npos);
    CHECK(exchange.url.find("the-code") == std::string::npos);
    CHECK(exchange.body.find(std::string("client_secret=") + kClientSecret) != std::string::npos);
    CHECK(exchange.body.find("code=the-code") != std::string::npos);
    CHECK(exchange.maxResponseBytes > 0);

    const HttpRequest& identity = client.requests[1];
    CHECK(identity.method == "GET");
    CHECK(identity.url == "https://api.github.com/user");
    CHECK(identity.url.find(kAccessToken) == std::string::npos);
    bool bearerSent = false;
    for (const auto& [name, value] : identity.headers) {
        if (name == "Authorization") bearerSent = value == std::string("Bearer ") + kAccessToken;
    }
    CHECK(bearerSent);
}

TEST_CASE("provider failures produce 502 and never leak secrets", "[auth][routes]") {
    const Config cfg = authConfig();

    struct Scenario {
        std::vector<HttpResponse> responses;
        const char*               expectedError;
    };

    HttpResponse transportFailure;
    transportFailure.error = "Could not resolve host";

    HttpResponse serverError;
    serverError.status = 500;
    serverError.body   = "upstream exploded";

    const std::vector<Scenario> scenarios = {
        {{transportFailure}, "github_exchange_failed"},
        {{serverError}, "github_exchange_failed"},
        {{ok(R"({"error":"bad_verification_code"})")}, "github_exchange_failed"},
        {{ok(R"({"token_type":"bearer"})")}, "github_exchange_failed"},
        {{ok(R"({"access_token":""})")}, "github_exchange_failed"},
        {{ok("not json at all")}, "github_exchange_failed"},
        {{tokenSuccess(), transportFailure}, "github_identity_failed"},
        {{tokenSuccess(), serverError}, "github_identity_failed"},
        {{tokenSuccess(), ok(R"({"id":1})")}, "github_identity_failed"},
        {{tokenSuccess(), ok(R"({"login":"","id":1})")}, "github_identity_failed"},
        {{tokenSuccess(), ok(R"({"login":"operator-one"})")}, "github_identity_failed"},
        {{tokenSuccess(), ok(R"({"login":"operator-one","id":"not-a-number"})")},
         "github_identity_failed"},
    };

    for (const Scenario& scenario : scenarios) {
        RecordingClient client;
        client.responses = scenario.responses;
        AuthService service(cfg, client.handler(), fixedClock());

        const crow::response login = service.handleGithubLogin(getRequest());
        const std::string stateCookie =
            setCookieHeaders(login).at(0).substr(0, setCookieHeaders(login).at(0).find(';'));
        const std::string location = headerValue(login, "Location");
        const std::string nonce    = location.substr(location.find("&state=") + 7);

        const crow::response response =
            service.handleGithubCallback(getRequest("code=abc&state=" + nonce, stateCookie));

        CHECK(response.code == 502);
        CHECK(json::parse(response.body)["error"] == scenario.expectedError);
        CHECK(response.body.find(kClientSecret) == std::string::npos);
        CHECK(response.body.find(kAccessToken) == std::string::npos);
        CHECK(response.body.find("upstream exploded") == std::string::npos);
        // No session cookie is issued on a failed sign-in.
        CHECK_FALSE(anyContains(setCookieHeaders(response),
                                std::string(kSessionCookieName) + "=v1."));
    }
}

// ── callback: session issuance (AC3) ─────────────────────────────────────────

TEST_CASE("a successful callback issues a hardened session cookie", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    client.responses = {tokenSuccess(), identitySuccess("Operator-One")};
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response login = service.handleGithubLogin(getRequest());
    const std::string stateCookie =
        setCookieHeaders(login).at(0).substr(0, setCookieHeaders(login).at(0).find(';'));
    const std::string location = headerValue(login, "Location");
    const std::string nonce    = location.substr(location.find("&state=") + 7);

    const crow::response callback =
        service.handleGithubCallback(getRequest("code=abc&state=" + nonce, stateCookie));
    REQUIRE(callback.code == 302);
    CHECK(headerValue(callback, "Location") == "/");

    std::string sessionHeader;
    for (const auto& header : setCookieHeaders(callback)) {
        if (header.rfind(std::string(kSessionCookieName) + "=v1.", 0) == 0) sessionHeader = header;
    }
    REQUIRE_FALSE(sessionHeader.empty());
    CHECK(sessionHeader.find("HttpOnly") != std::string::npos);
    CHECK(sessionHeader.find("Secure") != std::string::npos);
    CHECK(sessionHeader.find("SameSite=Lax") != std::string::npos);
    CHECK(sessionHeader.find("Max-Age=3600") != std::string::npos);
    CHECK(sessionHeader.find(kAccessToken) == std::string::npos);
    CHECK(sessionHeader.find(kClientSecret) == std::string::npos);

    // The cookie carries exactly the identity the provider reported.
    const std::string token =
        sessionHeader.substr(sessionHeader.find('=') + 1, sessionHeader.find(';') - sessionHeader.find('=') - 1);
    const auto identity = decodeSessionToken(cfg, token, kNow, nullptr);
    REQUIRE(identity.has_value());
    CHECK(identity->login == "Operator-One");
    CHECK(identity->githubId == 583231);
    CHECK(identity->expiresAtUnix == kNow + 3600);
}

TEST_CASE("signing in does not by itself grant admin access", "[auth][routes]") {
    const Config cfg = authConfig();
    // "outsider" is not on the allow-list but still completes the OAuth flow.
    const std::string cookie = issuedSessionCookie(cfg, "outsider");

    AdminRequestView view;
    view.cookieHeader = cookie;
    const auto decision = authorizeAdminRequest(cfg, view, kNow);
    CHECK_FALSE(decision.authorized);
    CHECK(decision.reason == AdminDenyReason::NotAllowListed);
}

// ── logout (AC5) ─────────────────────────────────────────────────────────────

TEST_CASE("logout clears the session cookie", "[auth][routes]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response response = service.handleLogout(getRequest());
    CHECK(response.code == 200);
    CHECK(json::parse(response.body)["ok"] == true);

    const auto cookies = setCookieHeaders(response);
    REQUIRE(cookies.size() == 1);
    CHECK(cookies[0].rfind(std::string(kSessionCookieName) + "=;", 0) == 0);
    CHECK(cookies[0].find("Max-Age=0") != std::string::npos);
    CHECK(cookies[0].find("HttpOnly") != std::string::npos);
}

// ── /api/v1/auth/me (AC26) ───────────────────────────────────────────────────

TEST_CASE("me reports the signed-in identity and admin status", "[auth][routes][me]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    const std::string cookie = issuedSessionCookie(cfg, "Operator-One");
    const crow::response response = service.handleMe(getRequest("", cookie));

    CHECK(response.code == 200);
    const auto body = json::parse(response.body);
    CHECK(body["authenticated"] == true);
    CHECK(body["login"] == "Operator-One");
    CHECK(body["avatar_url"] == "https://avatars.githubusercontent.com/u/583231?v=4");
    CHECK(body["is_admin"] == true);
    // The response never echoes the cookie or any secret.
    CHECK(response.body.find("v1.") == std::string::npos);
    CHECK(response.body.find(cfg.sessionSigningSecret) == std::string::npos);
    CHECK(headerValue(response, "Cache-Control") == "no-store");
}

TEST_CASE("me reports a signed-in non-admin as not admin", "[auth][routes][me]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    const crow::response response =
        service.handleMe(getRequest("", issuedSessionCookie(cfg, "outsider")));
    const auto body = json::parse(response.body);
    CHECK(body["authenticated"] == true);
    CHECK(body["login"] == "outsider");
    CHECK(body["is_admin"] == false);
}

TEST_CASE("me returns an identical signed-out body for every bad cookie", "[auth][routes][me]") {
    const Config cfg = authConfig();
    RecordingClient client;
    AuthService service(cfg, client.handler(), fixedClock());

    std::string tampered = issuedSessionCookie(cfg, "operator-one");
    tampered.back()      = (tampered.back() == 'A') ? 'B' : 'A';

    SessionIdentity stale{"operator-one", 1, kNow - 1};
    const std::string expired = std::string(kSessionCookieName) + "=" + encodeSessionToken(cfg, stale);

    const std::vector<std::string> cookies = {
        "",                                                   // missing
        std::string(kSessionCookieName) + "=garbage",         // malformed
        std::string(kSessionCookieName) + "=v1.YWJj.YWJj",    // bad signature
        tampered,                                             // tampered
        expired,                                              // expired
    };

    const json expected{{"authenticated", false}, {"is_admin", false}};
    for (const std::string& cookie : cookies) {
        const crow::response response = service.handleMe(getRequest("", cookie));
        CHECK(response.code == 200);
        CHECK(json::parse(response.body) == expected);
    }

    // Sensitivity: a valid cookie under the same service does authenticate, so
    // the signed-out results above reflect the cookie and not a broken handler.
    const crow::response valid =
        service.handleMe(getRequest("", issuedSessionCookie(cfg, "operator-one")));
    CHECK(json::parse(valid.body)["authenticated"] == true);
}

// ── the Crow guard adapter ───────────────────────────────────────────────────

TEST_CASE("adminGuard maps the decision onto a Crow response", "[auth][guard][routes]") {
    Config cfg;
    cfg.adminToken = "the-token";

    crow::request denied;
    denied.method = crow::HTTPMethod::POST;
    const auto deniedResponse = adminGuard(cfg, denied);
    REQUIRE(deniedResponse.has_value());
    CHECK(deniedResponse->code == 401);
    CHECK(json::parse(deniedResponse->body)["error"] == "unauthorized");
    // The rejection never reveals which mechanism was configured.
    CHECK(deniedResponse->body.find("the-token") == std::string::npos);

    crow::request allowed;
    allowed.method = crow::HTTPMethod::POST;
    allowed.add_header("X-Admin-Token", "the-token");
    CHECK_FALSE(adminGuard(cfg, allowed).has_value());
}

TEST_CASE("adminGuard returns 403 for a cookie POST without CSRF proof", "[auth][guard][routes]") {
    const Config cfg = authConfig();
    const std::string cookie = issuedSessionCookie(cfg, "operator-one");

    crow::request req;
    req.method = crow::HTTPMethod::POST;
    req.add_header("Cookie", cookie);

    const auto response = adminGuard(cfg, req);
    REQUIRE(response.has_value());
    CHECK(response->code == 403);
    CHECK(json::parse(response->body)["error"] == "csrf_check_failed");

    // Sensitivity: adding the same-origin proof authorizes the identical request.
    crow::request proven;
    proven.method = crow::HTTPMethod::POST;
    proven.add_header("Cookie", cookie);
    proven.add_header("Sec-Fetch-Site", "same-origin");
    CHECK_FALSE(adminGuard(cfg, proven).has_value());
}

TEST_CASE("adminRequestView copies exactly the headers the guard inspects", "[auth][guard][routes]") {
    crow::request req;
    req.method = crow::HTTPMethod::POST;
    req.add_header("X-Admin-Token", "t");
    req.add_header("Cookie", "pp_session=c");
    req.add_header("Sec-Fetch-Site", "same-origin");
    req.add_header("Origin", "https://puzzle.example");
    req.add_header("Host", "puzzle.example");

    const AdminRequestView view = adminRequestView(req);
    CHECK(view.isPost);
    CHECK(view.adminTokenHeader == "t");
    CHECK(view.cookieHeader == "pp_session=c");
    CHECK(view.secFetchSite == "same-origin");
    CHECK(view.origin == "https://puzzle.example");
    CHECK(view.host == "puzzle.example");

    crow::request getReq;
    getReq.method = crow::HTTPMethod::GET;
    CHECK_FALSE(adminRequestView(getReq).isPost);
}
