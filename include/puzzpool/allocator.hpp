#pragma once

#include <puzzpool/db.hpp>
#include <puzzpool/types.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace puzzpool {

class Allocator {
public:
    explicit Allocator(PoolDb& db);

    struct WorkAssignResult {
        int64_t     chunkId = 0;
        std::string startHex;
        std::string endHex;
    };

    void ensureAllocatorForPuzzle(int64_t puzzleId);

    cpp_int     chooseDefaultVirtualChunkSize(const cpp_int& range) const;
    std::string defaultAllocSeedForPuzzle(const PuzzleRow& p, const std::string& strategy) const;

    void seedSectors(int64_t puzzleId, const std::string& startHex, const std::string& endHex);
    void seedVirtualChunks(int64_t puzzleId, const std::string& startHex, const std::string& endHex,
                           const std::string& allocSeed, const cpp_int& virtualChunkSizeKeys);

    std::optional<WorkAssignResult> assignLegacyRandomChunk(
        const std::string& worker, const std::optional<double>& hashrate, const PuzzleRow& puzzle);

    std::optional<WorkAssignResult> assignVirtualChunkJob(
        const std::string& worker, const std::optional<double>& hashrate,
        const std::optional<std::string>& minChunkKeys,
        const std::optional<std::string>& chunkQuantumKeys,
        const PuzzleRow& puzzle);

    cpp_int normalizeHashrate(double v) const;
    cpp_int computeWorkerRequestedKeys(const cpp_int& hashrateBig,
                                       const cpp_int& minChunkKeys,
                                       const cpp_int& chunkQuantumKeys) const;

private:
    int64_t             readWorkerHashrate(const std::string& worker);
    void                setAllocCursor(int64_t puzzleId, int64_t cursor);
    void                advanceBootstrapStage(int64_t puzzleId, int stage);
    bool                rangeIsFree(int64_t puzzleId, int64_t start, int64_t endExclusive);
    int64_t             normalizeRunStartForCandidate(int64_t candidateIndex, int64_t neededChunks, int64_t totalChunks);

    std::optional<WorkAssignResult> assignBootstrap(const std::string& worker, const PuzzleRow& puzzle,
                                                    int64_t totalChunks, int64_t neededChunks, int stage);
    std::optional<int64_t> findBeginBootstrapRun(int64_t puzzleId, int64_t totalChunks, int64_t neededChunks);
    std::optional<int64_t> findEndBootstrapRun(int64_t puzzleId, int64_t totalChunks, int64_t neededChunks);
    std::optional<int64_t> findMidBootstrapRun(int64_t puzzleId, int64_t totalChunks, int64_t neededChunks);

    WorkAssignResult assignVirtualChunkRun(const std::string& worker, const PuzzleRow& puzzle,
                                           int64_t runStart, int64_t runCount, const std::string& generation);

    std::pair<std::string, std::string> virtualChunkRangeToHex(const PuzzleRow& puzzle,
                                                                int64_t start, int64_t endExclusive);

    PoolDb&       db_;
    const Config& cfg_;
};

} // namespace puzzpool
