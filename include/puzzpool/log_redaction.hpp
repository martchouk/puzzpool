#pragma once

#include <string>

namespace puzzpool {

// Removes query strings from a log line before it is written.
//
// The OAuth callback receives the GitHub authorization code and the state nonce
// as query parameters, and Crow logs the full request target for every response
// (`CROW_LOG_INFO << "Response: " << ... << req_.raw_url << ...`). Without this
// the process log — and therefore journald — would hold a live authorization
// code, which the project's security policy forbids.
//
// Every run of characters from a '?' up to the next whitespace is replaced with
// `?<redacted>`. That is deliberately blunt: it cannot be fooled by a parameter
// name the redactor has not heard of, and over-redacting a log line is cheap
// where under-redacting one leaks a credential. The path, method, status and
// timing that operators actually read are all left intact.
std::string redactQueryStrings(const std::string& message);

} // namespace puzzpool
