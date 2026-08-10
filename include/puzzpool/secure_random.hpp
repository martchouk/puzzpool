#pragma once

#include <cstddef>
#include <string>

namespace puzzpool {

/// `n` cryptographically secure random bytes.
///
/// Throws std::runtime_error when no entropy source is available. There is
/// deliberately no std::mt19937 or std::random_device fallback: an unguessable OAuth
/// state is a security property, and degrading to a predictable generator would keep
/// the service running while silently removing that property.
std::string secureRandomBytes(std::size_t n);

} // namespace puzzpool
