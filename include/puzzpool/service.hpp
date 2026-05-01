#pragma once

#include <puzzpool/allocator.hpp>
#include <puzzpool/config.hpp>
#include <puzzpool/db.hpp>
#include <puzzpool/submission_service.hpp>
#include <puzzpool/types.hpp>
#include <puzzpool/work_service.hpp>

#include <crow.h>
#include <nlohmann/json.hpp>

#include <mutex>
#include <shared_mutex>
#include <string>

namespace puzzpool {

// PoolService is the HTTP adapter layer. It parses requests, holds the mutex,
// delegates business logic to WorkService / SubmissionService / Allocator,
// and serialises responses. It owns no domain logic directly.
class PoolService {
public:
    explicit PoolService(const Config& cfg);

    crow::response handleStats(const crow::request& req);
    crow::response handleWork(const crow::request& req);
    crow::response handleHeartbeat(const crow::request& req);
    crow::response handleSubmit(const crow::request& req);
    crow::response handleActivatePuzzle(const crow::request& req);
    crow::response handleSetPuzzle(const crow::request& req);
    crow::response handleSetTestChunk(const crow::request& req);
    crow::response handleAdminPuzzles();

    void reclaimTimedOutChunks();

    const Config& cfg() const { return cfg_; }

private:
    void seedConfiguredKeyspaces();
    void ensureSingleActivePuzzle();
    void ensureAllocators();

    nlohmann::json buildStats(const PuzzleRow& puzzle);
    nlohmann::json puzzleJson(const PuzzleRow& p);

    static std::string    formatDouble(double v);
    static std::string    nowIsoUtc();
    static crow::response jsonResponse(const nlohmann::json& j, int code = 200);
    static crow::response errorJsonResponse(int code, const nlohmann::json& j);
    static crow::response errorResponse(int code, const std::string& msg);

    const Config       cfg_;
    PoolDb             db_;
    Allocator          allocator_;
    WorkService        ws_;
    SubmissionService  ss_;
    std::shared_mutex  mu_;
};

} // namespace puzzpool
