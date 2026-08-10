#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace puzzpool {

// Outbound HTTP request description. Secrets belong in `body` or `headers`;
// they must never be placed in `url`, because URLs are routinely logged by
// proxies and appear in provider access logs.
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int         timeoutSeconds   = 15;
    std::size_t maxResponseBytes = 256 * 1024;
};

struct HttpResponse {
    long        status = 0;
    std::string body;
    // Non-empty when the transport itself failed (DNS, TLS, timeout, oversized
    // response). Carries a short non-secret description only.
    std::string error;
};

// Injectable seam. Production code uses makeCurlHttpClient(); tests supply a
// stub so no test ever reaches the network.
using HttpClient = std::function<HttpResponse(const HttpRequest&)>;

// libcurl-backed client. TLS verification is enforced, redirects are not
// followed, the response body is capped at HttpRequest::maxResponseBytes, and
// the request body is passed in memory — never through a shell or argv.
HttpClient makeCurlHttpClient();

// application/x-www-form-urlencoded percent-encoding.
std::string urlEncode(const std::string& value);

} // namespace puzzpool
