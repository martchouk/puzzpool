#include <puzzpool/auth_service.hpp>

#include <puzzpool/admin_auth.hpp>
#include <puzzpool/base64.hpp>
#include <puzzpool/hash_utils.hpp>
#include <puzzpool/secure_random.hpp>
#include <puzzpool/session.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace puzzpool {

namespace {

using json = nlohmann::json;

constexpr const char* kAuthorizeUrl   = "https://github.com/login/oauth/authorize";
constexpr const char* kAccessTokenUrl = "https://github.com/login/oauth/access_token";
constexpr const char* kUserUrl        = "https://api.github.com/user";

/// The OAuth state is valid only for the length of a sign-in round trip.
constexpr int64_t kStateTtlSeconds = 600;

/// One attribute set per cookie, used for both the issuing and the clearing header.
/// RFC 6265 removes a cookie only when the clearing Set-Cookie matches the
/// original's name, Path and Domain — so hand-writing the two strings separately is
/// exactly how a "logout" comes to look successful while clearing nothing.
struct CookieSpec {
    std::string_view name;
    std::string_view path;
};

constexpr CookieSpec kSessionCookie{"pp_session", "/"};
constexpr CookieSpec kStateCookie{"pp_oauth_state", "/api/v1/auth"};

std::string cookieHeader(const CookieSpec& spec, std::string_view value,
                         long maxAgeSeconds) {
    return std::string(spec.name) + "=" + std::string(value)
         + "; Path=" + std::string(spec.path)
         + "; Max-Age=" + std::to_string(maxAgeSeconds)
         + "; HttpOnly; Secure; SameSite=Lax";
}

std::string clearingCookieHeader(const CookieSpec& spec) {
    return cookieHeader(spec, "", 0);
}

std::string urlEncode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        }
    }
    return out;
}

crow::response jsonResponse(const json& body, int code) {
    crow::response r(code, body.dump());
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    return r;
}

/// Configuration failures are 503 with a fixed, non-secret code (AC13/AC26).
crow::response unavailable() {
    return jsonResponse({{"error", "auth_unavailable"}}, 503);
}

/// Flow failures redirect, because the callback is always a top-level browser
/// navigation and answering it with raw JSON strands the operator on a JSON page.
/// The reason codes are a fixed vocabulary and never carry provider text.
crow::response redirectWithError(std::string_view reason) {
    crow::response r(302);
    r.set_header("Location", "/?auth=error&reason=" + urlEncode(reason));
    r.set_header("Cache-Control", "no-store");
    return r;
}

/// Parses without throwing: a malformed provider body must not become a 500.
json parseOrNull(const std::string& body) {
    return json::parse(body, nullptr, /*allow_exceptions=*/false);
}

AdminRequestView viewOf(const crow::request& req) {
    AdminRequestView v;
    v.isPost        = (req.method == crow::HTTPMethod::Post);
    v.cookieHeader  = req.get_header_value("Cookie");
    v.origin        = req.get_header_value("Origin");
    v.secFetchSite  = req.get_header_value("Sec-Fetch-Site");
    return v;
}

} // namespace

AuthService::AuthService(const Config& cfg, GitHubHttpClient client, Clock clock,
                         NonceSource nonce)
    : cfg_(cfg),
      http_(client.valid() ? std::move(client) : makeLibcurlGitHubClient()),
      clock_(clock ? std::move(clock)
                   : Clock{[] {
                         return std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                             .count();
                     }}),
      nonce_(nonce ? std::move(nonce) : NonceSource{&secureRandomBytes}) {}

// ── GET /api/v1/auth/github/login ────────────────────────────────────────────

