#include <puzzpool/hash_utils.hpp>

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif __has_include(<openssl/sha.h>)
#include <openssl/sha.h>
#else
#error "No SHA-256 implementation available."
#endif

namespace puzzpool {

// sha256Hex() below is deliberately left byte-for-byte as it was. It feeds
// keyedDigestHex(), which is the Feistel round function, and ADR-4 makes its output
// part of the on-disk allocation order of every existing puzzle. The HMAC added in
// this file therefore builds on its own private raw-digest and hex helpers rather
// than refactoring a shared formatter out of sha256Hex: duplicating a five-line hex
// loop is a much smaller price than a zero-padding slip that silently reallocates
// every puzzle. The RFC 4231 vectors in tests/test_hash_utils.cpp pin the HMAC path's
// own formatting independently.

std::string sha256Hex(const std::string& input) {
    unsigned char digest[32];

#if defined(__APPLE__)
    CC_SHA256(reinterpret_cast<const unsigned char*>(input.data()),
              static_cast<CC_LONG>(input.size()),
              digest);
#else
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
#endif

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char b : digest) {
        oss << std::setw(2) << static_cast<unsigned>(b);
    }
    return oss.str();
}

std::string keyedDigestHex(const std::string& key, const std::string& msg) {
    return sha256Hex(key + "\x1f" + msg);
}

namespace {

/// Raw 32-byte SHA-256 digest. Private to the HMAC path.
std::string sha256Raw(std::string_view input) {
    unsigned char digest[32];

#if defined(__APPLE__)
    CC_SHA256(reinterpret_cast<const unsigned char*>(input.data()),
              static_cast<CC_LONG>(input.size()),
              digest);
#else
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
#endif

    return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

/// Lowercase hex, two zero-padded digits per byte. Private to the HMAC path; the
/// per-iteration setw(2) under a sticky setfill('0') is the ordering that matters.
std::string toHex(std::string_view bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (char c : bytes) {
        oss << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(c));
    }
    return oss.str();
}

} // namespace

std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    constexpr std::size_t kBlock = 64;

    std::string block = key.size() > kBlock ? sha256Raw(key) : key;
    block.resize(kBlock, '\0');

    std::string inner(kBlock, '\0');
    std::string outer(kBlock, '\0');
    for (std::size_t i = 0; i < kBlock; ++i) {
        const auto b = static_cast<unsigned char>(block[i]);
        inner[i] = static_cast<char>(b ^ 0x36u);
        outer[i] = static_cast<char>(b ^ 0x5cu);
    }

    const std::string innerDigest = sha256Raw(inner + msg);
    return toHex(sha256Raw(outer + innerDigest));
}

bool constantTimeEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

} // namespace puzzpool
