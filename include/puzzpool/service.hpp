#pragma once

#include <puzzpool/allocator.hpp>
#include <puzzpool/config.hpp>
#include <puzzpool/db.hpp>
#include <puzzpool/types.hpp>

#include <crow.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace puzzpool {

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

    bool upsertWorkerAndDetectReactivation(
        const std::string& name, double hashrate, const std::string& version,
        const std::optional<std::string>& minChunkKeys,
        const std::optional<std::string>& chunkQuantumKeys);

    std::optional<ChunkRow> existingAssignedChunk(const std::string& name, int64_t puzzleId);
    std::optional<ChunkRow> reclaimChunk(const std::string& name, int64_t puzzleId);
    std::optional<ChunkRow> claimTestChunk(const std::string& name, const PuzzleRow& puzzle);
    ChunkRow                readChunk(SQLite::Statement& q);

    crow::response handleSubmitDone(const std::string& name, int64_t jobId, const nlohmann::json& body);
    crow::response handleSubmitFound(const std::string& name, int64_t jobId, const nlohmann::json& body);

    void clearTestChunkIfNeeded(int64_t jobId);

    nlohmann::json buildStats(const PuzzleRow& puzzle);
    nlohmann::json puzzleJson(const PuzzleRow& p);

    static std::string     formatDouble(double v);
    static std::string     nowIsoUtc();
    static crow::response  jsonResponse(const nlohmann::json& j, int code = 200);
    static crow::response  errorJsonResponse(int code, const nlohmann::json& j);
    static crow::response  errorResponse(int code, const std::string& msg);

    const Config cfg_;
    PoolDb       db_;
    Allocator    allocator_;
    std::mutex   mu_;
};

} // namespace puzzpool
