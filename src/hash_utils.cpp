#include <puzzpool/hash_utils.hpp>

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

std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    return sha256Hex(key + "\x1f" + msg);
}

} // namespace puzzpool
