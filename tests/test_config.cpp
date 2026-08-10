#include <puzzpool/admin_auth.hpp>
#include <puzzpool/config.hpp>
#include <puzzpool/env.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

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

std::string join(const std::vector<std::string>& lines, const std::string& sep) {
    std::string out;
    for (const auto& line : lines) {
        if (!out.empty()) out += sep;
        out += line;
    }
    return out;
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

// ── GitHub sign-in configuration (slice A of story #149) ─────────────────────

TEST_CASE("ADMIN_GITHUB_USERS parses, trims, and lowercases", "[config][auth]") {
    setEnvVar("ADMIN_GITHUB_USERS", "  Alice , BOB ,,carol  ", true);
    Config cfg = loadConfigFromEnv();
    CHECK(cfg.adminGithubUsers == std::vector<std::string>{"alice", "bob", "carol"});
    unsetEnvVar("ADMIN_GITHUB_USERS");
}

TEST_CASE("an empty ADMIN_GITHUB_USERS grants nobody access", "[config][auth]") {
    setEnvVar("ADMIN_GITHUB_USERS", "   ,  , ", true);
    Config cfg = loadConfigFromEnv();
    CHECK(cfg.adminGithubUsers.empty());
    CHECK_FALSE(isAllowedAdminLogin(cfg, "alice"));
    unsetEnvVar("ADMIN_GITHUB_USERS");
}

TEST_CASE("allow-list matching is case-insensitive (AC6)", "[config][auth]") {
    setEnvVar("ADMIN_GITHUB_USERS", "Alice", true);
    Config cfg = loadConfigFromEnv();
    CHECK(isAllowedAdminLogin(cfg, "ALICE"));
    CHECK(isAllowedAdminLogin(cfg, "alice"));
    CHECK(isAllowedAdminLogin(cfg, "AlIcE"));
    CHECK_FALSE(isAllowedAdminLogin(cfg, "alicee"));
    CHECK_FALSE(isAllowedAdminLogin(cfg, "alic"));
    CHECK_FALSE(isAllowedAdminLogin(cfg, ""));
    unsetEnvVar("ADMIN_GITHUB_USERS");
}

TEST_CASE("SESSION_TTL_MINUTES is clamped at both ends", "[config][auth]") {
    setEnvVar("SESSION_TTL_MINUTES", "7200000", true);
    CHECK(loadConfigFromEnv().sessionTtlMinutes == 43200);   // 30 days
    setEnvVar("SESSION_TTL_MINUTES", "-5", true);
    CHECK(loadConfigFromEnv().sessionTtlMinutes == 1);
    setEnvVar("SESSION_TTL_MINUTES", "60", true);
    CHECK(loadConfigFromEnv().sessionTtlMinutes == 60);
    unsetEnvVar("SESSION_TTL_MINUTES");
    CHECK(loadConfigFromEnv().sessionTtlMinutes == 720);
}

TEST_CASE("PUBLIC_BASE_URL loses its trailing slash", "[config][auth]") {
    setEnvVar("PUBLIC_BASE_URL", "https://puzzle.b58.de/", true);
    CHECK(loadConfigFromEnv().publicBaseUrl == "https://puzzle.b58.de");
    unsetEnvVar("PUBLIC_BASE_URL");
}

// AC20: credentials come from the stage's own environment; no Config::stage branch.
TEST_CASE("OAuth credentials do not vary with STAGE", "[config][auth]") {
    setEnvVar("GITHUB_OAUTH_CLIENT_ID", "id-from-env", true);
    setEnvVar("GITHUB_OAUTH_CLIENT_SECRET", "secret-from-env", true);
    setEnvVar("STAGE", "TEST", true);
    Config testStage = loadConfigFromEnv();
    setEnvVar("STAGE", "PROD", true);
    Config prodStage = loadConfigFromEnv();

    CHECK(testStage.githubOauthClientId == prodStage.githubOauthClientId);
    CHECK(testStage.githubOauthClientSecret == prodStage.githubOauthClientSecret);
    CHECK(prodStage.githubOauthClientId == "id-from-env");

    unsetEnvVar("GITHUB_OAUTH_CLIENT_ID");
    unsetEnvVar("GITHUB_OAUTH_CLIENT_SECRET");
    unsetEnvVar("STAGE");
}

// AC10 + AC13: actionable, non-secret startup diagnostics.
TEST_CASE("startup diagnostics warn when no admin mechanism is configured", "[config][auth]") {
    Config cfg;  // no adminToken, no allow-list, no signing secret
    const auto lines = startupAuthDiagnostics(cfg);
    REQUIRE(lines.size() >= 2);
    const std::string joined = join(lines, "\n");
    CHECK(joined.find("ADMIN_TOKEN") != std::string::npos);
    CHECK(joined.find("ADMIN_GITHUB_USERS") != std::string::npos);
    CHECK(joined.find("SESSION_SIGNING_SECRET") != std::string::npos);
}

TEST_CASE("startup diagnostics report a half-configured OAuth app", "[config][auth]") {
    Config cfg;
    cfg.adminToken = "t";
    cfg.sessionSigningSecret = "s";
    cfg.githubOauthClientId = "id";   // secret and base URL still missing
    const std::string joined = join(startupAuthDiagnostics(cfg), "\n");
    CHECK(joined.find("GITHUB_OAUTH_CLIENT_SECRET") != std::string::npos);
    CHECK(joined.find("PUBLIC_BASE_URL") != std::string::npos);
}

// Sensitivity: the diagnostic must disappear once a mechanism is configured, so the
// assertions above are detecting configuration state and not a hard-coded banner.
TEST_CASE("startup diagnostics stay quiet when auth is configured", "[config][auth]") {
    Config cfg;
    cfg.adminToken = "t";
    cfg.adminGithubUsers = {"alice"};
    cfg.sessionSigningSecret = "s";
    cfg.publicBaseUrl = "https://puzzle.b58.de";
    cfg.githubOauthClientId = "id";
    cfg.githubOauthClientSecret = "secret";
    CHECK(startupAuthDiagnostics(cfg).empty());
}

// AC22: a diagnostic must never carry a secret value.
TEST_CASE("startup diagnostics never contain secret values", "[config][auth][security]") {
    Config cfg;
    cfg.adminToken = "super-secret-token";
    cfg.sessionSigningSecret = "super-secret-signing-key";
    cfg.githubOauthClientSecret = "super-secret-oauth";
    const auto lines = startupAuthDiagnostics(cfg);
    for (const auto& line : lines) {
        CHECK(line.find("super-secret-token") == std::string::npos);
        CHECK(line.find("super-secret-signing-key") == std::string::npos);
        CHECK(line.find("super-secret-oauth") == std::string::npos);
    }
}
