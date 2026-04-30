#pragma once

#include <puzzpool/types.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace puzzpool {

struct Config {
    int         port           = 8888;
    std::string dbPath         = "pool.db";
    int         targetMinutes  = 10;
    int         timeoutMinutes = 15;
    uint64_t    targetSectors  = 65536;
    double      activeMinutes  = 1.167;
    int         reactivateMinutes = 15;

    std::string allocStrategyLegacy  = "legacy_random_shards_v1";
    std::string allocStrategyVChunks = "virtual_random_chunks_v1";
    std::string defaultAllocStrategy = "virtual_random_chunks_v1";

    cpp_int  defaultVirtualChunkSizeKeys   = cpp_int("30000000");
    bool     autoReseedEmptyVChunkPuzzles  = false;
    uint64_t maxAllocProbes                = 8192;
    std::string permutationMode = "feistel";
    std::string stage           = "PROD";
    std::string adminToken;

    cpp_int gpuBatchKeys = cpp_int("4278190080");
    std::map<std::string, std::pair<std::string, std::string>> keyspaces;
};

Config loadConfigFromEnv();

} // namespace puzzpool
