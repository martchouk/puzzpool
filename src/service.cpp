#include <puzzpool/service.hpp>

#include <string>

namespace puzzpool {

PoolService::PoolService(const Config& cfg, AddressStatusFetcher fetcher, bool refreshStatusesOnInit)
    : cfg_(cfg), db_(cfg), allocator_(db_), ws_(db_, allocator_), ss_(db_),
      fetchAddressStatus_(fetcher ? std::move(fetcher) : AddressStatusFetcher(fetchAddressStatusJson)) {
    seedConfiguredKeyspaces();
    ensureSingleActivePuzzle();
    ensureAllocators();
    if (refreshStatusesOnInit) refreshPuzzleStatuses();
}

int PoolService::reclaimTimedOutChunks() {
    std::unique_lock lock(mu_);
    return ws_.reclaimTimedOutChunks();
}

} // namespace puzzpool
