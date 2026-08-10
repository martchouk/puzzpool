#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace puzzpool {

/// The identity carried inside a session token. `githubId` is a string end to end;
/// GitHub sends it as a JSON integer and it is converted once, at the callback
/// boundary. AC26 derives the avatar URL from it.
struct SessionIdentity {
    std::string login;
    std::string githubId;
};

/// What a signed blob is for. The label is part of the signed input, so a state blob
/// and a session blob are not interchangeable even though they share one format and
/// one signing key.
enum class TokenPurpose { Session, State };

enum class SessionError { None, Malformed, WrongPurpose, BadSignature, Expired };

struct SessionVerification {
    SessionError    error = SessionError::Malformed;
    SessionIdentity identity;
    int64_t         expiresAt = 0;
};

/// Token format: v1.<purpose>.<b64url(subject)>.<b64url(id)>.<expiryUnix>.<hmacHex>
/// The MAC covers everything before the final dot. Base64url on the two variable
/// fields keeps the '.' delimiter unambiguous, so no login can forge a field boundary.
std::string issueSessionToken(const std::string& secret, const SessionIdentity& id,
                              int64_t expiresAtUnix);

/// A signed, expiring OAuth state blob carrying `nonce` as its subject.
std::string issueStateToken(const std::string& secret, const std::string& nonce,
                            int64_t expiresAtUnix);

/// Verifies shape, then signature, then purpose, then expiry — in that order. The
/// identity is never read out of a token whose signature has not been checked.
/// An empty secret never verifies anything (AC13).
SessionVerification verifySessionToken(const std::string& secret, std::string_view token,
                                       int64_t nowUnix);

SessionVerification verifyStateToken(const std::string& secret, std::string_view token,
                                     int64_t nowUnix);

/// Value of the cookie named exactly `name` in a raw `Cookie:` header, or nullopt.
/// The name is matched exactly — never as a prefix — so `pp_session_extra` and
/// `xpp_session` do not satisfy a lookup for `pp_session`.
std::optional<std::string> cookieValue(std::string_view cookieHeader, std::string_view name);

} // namespace puzzpool
