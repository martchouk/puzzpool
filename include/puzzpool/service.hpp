#pragma once

#include <puzzpool/allocator.hpp>
#include <puzzpool/config.hpp>
#include <puzzpool/db.hpp>
#include <puzzpool/submission_service.hpp>
#include <puzzpool/types.hpp>
#include <puzzpool/work_service.hpp>

#include <crow.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

namespace puzzpool {

// PoolService is the HTTP adapter layer. It parses requests, holds the mutex,
// delegates business logic to WorkService / SubmissionService / Allocator,
// and serialises responses. It owns no domain logic directly.
class PoolService {
public:
    using AddressStatusFetcher =
        std::function<std::optional<nlohmann::json>(const std::string&, const std::string&)>;

    explicit PoolService(const Config& cfg, AddressStatusFetcher fetcher = {}, bool refreshStatusesOnInit = true);

    crow::response handleStats(const crow::request& req);
    crow::response handleHeatmapVisualization(const crow::request& req);
    crow::response handleHilbertVisualization(const crow::request& req);
    crow::response handleAllocatorVisualization(const crow::request& req);
    crow::response handleWork(const crow::request& req);
    crow::response handleHeartbeat(const crow::request& req);
    crow::response handleSubmit(const crow::request& req);
    crow::response handleActivatePuzzle(const crow::request& req);
    crow::response handleSetPuzzle(const crow::request& req);
    crow::response handleSetTestChunk(const crow::request& req);
    crow::response handleAdminPuzzles();
    crow::response handleAdminReclaim();
    crow::response handleImportRanges(const crow::request& req);

    int reclaimTimedOutChunks();
    void refreshPuzzleStatuses();

    const Config& cfg() const { return cfg_; }

private:
    void seedConfiguredKeyspaces();
    void ensureSingleActivePuzzle();
    void ensureAllocators();
    void syncConfiguredPuzzleTargets();
    void invalidateVisualizationLocked(int64_t puzzleId);
    void invalidateAllVisualizationsLocked();
    std::uint64_t visualizationRevisionLocked(int64_t puzzleId);
    static std::optional<nlohmann::json> fetchAddressStatusJson(const std::string& apiBase, const std::string& address);

    nlohmann::json buildStats(const PuzzleRow& puzzle);
    nlohmann::json buildHeatmapVisualization(const PuzzleRow& puzzle);
    nlohmann::json buildHilbertVisualization(const PuzzleRow& puzzle);
    nlohmann::json buildAllocatorVisualization(const PuzzleRow& puzzle);
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
    AddressStatusFetcher fetchAddressStatus_;
    std::map<int64_t, std::uint64_t> visRevisions_;
    struct VisualizationCacheEntry {
        std::uint64_t revision = 0;
        nlohmann::json heatmap;
        nlohmann::json hilbert;
        nlohmann::json allocator;
    };
    std::map<int64_t, VisualizationCacheEntry> visCache_;
    std::shared_mutex  mu_;
};

} // namespace puzzpool
