#include <puzzpool/admin_auth.hpp>
#include <puzzpool/env.hpp>
#include <puzzpool/hash_utils.hpp>
#include <puzzpool/session.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace puzzpool {

namespace {

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return out;
}

/// Scheme + host + port of an absolute URL, i.e. everything before the path. Used to
/// compare an Origin header against PUBLIC_BASE_URL. An Origin header is already
/// exactly this shape; PUBLIC_BASE_URL may carry a path, which is stripped here.
std::string originOf(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) return std::string(url);
    const auto pathStart = url.find('/', schemeEnd + 3);
    return std::string(pathStart == std::string_view::npos ? url : url.substr(0, pathStart));
}

} // namespace

std::vector<std::string> parseAdminGithubUsers(const std::string& raw) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= raw.size()) {
        const auto comma = raw.find(',', pos);
        const auto end   = (comma == std::string::npos) ? raw.size() : comma;
        std::string login = toLower(trim(raw.substr(pos, end - pos)));
        if (!login.empty()) out.push_back(std::move(login));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

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

bool hasSameOriginProof(const Config& cfg, const AdminRequestView& req) {
    if (!req.secFetchSite.empty()) {
        // An explicit signal is definitive in both directions: a request that has
        // already told us it is cross-site does not get to overrule itself with an
        // Origin header it also controls.
        return req.secFetchSite == "same-origin" || req.secFetchSite == "none";
    }
    if (!req.origin.empty() && !cfg.publicBaseUrl.empty()) {
        return originOf(req.origin) == originOf(cfg.publicBaseUrl);
    }
    return false;   // absence is not proof
}

AdminAuthDecision authorizeAdmin(const Config& cfg, const AdminRequestView& req,
                                 int64_t nowUnix) {
    AdminAuthDecision d;

    // A configured X-Admin-Token authorizes on its own (AC12). It is not cookie
    // authorization, so the CSRF gate below deliberately does not apply to it: a
    // header no browser attaches automatically cannot be driven cross-site.
    if (!cfg.adminToken.empty() &&
        constantTimeEquals(req.adminTokenHeader, cfg.adminToken)) {
        d.allowed = true;
        d.mechanism = AdminAuthMechanism::Token;
        return d;
    }

    // AC9 / AC13 — fail closed. No signing secret or no allow-list means the cookie
    // mechanism does not exist, so there is nothing left that could authorize.
    if (cfg.sessionSigningSecret.empty() || cfg.adminGithubUsers.empty()) return d;

    const auto raw = cookieValue(req.cookieHeader, kSessionCookieName);
    if (!raw) return d;

    const auto session = verifySessionToken(cfg.sessionSigningSecret, *raw, nowUnix);
    if (session.error != SessionError::None) return d;

    // AC7 — read from cfg on every call, never cached, so removing a login denies an
    // already-issued cookie on the very next request.
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

} // namespace puzzpool
