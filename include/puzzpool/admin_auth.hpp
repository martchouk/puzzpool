#pragma once

#include <puzzpool/config.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace puzzpool {

/// Name of the signed session cookie issued by /api/v1/auth/github/callback.
inline constexpr std::string_view kSessionCookieName = "pp_session";
/// Name of the signed, browser-binding OAuth state cookie.
inline constexpr std::string_view kOauthStateCookieName = "pp_oauth_state";

/// Splits a comma-separated ADMIN_GITHUB_USERS value into trimmed, lowercased,
/// non-empty logins. Lives here rather than in config.cpp so configuration loading
/// does not grow authorization logic.
std::vector<std::string> parseAdminGithubUsers(const std::string& raw);

/// True when `login` is on the configured allow-list. Case-insensitive (AC6). An
/// empty list or an empty login is always false — the list is an allow-list, so
/// "unconfigured" means "nobody", never "everybody".
bool isAllowedAdminLogin(const Config& cfg, std::string_view login);

/// Actionable, secret-free startup diagnostics (AC10, AC13). Returns the lines to
/// print; main() only prints them, so the content is directly testable.
std::vector<std::string> startupAuthDiagnostics(const Config& cfg);

// ── The admin authorization decision ─────────────────────────────────────────
// Implemented in the same translation unit; see src/admin_auth.cpp.

enum class AdminAuthMechanism { None, Token, Cookie };

/// A Crow-free view of the parts of a request that authorization depends on, so
/// every allow and deny path is testable without an HTTP server.
struct AdminRequestView {
    bool        isPost = false;
    std::string adminTokenHeader;   // X-Admin-Token
    std::string cookieHeader;       // raw Cookie header
    std::string origin;             // Origin
    std::string secFetchSite;       // Sec-Fetch-Site
};

struct AdminAuthDecision {
    bool               allowed    = false;
    int                statusCode = 401;
    std::string        error      = "unauthorized";  // fixed, non-secret codes only
    AdminAuthMechanism mechanism  = AdminAuthMechanism::None;
    std::string        login;                        // set only when allowed by cookie
};

/// True when a cookie-authorized request carries affirmative proof that it came from
/// our own origin (AC14). Absence of every signal is NOT proof.
bool hasSameOriginProof(const Config& cfg, const AdminRequestView& req);

/// The whole admin authorization decision. Default-deny: with neither ADMIN_TOKEN nor
/// a usable GitHub session configured, every request is denied (AC9).
AdminAuthDecision authorizeAdmin(const Config& cfg, const AdminRequestView& req,
                                 int64_t nowUnix);

} // namespace puzzpool
