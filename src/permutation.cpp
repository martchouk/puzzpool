#include <puzzpool/permutation.hpp>
#include <puzzpool/hash_utils.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace puzzpool {

cpp_int gcdBigInt(cpp_int a, cpp_int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        cpp_int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

unsigned nextEven(unsigned n) {
    return (n % 2 == 0) ? n : (n + 1);
}

cpp_int feistelRoundValue(const cpp_int& right, const std::string& roundKey, const cpp_int& mask) {
    return hexToInt(hmacSha256Hex(roundKey, bigToDec(right))) & mask;
}

cpp_int permutePow2Feistel(const cpp_int& x, unsigned bits, const std::string& key) {
    const unsigned halfBits = bits / 2;
    const cpp_int halfMask = (cpp_int(1) << halfBits) - 1;

    cpp_int left  = (x >> halfBits) & halfMask;
    cpp_int right = x & halfMask;

    constexpr int ROUNDS = 6;
    for (int round = 0; round < ROUNDS; ++round) {
        cpp_int f      = feistelRoundValue(right, key + ":round:" + std::to_string(round), halfMask);
        cpp_int newLeft  = right;
        cpp_int newRight = (left ^ f) & halfMask;
        left  = newLeft;
        right = newRight;
    }
    return ((left & halfMask) << halfBits) | (right & halfMask);
}

cpp_int permuteIndexFeistel(const cpp_int& orderIndex, const cpp_int& n, const std::string& key) {
    if (n <= 1) return 0;
    if (orderIndex < 0 || orderIndex >= n) throw std::runtime_error("permuteIndexFeistel out of range");

    unsigned bits = nextEven(std::max<cpp_int>(1, bitLength(n - 1)).convert_to<unsigned>());
    cpp_int x = orderIndex;
    for (;;) {
        x = permutePow2Feistel(x, bits, key);
        if (x < n) return x;
    }
}

AffineParams deriveAffinePermutationParams(const std::string& seedHex, const cpp_int& n) {
    if (n <= 1) return {1, 0};
    cpp_int counter = 0;
    cpp_int a = 1;
    for (;;) {
        a = hexToInt(sha256Hex(seedHex + ":a:" + bigToDec(counter))) % n;
        if (a == 0) a = 1;
        if (gcdBigInt(a, n) == 1) break;
        ++counter;
    }
    cpp_int b = hexToInt(sha256Hex(seedHex + ":b")) % n;
    return {a, b};
}

cpp_int permuteIndexAffine(const cpp_int& orderIndex, const cpp_int& n,
                             const cpp_int& a, const cpp_int& b) {
    return (a * orderIndex + b) % n;
}

} // namespace puzzpool
