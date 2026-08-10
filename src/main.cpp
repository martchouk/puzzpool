#include <puzzpool/admin_auth.hpp>
#include <puzzpool/admin_guard.hpp>
#include <puzzpool/auth_service.hpp>
#include <puzzpool/config.hpp>
#include <puzzpool/service.hpp>

#include <crow.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

int main() {
    try {
        puzzpool::Config cfg = puzzpool::loadConfigFromEnv();
        puzzpool::PoolService service(cfg);

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

        // /api/v1/auth/* is intentionally public (AC23): these routes are how an
        // anonymous browser signs in, so they cannot sit behind the admin guard.
        puzzpool::AuthService auth(cfg);

        CROW_ROUTE(app, "/api/v1/auth/github/login").methods(crow::HTTPMethod::GET)
        ([&auth](const crow::request& req) { return auth.handleGithubLogin(req); });

        CROW_ROUTE(app, "/api/v1/auth/github/callback").methods(crow::HTTPMethod::GET)
        ([&auth](const crow::request& req) { return auth.handleGithubCallback(req); });

        CROW_ROUTE(app, "/api/v1/auth/logout").methods(crow::HTTPMethod::POST)
        ([&auth](const crow::request& req) { return auth.handleLogout(req); });

        CROW_ROUTE(app, "/api/v1/auth/me").methods(crow::HTTPMethod::GET)
        ([&auth](const crow::request& req) { return auth.handleAuthMe(req); });

        // Every /api/v1/admin/* route goes through the one guard in puzzpool_core.
        // It is default-deny: with neither ADMIN_TOKEN nor a usable GitHub session
        // configured, all six routes below return 401 (AC9). Adding a route here
        // without this line makes it public — tests/test_admin_routes_guarded.sh is
        // the regression that checks all six, because main.cpp is outside every
        // Catch2 target.

        CROW_ROUTE(app, "/api/v1/admin/activate-puzzle").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(cfg, req)) return std::move(*denied);
            return service.handleActivatePuzzle(req);
        });

        CROW_ROUTE(app, "/api/v1/admin/set-puzzle").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(cfg, req)) return std::move(*denied);
            return service.handleSetPuzzle(req);
        });

        CROW_ROUTE(app, "/api/v1/admin/set-test-chunk").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(cfg, req)) return std::move(*denied);
            return service.handleSetTestChunk(req);
        });

        CROW_ROUTE(app, "/api/v1/admin/puzzles").methods(crow::HTTPMethod::GET)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(cfg, req)) return std::move(*denied);
            return service.handleAdminPuzzles();
        });

        CROW_ROUTE(app, "/api/v1/admin/reclaim").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(cfg, req)) return std::move(*denied);
            return service.handleAdminReclaim();
        });

        CROW_ROUTE(app, "/api/v1/admin/import-ranges").methods(crow::HTTPMethod::POST)
        ([&](const crow::request& req) {
            if (auto denied = adminGuard(cfg, req)) return std::move(*denied);
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
        if (!cfg.adminToken.empty()) std::cout << "[puzzpool-cpp] admin token auth: enabled\n";
        if (!cfg.adminGithubUsers.empty())
            std::cout << "[puzzpool-cpp] github sign-in auth: " << cfg.adminGithubUsers.size()
                      << " allow-listed login(s)\n";
        for (const auto& line : puzzpool::startupAuthDiagnostics(cfg)) {
            std::cerr << "[puzzpool-cpp] " << line << "\n";
        }

        app.port(static_cast<uint16_t>(cfg.port)).bindaddr("127.0.0.1").multithreaded().run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