crow::response AuthService::handleGithubLogin(const crow::request& req) {
    (void)req;
    if (cfg_.sessionSigningSecret.empty() || cfg_.githubOauthClientId.empty() ||
        cfg_.githubOauthClientSecret.empty() || cfg_.publicBaseUrl.empty()) {
        return unavailable();
    }

    // The bare nonce goes in the URL; the signed blob carrying it stays in the
    // cookie. The signature makes the blob unforgeable, the cookie makes the state
    // browser-bound, and comparing the two in the callback is what closes the
    // login-CSRF hole. Leaking the blob into the redirect URL would hand it to
    // GitHub's logs and to any Referer along the way.
    const std::string nonce = base64UrlEncode(nonce_(32));
    const std::string stateBlob =
        issueStateToken(cfg_.sessionSigningSecret, nonce, clock_() + kStateTtlSeconds);

    const std::string redirectUri = cfg_.publicBaseUrl + "/api/v1/auth/github/callback";
    const std::string location =
        std::string(kAuthorizeUrl) + "?client_id=" + urlEncode(cfg_.githubOauthClientId)
        + "&redirect_uri=" + urlEncode(redirectUri)
        + "&state=" + urlEncode(nonce)
        + "&scope=";   // no scopes: the public profile is all this needs

    crow::response r(302);
    r.set_header("Location", location);
    r.set_header("Cache-Control", "no-store");
    r.add_header("Set-Cookie", cookieHeader(kStateCookie, stateBlob, kStateTtlSeconds));
    return r;
}

// ── GET /api/v1/auth/github/callback ─────────────────────────────────────────

crow::response AuthService::handleGithubCallback(const crow::request& req) {
    if (cfg_.sessionSigningSecret.empty() || cfg_.githubOauthClientId.empty() ||
        cfg_.githubOauthClientSecret.empty() || cfg_.publicBaseUrl.empty()) {
        return unavailable();
    }

    // Clearing the state cookie on every path — success and failure alike — is what
    // makes the state single-use; a failed attempt must not leave a replayable blob
    // behind for the rest of its 600-second window.
    const std::string clearState = clearingCookieHeader(kStateCookie);
    auto fail = [&clearState](std::string_view reason) {
        crow::response r = redirectWithError(reason);
        r.add_header("Set-Cookie", clearState);
        return r;
    };

    const char* codeParam  = req.url_params.get("code");
    const char* stateParam = req.url_params.get("state");
    if (!codeParam || !stateParam) return fail("invalid_state");

    const std::string code  = codeParam;
    const std::string state = stateParam;
    if (code.empty() || state.empty()) return fail("invalid_state");

    const auto stateCookie = cookieValue(req.get_header_value("Cookie"),
                                         kOauthStateCookieName);
    if (!stateCookie) return fail("invalid_state");

    // verifyStateToken, not verifySessionToken: a session blob presented here must
    // not pass, which is what the purpose label inside the MAC buys.
    const auto verified = verifyStateToken(cfg_.sessionSigningSecret, *stateCookie,
                                           clock_());
    if (verified.error != SessionError::None) return fail("invalid_state");
    if (!constantTimeEquals(verified.identity.login, state)) return fail("invalid_state");

    // Exchange the code. The client secret and the code travel in the request body.
    const std::string redirectUri = cfg_.publicBaseUrl + "/api/v1/auth/github/callback";
    const std::string form =
        "client_id=" + urlEncode(cfg_.githubOauthClientId)
        + "&client_secret=" + urlEncode(cfg_.githubOauthClientSecret)
        + "&code=" + urlEncode(code)
        + "&redirect_uri=" + urlEncode(redirectUri);

    const HttpResult tokenResponse = http_.postForm(
        kAccessTokenUrl, form, {"Accept: application/json", "User-Agent: puzzpool"});
    if (tokenResponse.transportError) return fail("provider_unreachable");
    if (tokenResponse.status < 200 || tokenResponse.status >= 300) {
        return fail("exchange_failed");
    }

    const json tokenJson = parseOrNull(tokenResponse.body);
    if (tokenJson.is_discarded() || !tokenJson.is_object()) return fail("exchange_failed");
    // GitHub answers a rejected code with HTTP 200 and an `error` field, so the
    // status alone is not enough to call this a success.
    if (!tokenJson.contains("access_token") || !tokenJson["access_token"].is_string()) {
        return fail("exchange_failed");
    }
    const std::string accessToken = tokenJson["access_token"].get<std::string>();
    if (accessToken.empty()) return fail("exchange_failed");

    const HttpResult userResponse = http_.get(
        kUserUrl, {"Authorization: Bearer " + accessToken,
                   "Accept: application/vnd.github+json",
                   "User-Agent: puzzpool"});
    if (userResponse.transportError) return fail("provider_unreachable");
    if (userResponse.status < 200 || userResponse.status >= 300) {
        return fail("provider_error");
    }

    const json userJson = parseOrNull(userResponse.body);
    if (userJson.is_discarded() || !userJson.is_object()) return fail("provider_error");
    if (!userJson.contains("login") || !userJson["login"].is_string()) {
        return fail("provider_error");
    }
    if (!userJson.contains("id") || !userJson["id"].is_number_integer()) {
        return fail("provider_error");
    }
    const std::string login = userJson["login"].get<std::string>();
    if (login.empty()) return fail("provider_error");
    // The id is a string end to end; this is the one place it is converted.
    const std::string githubId = std::to_string(userJson["id"].get<int64_t>());

    // The session is issued before any allow-list check: authentication and
    // authorization are separate, and slice B needs to tell "signed in" apart from
    // "signed in and allowed to act". The admin guard is what protects admin routes.
    const int64_t expiresAt = clock_() + static_cast<int64_t>(cfg_.sessionTtlMinutes) * 60;
    const std::string sessionToken =
        issueSessionToken(cfg_.sessionSigningSecret, {login, githubId}, expiresAt);

    crow::response r(302);
    r.set_header("Location", "/");
    r.set_header("Cache-Control", "no-store");
    r.add_header("Set-Cookie", clearState);
    r.add_header("Set-Cookie",
                 cookieHeader(kSessionCookie, sessionToken,
                              static_cast<long>(cfg_.sessionTtlMinutes) * 60));
    return r;
}

