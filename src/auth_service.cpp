#include <puzzpool/auth_service.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <utility>

namespace puzzpool {

namespace {

using json = nlohmann::json;

constexpr const char* kGithubAuthorizeUrl   = "https://github.com/login/oauth/authorize";
constexpr const char* kGithubAccessTokenUrl = "https://github.com/login/oauth/access_token";
constexpr const char* kGithubUserUrl        = "https://api.github.com/user";
constexpr const char* kUserAgent            = "puzzpool";

crow::response jsonResponse(int code, const json& body) {
    crow::response response;
    response.code = code;
    response.set_header("Content-Type", "application/json");
    response.set_header("Cache-Control", "no-store");
    response.body = body.dump();
    return response;
}

// Query-parameter lookup that tolerates the absent case without dereferencing a
// null char*.
std::string queryParam(const crow::request& req, const std::string& name) {
    const char* value = req.url_params.get(name);
    return value ? std::string(value) : std::string();
}

} // namespace

// ── Crow adapters for the central guard ───────────────────────────────────────

AdminRequestView adminRequestView(const crow::request& req) {
    AdminRequestView view;
    view.isPost           = req.method == crow::HTTPMethod::POST;
    view.adminTokenHeader = req.get_header_value("X-Admin-Token");
    view.cookieHeader     = req.get_header_value("Cookie");
    view.secFetchSite     = req.get_header_value("Sec-Fetch-Site");
    view.origin           = req.get_header_value("Origin");
    view.host             = req.get_header_value("Host");
    return view;
}

std::optional<crow::response> adminGuard(const Config& cfg, const crow::request& req) {
    const auto nowUnix = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

    const AdminAuthResult decision = authorizeAdminRequest(cfg, adminRequestView(req), nowUnix);
    if (decision.authorized) return std::nullopt;

    // The body never names which credential failed or echoes any submitted value.
    const char* error = decision.reason == AdminDenyReason::CsrfCheckFailed ? "csrf_check_failed"
                                                                           : "unauthorized";
    return jsonResponse(decision.statusCode, json{{"error", error}});
}

// ── AuthService ───────────────────────────────────────────────────────────────

AuthService::AuthService(const Config& cfg, HttpClient http, Clock clock)
  : cfg_(cfg),
    http_(http ? std::move(http) : makeCurlHttpClient()),
    clock_(std::move(clock)) {}

std::int64_t AuthService::now() const {
    if (clock_) return clock_();
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

crow::response AuthService::serviceUnavailable(const std::string& reason) const {
    // `reason` is a fixed machine-readable label naming the missing *variable*,
    // never its value.
    return jsonResponse(503, json{{"error", "auth_unavailable"}, {"reason", reason}});
}

crow::response AuthService::handleGithubLogin(const crow::request& req) {
    (void)req;
    if (!sessionSigningEnabled(cfg_)) return serviceUnavailable("session_signing_secret_missing");
    if (cfg_.githubOauthClientId.empty() || cfg_.githubOauthClientSecret.empty()) {
        return serviceUnavailable("github_oauth_not_configured");
    }

    // The state is unguessable (256 bits from the system CSPRNG), browser-bound
    // (only the browser holding the signed cookie can present it), and
    // single-use (the callback clears the cookie whatever the outcome).
    const std::string nonce = randomHexToken(32);
    const json statePayload{{"n", nonce}, {"exp", now() + kOauthStateTtlSeconds}};

    std::string location = std::string(kGithubAuthorizeUrl) +
                           "?client_id=" + urlEncode(cfg_.githubOauthClientId) +
                           "&state=" + urlEncode(nonce);
    if (!cfg_.githubOauthCallbackUrl.empty()) {
        location += "&redirect_uri=" + urlEncode(cfg_.githubOauthCallbackUrl);
    }
    // No `scope` parameter: the flow only needs the public identity of the user.

    crow::response response;
    response.code = 302;
    response.set_header("Location", location);
    response.set_header("Cache-Control", "no-store");
    response.add_header("Set-Cookie",
                        setCookieHeader(kOauthStateCookieName,
                                        signValue(cfg_, kOauthStatePurpose, statePayload.dump()),
                                        "Lax",
                                        kOauthStateTtlSeconds));
    return response;
}

crow::response AuthService::handleGithubCallback(const crow::request& req) {
    if (!sessionSigningEnabled(cfg_)) return serviceUnavailable("session_signing_secret_missing");
    if (cfg_.githubOauthClientId.empty() || cfg_.githubOauthClientSecret.empty()) {
        return serviceUnavailable("github_oauth_not_configured");
    }

    // Consume the state cookie on every path, so a nonce can never be replayed.
    const std::string clearState = clearCookieHeader(kOauthStateCookieName);
    const auto        reject     = [&clearState](int code, const char* error) {
        crow::response response = jsonResponse(code, json{{"error", error}});
        response.add_header("Set-Cookie", clearState);
        return response;
    };

    const std::string code  = queryParam(req, "code");
    const std::string state = queryParam(req, "state");
    if (code.empty() || state.empty()) return reject(400, "invalid_request");

    const std::string signedState =
        cookieValue(req.get_header_value("Cookie"), kOauthStateCookieName);
    const auto statePayload =
        verifySignedValue(cfg_, kOauthStatePurpose, signedState, now(), nullptr);
    if (!statePayload) return reject(400, "invalid_state");

    json parsedState = json::parse(*statePayload, nullptr, false);
    if (parsedState.is_discarded() || !parsedState.is_object() || !parsedState.contains("n") ||
        !parsedState["n"].is_string()) {
        return reject(400, "invalid_state");
    }
    if (!constantTimeEquals(parsedState["n"].get<std::string>(), state)) {
        return reject(400, "invalid_state");
    }

    // Exchange the code. The client secret travels in the request body over TLS
    // — never in the URL, a shell command, or process arguments.
    HttpRequest exchange;
    exchange.method  = "POST";
    exchange.url     = kGithubAccessTokenUrl;
    exchange.headers = {
        {"Accept", "application/json"},
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"User-Agent", kUserAgent},
    };
    exchange.body = "client_id=" + urlEncode(cfg_.githubOauthClientId) +
                    "&client_secret=" + urlEncode(cfg_.githubOauthClientSecret) +
                    "&code=" + urlEncode(code);
    if (!cfg_.githubOauthCallbackUrl.empty()) {
        exchange.body += "&redirect_uri=" + urlEncode(cfg_.githubOauthCallbackUrl);
    }

    const HttpResponse exchangeResponse = http_(exchange);
    if (!exchangeResponse.error.empty() || exchangeResponse.status != 200) {
        return reject(502, "github_exchange_failed");
    }

    json exchangeBody = json::parse(exchangeResponse.body, nullptr, false);
    if (exchangeBody.is_discarded() || !exchangeBody.is_object() ||
        exchangeBody.contains("error") || !exchangeBody.contains("access_token") ||
        !exchangeBody["access_token"].is_string() ||
        exchangeBody["access_token"].get<std::string>().empty()) {
        return reject(502, "github_exchange_failed");
    }

    HttpRequest identity;
    identity.method  = "GET";
    identity.url     = kGithubUserUrl;
    identity.headers = {
        {"Authorization", "Bearer " + exchangeBody["access_token"].get<std::string>()},
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", kUserAgent},
    };

    const HttpResponse identityResponse = http_(identity);
    if (!identityResponse.error.empty() || identityResponse.status != 200) {
        return reject(502, "github_identity_failed");
    }

    json identityBody = json::parse(identityResponse.body, nullptr, false);
    if (identityBody.is_discarded() || !identityBody.is_object() ||
        !identityBody.contains("login") || !identityBody["login"].is_string() ||
        identityBody["login"].get<std::string>().empty() || !identityBody.contains("id") ||
        !identityBody["id"].is_number_integer()) {
        return reject(502, "github_identity_failed");
    }

    SessionIdentity session;
    session.login         = identityBody["login"].get<std::string>();
    session.githubId      = identityBody["id"].get<std::int64_t>();
    session.expiresAtUnix = now() + static_cast<std::int64_t>(cfg_.sessionTtlMinutes) * 60;

    // Signing in is not the same as being an admin: authorization is decided per
    // request from the allow-list (AC7), so the cookie is issued to any GitHub
    // user and grants nothing on its own.
    crow::response response;
    response.code = 302;
    response.set_header("Location", "/");
    response.set_header("Cache-Control", "no-store");
    response.add_header("Set-Cookie", clearState);
    response.add_header("Set-Cookie",
                        setCookieHeader(kSessionCookieName,
                                        encodeSessionToken(cfg_, session),
                                        "Lax",
                                        static_cast<long>(cfg_.sessionTtlMinutes) * 60));
    return response;
}

crow::response AuthService::handleLogout(const crow::request& req) {
    (void)req;
    if (!sessionSigningEnabled(cfg_)) return serviceUnavailable("session_signing_secret_missing");

    crow::response response = jsonResponse(200, json{{"ok", true}});
    response.add_header("Set-Cookie", clearCookieHeader(kSessionCookieName));
    return response;
}

crow::response AuthService::handleMe(const crow::request& req) {
    if (!sessionSigningEnabled(cfg_)) return serviceUnavailable("session_signing_secret_missing");

    const std::string token = cookieValue(req.get_header_value("Cookie"), kSessionCookieName);
    const auto identity     = decodeSessionToken(cfg_, token, now(), nullptr);

    // Missing, malformed, badly-signed and expired cookies all produce the same
    // signed-out body; nothing about the presented cookie is echoed back.
    if (!identity) {
        return jsonResponse(200, json{{"authenticated", false}, {"is_admin", false}});
    }

    return jsonResponse(200,
                        json{
                            {"authenticated", true},
                            {"login", identity->login},
                            {"avatar_url", githubAvatarUrl(identity->githubId)},
                            {"is_admin", isAllowedAdminLogin(cfg_.adminGithubUsers, identity->login)},
                        });
}

} // namespace puzzpool
