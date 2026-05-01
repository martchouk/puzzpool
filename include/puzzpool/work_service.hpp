#pragma once

#include <puzzpool/allocator.hpp>
#include <puzzpool/db.hpp>
#include <puzzpool/types.hpp>

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <optional>
#include <string>

namespace puzzpool {

class WorkService {
public:
    WorkService(PoolDb& db, Allocator& allocator);

    struct AssignResult {
        bool ok = true;
        int  errorCode = 0;
        std::string error;
        int64_t     jobId = 0;
        std::string startHex;
        std::string endHex;
    };

    AssignResult assignWork(
        const std::string&              name,
        std::optional<double>           hashrate,
        const std::string&              version,
        std::optional<std::string>      minChunkKeys,
        std::optional<std::string>      chunkQuantumKeys);

    bool heartbeat(const std::string& name, int64_t jobId);

    int reclaimTimedOutChunks();

    // Returns true if this is the worker's first /work call after being idle
    // longer than reactivate_minutes (stale assigned chunks are reclaimed before
    // returning true so callers can skip reclaim-first assignment logic).
    bool upsertWorkerAndDetectReactivation(
        const std::string&              name,
        double                          hashrate,
        const std::string&              version,
        const std::optional<std::string>& minChunkKeys,
        const std::optional<std::string>& chunkQuantumKeys);

    std::optional<ChunkRow> existingAssignedChunk(const std::string& name, int64_t puzzleId);
    std::optional<ChunkRow> reclaimChunk(const std::string& name, int64_t puzzleId);
    std::optional<ChunkRow> claimTestChunk(const std::string& name, const PuzzleRow& puzzle);
    ChunkRow                readChunk(SQLite::Statement& q);

private:
    PoolDb&    db_;
    Allocator& allocator_;
};

} // namespace puzzpool