// ── POST /api/v1/auth/logout ─────────────────────────────────────────────────

crow::response AuthService::handleLogout(const crow::request& req) {
    // Deliberately no configuration gate. Clearing a cookie needs no signing key,
    // and a 503 here would strand a stale cookie in the browser at exactly the
    // moment the secret was rotated away — the one request that sheds it would be
    // the one that stops working.
    if (!hasSameOriginProof(cfg_, viewOf(req))) {
        return jsonResponse({{"error", "csrf_check_failed"}}, 403);
    }

    crow::response r = jsonResponse({{"authenticated", false}}, 200);
    r.add_header("Set-Cookie", clearingCookieHeader(kSessionCookie));
    return r;
}

// ── GET /api/v1/auth/me ──────────────────────────────────────────────────────

crow::response AuthService::handleAuthMe(const crow::request& req) {
    if (cfg_.sessionSigningSecret.empty()) return unavailable();

    // Every failure mode answers with the same signed-out body and no `reason`:
    // distinguishing "expired" from "forged" tells an attacker which half of the
    // token to work on.
    const json signedOut = {{"authenticated", false}};

    const auto raw = cookieValue(req.get_header_value("Cookie"), kSessionCookieName);
    if (!raw) return jsonResponse(signedOut, 200);

    const auto session = verifySessionToken(cfg_.sessionSigningSecret, *raw, clock_());
    if (session.error != SessionError::None) return jsonResponse(signedOut, 200);

    return jsonResponse({{"authenticated", true},
                         {"login", session.identity.login},
                         {"avatar_url", "https://avatars.githubusercontent.com/u/"
                                        + session.identity.githubId + "?v=4"},
                         {"is_admin", isAllowedAdminLogin(cfg_, session.identity.login)}},
                        200);
}

} // namespace puzzpool
