#pragma once

#include <puzzpool/config.hpp>

#include <crow.h>

#include <optional>

namespace puzzpool {

/// The single authorization gate for every /api/v1/admin/* route.
///
/// Returns std::nullopt when the request is authorized and a ready-to-return error
/// response otherwise. All it does is translate crow::request into an
/// AdminRequestView and authorizeAdmin()'s decision into a crow::response — every
/// decision lives in admin_auth.cpp, where it is testable without an HTTP server.
std::optional<crow::response> adminGuard(const Config& cfg, const crow::request& req);

} // namespace puzzpool
