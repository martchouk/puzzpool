#pragma once

#include <string>

namespace puzzpool {

std::string sha256Hex(const std::string& input);
std::string hmacSha256Hex(const std::string& key, const std::string& msg);

} // namespace puzzpool
