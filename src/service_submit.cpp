#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace puzzpool {

using json = nlohmann::json;

crow::response PoolService::handleSubmit(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        std::string name   = body.value("name", "");
        int64_t     jobId  = body.value("job_id", 0LL);
        std::string status = body.value("status", "");
        if (status != "done" && status != "FOUND")
            return errorResponse(400, "status must be \"done\" or \"FOUND\"");

        if (status == "FOUND") return handleSubmitFound(name, jobId, body);
        return handleSubmitDone(name, jobId, body);
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

crow::response PoolService::handleSubmitDone(const std::string& name, int64_t jobId, const json& body) {
    if (!body.contains("keys_scanned"))
        return errorResponse(400, "keys_scanned is required for status: done");
    if (!body["keys_scanned"].is_number_integer() || body["keys_scanned"].get<long long>() < 0)
        return errorResponse(400, "keys_scanned must be a non-negative integer");
    cpp_int reported = body["keys_scanned"].get<long long>();

    SQLite::Transaction tx(db_.raw());
    SQLite::Statement chunkQ(db_.raw(),
        "SELECT start_hex, end_hex FROM chunks WHERE id = ? AND worker_name = ? AND status = 'assigned'");
    chunkQ.bind(1, jobId);
    chunkQ.bind(2, name);
    if (!chunkQ.executeStep()) return jsonResponse({{"accepted", false}});

    cpp_int expectedSize = hexToInt(chunkQ.getColumn(1).getString()) - hexToInt(chunkQ.getColumn(0).getString());
    if (reported < expectedSize) {
        SQLite::Statement upd(db_.raw(), R"SQL(
            UPDATE chunks
            SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL, assigned_at = NULL, heartbeat_at = NULL
            WHERE id = ? AND worker_name = ? AND status = 'assigned'
        )SQL");
        upd.bind(1, jobId);
        upd.bind(2, name);
        upd.exec();
        tx.commit();
        return errorJsonResponse(400, {
            {"accepted", false},
            {"error", "chunk #" + std::to_string(jobId) + " not accepted, reported size: " +
                      bigToDec(reported) + ", expected size: " + bigToDec(expectedSize) + ". Chunk reclaimed."}
        });
    }

    SQLite::Statement done(db_.raw(),
        "UPDATE chunks SET status = 'completed' WHERE id = ? AND worker_name = ? AND status = 'assigned'");
    done.bind(1, jobId);
    done.bind(2, name);
    done.exec();
    tx.commit();
    clearTestChunkIfNeeded(jobId);
    return jsonResponse({{"accepted", true}});
}

crow::response PoolService::handleSubmitFound(const std::string& name, int64_t jobId, const json& body) {
    if (!body.contains("findings") || !body["findings"].is_array() || body["findings"].empty())
        return errorResponse(400, "findings must be a non-empty array");

    std::vector<json> findings;
    std::set<std::string> seen;
    for (const auto& f : body["findings"]) {
        if (!f.is_object() || !f.contains("found_key"))
            return errorResponse(400, "each finding must include found_key");
        std::string foundKey = f["found_key"].get<std::string>();
        if (!isValidHex(foundKey)) return errorResponse(400, "each found_key must be a valid hex string");
        foundKey = normalizeHex(foundKey);
        if (seen.insert(foundKey).second) {
            findings.push_back({
                {"found_key",     foundKey},
                {"found_address", f.contains("found_address") && !f["found_address"].is_null()
                                  ? f["found_address"] : json(nullptr)}
            });
        }
    }

    const std::string primaryKey  = findings.front()["found_key"].get<std::string>();
    const std::string primaryAddr = findings.front()["found_address"].is_null()
        ? "" : findings.front()["found_address"].get<std::string>();

    SQLite::Transaction tx(db_.raw());
    SQLite::Statement upd(db_.raw(), R"SQL(
        UPDATE chunks
        SET status = 'FOUND',
            found_key = COALESCE(found_key, ?),
            found_address = COALESCE(found_address, ?)
        WHERE id = ? AND worker_name = ? AND status = 'assigned'
    )SQL");
    upd.bind(1, primaryKey);
    if (!primaryAddr.empty()) upd.bind(2, primaryAddr); else upd.bind(2);
    upd.bind(3, jobId);
    upd.bind(4, name);
    upd.exec();
    bool accepted = db_.raw().getChanges() > 0;

    if (!accepted) {
        SQLite::Statement already(db_.raw(),
            "SELECT id FROM findings WHERE chunk_id = ? AND worker_name = ? AND found_key = ?");
        already.bind(1, jobId);
        already.bind(2, name);
        already.bind(3, primaryKey);
        if (!already.executeStep()) {
            SQLite::Statement late(db_.raw(),
                "SELECT id FROM chunks WHERE id = ? AND prev_worker_name = ? AND status IN ('assigned','reclaimed')");
            late.bind(1, jobId);
            late.bind(2, name);
            if (!late.executeStep()) return jsonResponse({{"accepted", false}});

            SQLite::Statement lateUpd(db_.raw(), R"SQL(
                UPDATE chunks
                SET status = 'FOUND', worker_name = ?,
                    found_key = COALESCE(found_key, ?),
                    found_address = COALESCE(found_address, ?)
                WHERE id = ?
            )SQL");
            lateUpd.bind(1, name);
            lateUpd.bind(2, primaryKey);
            if (!primaryAddr.empty()) lateUpd.bind(3, primaryAddr); else lateUpd.bind(3);
            lateUpd.bind(4, jobId);
            lateUpd.exec();
        }
    }

    SQLite::Statement ins(db_.raw(),
        "INSERT OR IGNORE INTO findings (chunk_id, worker_name, found_key, found_address) VALUES (?, ?, ?, ?)");
    std::vector<json> inserted;
    for (const auto& f : findings) {
        ins.reset();
        ins.bind(1, jobId);
        ins.bind(2, name);
        ins.bind(3, f["found_key"].get<std::string>());
        if (f["found_address"].is_null()) ins.bind(4); else ins.bind(4, f["found_address"].get<std::string>());
        ins.exec();
        if (db_.raw().getChanges() > 0) inserted.push_back(f);
    }
    tx.commit();

    for (const auto& f : inserted) {
        std::ofstream bingo("BINGO_FOUND_KEYS.txt", std::ios::app);
        bingo << "[" << nowIsoUtc() << "] BINGO! Job: " << jobId << " | Worker: " << name
              << " | KEY: " << f["found_key"].get<std::string>() << " | ADDR: "
              << (f["found_address"].is_null() ? "Unknown" : f["found_address"].get<std::string>()) << "\n";
    }

    clearTestChunkIfNeeded(jobId);
    return jsonResponse({{"accepted", true}});
}

void PoolService::clearTestChunkIfNeeded(int64_t jobId) {
    SQLite::Statement q(db_.raw(), "SELECT is_test, puzzle_id, start_hex, end_hex FROM chunks WHERE id = ?");
    q.bind(1, jobId);
    if (!q.executeStep()) return;
    if (q.getColumn(0).getInt() != 1) return;
    SQLite::Statement upd(db_.raw(), R"SQL(
        UPDATE puzzles SET test_start_hex = NULL, test_end_hex = NULL
        WHERE id = ? AND test_start_hex = ? AND test_end_hex = ?
    )SQL");
    upd.bind(1, q.getColumn(1).getInt64());
    upd.bind(2, q.getColumn(2).getString());
    upd.bind(3, q.getColumn(3).getString());
    upd.exec();
}

} // namespace puzzpool
