#include <puzzpool/secure_random.hpp>

#include <cstddef>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>

#if defined(__APPLE__)
#include <cstdlib>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#endif

namespace puzzpool {

std::string secureRandomBytes(std::size_t n) {
    std::string out(n, '\0');
    if (n == 0) return out;

#if defined(__APPLE__)
    arc4random_buf(out.data(), n);
    return out;
#elif defined(__linux__)
    std::size_t filled = 0;
    while (filled < n) {
        const ssize_t got = ::getrandom(out.data() + filled, n - filled, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;   // fall through to /dev/urandom
        }
        filled += static_cast<std::size_t>(got);
    }
    if (filled == n) return out;
#endif

    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom || !urandom.read(out.data(), static_cast<std::streamsize>(n))) {
        throw std::runtime_error("secureRandomBytes: no cryptographic entropy source");
    }
    return out;
}

} // namespace puzzpool
