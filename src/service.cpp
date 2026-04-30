#include <puzzpool/service.hpp>

#include <string>

namespace puzzpool {

PoolService::PoolService(const Config& cfg)
    : cfg_(cfg), db_(cfg), allocator_(db_) {
    seedConfiguredKeyspaces();
    ensureSingleActivePuzzle();
    ensureAllocators();
}

void PoolService::reclaimTimedOutChunks() {
    std::unique_lock lock(mu_);
    SQLite::Statement q(db_.raw(), R"SQL(
        UPDATE chunks
        SET status = 'reclaimed',
            prev_worker_name = worker_name,
            worker_name = NULL,
            assigned_at = NULL,
            heartbeat_at = NULL
        WHERE status = 'assigned'
          AND COALESCE(heartbeat_at, assigned_at) < datetime('now', ?)
    )SQL");
    q.bind(1, "-" + std::to_string(cfg_.timeoutMinutes) + " minutes");
    q.exec();
}

} // namespace puzzpool
