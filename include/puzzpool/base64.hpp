#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace puzzpool {

/// Base64url (RFC 4648 §5) without padding: alphabet A-Za-z0-9-_ and no '='.
/// Used for the variable-length fields of signed tokens, where '+', '/' and '='
/// would all be ambiguous in a URL or a cookie value.
std::string base64UrlEncode(std::string_view bytes);

/// Inverse of base64UrlEncode. Returns std::nullopt for any out-of-alphabet
/// character, for padding, and for a length that cannot encode whole bytes.
std::optional<std::string> base64UrlDecode(std::string_view text);

} // namespace puzzpool
