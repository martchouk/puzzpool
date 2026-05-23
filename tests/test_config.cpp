#include <puzzpool/config.hpp>
#include <puzzpool/env.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>

#ifdef _WIN32
#include <stdlib.h>
#else
#include <cstdlib>
#endif

using namespace puzzpool;

namespace {

void unsetEnvVar(const char* key) {
#ifdef _WIN32
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}

} // namespace

TEST_CASE("loadConfigFromEnv discovers KEYSPACE entries from process environment", "[config]") {
    unsetEnvVar("KEYSPACE_PROCESS_ONLY");
    setEnvVar("KEYSPACE_PROCESS_ONLY", "0x1:0x10", true);

    Config cfg = loadConfigFromEnv();

    REQUIRE(cfg.keyspaces.contains("PROCESS ONLY"));
    CHECK(cfg.keyspaces.at("PROCESS ONLY").first == "0x1");
    CHECK(cfg.keyspaces.at("PROCESS ONLY").second == "0x10");

    unsetEnvVar("KEYSPACE_PROCESS_ONLY");
}

TEST_CASE("process environment KEYSPACE entries override .env entries", "[config]") {
    unsetEnvVar("KEYSPACE_PROCESS_OVERRIDE");
    {
        std::ofstream env(".env");
        env << "KEYSPACE_PROCESS_OVERRIDE=0x1:0x2\n";
    }
    setEnvVar("KEYSPACE_PROCESS_OVERRIDE", "0x3:0x5", true);

    Config cfg = loadConfigFromEnv();

    REQUIRE(cfg.keyspaces.contains("PROCESS OVERRIDE"));
    CHECK(cfg.keyspaces.at("PROCESS OVERRIDE").first == "0x3");
    CHECK(cfg.keyspaces.at("PROCESS OVERRIDE").second == "0x5");

    unsetEnvVar("KEYSPACE_PROCESS_OVERRIDE");
    std::remove(".env");
}

TEST_CASE("loadConfigFromEnv parses block explorer settings and puzzle status targets", "[config]") {
    unsetEnvVar("BLOCKEXPLORER_API");
    unsetEnvVar("BLOCKEXPLORER_URL");
    unsetEnvVar("BLOCKEXPLORER_POLL_SEC");
    unsetEnvVar("PUZZLE_71_TARGET");
    unsetEnvVar("PUZZLE_ALL_BTC_TARGET");
    {
        std::ofstream env(".env");
        env << "BLOCKEXPLORER_API=https://mempool.space/api/address/\n";
        env << "BLOCKEXPLORER_URL=https://mempool.space/address/\n";
        env << "BLOCKEXPLORER_POLL_SEC=123\n";
        env << "PUZZLE_71_TARGET=1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU\n";
        env << "PUZZLE_ALL_BTC_TARGET=5\n";
    }

    Config cfg = loadConfigFromEnv();

    CHECK(cfg.blockExplorerApi == "https://mempool.space/api/address/");
    CHECK(cfg.blockExplorerUrl == "https://mempool.space/address/");
    CHECK(cfg.blockExplorerPollSec == 123);
    REQUIRE(cfg.puzzleStatusTargets.contains("PUZZLE 71"));
    CHECK(cfg.puzzleStatusTargets.at("PUZZLE 71").type == PuzzleStatusTargetType::Address);
    CHECK(cfg.puzzleStatusTargets.at("PUZZLE 71").value == "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU");
    REQUIRE(cfg.puzzleStatusTargets.contains("ALL BTC"));
    CHECK(cfg.puzzleStatusTargets.at("ALL BTC").type == PuzzleStatusTargetType::FindingsThreshold);
    CHECK(cfg.puzzleStatusTargets.at("ALL BTC").value == "5");

    std::remove(".env");
}
