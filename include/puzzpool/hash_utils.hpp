#pragma once

#include <string>

namespace puzzpool {

std::string sha256Hex(const std::string& input);

// Raw 32-byte SHA-256 digest. Building block for hmacSha256Raw().
std::string sha256Raw(const std::string& input);

// Legacy deterministic keyed digest: sha256(key || 0x1f || msg).
//
// This is deliberately NOT a message-authentication code. A secret-prefix
// construction over a Merkle-Damgard hash is length-extension forgeable, so it
// must never be used to authenticate a cookie, token, or any other value an
// attacker can influence.
//
// Its output is frozen: it is the Feistel round function behind
// `virtual_random_chunks_v1` (ADR-4), so changing a single output byte would
// reorder allocation for every puzzle that has already issued work. Use
// hmacSha256Hex() for authentication instead.
std::string keyedDigestHex(const std::string& key, const std::string& msg);

// RFC 2104 HMAC-SHA-256. Use this — and a signing key distinct from any
// allocator seed — whenever a value must be authenticated.
std::string hmacSha256Raw(const std::string& key, const std::string& msg);
std::string hmacSha256Hex(const std::string& key, const std::string& msg);

} // namespace puzzpool
