#include <puzzpool/admin_guard.hpp>

#include <puzzpool/admin_auth.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace puzzpool {

namespace {

int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::optional<crow::response> adminGuard(const Config& cfg, const crow::request& req) {
    AdminRequestView view;
    view.isPost           = (req.method == crow::HTTPMethod::Post);
    view.adminTokenHeader = req.get_header_value("X-Admin-Token");
    view.cookieHeader     = req.get_header_value("Cookie");
    view.origin           = req.get_header_value("Origin");
    view.secFetchSite     = req.get_header_value("Sec-Fetch-Site");

    const auto decision = authorizeAdmin(cfg, view, nowUnixSeconds());
    if (decision.allowed) return std::nullopt;

    crow::response r;
    r.code = decision.statusCode;
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    r.body = nlohmann::json({{"error", decision.error}}).dump();
    return r;
}

} // namespace puzzpool
