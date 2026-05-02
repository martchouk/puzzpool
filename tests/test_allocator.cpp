#include "test_helpers.hpp"

#include <puzzpool/allocator.hpp>
#include <puzzpool/hex_bigint.hpp>
#include <puzzpool/work_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace puzzpool;
using namespace puzzpool::test;

// ── fixture ──────────────────────────────────────────────────────────────────

struct AllocFixture {
    Config    cfg = memConfig();
    PoolDb    db{cfg};
    Allocator alloc{db};
    WorkService ws{db, alloc};

    // Puzzle over [1, 5*10^9] — 5 billion keys → seeds 5 sectors (1B keys each)
    static constexpr const char* START = "0000000000000000000000000000000000000000000000000000000000000001";
    static constexpr const char* END   = "000000000000000000000000000000000000000000000000000000012a05f201";

    int64_t puzzleId;

    AllocFixture() {
        puzzleId = insertPuzzle(db, START, END, "legacy_random_shards_v1");
        alloc.seedSectors(puzzleId, START, END);
        alloc.ensureAllocatorForPuzzle(puzzleId);
    }

    PuzzleRow puzzle() { return *db.puzzleById(puzzleId); }
};

// ── upsertWorkerAndDetectReactivation ─────────────────────────────────────────

TEST_CASE("upsertWorker: new worker is not considered reactivating", "[allocator][worker]") {
    AllocFixture f;
    bool reactive = f.ws.upsertWorkerAndDetectReactivation("newbie", 1e6, "v1", {}, {});
    CHECK_FALSE(reactive);
}

TEST_CASE("upsertWorker: fresh worker calling again is not reactivating", "[allocator][worker]") {
    AllocFixture f;
    f.ws.upsertWorkerAndDetectReactivation("bob", 1e6, "v1", {}, {});
    bool reactive = f.ws.upsertWorkerAndDetectReactivation("bob", 1e6, "v1", {}, {});
    CHECK_FALSE(reactive);
}

// ── assignWork ────────────────────────────────────────────────────────────────

TEST_CASE("assignWork: returns a valid chunk for fresh worker", "[allocator][work]") {
    AllocFixture f;
    auto r = f.ws.assignWork("alice", 1e6, "v1", {}, {});
    REQUIRE(r.ok);
    CHECK(r.jobId > 0);
    CHECK_FALSE(r.startHex.empty());
    CHECK_FALSE(r.endHex.empty());
    CHECK(hexToInt(r.endHex) > hexToInt(r.startHex));
}

TEST_CASE("assignWork: same worker gets same chunk back", "[allocator][work]") {
    AllocFixture f;
    auto r1 = f.ws.assignWork("alice", 1e6, "v1", {}, {});
    auto r2 = f.ws.assignWork("alice", 1e6, "v1", {}, {});
    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    CHECK(r1.jobId == r2.jobId);
    CHECK(r1.startHex == r2.startHex);
}

TEST_CASE("assignWork: two workers get non-overlapping ranges", "[allocator][work]") {
    AllocFixture f;
    auto r1 = f.ws.assignWork("alice", 1e6, "v1", {}, {});
    auto r2 = f.ws.assignWork("bob",   1e6, "v1", {}, {});
    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    CHECK(r1.jobId != r2.jobId);

    // Ranges must not overlap: [s1, e1) and [s2, e2) are disjoint
    cpp_int s1 = hexToInt(r1.startHex), e1 = hexToInt(r1.endHex);
    cpp_int s2 = hexToInt(r2.startHex), e2 = hexToInt(r2.endHex);
    bool disjoint = (e1 <= s2) || (e2 <= s1);
    CHECK(disjoint);
}

// ── reclaim behaviour ─────────────────────────────────────────────────────────

TEST_CASE("reclaimChunk: reclaimed chunk can be re-assigned to another worker", "[allocator][reclaim]") {
    AllocFixture f;
    auto r1 = f.ws.assignWork("alice", 1e6, "v1", {}, {});
    REQUIRE(r1.ok);

    // Manually reclaim the chunk
    SQLite::Statement upd(f.db.raw(),
        "UPDATE chunks SET status='reclaimed', worker_name=NULL WHERE id=?");
    upd.bind(1, r1.jobId);
    upd.exec();

    // Bob should pick it up
    auto r2 = f.ws.reclaimChunk("bob", f.puzzleId);
    REQUIRE(r2.has_value());
    CHECK(r2->id == r1.jobId);
    CHECK(r2->workerName == "bob");
}

TEST_CASE("existingAssignedChunk: returns the chunk already assigned to worker", "[allocator][reclaim]") {
    AllocFixture f;
    auto r1 = f.ws.assignWork("alice", 1e6, "v1", {}, {});
    REQUIRE(r1.ok);

    auto existing = f.ws.existingAssignedChunk("alice", f.puzzleId);
    REQUIRE(existing.has_value());
    CHECK(existing->id == r1.jobId);
}

TEST_CASE("existingAssignedChunk: returns nullopt for worker with no assignment", "[allocator][reclaim]") {
    AllocFixture f;
    auto existing = f.ws.existingAssignedChunk("nobody", f.puzzleId);
    CHECK_FALSE(existing.has_value());
}

// ── reclaimTimedOutChunks ─────────────────────────────────────────────────────

TEST_CASE("reclaimTimedOutChunks: stale chunks become reclaimed", "[allocator][reclaim]") {
    AllocFixture f;
    auto r = f.ws.assignWork("alice", 1e6, "v1", {}, {});
    REQUIRE(r.ok);

    // Backdate heartbeat_at so the chunk is considered timed out
    SQLite::Statement upd(f.db.raw(),
        "UPDATE chunks SET heartbeat_at = datetime('now', '-60 minutes') WHERE id = ?");
    upd.bind(1, r.jobId);
    upd.exec();

    f.ws.reclaimTimedOutChunks();

    SQLite::Statement q(f.db.raw(), "SELECT status FROM chunks WHERE id = ?");
    q.bind(1, r.jobId);
    REQUIRE(q.executeStep());
    CHECK(q.getColumn(0).getString() == "reclaimed");
}

TEST_CASE("reclaimTimedOutChunks: fresh chunks are not reclaimed", "[allocator][reclaim]") {
    AllocFixture f;
    auto r = f.ws.assignWork("alice", 1e6, "v1", {}, {});
    REQUIRE(r.ok);

    f.ws.reclaimTimedOutChunks(); // nothing should be timed out

    SQLite::Statement q(f.db.raw(), "SELECT status FROM chunks WHERE id = ?");
    q.bind(1, r.jobId);
    REQUIRE(q.executeStep());
    CHECK(q.getColumn(0).getString() == "assigned");
}
