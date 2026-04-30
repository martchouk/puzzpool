#include <puzzpool/hex_bigint.hpp>

#include <algorithm>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace puzzpool {

std::string strip0x(std::string s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return s.substr(2);
    return s;
}

bool isValidHex(const std::string& s) {
    static const std::regex kHexRegex(R"(^(0x)?[0-9a-fA-F]{1,64}$)");
    return std::regex_match(s, kHexRegex);
}

cpp_int hexToInt(const std::string& hex) {
    std::string s = strip0x(hex);
    cpp_int out = 0;
    for (char ch : s) {
        out <<= 4;
        if (ch >= '0' && ch <= '9')      out += ch - '0';
        else if (ch >= 'a' && ch <= 'f') out += 10 + ch - 'a';
        else if (ch >= 'A' && ch <= 'F') out += 10 + ch - 'A';
        else throw std::runtime_error("invalid hex");
    }
    return out;
}

std::string intToHex(cpp_int v, size_t width) {
    if (v < 0) throw std::runtime_error("negative bigint not allowed");
    if (v == 0) return std::string(width, '0');
    static const char* digits = "0123456789abcdef";
    std::string out;
    while (v > 0) {
        unsigned nibble = static_cast<unsigned>(v & 0xf);
        out.push_back(digits[nibble]);
        v >>= 4;
    }
    std::reverse(out.begin(), out.end());
    if (out.size() < width) out = std::string(width - out.size(), '0') + out;
    return out;
}

std::string normalizeHex(const std::string& s) {
    return intToHex(hexToInt(s), 64);
}

cpp_int ceilDiv(const cpp_int& a, const cpp_int& b) {
    return (a + b - 1) / b;
}

cpp_int minBig(const cpp_int& a, const cpp_int& b) { return a < b ? a : b; }
cpp_int maxBig(const cpp_int& a, const cpp_int& b) { return a > b ? a : b; }

std::string bigToDec(const cpp_int& v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

cpp_int parsePositiveBigInt(const std::string& s, const cpp_int& fallback) {
    if (s.empty()) return fallback;
    try {
        cpp_int v(s);
        if (v <= 0) return fallback;
        return v;
    } catch (...) {
        return fallback;
    }
}

RangeNorm normalizedRange(const std::string& startHex, const std::string& endHex) {
    RangeNorm r{hexToInt(startHex), hexToInt(endHex), 0};
    if (r.end <= r.start) throw std::runtime_error("Puzzle range must be > 0");
    r.range = r.end - r.start;
    return r;
}

cpp_int bitLength(const cpp_int& n) {
    if (n <= 0) return 0;
    cpp_int x = n;
    cpp_int bits = 0;
    while (x > 0) {
        x >>= 1;
        ++bits;
    }
    return bits;
}

} // namespace puzzpool
