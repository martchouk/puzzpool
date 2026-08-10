#pragma once

#include <puzzpool/config.hpp>
#include <puzzpool/github_client.hpp>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace puzzpool {

/// The four /api/v1/auth/* handlers.
///
/// A deliberate sibling of PoolService rather than a member: it touches no database
/// and no service mutex, so folding it in would widen that class's lock scope and
/// dependencies for nothing. Every source of non-determinism — the HTTP client, the
/// clock, and the nonce generator — is injected, so no test touches the network or
/// the wall clock.
class AuthService {
public:
    using Clock       = std::function<int64_t()>;                 ///< unix seconds
    using NonceSource = std::function<std::string(std::size_t)>;  ///< raw random bytes

    explicit AuthService(const Config& cfg,
                         GitHubHttpClient client = {},
                         Clock clock = {},
                         NonceSource nonce = {});

    crow::response handleGithubLogin(const crow::request& req);
    crow::response handleGithubCallback(const crow::request& req);
    crow::response handleLogout(const crow::request& req);
    crow::response handleAuthMe(const crow::request& req);

private:
    const Config     cfg_;
    GitHubHttpClient http_;
    Clock            clock_;
    NonceSource      nonce_;
};

} // namespace puzzpool
