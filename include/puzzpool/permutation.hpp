#pragma once

#include <puzzpool/types.hpp>

#include <string>

namespace puzzpool {

cpp_int  gcdBigInt(cpp_int a, cpp_int b);
unsigned nextEven(unsigned n);

cpp_int feistelRoundValue(const cpp_int& right, const std::string& roundKey, const cpp_int& mask);
cpp_int permutePow2Feistel(const cpp_int& x, unsigned bits, const std::string& key);
cpp_int permuteIndexFeistel(const cpp_int& orderIndex, const cpp_int& n, const std::string& key);

AffineParams deriveAffinePermutationParams(const std::string& seedHex, const cpp_int& n);
cpp_int      permuteIndexAffine(const cpp_int& orderIndex, const cpp_int& n,
                                 const cpp_int& a, const cpp_int& b);

} // namespace puzzpool
