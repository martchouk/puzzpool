#include <puzzpool/submission_service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace puzzpool {

using json = nlohmann::json;

SubmissionService::SubmissionService(PoolDb& db) : db_(db) {}

SubmissionService::SubmitResult SubmissionService::submitDone(
    const std::string& name, int64_t jobId, const json& body) {

    if (!body.contains("keys_scanned"))
        return {false, true, 400, "keys_scanned is required for status: done"};
    cpp_int reported;
    const auto& ks = body["keys_scanned"];
    if (ks.is_number_integer()) {
        long long v = ks.get<long long>();
        if (v < 0) return {false, true, 400, "keys_scanned must be a non-negative integer"};
        reported = v;
    } else if (ks.is_string()) {
        std::string s = ks.get<std::string>();
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
            return {false, true, 400, "keys_scanned must be a non-negative integer or decimal string"};
        reported = cpp_int(s);
    } else {
        return {false, true, 400, "keys_scanned must be a non-negative integer or decimal string"};
    }

    SQLite::Transaction tx(db_.raw());
    SQLite::Statement chunkQ(db_.raw(),
        "SELECT start_hex, end_hex FROM chunks WHERE id = ? AND worker_name = ? AND status = 'assigned'");
    chunkQ.bind(1, jobId);
    chunkQ.bind(2, name);
    if (!chunkQ.executeStep()) {
        tx.commit();
        return {false, false, 200, {}};
    }

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
        return {false, true, 400,
            "chunk #" + std::to_string(jobId) + " not accepted, reported size: " +
            bigToDec(reported) + ", expected size: " + bigToDec(expectedSize) + ". Chunk reclaimed."};
    }

    SQLite::Statement done(db_.raw(),
        "UPDATE chunks SET status = 'completed' WHERE id = ? AND worker_name = ? AND status = 'assigned'");
    done.bind(1, jobId);
    done.bind(2, name);
    done.exec();
    tx.commit();
    clearTestChunkIfNeeded(jobId);
    return {true, false, 0, {}};
}

SubmissionService::SubmitResult SubmissionService::submitFound(
    const std::string& name, int64_t jobId, const json& body) {

    if (!body.contains("findings") || !body["findings"].is_array() || body["findings"].empty())
        return {false, true, 400, "findings must be a non-empty array"};

    std::vector<json> findings;
    std::set<std::string> seen;
    for (const auto& f : body["findings"]) {
        if (!f.is_object() || !f.contains("found_key") || !f["found_key"].is_string())
            return {false, true, 400, "each finding must include found_key as a string"};
        std::string foundKey = f["found_key"].get<std::string>();
        if (!isValidHex(foundKey)) return {false, true, 400, "each found_key must be a valid hex string"};
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
            if (!late.executeStep()) {
                tx.commit();
                return {false, false, 0, {}};
            }

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
        // Append to bingo file on finding
        std::ofstream bingo("BINGO_FOUND_KEYS.txt", std::ios::app);
        std::time_t t = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
        bingo << "[" << buf << "] BINGO! Job: " << jobId << " | Worker: " << name
              << " | KEY: " << f["found_key"].get<std::string>() << " | ADDR: "
              << (f["found_address"].is_null() ? "Unknown" : f["found_address"].get<std::string>()) << "\n";
    }

    clearTestChunkIfNeeded(jobId);
    return {true, false, 0, {}};
}

void SubmissionService::clearTestChunkIfNeeded(int64_t jobId) {
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
