#include <puzzpool/config.hpp>
#include <puzzpool/env.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace puzzpool {

Config loadConfigFromEnv() {
    loadDotEnv(".env", false);
    auto envMap = parseDotEnvFile(".env");
    for (const auto& [key, value] : processEnvMap())
        envMap[key] = value;

    Config cfg;
    cfg.port           = getEnvInt("PORT", 8888);
    cfg.dbPath         = getEnvOr("DB_PATH", "pool.db");
    cfg.targetMinutes  = getEnvInt("TARGET_MINUTES", 10);
    cfg.timeoutMinutes = getEnvInt("TIMEOUT_MINUTES", 15);
    cfg.targetSectors  = static_cast<uint64_t>(std::max(1, getEnvInt("TARGET_SECTORS", 65536)));

    const double rawActiveMinutes = getEnvDouble("ACTIVE_MINUTES", 1.167);
    cfg.activeMinutes    = std::max(0.1, std::min(rawActiveMinutes, cfg.timeoutMinutes / 2.0));
    cfg.reactivateMinutes = cfg.timeoutMinutes;

    cfg.defaultVirtualChunkSizeKeys  = getEnvBigInt("DEFAULT_VIRTUAL_CHUNK_SIZE_KEYS", cpp_int("30000000"));
    cfg.autoReseedEmptyVChunkPuzzles = getEnvBool01("AUTO_RESEED_EMPTY_VCHUNK_PUZZLES", false);
    cfg.maxAllocProbes  = static_cast<uint64_t>(std::max(1, getEnvInt("MAX_ALLOC_PROBES", 8192)));
    cfg.permutationMode = getEnvOr("PERMUTATION_MODE", "feistel");
    cfg.stage           = getEnvOr("STAGE", "PROD");
    cfg.adminToken      = getEnvOr("ADMIN_TOKEN", "");

    for (const auto& [key, value] : envMap) {
        if (key.rfind("KEYSPACE_", 0) != 0) continue;

        const std::string rawName = key.substr(std::string("KEYSPACE_").size());
        const auto pos = value.find(':');
        if (pos == std::string::npos) continue;

        std::string prettyName = rawName;
        std::replace(prettyName.begin(), prettyName.end(), '_', ' ');

        const std::string startHex = trim(value.substr(0, pos));
        const std::string endHex   = trim(value.substr(pos + 1));

        if (!isValidHex(startHex) || !isValidHex(endHex)) continue;

        cfg.keyspaces[prettyName] = {startHex, endHex};
    }

    return cfg;
}

} // namespace puzzpool
