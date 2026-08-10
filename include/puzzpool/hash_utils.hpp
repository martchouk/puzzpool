#pragma once

#include <string>
#include <string_view>

namespace puzzpool {

/// Lowercase hex SHA-256 of `input`, 64 characters, two zero-padded digits per byte.
std::string sha256Hex(const std::string& input);

/// Keyed digest: sha256(key || 0x1f || msg).
///
/// WARNING: this is deliberately NOT an HMAC. It is a secret-prefix construction and
/// is vulnerable to length extension. It exists only as the Feistel round function in
/// src/permutation.cpp, where its output bytes are frozen because changing them would
/// reorder allocation for every existing puzzle (ADR-4). Never use it as a MAC — use
/// hmacSha256Hex() for anything that must resist forgery.
std::string keyedDigestHex(const std::string& key, const std::string& msg);

/// RFC 2104 HMAC-SHA-256, lowercase hex. The MAC for session cookies and OAuth state.
std::string hmacSha256Hex(const std::string& key, const std::string& msg);

/// Compares without leaking, through timing, *where* two equal-length inputs differ.
/// The lengths themselves are not secret and short-circuit.
bool constantTimeEquals(std::string_view a, std::string_view b);

} // namespace puzzpool
