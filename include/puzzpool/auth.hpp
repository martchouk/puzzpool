#pragma once

#include <puzzpool/config.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Authentication and authorization decisions, deliberately free of any Crow
// dependency so every allow and deny path is directly unit-testable. The Crow
// adapters live in <puzzpool/auth_service.hpp>.
namespace puzzpool {

// ── Signed-value primitives ───────────────────────────────────────────────────

// Cookie names. Both are HttpOnly; neither is readable from JavaScript.
inline constexpr const char* kSessionCookieName    = "pp_session";
inline constexpr const char* kOauthStateCookieName = "pp_oauth_state";

// Domain-separation labels: a token minted for one purpose must never verify
// for another, even though both are signed with SESSION_SIGNING_SECRET.
inline constexpr const char* kSessionPurpose    = "session";
inline constexpr const char* kOauthStatePurpose = "oauth_state";

// How long an in-flight OAuth authorization may take to come back.
inline constexpr int kOauthStateTtlSeconds = 600;

enum class SignatureError {
    None,
    NotConfigured, // SESSION_SIGNING_SECRET unset or empty — fail closed
    Missing,       // no token supplied
    Malformed,     // wrong shape, bad base64url, or unparsable payload
    BadSignature,
    Expired,
};

// base64url (RFC 4648 §5) without padding.
std::string                base64UrlEncode(const std::string& raw);
std::optional<std::string> base64UrlDecode(const std::string& encoded);

// Comparison whose running time does not depend on where the first difference
// lies. Differing lengths return false immediately, which leaks only a length
// the caller already knows.
bool constantTimeEquals(const std::string& a, const std::string& b);

// Cryptographically secure random bytes, returned lowercase-hex.
// Throws std::runtime_error if the system entropy source is unavailable.
std::string randomHexToken(std::size_t byteCount);

// True when a session may be signed or accepted at all (AC13/D14).
bool sessionSigningEnabled(const Config& cfg);

// `v1.<base64url(payload)>.<base64url(hmac)>`; the payload must carry an "exp"
// field holding an absolute Unix expiry.
std::string signValue(const Config& cfg, const std::string& purpose, const std::string& payloadJson);
std::optional<std::string> verifySignedValue(const Config& cfg,
                                             const std::string& purpose,
                                             const std::string& token,
                                             std::int64_t nowUnix,
                                             SignatureError* error = nullptr);

// ── Session identity ──────────────────────────────────────────────────────────

struct SessionIdentity {
    std::string  login;
    std::int64_t githubId      = 0;
    std::int64_t expiresAtUnix = 0;
};

std::string encodeSessionToken(const Config& cfg, const SessionIdentity& identity);
std::optional<SessionIdentity> decodeSessionToken(const Config& cfg,
                                                  const std::string& token,
                                                  std::int64_t nowUnix,
                                                  SignatureError* error = nullptr);

std::string githubAvatarUrl(std::int64_t githubId);

// ── Allow-list ────────────────────────────────────────────────────────────────

// Matches case-insensitively against the normalized list produced by
// parseAdminGithubUsers() (declared in <puzzpool/config.hpp>). An empty list or
// an empty login denies (AC6).
bool isAllowedAdminLogin(const std::vector<std::string>& allowList, const std::string& login);

// ── Cookies ───────────────────────────────────────────────────────────────────

// Value of `name` in an RFC 6265 Cookie header, or "" when absent.
std::string cookieValue(const std::string& cookieHeader, const std::string& name);

std::string setCookieHeader(const std::string& name,
                            const std::string& value,
                            const std::string& sameSite,
                            long maxAgeSeconds);
std::string clearCookieHeader(const std::string& name);

// ── Central admin guard ───────────────────────────────────────────────────────

// The subset of a request the guard is allowed to look at. Keeping it explicit
// means a test can express any header combination without an HTTP server.
struct AdminRequestView {
    bool        isPost = false;
    std::string adminTokenHeader; // X-Admin-Token
    std::string cookieHeader;     // Cookie
    std::string secFetchSite;     // Sec-Fetch-Site
    std::string origin;           // Origin
    std::string host;             // Host
};

// AC14: a cookie-authorized POST must prove it was issued same-origin, either
// by `Sec-Fetch-Site: same-origin` or by an `Origin` whose authority equals the
// request's `Host`. A request carrying neither has no proof and is rejected.
bool hasSameSiteProof(const AdminRequestView& view);

enum class AdminDenyReason {
    None,
    NotConfigured, // neither ADMIN_TOKEN nor ADMIN_GITHUB_USERS usable (AC9)
    NoCredential,
    BadToken,
    BadSession,
    NotAllowListed,
    CsrfCheckFailed,
};

struct AdminAuthResult {
    bool            authorized = false;
    AdminDenyReason reason     = AdminDenyReason::None;
    int             statusCode = 401;
    std::string     login;    // set only when authorized through a session cookie
    bool            viaToken  = false;
    bool            viaCookie = false;
};

// The single authorization decision for every /api/v1/admin/* route. Default
// deny: an unconfigured or blank authentication setup authorizes nobody.
AdminAuthResult authorizeAdminRequest(const Config& cfg,
                                      const AdminRequestView& view,
                                      std::int64_t nowUnix);

// Non-secret startup diagnostics for AC10/AC13. Never contains a secret value.
std::vector<std::string> startupAuthDiagnostics(const Config& cfg);

} // namespace puzzpool
