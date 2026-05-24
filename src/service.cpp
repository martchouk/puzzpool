#include <puzzpool/service.hpp>

#include <string>

namespace puzzpool {

PoolService::PoolService(const Config& cfg,
                         AddressStatusFetcher fetcher,
                         bool refreshStatusesOnInit,
                         VisualizationBuildHook buildHook)
    : cfg_(cfg), db_(cfg), allocator_(db_), ws_(db_, allocator_), ss_(db_),
      fetchAddressStatus_(fetcher ? std::move(fetcher) : AddressStatusFetcher(fetchAddressStatusJson)),
      visualizationBuildHook_(std::move(buildHook)) {
    seedConfiguredKeyspaces();
    ensureSingleActivePuzzle();
    ensureAllocators();
    if (refreshStatusesOnInit) refreshPuzzleStatuses();
}

int PoolService::reclaimTimedOutChunks() {
    std::unique_lock lock(mu_);
    const int reclaimed = ws_.reclaimTimedOutChunks();
    if (reclaimed > 0) invalidateAllVisualizationsLocked();
    return reclaimed;
}

void PoolService::invalidateVisualizationLocked(int64_t puzzleId) {
    visRevisions_[puzzleId] = visualizationRevisionLocked(puzzleId) + 1;
    visCache_.erase(puzzleId);
}

void PoolService::invalidateAllVisualizationsLocked() {
    for (auto& [puzzleId, revision] : visRevisions_) {
        revision += 1;
        visCache_.erase(puzzleId);
    }
    for (const auto& puzzle : db_.listPuzzles()) {
        const int64_t puzzleId = puzzle.at("id").get<int64_t>();
        if (!visRevisions_.contains(puzzleId)) visRevisions_[puzzleId] = 1;
        visCache_.erase(puzzleId);
    }
}

std::uint64_t PoolService::visualizationRevisionLocked(int64_t puzzleId) {
    auto it = visRevisions_.find(puzzleId);
    if (it == visRevisions_.end()) {
        it = visRevisions_.emplace(puzzleId, 1).first;
    }
    return it->second;
}

} // namespace puzzpool
