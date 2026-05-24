#include "test_helpers.hpp"

#include <puzzpool/service.hpp>

#include <catch2/catch_test_macros.hpp>
#include <crow.h>
#include <nlohmann/json.hpp>

using namespace puzzpool;
using namespace puzzpool::test;
using json = nlohmann::json;

TEST_CASE("handleStats returns vis_revision and omits legacy chunks_vis payload", "[visualization][stats]") {
    Config cfg = memConfig();
    PoolService svc{cfg};

    crow::request req;
    auto resp = svc.handleStats(req);
    REQUIRE(resp.code == 200);

    auto body = json::parse(resp.body);
    CHECK(body.contains("vis_revision"));
    CHECK_FALSE(body.contains("chunks_vis"));
}

TEST_CASE("heatmap visualization endpoint returns aggregate cell payload", "[visualization][heatmap]") {
    Config cfg = memConfig();
    PoolService svc{cfg};

    crow::request req;
    auto resp = svc.handleHeatmapVisualization(req);
    REQUIRE(resp.code == 200);

    auto body = json::parse(resp.body);
    CHECK(body.contains("puzzle_id"));
    CHECK(body.contains("loaded_at"));
    REQUIRE(body.contains("cells"));
    CHECK(body["cells"].is_array());
}

TEST_CASE("hilbert visualization endpoint returns aggregate cell payload", "[visualization][hilbert]") {
    Config cfg = memConfig();
    PoolService svc{cfg};

    crow::request req;
    auto resp = svc.handleHilbertVisualization(req);
    REQUIRE(resp.code == 200);

    auto body = json::parse(resp.body);
    CHECK(body.contains("puzzle_id"));
    CHECK(body.contains("loaded_at"));
    REQUIRE(body.contains("cells"));
    CHECK(body["cells"].is_array());
}

TEST_CASE("allocator visualization endpoint returns generation payloads", "[visualization][allocator]") {
    Config cfg = memConfig();
    PoolService svc{cfg};

    crow::request req;
    auto resp = svc.handleAllocatorVisualization(req);
    REQUIRE(resp.code == 200);

    auto body = json::parse(resp.body);
    CHECK(body.contains("puzzle_id"));
    CHECK(body.contains("loaded_at"));
    REQUIRE(body.contains("generations"));
    CHECK(body["generations"].contains("all"));
    CHECK(body["generations"].contains("legacy"));
    CHECK(body["generations"].contains("affine"));
    CHECK(body["generations"].contains("feistel"));
}
