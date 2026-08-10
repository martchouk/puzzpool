#include <puzzpool/session.hpp>

#include <puzzpool/base64.hpp>
#include <puzzpool/hash_utils.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace puzzpool {

namespace {

constexpr std::size_t kTokenFields = 6;

constexpr std::string_view purposeLabel(TokenPurpose p) {
    return p == TokenPurpose::Session ? "session" : "state";
}

/// Splits on every delimiter, preserving empty fields — the state blob's id field is
/// deliberately empty, so a splitter that collapsed empties would shift every field
/// after it and turn a valid token into a malformed one.
std::vector<std::string_view> splitAll(std::string_view s, char delim) {
    std::vector<std::string_view> parts;
    std::size_t pos = 0;
    for (;;) {
        const auto next = s.find(delim, pos);
        if (next == std::string_view::npos) {
            parts.push_back(s.substr(pos));
            return parts;
        }
        parts.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
}

bool parseInt64(std::string_view text, int64_t& out) {
    if (text.empty()) return false;
    const auto* begin = text.data();
    const auto* end   = text.data() + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

std::string signingInput(TokenPurpose purpose, const SessionIdentity& id,
                         int64_t expiresAt) {
    return std::string("v1.") + std::string(purposeLabel(purpose)) + "."
         + base64UrlEncode(id.login) + "." + base64UrlEncode(id.githubId) + "."
         + std::to_string(expiresAt);
}

std::string issueSignedBlob(const std::string& secret, TokenPurpose purpose,
                            const SessionIdentity& id, int64_t expiresAtUnix) {
    const std::string payload = signingInput(purpose, id, expiresAtUnix);
    return payload + "." + hmacSha256Hex(secret, payload);
}

SessionVerification verifySignedBlob(const std::string& secret, TokenPurpose expected,
                                     std::string_view token, int64_t nowUnix) {
    SessionVerification out;

    const auto parts = splitAll(token, '.');
    if (parts.size() != kTokenFields || parts[0] != "v1") {
        out.error = SessionError::Malformed;
        return out;
    }

    const auto subject = base64UrlDecode(parts[2]);
    const auto id      = base64UrlDecode(parts[3]);
    int64_t expiresAt = 0;
    if (!subject || !id || !parseInt64(parts[4], expiresAt)) {
        out.error = SessionError::Malformed;
        return out;
    }

    // Signature before purpose and before expiry: checking the label first would be
    // answering "is this the right kind of blob?" about bytes nobody has
    // authenticated yet. It also means a rewritten label fails as BadSignature,
    // which is what makes the label genuinely covered by the MAC.
    const std::string payload(token.substr(0, token.rfind('.')));
    if (secret.empty() || !constantTimeEquals(parts[5], hmacSha256Hex(secret, payload))) {
        out.error = SessionError::BadSignature;
        return out;
    }
    if (parts[1] != purposeLabel(expected)) {
        out.error = SessionError::WrongPurpose;
        return out;
    }
    if (nowUnix > expiresAt) {
        out.error = SessionError::Expired;
        return out;
    }

    out.error = SessionError::None;
    out.identity = {*subject, *id};
    out.expiresAt = expiresAt;
    return out;
}

std::string_view trimSpace(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

} // namespace

std::string issueSessionToken(const std::string& secret, const SessionIdentity& id,
                              int64_t expiresAtUnix) {
    return issueSignedBlob(secret, TokenPurpose::Session, id, expiresAtUnix);
}

std::string issueStateToken(const std::string& secret, const std::string& nonce,
                            int64_t expiresAtUnix) {
    return issueSignedBlob(secret, TokenPurpose::State, {nonce, ""}, expiresAtUnix);
}

SessionVerification verifySessionToken(const std::string& secret, std::string_view token,
                                       int64_t nowUnix) {
    return verifySignedBlob(secret, TokenPurpose::Session, token, nowUnix);
}

SessionVerification verifyStateToken(const std::string& secret, std::string_view token,
                                     int64_t nowUnix) {
    return verifySignedBlob(secret, TokenPurpose::State, token, nowUnix);
}

std::optional<std::string> cookieValue(std::string_view cookieHeader,
                                       std::string_view name) {
    std::size_t pos = 0;
    while (pos <= cookieHeader.size()) {
        const auto semi = cookieHeader.find(';', pos);
        const auto end  = (semi == std::string_view::npos) ? cookieHeader.size() : semi;
        const std::string_view pair = trimSpace(cookieHeader.substr(pos, end - pos));

        const auto eq = pair.find('=');
        if (eq != std::string_view::npos && trimSpace(pair.substr(0, eq)) == name) {
            return std::string(trimSpace(pair.substr(eq + 1)));
        }

        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
    return std::nullopt;
}

} // namespace puzzpool
