#include <puzzpool/auth.hpp>
#include <puzzpool/auth_service.hpp>
#include <puzzpool/config.hpp>
#include <puzzpool/log_redaction.hpp>
#include <puzzpool/service.hpp>

#include <crow.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

namespace {

// Crow logs the raw request target for every response, which on the OAuth
// callback contains the GitHub authorization code and the state nonce. Raising
// CROW_LOG_LEVEL would silence request logging altogether; redacting keeps the
// operational log an operator relies on and removes only the query string.
class RedactingLogHandler : public crow::CerrLogHandler {
public:
    void log(const std::string& message, crow::LogLevel level) override {
        crow::CerrLogHandler::log(puzzpool::redactQueryStrings(message), level);
    }
};

} // namespace

int main() {
    try {
        // Installed before any route is registered, so no request can be logged
        // through the default handler. The handler outlives the server.
        static RedactingLogHandler logHandler;
        crow::logger::setHandler(&logHandler);

        puzzpool::Config cfg = puzzpool::loadConfigFromEnv();
        puzzpool::PoolService service(cfg);
        puzzpool::AuthService authService(cfg);

        crow::SimpleApp app;

        CROW_ROUTE(app, "/")([] {
            crow::response r;
            r.code = 200;
            r.set_header("Cache-Control", "no-store");
            std::ifstream in("public/index.html");
            if (!in) {
                r.code = 404;
                r.body = "public/index.html not found; run: npm run build --prefix frontend";
                return r;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            r.set_header("Content-Type", "text/html; charset=utf-8");
            r.body = ss.str();
            return r;
        });

        CROW_ROUTE(app, "/api/v1/stats").methods(crow::HTTPMethod::GET)
        ([&service](const crow::request& req) {
            return service.handleStats(req);
        });

        CROW_ROUTE(app, "/api/v1/visualization/heatmap").methods(crow::HTTPMethod::GET)
        ([&service](const crow::request& req) {
            return service.handleHeatmapVisualization(req);
        });

        CROW_ROUTE(app, "/api/v1/visualization/hilbert").methods(crow::HTTPMethod::GET)
        ([&service](const crow::request& req) {
            return service.handleHilbertVisualization(req);
        });

        CROW_ROUTE(app, "/api/v1/visualization/allocator").methods(crow::HTTPMethod::GET)
        ([&service](const crow::request& req) {
            return service.handleAllocatorVisualization(req);
        });

        CROW_ROUTE(app, "/api/v1/work").methods(crow::HTTPMethod::POST)
        ([&service](const crow::request& req) {
            return service.handleWork(req);
        });

        CROW_ROUTE(app, "/api/v1/heartbeat").methods(crow::HTTPMethod::POST)
        ([&service](const crow::request& req) {
            return service.handleHeartbeat(req);
        });

        CROW_ROUTE(app, "/api/v1/submit").methods(crow::HTTPMethod::POST)
        ([&service](const crow::request& req) {
            return service.handleSubmit(req);
        });

        // ── Auth API (public per AC23; fails closed with 503 when unconfigured) ──

        CROW_ROUTE(app, "/api/v1/auth/github/login").methods(crow::HTTPMethod::GET)
        ([&authService](const crow::request& req) {
            return authService.handleGithubLogin(req);
        });

        CROW_ROUTE(app, "/api/v1/auth/github/callback").methods(crow::HTTPMethod::GET)
        ([&authService](const crow::request& req) {
            return authService.handleGithubCallback(req);
        });

        CROW_ROUTE(app, "/api/v1/auth/logout").methods(crow::HTTPMethod::POST)
        ([&authService](const crow::request& req) {
            return authService.handleLogout(req);
        });

        CROW_ROUTE(app, "/api/v1/auth/me").methods(crow::HTTPMethod::GET)
        ([&authService](const crow::request& req) {
            return authService.handleMe(req);
        });

        // The authorization decision lives in puzzpool_core so every allow and
        // deny path is unit-tested; this lambda only wires it to the routes.
        auto adminGuard = [&cfg](const crow::request& req) {
            return puzzpool::adminGuard(cfg, req);
        };

        CROW_ROUTE(app, "/api/v1/admin/activate-puzzle").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(req)) return std::move(*denied);
            return service.handleActivatePuzzle(req);
        });

        CROW_ROUTE(app, "/api/v1/admin/set-puzzle").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(req)) return std::move(*denied);
            return service.handleSetPuzzle(req);
        });

        CROW_ROUTE(app, "/api/v1/admin/set-test-chunk").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(req)) return std::move(*denied);
            return service.handleSetTestChunk(req);
        });

        CROW_ROUTE(app, "/api/v1/admin/puzzles").methods(crow::HTTPMethod::GET)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(req)) return std::move(*denied);
            return service.handleAdminPuzzles();
        });

        CROW_ROUTE(app, "/api/v1/admin/reclaim").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(req)) return std::move(*denied);
            return service.handleAdminReclaim();
        });

        CROW_ROUTE(app, "/api/v1/admin/import-ranges").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(req)) return std::move(*denied);
            return service.handleImportRanges(req);
        });

        std::thread reclaimer([&service] {
            using namespace std::chrono_literals;
            for (;;) {
                std::this_thread::sleep_for(60s);
                try { service.reclaimTimedOutChunks(); } catch (...) {}
            }
        });
        reclaimer.detach();

        std::thread statusRefresher([&service, &cfg] {
            const auto interval = std::chrono::seconds(std::max(30, cfg.blockExplorerPollSec));
            for (;;) {
                std::this_thread::sleep_for(interval);
                try { service.refreshPuzzleStatuses(); } catch (...) {}
            }
        });
        statusRefresher.detach();

        std::cout << "[puzzpool-cpp] server running on http://127.0.0.1:" << cfg.port << "\n";
        std::cout << "[puzzpool-cpp] database: " << cfg.dbPath << "\n";
        std::cout << "[puzzpool-cpp] admin token auth: "
                  << (cfg.adminToken.empty() ? "disabled" : "enabled") << "\n";
        std::cout << "[puzzpool-cpp] admin GitHub allow-list: "
                  << (cfg.adminGithubUsers.empty()
                          ? std::string("empty")
                          : std::to_string(cfg.adminGithubUsers.size()) + " login(s)")
                  << "\n";
        // Names variables, never values (AC10, AC13, AC22).
        for (const auto& diagnostic : puzzpool::startupAuthDiagnostics(cfg)) {
            std::cerr << "[puzzpool-cpp] " << diagnostic << "\n";
        }

        app.port(static_cast<uint16_t>(cfg.port)).bindaddr("127.0.0.1").multithreaded().run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
