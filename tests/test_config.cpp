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
