#pragma once

#include <boost/multiprecision/cpp_int.hpp>

#include <cstdint>
#include <string>

namespace puzzpool {

using cpp_int = boost::multiprecision::cpp_int;

struct RangeNorm {
    cpp_int start;
    cpp_int end;
    cpp_int range;
};

struct AffineParams {
    cpp_int a;
    cpp_int b;
};

struct PuzzleRow {
    int64_t id = 0;
    std::string name;
    std::string startHex;
    std::string endHex;
    int active = 0;
    std::string testStartHex;
    std::string testEndHex;
    std::string allocStrategy;
    std::string allocSeed;
    cpp_int allocCursor = 0;
    std::string virtualChunkSizeKeys;
    cpp_int virtualChunkCount = 0;
    int bootstrapStage = 0;
};

struct ChunkRow {
    int64_t id = 0;
    int64_t puzzleId = 0;
    std::string startHex;
    std::string endHex;
    std::string status;
    std::string workerName;
    std::string prevWorkerName;
    std::string assignedAt;
    std::string heartbeatAt;
    int isTest = 0;
    int64_t sectorId = 0;
    int64_t vchunkStart = -1;
    int64_t vchunkEnd = -1;
    std::string allocGeneration;
};

} // namespace puzzpool
