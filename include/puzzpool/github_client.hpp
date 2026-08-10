#pragma once

#include <functional>
#include <string>
#include <vector>

namespace puzzpool {

struct HttpResult {
    long        status = 0;      ///< 0 means the request never completed.
    std::string body;
    bool        transportError = false;
};

/// Injectable HTTP seam for the GitHub OAuth exchange.
///
/// Mirrors PoolService::AddressStatusFetcher so the codebase keeps one seam idiom.
/// Tests supply fakes and therefore never touch the network; production supplies
/// makeLibcurlGitHubClient(). The seam is what makes it structurally true — rather
/// than a convention — that the client secret and authorization code never reach a
/// shell command or a process argument.
struct GitHubHttpClient {
    std::function<HttpResult(const std::string& url,
                             const std::string& formBody,
                             const std::vector<std::string>& headers)> postForm;
    std::function<HttpResult(const std::string& url,
                             const std::vector<std::string>& headers)> get;

    bool valid() const { return static_cast<bool>(postForm) && static_cast<bool>(get); }
};

/// libcurl-backed client. TLS verification on, redirects off, bounded response body.
GitHubHttpClient makeLibcurlGitHubClient(int timeoutSeconds = 10);

} // namespace puzzpool
