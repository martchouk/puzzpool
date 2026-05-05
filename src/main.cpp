#include <puzzpool/config.hpp>
#include <puzzpool/service.hpp>

#include <crow.h>
#include <nlohmann/json.hpp>

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
                r.body = "public/index.html not found";
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

        auto adminGuard = [&cfg](const crow::request& req) -> std::optional<crow::response> {
            if (cfg.adminToken.empty()) return std::nullopt;
            auto token = req.get_header_value("X-Admin-Token");
            if (token == cfg.adminToken) return std::nullopt;
            crow::response r;
            r.code = 401;
            r.set_header("Content-Type", "application/json");
            r.body = nlohmann::json({{"error", "unauthorized"}}).dump();
            return r;
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

        std::cout << "[puzzpool-cpp] server running on http://127.0.0.1:" << cfg.port << "\n";
        std::cout << "[puzzpool-cpp] database: " << cfg.dbPath << "\n";
        if (!cfg.adminToken.empty()) std::cout << "[puzzpool-cpp] admin token auth: enabled\n";

        app.port(static_cast<uint16_t>(cfg.port)).bindaddr("127.0.0.1").multithreaded().run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
