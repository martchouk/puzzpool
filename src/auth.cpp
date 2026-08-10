#include <puzzpool/auth.hpp>

#include <puzzpool/env.hpp>
#include <puzzpool/hash_utils.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace puzzpool {

namespace {

using json = nlohmann::json;

constexpr const char* kTokenVersion = "v1";
constexpr char        kAlphabet[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return s;
}

int decodeBase64UrlChar(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// Splits `v1.<payload>.<signature>`; returns false for any other shape.
bool splitToken(const std::string& token,
                std::string& version,
                std::string& encodedPayload,
                std::string& encodedSignature) {
    const auto first = token.find('.');
    if (first == std::string::npos) return false;
    const auto second = token.find('.', first + 1);
    if (second == std::string::npos) return false;
    if (token.find('.', second + 1) != std::string::npos) return false;

    version          = token.substr(0, first);
    encodedPayload   = token.substr(first + 1, second - first - 1);
    encodedSignature = token.substr(second + 1);
    return !version.empty() && !encodedPayload.empty() && !encodedSignature.empty();
}

std::string signingInput(const std::string& purpose, const std::string& encodedPayload) {
    return purpose + "." + kTokenVersion + "." + encodedPayload;
}

void setError(SignatureError* slot, SignatureError value) {
    if (slot) *slot = value;
}

} // namespace

// ── base64url ─────────────────────────────────────────────────────────────────

std::string base64UrlEncode(const std::string& raw) {
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);

    std::size_t i = 0;
    while (i + 2 < raw.size()) {
        const auto b0 = static_cast<unsigned char>(raw[i]);
        const auto b1 = static_cast<unsigned char>(raw[i + 1]);
        const auto b2 = static_cast<unsigned char>(raw[i + 2]);
        out += kAlphabet[b0 >> 2];
        out += kAlphabet[((b0 & 0x03u) << 4) | (b1 >> 4)];
        out += kAlphabet[((b1 & 0x0fu) << 2) | (b2 >> 6)];
        out += kAlphabet[b2 & 0x3fu];
        i += 3;
    }

    const std::size_t remaining = raw.size() - i;
    if (remaining == 1) {
        const auto b0 = static_cast<unsigned char>(raw[i]);
        out += kAlphabet[b0 >> 2];
        out += kAlphabet[(b0 & 0x03u) << 4];
    } else if (remaining == 2) {
        const auto b0 = static_cast<unsigned char>(raw[i]);
        const auto b1 = static_cast<unsigned char>(raw[i + 1]);
        out += kAlphabet[b0 >> 2];
        out += kAlphabet[((b0 & 0x03u) << 4) | (b1 >> 4)];
        out += kAlphabet[(b1 & 0x0fu) << 2];
    }
    return out;
}

std::optional<std::string> base64UrlDecode(const std::string& encoded) {
    if (encoded.size() % 4 == 1) return std::nullopt;

    std::string out;
    out.reserve(encoded.size() / 4 * 3);

    std::uint32_t buffer = 0;
    int           bits   = 0;
    for (const char c : encoded) {
        const int value = decodeBase64UrlChar(static_cast<unsigned char>(c));
        if (value < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xffu);
        }
    }
    // Leftover bits of a well-formed unpadded encoding are always zero.
    if (bits > 0 && (buffer & ((1u << bits) - 1u)) != 0) return std::nullopt;
    return out;
}

// ── Comparison and randomness ────────────────────────────────────────────────

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

std::string randomHexToken(std::size_t byteCount) {
    std::string raw(byteCount, '\0');
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.read(raw.data(), static_cast<std::streamsize>(byteCount))) {
        throw std::runtime_error("secure random source unavailable");
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(byteCount * 2);
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out += kHex[b >> 4];
        out += kHex[b & 0x0fu];
    }
    return out;
}

// ── Signed values ─────────────────────────────────────────────────────────────

bool sessionSigningEnabled(const Config& cfg) {
    return !cfg.sessionSigningSecret.empty();
}

std::string signValue(const Config& cfg, const std::string& purpose, const std::string& payloadJson) {
    if (!sessionSigningEnabled(cfg)) {
        // Signing with an empty or default key is forbidden (AC13).
        throw std::runtime_error("SESSION_SIGNING_SECRET is not configured");
    }
    const std::string encodedPayload = base64UrlEncode(payloadJson);
    const std::string signature =
        hmacSha256Raw(cfg.sessionSigningSecret, signingInput(purpose, encodedPayload));
    return std::string(kTokenVersion) + "." + encodedPayload + "." + base64UrlEncode(signature);
}

