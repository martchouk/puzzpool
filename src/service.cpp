#include <puzzpool/service.hpp>

#include <string>

namespace puzzpool {

PoolService::PoolService(const Config& cfg)
    : cfg_(cfg), db_(cfg), allocator_(db_), ws_(db_, allocator_), ss_(db_) {
    seedConfiguredKeyspaces();
    ensureSingleActivePuzzle();
    ensureAllocators();
}

void PoolService::reclaimTimedOutChunks() {
    std::unique_lock lock(mu_);
    ws_.reclaimTimedOutChunks();
}

} // namespace puzzpool
