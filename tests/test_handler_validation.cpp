#include "test_helpers.hpp"

#include <puzzpool/service.hpp>

#include <catch2/catch_test_macros.hpp>
#include <crow.h>
#include <nlohmann/json.hpp>

#include <string>

using namespace puzzpool;
using namespace puzzpool::test;
using json = nlohmann::json;

static crow::request body(const std::string& s) {
    crow::request req;
    req.body = s;
    return req;
}

// PoolService with memConfig auto-seeds an active puzzle on construction.

// ── handleSubmit validation ───────────────────────────────────────────────────

TEST_CASE("handleSubmit: job_id as string returns 400", "[handler][validation]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSubmit(body(R"({"name":"t","job_id":"bad","status":"done","keys_scanned":1})"));
    CHECK(resp.code == 400);
}

TEST_CASE("handleSubmit: job_id > INT64_MAX returns 400", "[handler][validation]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSubmit(body(R"({"name":"t","job_id":9223372036854775808,"status":"done","keys_scanned":1})"));
    CHECK(resp.code == 400);
}

TEST_CASE("handleSubmit: missing job_id returns 400", "[handler][validation]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSubmit(body(R"({"name":"t","status":"done","keys_scanned":1})"));
    CHECK(resp.code == 400);
}

TEST_CASE("handleSubmit: null status returns 400", "[handler][validation]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSubmit(body(R"({"name":"t","job_id":1,"status":null})"));
    CHECK(resp.code == 400);
}

TEST_CASE("handleSubmit: non-string found_key returns 400", "[handler][validation]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSubmit(body(R"({"name":"t","job_id":1,"status":"FOUND","findings":[{"found_key":123}]})"));
    CHECK(resp.code == 400);
}

// ── handleSetTestChunk ────────────────────────────────────────────────────────

TEST_CASE("handleSetTestChunk: null start_hex clears chunk (not 500)", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetTestChunk(body(R"({"start_hex":null})"));
    CHECK(resp.code == 200);
    auto b = json::parse(resp.body);
    CHECK(b["ok"] == true);
    CHECK(b["test_chunk"].is_null());
}

TEST_CASE("handleSetTestChunk: missing start_hex clears chunk", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetTestChunk(body("{}"));
    CHECK(resp.code == 200);
}

TEST_CASE("handleSetTestChunk: null end_hex does not crash", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetTestChunk(body(R"({"start_hex":"0000000000000001","end_hex":null})"));
    CHECK(resp.code == 200);
}

// ── handleSetPuzzle validation ────────────────────────────────────────────────

TEST_CASE("handleSetPuzzle: virtual_chunk_size_keys '0' returns 400", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetPuzzle(body(
        R"({"name":"x","start_hex":"1","end_hex":"ffff",)"
        R"("alloc_strategy":"virtual_random_chunks_v1","virtual_chunk_size_keys":"0"})"));
    CHECK(resp.code == 400);
}

TEST_CASE("handleSetPuzzle: virtual_chunk_size_keys '000' returns 400", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetPuzzle(body(
        R"({"name":"x","start_hex":"1","end_hex":"ffff",)"
        R"("alloc_strategy":"virtual_random_chunks_v1","virtual_chunk_size_keys":"000"})"));
    CHECK(resp.code == 400);
}

TEST_CASE("handleSetPuzzle: non-string virtual_chunk_size_keys returns 400", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetPuzzle(body(
        R"({"name":"x","start_hex":"1","end_hex":"ffff",)"
        R"("alloc_strategy":"virtual_random_chunks_v1","virtual_chunk_size_keys":100})"));
    CHECK(resp.code == 400);
}

TEST_CASE("handleSetPuzzle: valid legacy puzzle creation returns 200", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetPuzzle(body(
        R"({"name":"legacypuzzle","start_hex":"1","end_hex":"ffff",)"
        R"("alloc_strategy":"legacy_random_shards_v1"})"));
    CHECK(resp.code == 200);
    auto b = json::parse(resp.body);
    CHECK(b["ok"] == true);
    CHECK(b["puzzle"]["name"] == "legacypuzzle");
}

TEST_CASE("handleSetPuzzle: valid vchunk puzzle creation returns 200", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetPuzzle(body(
        R"({"name":"vp","start_hex":"1","end_hex":"186a0",)"
        R"("alloc_strategy":"virtual_random_chunks_v1","virtual_chunk_size_keys":"100"})"));
    CHECK(resp.code == 200);
    auto b = json::parse(resp.body);
    CHECK(b["ok"] == true);
    CHECK(b["puzzle"]["name"] == "vp");
}

TEST_CASE("handleSetPuzzle: missing name returns 400", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetPuzzle(body(R"({"start_hex":"1","end_hex":"ffff"})"));
    CHECK(resp.code == 400);
}

TEST_CASE("handleSetPuzzle: end_hex <= start_hex returns 400", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetPuzzle(body(R"({"name":"bad","start_hex":"ffff","end_hex":"1"})"));
    CHECK(resp.code == 400);
}

// ── handleAdminReclaim ────────────────────────────────────────────────────────

TEST_CASE("handleAdminReclaim: returns 200 with ok and reclaimed count", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleAdminReclaim();
    CHECK(resp.code == 200);
    auto b = json::parse(resp.body);
    CHECK(b["ok"] == true);
    CHECK(b["reclaimed"].is_number());
}

// ── handleAdminPuzzles ────────────────────────────────────────────────────────

TEST_CASE("handleAdminPuzzles: returns 200 with non-empty puzzle array", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleAdminPuzzles();
    CHECK(resp.code == 200);
    auto b = json::parse(resp.body);
    REQUIRE(b["puzzles"].is_array());
    CHECK(b["puzzles"].size() >= 1);
}

TEST_CASE("handleSetPuzzle: All BTC keyspace returns 200 (not 400)", "[handler][admin]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    auto resp = svc.handleSetPuzzle(body(
        R"({"name":"allbtc",)"
        R"("start_hex":"0000000000000000000000000000000000000000000000000000000000000001",)"
        R"("end_hex":"fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141",)"
        R"("alloc_strategy":"virtual_random_chunks_v1","virtual_chunk_size_keys":"33554432"})"));
    CHECK(resp.code == 200);
    auto b = json::parse(resp.body);
    CHECK(b["ok"] == true);
    CHECK(b["puzzle"]["name"] == "allbtc");
    // virtual_chunk_count should be a string (decimal) since it exceeds int64
    CHECK(b["puzzle"]["virtual_chunk_count"].is_string());
}

// ── handleStats puzzle_id validation ─────────────────────────────────────────

TEST_CASE("handleStats: puzzle_id=abc returns 400", "[handler][validation]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    crow::request req;
    req.url_params = crow::query_string("puzzle_id=abc", false);
    CHECK(svc.handleStats(req).code == 400);
}

TEST_CASE("handleStats: puzzle_id=123abc returns 400", "[handler][validation]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    crow::request req;
    req.url_params = crow::query_string("puzzle_id=123abc", false);
    CHECK(svc.handleStats(req).code == 400);
}

TEST_CASE("handleStats: no puzzle_id returns 200", "[handler][validation]") {
    Config cfg = memConfig(); PoolService svc{cfg};
    crow::request req;
    CHECK(svc.handleStats(req).code == 200);
}
