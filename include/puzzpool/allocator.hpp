#pragma once

#include <puzzpool/db.hpp>
#include <puzzpool/types.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace puzzpool {

class Allocator {
public:
    explicit Allocator(PoolDb& db);

    struct WorkAssignResult {
        int64_t     chunkId = 0;
        std::string startHex;
        std::string endHex;
    };

    void    ensureAllocatorForPuzzle(int64_t puzzleId);
    void    loadBlockedRanges(int64_t puzzleId);
    int     insertOrMergeBlockedRange(int64_t puzzleId, const cpp_int& vStart, const cpp_int& vEnd, const std::string& source);

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
    void                setAllocCursor(int64_t puzzleId, const cpp_int& cursor);
    void                advanceBootstrapStage(int64_t puzzleId, int stage);
    bool                rangeIsFree(int64_t puzzleId, const cpp_int& start, const cpp_int& endExclusive);
    bool                overlapsBlockedInMemory(int64_t puzzleId, const cpp_int& start, const cpp_int& endExclusive);
    cpp_int             normalizeRunStartForCandidate(const cpp_int& candidateIndex, const cpp_int& neededChunks, const cpp_int& totalChunks);

    std::optional<WorkAssignResult> assignBootstrap(const std::string& worker, const PuzzleRow& puzzle,
                                                    const cpp_int& totalChunks, const cpp_int& neededChunks, int stage);
    std::optional<cpp_int> findBeginBootstrapRun(int64_t puzzleId, const cpp_int& totalChunks, const cpp_int& neededChunks);
    std::optional<cpp_int> findEndBootstrapRun(int64_t puzzleId, const cpp_int& totalChunks, const cpp_int& neededChunks);
    std::optional<cpp_int> findMidBootstrapRun(int64_t puzzleId, const cpp_int& totalChunks, const cpp_int& neededChunks);

    WorkAssignResult assignVirtualChunkRun(const std::string& worker, const PuzzleRow& puzzle,
                                           const cpp_int& runStart, const cpp_int& runCount, const std::string& generation);

    std::pair<std::string, std::string> virtualChunkRangeToHex(const PuzzleRow& puzzle,
                                                                const cpp_int& start, const cpp_int& endExclusive);

    PoolDb&       db_;
    const Config& cfg_;
    std::map<int64_t, std::vector<std::pair<cpp_int, cpp_int>>> blockedRanges_;
};

} // namespace puzzpool
