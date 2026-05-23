#include "test_helpers.hpp"

#include <puzzpool/puzzle_status.hpp>
#include <puzzpool/service.hpp>

#include <catch2/catch_test_macros.hpp>
#include <crow.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

using namespace puzzpool;
using namespace puzzpool::test;
using json = nlohmann::json;

TEST_CASE("bitcoin address validation accepts known-valid legacy address", "[puzzle-status]") {
    CHECK(isValidBitcoinAddress("1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"));
}

TEST_CASE("bitcoin address validation rejects malformed address", "[puzzle-status]") {
    CHECK_FALSE(isValidBitcoinAddress("not-a-bitcoin-address"));
}

TEST_CASE("address target status is unsolved when funded and unspent", "[puzzle-status]") {
    json response = {
        {"address", "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"},
        {"chain_stats", {
            {"funded_txo_count", 28},
            {"funded_txo_sum", 710057266},
            {"spent_txo_count", 0},
            {"spent_txo_sum", 0},
            {"tx_count", 28}
        }},
        {"mempool_stats", {
            {"funded_txo_count", 0},
            {"funded_txo_sum", 0},
            {"spent_txo_count", 0},
            {"spent_txo_sum", 0},
            {"tx_count", 0}
        }}
    };

    CHECK(evaluateAddressTargetStatus(response) == PuzzleStatusState::Unsolved);
}

TEST_CASE("address target status is solved when any spend exists", "[puzzle-status]") {
    json response = {
        {"chain_stats", {
            {"funded_txo_sum", 1000},
            {"spent_txo_count", 1},
            {"spent_txo_sum", 500}
        }},
        {"mempool_stats", {
            {"funded_txo_sum", 0},
            {"spent_txo_count", 0},
            {"spent_txo_sum", 0}
        }}
    };

    CHECK(evaluateAddressTargetStatus(response) == PuzzleStatusState::Solved);
}

TEST_CASE("address target status is unknown when never funded", "[puzzle-status]") {
    json response = {
        {"chain_stats", {
            {"funded_txo_sum", 0},
            {"spent_txo_count", 0},
            {"spent_txo_sum", 0}
        }},
        {"mempool_stats", {
            {"funded_txo_sum", 0},
            {"spent_txo_count", 0},
            {"spent_txo_sum", 0}
        }}
    };

    CHECK(evaluateAddressTargetStatus(response) == PuzzleStatusState::Unknown);
}

TEST_CASE("findings threshold status uses distinct found keys", "[puzzle-status]") {
    CHECK(evaluateFindingsThresholdStatus(4, 5) == PuzzleStatusState::Unsolved);
    CHECK(evaluateFindingsThresholdStatus(5, 5) == PuzzleStatusState::Solved);
}

TEST_CASE("handleStats serializes cached puzzle status", "[puzzle-status][stats]") {
    Config cfg = memConfig();
    cfg.dbPath = "test-puzzle-status.db";
    PoolService svc{cfg};

    SQLite::Database db(cfg.dbPath, SQLite::OPEN_READWRITE);
    db.exec("UPDATE puzzles SET status_target_type = 'address', status_target_value = '1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU', status_state = 'unsolved', status_checked_at = '2026-05-23T10:00:00Z', status_link = 'https://mempool.space/address/1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU'");

    crow::request req;
    auto resp = svc.handleStats(req);
    REQUIRE(resp.code == 200);
    auto body = json::parse(resp.body);
    REQUIRE(body["puzzle"]["status"].is_object());
    CHECK(body["puzzle"]["status"]["state"] == "unsolved");
    CHECK(body["puzzle"]["status"]["target_type"] == "address");
    CHECK(body["puzzle"]["status"]["link"] == "https://mempool.space/address/1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU");

    std::remove("test-puzzle-status.db");
}
