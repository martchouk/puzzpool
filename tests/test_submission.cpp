#include "test_helpers.hpp"

#include <puzzpool/submission_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>

using namespace puzzpool;
using namespace puzzpool::test;
using json = nlohmann::json;

// ── fixture ──────────────────────────────────────────────────────────────────

struct SubmitFixture {
    Config            cfg = memConfig();
    PoolDb            db{cfg};
    SubmissionService ss{db};
    int64_t           puzzleId;
    int64_t           chunkId;

    SubmitFixture() {
        puzzleId = insertPuzzle(db);
        chunkId  = insertChunk(db, puzzleId,
            "0000000000000000000000000000000000000000000000000000000000000001",
            "0000000000000000000000000000000000000000000000000000000000000064",
            "assigned", "worker1");
    }
};

// ── submitDone ────────────────────────────────────────────────────────────────

TEST_CASE("submitDone: exact key count accepted", "[submission]") {
    SubmitFixture f;
    // chunk size = 0x64 - 0x01 = 99 decimal
    json body = {{"keys_scanned", 99}};
    auto r = f.ss.submitDone("worker1", f.chunkId, body);
    REQUIRE(r.accepted);
    REQUIRE_FALSE(r.hasError);

    // chunk should now be 'completed'
    SQLite::Statement q(f.db.raw(), "SELECT status FROM chunks WHERE id = ?");
    q.bind(1, f.chunkId);
    REQUIRE(q.executeStep());
    CHECK(q.getColumn(0).getString() == "completed");
}

TEST_CASE("submitDone: larger scan also accepted", "[submission]") {
    SubmitFixture f;
    json body = {{"keys_scanned", 1000}};
    auto r = f.ss.submitDone("worker1", f.chunkId, body);
    REQUIRE(r.accepted);
}

TEST_CASE("submitDone: underscan rejected and chunk reclaimed", "[submission]") {
    SubmitFixture f;
    json body = {{"keys_scanned", 10}}; // 10 < 99
    auto r = f.ss.submitDone("worker1", f.chunkId, body);
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.hasError);
    REQUIRE(r.errorCode == 400);

    // chunk should be 'reclaimed'
    SQLite::Statement q(f.db.raw(), "SELECT status FROM chunks WHERE id = ?");
    q.bind(1, f.chunkId);
    REQUIRE(q.executeStep());
    CHECK(q.getColumn(0).getString() == "reclaimed");
}

TEST_CASE("submitDone: wrong worker not accepted", "[submission]") {
    SubmitFixture f;
    json body = {{"keys_scanned", 99}};
    auto r = f.ss.submitDone("other_worker", f.chunkId, body);
    REQUIRE_FALSE(r.accepted);
    REQUIRE_FALSE(r.hasError); // not accepted, no error — chunk stays assigned

    SQLite::Statement q(f.db.raw(), "SELECT status FROM chunks WHERE id = ?");
    q.bind(1, f.chunkId);
    REQUIRE(q.executeStep());
    CHECK(q.getColumn(0).getString() == "assigned");
}

TEST_CASE("submitDone: missing keys_scanned returns error", "[submission]") {
    SubmitFixture f;
    json body = {};
    auto r = f.ss.submitDone("worker1", f.chunkId, body);
    REQUIRE(r.hasError);
    REQUIRE(r.errorCode == 400);
}

TEST_CASE("submitDone: negative keys_scanned rejected", "[submission]") {
    SubmitFixture f;
    json body = {{"keys_scanned", -1}};
    auto r = f.ss.submitDone("worker1", f.chunkId, body);
    REQUIRE(r.hasError);
    REQUIRE(r.errorCode == 400);
}

// ── submitFound ───────────────────────────────────────────────────────────────

TEST_CASE("submitFound: valid finding accepted and recorded", "[submission]") {
    SubmitFixture f;
    json body = {
        {"findings", {{{"found_key", "0x0000000000000000000000000000000000000000000000000000000000000042"}}}
    }};
    auto r = f.ss.submitFound("worker1", f.chunkId, body);
    REQUIRE(r.accepted);
    REQUIRE_FALSE(r.hasError);

    // chunk should be 'FOUND'
    SQLite::Statement q(f.db.raw(), "SELECT status FROM chunks WHERE id = ?");
    q.bind(1, f.chunkId);
    REQUIRE(q.executeStep());
    CHECK(q.getColumn(0).getString() == "FOUND");

    // finding recorded in findings table
    SQLite::Statement fq(f.db.raw(), "SELECT COUNT(*) FROM findings WHERE chunk_id = ?");
    fq.bind(1, f.chunkId);
    REQUIRE(fq.executeStep());
    CHECK(fq.getColumn(0).getInt() == 1);
}

TEST_CASE("submitFound: duplicate found_key deduplicated", "[submission]") {
    SubmitFixture f;
    const std::string key = "0x0000000000000000000000000000000000000000000000000000000000000042";
    json body = {{"findings", {
        {{"found_key", key}},
        {{"found_key", key}} // exact duplicate
    }}};
    auto r = f.ss.submitFound("worker1", f.chunkId, body);
    REQUIRE(r.accepted);

    // Only one row in findings despite two entries
    SQLite::Statement fq(f.db.raw(), "SELECT COUNT(*) FROM findings WHERE chunk_id = ?");
    fq.bind(1, f.chunkId);
    REQUIRE(fq.executeStep());
    CHECK(fq.getColumn(0).getInt() == 1);
}

TEST_CASE("submitFound: empty findings array rejected", "[submission]") {
    SubmitFixture f;
    json body = {{"findings", json::array()}};
    auto r = f.ss.submitFound("worker1", f.chunkId, body);
    REQUIRE(r.hasError);
    REQUIRE(r.errorCode == 400);
}

TEST_CASE("submitFound: invalid found_key hex rejected", "[submission]") {
    SubmitFixture f;
    json body = {{"findings", {{{"found_key", "not-hex"}}}}};
    auto r = f.ss.submitFound("worker1", f.chunkId, body);
    REQUIRE(r.hasError);
    REQUIRE(r.errorCode == 400);
}

// ── clearTestChunkIfNeeded ────────────────────────────────────────────────────

TEST_CASE("clearTestChunkIfNeeded: clears test_start/end on puzzle after test chunk completes", "[submission]") {
    Config cfg = memConfig();
    PoolDb db{cfg};
    SubmissionService ss{db};

    // Set up puzzle with a test chunk
    db.exec("UPDATE puzzles SET active = 0");
    SQLite::Statement pIns(db.raw(),
        "INSERT INTO puzzles (name, start_hex, end_hex, active, alloc_strategy, alloc_seed, alloc_cursor_hex,"
        " test_start_hex, test_end_hex)"
        " VALUES ('tp', '01', '64', 1, 'legacy_random_shards_v1', 's',"
        " '0000000000000000000000000000000000000000000000000000000000000000', '01', '64')");
    pIns.exec();
    int64_t pid = db.raw().getLastInsertRowid();

    // Insert a test chunk
    SQLite::Statement cIns(db.raw(),
        "INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at, heartbeat_at, is_test)"
        " VALUES (?, '01', '64', 'assigned', 'w1', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 1)");
    cIns.bind(1, pid);
    cIns.exec();
    int64_t cid = db.raw().getLastInsertRowid();

    ss.clearTestChunkIfNeeded(cid);

    // test_start_hex and test_end_hex should be NULL now
    SQLite::Statement q(db.raw(), "SELECT test_start_hex FROM puzzles WHERE id = ?");
    q.bind(1, pid);
    REQUIRE(q.executeStep());
    CHECK(q.isColumnNull(0));
}
