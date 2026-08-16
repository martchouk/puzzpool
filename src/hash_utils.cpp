#include <puzzpool/hash_utils.hpp>

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif __has_include(<openssl/sha.h>)
#include <openssl/sha.h>
#else
#error "No SHA-256 implementation available."
#endif

namespace puzzpool {

namespace {

constexpr std::size_t kSha256DigestBytes = 32;
constexpr std::size_t kSha256BlockBytes  = 64;

std::string toHex(const std::string& raw) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const char c : raw) {
        oss << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(c));
    }
    return oss.str();
}

} // namespace

std::string sha256Raw(const std::string& input) {
    unsigned char digest[kSha256DigestBytes];

#if defined(__APPLE__)
    CC_SHA256(reinterpret_cast<const unsigned char*>(input.data()),
              static_cast<CC_LONG>(input.size()),
              digest);
#else
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
#endif

    return std::string(reinterpret_cast<const char*>(digest), kSha256DigestBytes);
}

std::string sha256Hex(const std::string& input) {
    return toHex(sha256Raw(input));
}

std::string keyedDigestHex(const std::string& key, const std::string& msg) {
    return sha256Hex(key + "\x1f" + msg);
}

std::string hmacSha256Raw(const std::string& key, const std::string& msg) {
    // RFC 2104: keys longer than the block size are hashed first; shorter keys
    // are right-padded with zero bytes.
    std::string block = (key.size() > kSha256BlockBytes) ? sha256Raw(key) : key;
    block.resize(kSha256BlockBytes, '\0');

    std::string inner(kSha256BlockBytes, '\0');
    std::string outer(kSha256BlockBytes, '\0');
    for (std::size_t i = 0; i < kSha256BlockBytes; ++i) {
        const auto k = static_cast<unsigned char>(block[i]);
        inner[i] = static_cast<char>(k ^ 0x36u);
        outer[i] = static_cast<char>(k ^ 0x5cu);
    }

    return sha256Raw(outer + sha256Raw(inner + msg));
}

std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    return toHex(hmacSha256Raw(key, msg));
}

} // namespace puzzpool
