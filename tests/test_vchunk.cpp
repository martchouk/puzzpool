#include "test_helpers.hpp"

#include <puzzpool/allocator.hpp>
#include <puzzpool/hex_bigint.hpp>
#include <puzzpool/submission_service.hpp>
#include <puzzpool/work_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>

using namespace puzzpool;
using namespace puzzpool::test;
using json = nlohmann::json;

// ── fixture ───────────────────────────────────────────────────────────────────

struct VChunkFixture {
    Config          cfg = memConfig();
    PoolDb          db{cfg};
    Allocator       alloc{db};
    WorkService     ws{db, alloc};
    SubmissionService ss{db};

    // [1, 100001) — 100 000 keys, chunk size 1 000 → 100 virtual chunks
    static constexpr const char* START = "0000000000000000000000000000000000000000000000000000000000000001";
    static constexpr const char* END   = "00000000000000000000000000000000000000000000000000000000000186a1";
    static constexpr const char* SEED  = "testseed";

    int64_t puzzleId;

    VChunkFixture() {
        db.exec("UPDATE puzzles SET active = 0");
        SQLite::Statement ins(db.raw(), R"SQL(
            INSERT INTO puzzles
                (name, start_hex, end_hex, active, alloc_strategy, alloc_seed,
                 alloc_cursor, virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage)
            VALUES ('vtest', ?, ?, 1, 'virtual_random_chunks_v1', ?, 0, '1000', NULL, 0)
        )SQL");
        ins.bind(1, START);
        ins.bind(2, END);
        ins.bind(3, SEED);
        ins.exec();
        puzzleId = db.raw().getLastInsertRowid();
        alloc.seedVirtualChunks(puzzleId, START, END, SEED, cpp_int("1000"));
        alloc.ensureAllocatorForPuzzle(puzzleId);
    }
};

// ── seedVirtualChunks ─────────────────────────────────────────────────────────

TEST_CASE("seedVirtualChunks: correct number of vchunk slots created", "[vchunk]") {
    VChunkFixture f;
    // 100 000 keys / 1 000 per chunk = 100 virtual chunks
    SQLite::Statement q(f.db.raw(),
        "SELECT virtual_chunk_count FROM puzzles WHERE id = ?");
    q.bind(1, f.puzzleId);
    REQUIRE(q.executeStep());
    CHECK(q.getColumn(0).getInt64() == 100);
}

// ── assignWork with vchunk ────────────────────────────────────────────────────

TEST_CASE("vchunk assignWork: returns a valid chunk", "[vchunk][work]") {
    VChunkFixture f;
    auto r = f.ws.assignWork("alice", 1.0, "v1", {}, {});
    REQUIRE(r.ok);
    CHECK(r.jobId > 0);
    CHECK(hexToInt(r.endHex) > hexToInt(r.startHex));
}

TEST_CASE("vchunk assignWork: same worker gets same chunk back", "[vchunk][work]") {
    VChunkFixture f;
    auto r1 = f.ws.assignWork("alice", 1.0, "v1", {}, {});
    auto r2 = f.ws.assignWork("alice", 1.0, "v1", {}, {});
    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    CHECK(r1.jobId == r2.jobId);
}

TEST_CASE("vchunk assignWork: two workers get non-overlapping ranges", "[vchunk][work]") {
    VChunkFixture f;
    auto r1 = f.ws.assignWork("alice", 1.0, "v1", {}, {});
    auto r2 = f.ws.assignWork("bob",   1.0, "v1", {}, {});
    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    CHECK(r1.jobId != r2.jobId);

    cpp_int s1 = hexToInt(r1.startHex), e1 = hexToInt(r1.endHex);
    cpp_int s2 = hexToInt(r2.startHex), e2 = hexToInt(r2.endHex);
    CHECK((e1 <= s2 || e2 <= s1));
}

TEST_CASE("vchunk assignWork: many workers all get non-overlapping ranges", "[vchunk][work]") {
    VChunkFixture f;
    struct Range { cpp_int s, e; };
    std::vector<Range> ranges;
    for (int i = 0; i < 10; ++i) {
        auto r = f.ws.assignWork("worker" + std::to_string(i), 1.0, "v1", {}, {});
        REQUIRE(r.ok);
        ranges.push_back({hexToInt(r.startHex), hexToInt(r.endHex)});
    }
    for (size_t i = 0; i < ranges.size(); ++i) {
        for (size_t j = i + 1; j < ranges.size(); ++j) {
            bool disjoint = (ranges[i].e <= ranges[j].s) || (ranges[j].e <= ranges[i].s);
            CHECK(disjoint);
        }
    }
}

// ── ensureAllocatorForPuzzle idempotency ──────────────────────────────────────

TEST_CASE("ensureAllocatorForPuzzle: calling twice does not duplicate chunks", "[vchunk]") {
    VChunkFixture f;
    // Already called once in fixture; call again
    f.alloc.ensureAllocatorForPuzzle(f.puzzleId);

    SQLite::Statement q(f.db.raw(),
        "SELECT COUNT(*) FROM chunks WHERE puzzle_id = ? AND is_test = 0");
    q.bind(1, f.puzzleId);
    REQUIRE(q.executeStep());
    int64_t countAfter = q.getColumn(0).getInt64();

    // Assign one chunk to advance state, then call again
    f.ws.assignWork("probe", 1.0, "v1", {}, {});
    f.alloc.ensureAllocatorForPuzzle(f.puzzleId);

    SQLite::Statement q2(f.db.raw(),
        "SELECT COUNT(*) FROM chunks WHERE puzzle_id = ? AND is_test = 0");
    q2.bind(1, f.puzzleId);
    REQUIRE(q2.executeStep());
    CHECK(q2.getColumn(0).getInt64() == countAfter + 1);
}

// ── submitDone with decimal-string keys_scanned (regression for #72) ──────────

TEST_CASE("vchunk submitDone: decimal-string keys_scanned accepted", "[vchunk][submission]") {
    VChunkFixture f;
    auto r = f.ws.assignWork("alice", 1.0, "v1", {}, {});
    REQUIRE(r.ok);

    // Chunk size is 1000; submit with decimal-string >= 1000
    json b = {{"keys_scanned", "1000"}};
    auto result = f.ss.submitDone("alice", r.jobId, b);
    CHECK(result.accepted);
    CHECK_FALSE(result.hasError);
}

TEST_CASE("vchunk submitDone: decimal-string underscan reclaims chunk", "[vchunk][submission]") {
    VChunkFixture f;
    auto r = f.ws.assignWork("alice", 1.0, "v1", {}, {});
    REQUIRE(r.ok);

    json b = {{"keys_scanned", "500"}};  // 500 < 1000
    auto result = f.ss.submitDone("alice", r.jobId, b);
    CHECK_FALSE(result.accepted);
    CHECK(result.hasError);
    CHECK(result.errorCode == 400);
}

// ── reclaimTimedOutChunks returns count ──────────────────────────────────────

TEST_CASE("reclaimTimedOutChunks: returns number of reclaimed chunks", "[vchunk][reclaim]") {
    VChunkFixture f;
    auto r1 = f.ws.assignWork("alice", 1.0, "v1", {}, {});
    auto r2 = f.ws.assignWork("bob",   1.0, "v1", {}, {});
    REQUIRE(r1.ok); REQUIRE(r2.ok);

    SQLite::Statement upd(f.db.raw(),
        "UPDATE chunks SET heartbeat_at = datetime('now', '-60 minutes') WHERE status = 'assigned'");
    upd.exec();

    int count = f.ws.reclaimTimedOutChunks();
    CHECK(count == 2);
}
