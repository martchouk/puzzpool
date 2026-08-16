#pragma once

#include <puzzpool/auth.hpp>
#include <puzzpool/config.hpp>
#include <puzzpool/http_client.hpp>

#include <crow.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace puzzpool {

// ── Crow adapters for the central guard ───────────────────────────────────────

AdminRequestView adminRequestView(const crow::request& req);

// std::nullopt means "allow"; otherwise the response the route must return.
// This is the only authorization decision main.cpp makes for an admin route.
//
// `nowUnix` is the Unix time in seconds the session expiry is judged against.
// std::nullopt reads the system clock, which is what production wants; tests
// pass an explicit instant so a cookie's expiry cannot drift into the past as
// wall-clock time advances.
std::optional<crow::response> adminGuard(const Config& cfg,
                                         const crow::request& req,
                                         std::optional<std::int64_t> nowUnix = std::nullopt);

// ── GitHub OAuth endpoints ────────────────────────────────────────────────────

// AuthService owns no shared state and touches no database, so it holds no lock
// and its blocking provider calls never run under the PoolService mutex.
class AuthService {
public:
    // Returns the current Unix time in seconds; injectable so expiry paths are
    // deterministic in tests.
    using Clock = std::function<std::int64_t()>;

    explicit AuthService(const Config& cfg, HttpClient http = {}, Clock clock = {});

    crow::response handleGithubLogin(const crow::request& req);
    crow::response handleGithubCallback(const crow::request& req);
    crow::response handleLogout(const crow::request& req);
    crow::response handleMe(const crow::request& req);

private:
    // 503 body used whenever the OAuth/cookie path is disabled by configuration.
    crow::response serviceUnavailable(const std::string& reason) const;
    std::int64_t   now() const;

    const Config cfg_;
    HttpClient   http_;
    Clock        clock_;
};

} // namespace puzzpool
