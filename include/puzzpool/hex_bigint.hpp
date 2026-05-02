#pragma once

#include <puzzpool/types.hpp>

#include <string>

namespace puzzpool {

std::string strip0x(std::string s);
bool        isValidHex(const std::string& s);
cpp_int     hexToInt(const std::string& hex);
std::string intToHex(cpp_int v, size_t width = 64);
std::string normalizeHex(const std::string& s);
cpp_int     ceilDiv(const cpp_int& a, const cpp_int& b);
cpp_int     minBig(const cpp_int& a, const cpp_int& b);
cpp_int     maxBig(const cpp_int& a, const cpp_int& b);
std::string bigToDec(const cpp_int& v);
cpp_int     parsePositiveBigInt(const std::string& s, const cpp_int& fallback = 0);
RangeNorm   normalizedRange(const std::string& startHex, const std::string& endHex);
cpp_int     bitLength(const cpp_int& n);

} // namespace puzzpool