std::optional<std::string> verifySignedValue(const Config& cfg,
                                             const std::string& purpose,
                                             const std::string& token,
                                             std::int64_t nowUnix,
                                             SignatureError* error) {
    setError(error, SignatureError::None);

    if (!sessionSigningEnabled(cfg)) {
        setError(error, SignatureError::NotConfigured);
        return std::nullopt;
    }
    if (token.empty()) {
        setError(error, SignatureError::Missing);
        return std::nullopt;
    }

    std::string version, encodedPayload, encodedSignature;
    if (!splitToken(token, version, encodedPayload, encodedSignature) || version != kTokenVersion) {
        setError(error, SignatureError::Malformed);
        return std::nullopt;
    }

    const auto signature = base64UrlDecode(encodedSignature);
    if (!signature) {
        setError(error, SignatureError::Malformed);
        return std::nullopt;
    }

    const std::string expected =
        hmacSha256Raw(cfg.sessionSigningSecret, signingInput(purpose, encodedPayload));
    if (!constantTimeEquals(*signature, expected)) {
        setError(error, SignatureError::BadSignature);
        return std::nullopt;
    }

    const auto payload = base64UrlDecode(encodedPayload);
    if (!payload) {
        setError(error, SignatureError::Malformed);
        return std::nullopt;
    }

    json parsed = json::parse(*payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("exp") ||
        !parsed["exp"].is_number_integer()) {
        setError(error, SignatureError::Malformed);
        return std::nullopt;
    }
    if (parsed["exp"].get<std::int64_t>() <= nowUnix) {
        setError(error, SignatureError::Expired);
        return std::nullopt;
    }

    return payload;
}

// ── Session identity ──────────────────────────────────────────────────────────

std::string encodeSessionToken(const Config& cfg, const SessionIdentity& identity) {
    const json payload{
        {"login", identity.login},
        {"gid", identity.githubId},
        {"exp", identity.expiresAtUnix},
    };
    return signValue(cfg, kSessionPurpose, payload.dump());
}

std::optional<SessionIdentity> decodeSessionToken(const Config& cfg,
                                                  const std::string& token,
                                                  std::int64_t nowUnix,
                                                  SignatureError* error) {
    const auto payload = verifySignedValue(cfg, kSessionPurpose, token, nowUnix, error);
    if (!payload) return std::nullopt;

    json parsed = json::parse(*payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("login") ||
        !parsed["login"].is_string() || !parsed.contains("gid") ||
        !parsed["gid"].is_number_integer()) {
        setError(error, SignatureError::Malformed);
        return std::nullopt;
    }

    SessionIdentity identity;
    identity.login         = parsed["login"].get<std::string>();
    identity.githubId      = parsed["gid"].get<std::int64_t>();
    identity.expiresAtUnix = parsed["exp"].get<std::int64_t>();
    if (identity.login.empty()) {
        setError(error, SignatureError::Malformed);
        return std::nullopt;
    }
    return identity;
}

std::string githubAvatarUrl(std::int64_t githubId) {
    // Derived from the immutable numeric id, so it stays correct across renames
    // and never embeds an attacker-controlled string in a URL.
    return "https://avatars.githubusercontent.com/u/" + std::to_string(githubId) + "?v=4";
}

// ── Allow-list ────────────────────────────────────────────────────────────────

bool isAllowedAdminLogin(const std::vector<std::string>& allowList, const std::string& login) {
    if (allowList.empty() || login.empty()) return false;
    const std::string normalized = toLower(login);
    return std::find(allowList.begin(), allowList.end(), normalized) != allowList.end();
}

// ── Cookies ───────────────────────────────────────────────────────────────────

std::string cookieValue(const std::string& cookieHeader, const std::string& name) {
    std::size_t pos = 0;
    while (pos < cookieHeader.size()) {
        std::size_t end = cookieHeader.find(';', pos);
        if (end == std::string::npos) end = cookieHeader.size();

        const std::string pair = trim(cookieHeader.substr(pos, end - pos));
        const auto        eq   = pair.find('=');
        if (eq != std::string::npos && trim(pair.substr(0, eq)) == name) {
            return trim(pair.substr(eq + 1));
        }
        pos = end + 1;
    }
    return "";
}

std::string setCookieHeader(const std::string& name,
                            const std::string& value,
                            const std::string& sameSite,
                            long maxAgeSeconds) {
    std::ostringstream out;
    out << name << "=" << value
        << "; Path=/"
        << "; HttpOnly"
        << "; Secure"
        << "; SameSite=" << sameSite
        << "; Max-Age=" << maxAgeSeconds;
    return out.str();
}

