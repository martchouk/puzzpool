#include "test_helpers.hpp"

#include <puzzpool/service.hpp>

#include <catch2/catch_test_macros.hpp>
#include <crow.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

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

TEST_CASE("vis_revision increments after work assignment and submit", "[visualization][stats][revision]") {
    Config cfg = memConfig();
    PoolService svc{cfg};

    crow::request statsReq;
    auto initialStats = json::parse(svc.handleStats(statsReq).body);
    const auto initialRevision = initialStats["vis_revision"].get<std::uint64_t>();

    crow::request workReq;
    workReq.body = R"({"name":"worker-rev","hashrate":1000000})";
    auto workResp = svc.handleWork(workReq);
    REQUIRE(workResp.code == 200);
    const auto jobId = json::parse(workResp.body)["job_id"].get<int64_t>();

    auto afterWorkStats = json::parse(svc.handleStats(statsReq).body);
    const auto afterWorkRevision = afterWorkStats["vis_revision"].get<std::uint64_t>();
    CHECK(afterWorkRevision == initialRevision + 1);

    crow::request submitReq;
    submitReq.body = json{
        {"name", "worker-rev"},
        {"job_id", jobId},
        {"status", "done"},
        {"keys_scanned", "99999999999999999999"}
    }.dump();
    auto submitResp = svc.handleSubmit(submitReq);
    REQUIRE(submitResp.code == 200);

    auto afterSubmitStats = json::parse(svc.handleStats(statsReq).body);
    const auto afterSubmitRevision = afterSubmitStats["vis_revision"].get<std::uint64_t>();
    CHECK(afterSubmitRevision == afterWorkRevision + 1);
}

TEST_CASE("handleStats omits private found keys from finder responses", "[visualization][stats][security]") {
    Config cfg = memConfig();
    PoolService svc{cfg};

    crow::request workReq;
    workReq.body = R"({"name":"worker-a","hashrate":1000000})";
    auto workResp = svc.handleWork(workReq);
    REQUIRE(workResp.code == 200);
    const auto jobId = json::parse(workResp.body)["job_id"].get<int64_t>();

    crow::request submitReq;
    submitReq.body = json{
        {"name", "worker-a"},
        {"job_id", jobId},
        {"status", "FOUND"},
        {"findings", json::array({json{{"found_key", "0000000000000000000000000000000000000000000000000000000000000042"}, {"found_address", "1ABC"}}})}
    }.dump();
    auto submitResp = svc.handleSubmit(submitReq);
    REQUIRE(submitResp.code == 200);

    crow::request req;
    auto statsResp = svc.handleStats(req);
    REQUIRE(statsResp.code == 200);

    auto body = json::parse(statsResp.body);
    REQUIRE(body.contains("finders"));
    REQUIRE(body["finders"].is_array());
    REQUIRE(body["finders"].size() == 1);
    CHECK_FALSE(body["finders"][0].contains("found_key"));
    CHECK(body["finders"][0]["found_address"] == "1ABC");
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

TEST_CASE("handleStats remains responsive while visualization rebuild is in flight", "[visualization][stats][concurrency]") {
    Config cfg = memConfig();
    cfg.dbPath = "test-visualization-concurrency.db";

    std::atomic<bool> buildStarted = false;
    std::promise<void> releaseBuild;
    auto releaseFuture = releaseBuild.get_future().share();

    PoolService svc{
        cfg,
        {},
        false,
        [&](std::string_view panel, int64_t) {
            if (panel != "heatmap") return;
            buildStarted.store(true);
            releaseFuture.wait();
        }
    };

    crow::request visReq;
    auto visFuture = std::async(std::launch::async, [&] {
        return svc.handleHeatmapVisualization(visReq);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!buildStarted.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(buildStarted.load());

    crow::request statsReq;
    auto statsFuture = std::async(std::launch::async, [&] {
        return svc.handleStats(statsReq);
    });

    REQUIRE(statsFuture.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready);
    CHECK(statsFuture.get().code == 200);

    releaseBuild.set_value();
    CHECK(visFuture.get().code == 200);

    std::remove("test-visualization-concurrency.db");
}