std::string clearCookieHeader(const std::string& name) {
    return std::string(name) + "=; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=0";
}

// ── CSRF ──────────────────────────────────────────────────────────────────────

bool hasSameSiteProof(const AdminRequestView& view) {
    if (!view.secFetchSite.empty()) {
        // Modern browsers always send this on fetch/XHR. `same-origin` is the
        // only value a same-origin dashboard call produces.
        return view.secFetchSite == "same-origin";
    }

    if (view.origin.empty() || view.host.empty()) return false;

    const auto schemeEnd = view.origin.find("://");
    if (schemeEnd == std::string::npos) return false;
    const std::string originAuthority = view.origin.substr(schemeEnd + 3);
    return !originAuthority.empty() && originAuthority == view.host;
}

// ── Central admin guard ───────────────────────────────────────────────────────

AdminAuthResult authorizeAdminRequest(const Config& cfg,
                                      const AdminRequestView& view,
                                      std::int64_t nowUnix) {
    AdminAuthResult result;

    const bool tokenConfigured  = !cfg.adminToken.empty();
    const bool cookieConfigured = sessionSigningEnabled(cfg) && !cfg.adminGithubUsers.empty();

    if (!tokenConfigured && !cookieConfigured) {
        // AC9: no configured mechanism authorizes nobody — never fail open.
        result.reason     = AdminDenyReason::NotConfigured;
        result.statusCode = 401;
        return result;
    }

    if (tokenConfigured && !view.adminTokenHeader.empty() &&
        constantTimeEquals(view.adminTokenHeader, cfg.adminToken)) {
        // A header credential is not sent ambiently by a browser, so it needs no
        // CSRF proof of its own.
        result.authorized = true;
        result.viaToken   = true;
        return result;
    }

    if (cookieConfigured) {
        const std::string session = cookieValue(view.cookieHeader, kSessionCookieName);
        SignatureError    error   = SignatureError::None;
        const auto        identity = decodeSessionToken(cfg, session, nowUnix, &error);
        if (identity) {
            // AC7: the allow-list is consulted per request, so a removal takes
            // effect on the next call without invalidating cookies.
            if (!isAllowedAdminLogin(cfg.adminGithubUsers, identity->login)) {
                result.reason     = AdminDenyReason::NotAllowListed;
                result.statusCode = 401;
                return result;
            }
            if (view.isPost && !hasSameSiteProof(view)) {
                result.reason     = AdminDenyReason::CsrfCheckFailed;
                result.statusCode = 403;
                return result;
            }
            result.authorized = true;
            result.viaCookie  = true;
            result.login      = identity->login;
            return result;
        }
        if (error != SignatureError::Missing && error != SignatureError::NotConfigured) {
            result.reason     = AdminDenyReason::BadSession;
            result.statusCode = 401;
            return result;
        }
    }

    result.reason     = view.adminTokenHeader.empty() ? AdminDenyReason::NoCredential
                                                      : AdminDenyReason::BadToken;
    result.statusCode = 401;
    return result;
}

std::vector<std::string> startupAuthDiagnostics(const Config& cfg) {
    std::vector<std::string> messages;

    if (cfg.adminToken.empty() && cfg.adminGithubUsers.empty()) {
        messages.push_back(
            "WARNING: neither ADMIN_TOKEN nor ADMIN_GITHUB_USERS is configured — "
            "every /api/v1/admin/* route will return 401. Set one of them to restore admin access.");
    }
    if (!sessionSigningEnabled(cfg)) {
        messages.push_back(
            "WARNING: SESSION_SIGNING_SECRET is unset — /api/v1/auth/* returns 503 and no "
            "session cookie is issued or accepted.");
    } else if (cfg.githubOauthClientId.empty() || cfg.githubOauthClientSecret.empty()) {
        messages.push_back(
            "WARNING: GITHUB_OAUTH_CLIENT_ID or GITHUB_OAUTH_CLIENT_SECRET is unset — "
            "GitHub sign-in returns 503.");
    }
    if (!cfg.adminGithubUsers.empty() && !sessionSigningEnabled(cfg)) {
        messages.push_back(
            "WARNING: ADMIN_GITHUB_USERS is configured but SESSION_SIGNING_SECRET is not — "
            "the allow-list cannot authorize anyone until a signing secret is set.");
    }

    return messages;
}

} // namespace puzzpool
