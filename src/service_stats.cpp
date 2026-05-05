#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace puzzpool {

using json = nlohmann::json;

crow::response PoolService::handleStats(const crow::request& req) {
    std::shared_lock lock(mu_);
    try {
        std::optional<PuzzleRow> puzzle;
        if (const char* pId = req.url_params.get("puzzle_id")) {
            try {
                std::size_t pos = 0;
                int64_t id = std::stoll(pId, &pos);
                if (pos != std::string_view(pId).size())
                    return errorResponse(400, "puzzle_id must be a valid integer");
                puzzle = db_.puzzleById(id);
            } catch (const std::exception&) { return errorResponse(400, "puzzle_id must be a valid integer"); }
        } else {
            puzzle = db_.activePuzzle();
        }

        json out;
        out["stage"]           = cfg_.stage;
        out["target_minutes"]  = cfg_.targetMinutes;
        out["timeout_minutes"] = cfg_.timeoutMinutes;
        out["active_minutes"]  = cfg_.activeMinutes;
        out["puzzles"]         = db_.listPuzzles();

        if (!puzzle) {
            out["puzzle"]                 = nullptr;
            out["active_workers_count"]   = 0;
            out["inactive_workers_count"] = 0;
            out["total_hashrate"]         = 0;
            out["completed_chunks"]       = 0;
            out["reclaimed_chunks"]       = 0;
            out["total_keys_completed"]   = "0";
            out["virtual_chunks"] = {{"total", 0}, {"started_vchunks", 0}, {"completed_vchunks", 0}, {"virtual_chunk_size_keys", nullptr}, {"blocked_vchunk_count", "0"}};
            out["shards"]         = out["virtual_chunks"];
            out["workers"]        = json::array();
            out["scores"]         = json::array();
            out["finders"]        = json::array();
            out["chunks_vis"]     = json::array();
            out["alloc_generations"] = {{"legacy", 0}, {"affine", 0}, {"feistel", 0}};
            return jsonResponse(out);
        }

        auto stats = buildStats(*puzzle);
        for (auto& [k, v] : stats.items()) out[k] = v;
        return jsonResponse(out);
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

json PoolService::buildStats(const PuzzleRow& puzzle) {
    json out;
    out["puzzle"] = puzzleJson(puzzle);

    json visibleWorkers = json::array();
    double totalHashrate = 0;
    SQLite::Statement wq(db_.raw(), R"SQL(
        SELECT w.name, w.hashrate, w.last_seen, w.version, w.min_chunk_keys, w.chunk_quantum_keys,
               CASE WHEN w.last_seen >= datetime('now', ?) THEN 1 ELSE 0 END AS fresh,
               CASE WHEN EXISTS (
                   SELECT 1 FROM chunks c2 WHERE c2.worker_name = w.name AND c2.puzzle_id = ? AND c2.status = 'assigned'
               ) THEN 1 ELSE 0 END AS assigned_here,
               CASE WHEN w.last_seen >= datetime('now', ?) AND EXISTS (
                   SELECT 1 FROM chunks c2 WHERE c2.worker_name = w.name AND c2.puzzle_id = ? AND c2.status = 'assigned'
               ) THEN 1 ELSE 0 END AS active
        FROM workers w
        WHERE w.last_seen >= datetime('now', ?)
          AND EXISTS (
            SELECT 1 FROM chunks c
            WHERE (c.worker_name = w.name OR c.prev_worker_name = w.name)
              AND c.puzzle_id = ?
          )
        ORDER BY w.hashrate DESC
    )SQL");
    auto mkInterval = [](const std::string& n) {
        std::ostringstream oss; oss << '-' << n << " minutes"; return oss.str();
    };
    const std::string activeParam  = mkInterval(formatDouble(cfg_.activeMinutes));
    const std::string timeoutParam = mkInterval(std::to_string(cfg_.timeoutMinutes));
    wq.bind(1, activeParam);
    wq.bind(2, puzzle.id);
    wq.bind(3, activeParam);
    wq.bind(4, puzzle.id);
    wq.bind(5, timeoutParam);
    wq.bind(6, puzzle.id);

    std::map<std::string, json> workerAssignedMap;
    SQLite::Statement aq(db_.raw(), R"SQL(
        SELECT id, worker_name, assigned_at, heartbeat_at, start_hex, end_hex,
               vchunk_start_hex, vchunk_end_hex
        FROM chunks WHERE status = 'assigned' AND puzzle_id = ? AND is_test = 0
    )SQL");
    aq.bind(1, puzzle.id);
    while (aq.executeStep()) {
        std::string worker = aq.getColumn(1).getString();
        json j;
        j["current_chunk"] = aq.getColumn(0).getInt64();
        bool hasHex = !aq.isColumnNull(6) && !aq.isColumnNull(7);
        if (hasHex) {
            cpp_int s = hexToInt(aq.getColumn(6).getString());
            cpp_int e = hexToInt(aq.getColumn(7).getString());
            j["current_vchunk_run"]       = bigToDec(s) + ".." + bigToDec(e - 1);
            j["current_vchunk_run_start"] = bigToDec(s);
            j["current_vchunk_run_end"]   = bigToDec(e);
        } else {
            j["current_vchunk_run"]       = nullptr;
            j["current_vchunk_run_start"] = nullptr;
            j["current_vchunk_run_end"]   = nullptr;
        }
        j["assigned_at"]           = aq.isColumnNull(2) ? json(nullptr) : json(aq.getColumn(2).getString());
        j["heartbeat_at"]          = aq.isColumnNull(3) ? json(nullptr) : json(aq.getColumn(3).getString());
        j["current_job_start_hex"] = aq.isColumnNull(4) ? json(nullptr) : json(aq.getColumn(4).getString());
        j["current_job_end_hex"]   = aq.isColumnNull(5) ? json(nullptr) : json(aq.getColumn(5).getString());
        if (!aq.isColumnNull(4) && !aq.isColumnNull(5))
            j["current_job_keys"] = bigToDec(hexToInt(aq.getColumn(5).getString()) - hexToInt(aq.getColumn(4).getString()));
        else
            j["current_job_keys"] = nullptr;
        workerAssignedMap[worker] = j;
    }

    int activeWorkers   = 0;
    int inactiveWorkers = 0;
    while (wq.executeStep()) {
        json w;
        std::string name = wq.getColumn(0).getString();
        w["name"]               = name;
        w["hashrate"]           = wq.isColumnNull(1) ? 0.0 : wq.getColumn(1).getDouble();
        w["last_seen"]          = wq.isColumnNull(2) ? json(nullptr) : json(wq.getColumn(2).getString());
        w["version"]            = wq.isColumnNull(3) ? json(nullptr) : json(wq.getColumn(3).getString());
        w["min_chunk_keys"]     = wq.isColumnNull(4) ? json(nullptr) : json(wq.getColumn(4).getString());
        w["chunk_quantum_keys"] = wq.isColumnNull(5) ? json(nullptr) : json(wq.getColumn(5).getString());
        w["fresh"]              = wq.getColumn(6).getInt() == 1;
        w["assigned_here"]      = wq.getColumn(7).getInt() == 1;
        w["active"]             = wq.getColumn(8).getInt() == 1;
        if (w["active"].get<bool>()) {
            totalHashrate += w["hashrate"].get<double>();
            activeWorkers++;
        } else {
            inactiveWorkers++;
        }
        if (workerAssignedMap.count(name)) {
            for (auto& [k, v] : workerAssignedMap[name].items()) w[k] = v;
        } else {
            w["current_chunk"]            = nullptr;
            w["current_vchunk_run"]       = nullptr;
            w["current_vchunk_run_start"] = nullptr;
            w["current_vchunk_run_end"]   = nullptr;
            w["assigned_at"]              = nullptr;
            w["heartbeat_at"]             = nullptr;
            w["current_job_start_hex"]    = nullptr;
            w["current_job_end_hex"]      = nullptr;
            w["current_job_keys"]         = nullptr;
        }
        visibleWorkers.push_back(w);
    }

    out["workers"]                = visibleWorkers;
    out["active_workers_count"]   = activeWorkers;
    out["inactive_workers_count"] = inactiveWorkers;
    out["total_hashrate"]         = totalHashrate;

    auto scalarCount = [&](const std::string& sql) -> int64_t {
        SQLite::Statement q(db_.raw(), sql);
        q.bind(1, puzzle.id);
        q.executeStep();
        return q.getColumn(0).getInt64();
    };
    out["completed_chunks"] = scalarCount("SELECT COUNT(*) FROM chunks WHERE puzzle_id = ? AND (status = 'completed' OR status = 'FOUND') AND is_test = 0");
    out["reclaimed_chunks"] = scalarCount("SELECT COUNT(*) FROM chunks WHERE puzzle_id = ? AND status = 'reclaimed' AND is_test = 0");

    std::vector<std::pair<cpp_int, cpp_int>> doneRanges;
    std::map<std::string, std::pair<int64_t, cpp_int>> scoreMap;
    SQLite::Statement dq(db_.raw(),
        "SELECT worker_name, start_hex, end_hex FROM chunks WHERE puzzle_id = ? AND (status = 'completed' OR status = 'FOUND') AND worker_name IS NOT NULL AND is_test = 0");
    dq.bind(1, puzzle.id);
    while (dq.executeStep()) {
        std::string worker = dq.getColumn(0).getString();
        cpp_int s = hexToInt(dq.getColumn(1).getString());
        cpp_int e = hexToInt(dq.getColumn(2).getString());
        doneRanges.push_back({s, e});
        auto& slot = scoreMap[worker];
        slot.first  += 1;
        slot.second += (e - s);
    }
    std::sort(doneRanges.begin(), doneRanges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    cpp_int totalKeysCompleted = 0;
    if (!doneRanges.empty()) {
        cpp_int ms = doneRanges.front().first;
        cpp_int me = doneRanges.front().second;
        for (size_t i = 1; i < doneRanges.size(); ++i) {
            if (doneRanges[i].first <= me) {
                if (doneRanges[i].second > me) me = doneRanges[i].second;
            } else {
                totalKeysCompleted += me - ms;
                ms = doneRanges[i].first;
                me = doneRanges[i].second;
            }
        }
        totalKeysCompleted += me - ms;
    }
    out["total_keys_completed"] = bigToDec(totalKeysCompleted);

    json scores = json::array();
    std::vector<std::pair<std::string, std::pair<int64_t, cpp_int>>> scoreVec(scoreMap.begin(), scoreMap.end());
    std::sort(scoreVec.begin(), scoreVec.end(), [](const auto& a, const auto& b) {
        return a.second.second > b.second.second;
    });
    for (const auto& [worker, stats] : scoreVec) {
        scores.push_back({
            {"worker_name",      worker},
            {"completed_chunks", stats.first},
            {"total_keys",       bigToDec(stats.second)}
        });
    }
    out["scores"] = scores;

    json finders = json::array();
    SQLite::Statement fq(db_.raw(), R"SQL(
        SELECT f.worker_name, f.found_key, f.found_address, f.created_at,
               c.id AS chunk_global, c.vchunk_start_hex, c.vchunk_end_hex
        FROM findings f JOIN chunks c ON c.id = f.chunk_id
        WHERE c.puzzle_id = ? AND c.is_test = 0
        ORDER BY f.id ASC
    )SQL");
    fq.bind(1, puzzle.id);
    while (fq.executeStep()) {
        finders.push_back({
            {"worker_name",  fq.getColumn(0).getString()},
            {"found_key",    fq.getColumn(1).getString()},
            {"found_address", fq.isColumnNull(2) ? json(nullptr) : json(fq.getColumn(2).getString())},
            {"created_at",   fq.isColumnNull(3) ? json(nullptr) : json(fq.getColumn(3).getString())},
            {"chunk_global", fq.getColumn(4).getInt64()},
            {"vchunk_start", fq.isColumnNull(5) ? json(nullptr) : json(bigToDec(hexToInt(fq.getColumn(5).getString())))},
            {"vchunk_end",   fq.isColumnNull(6) ? json(nullptr) : json(bigToDec(hexToInt(fq.getColumn(6).getString())))}
        });
    }
    out["finders"] = finders;

    json chunksVis = json::array();
    cpp_int pStart = hexToInt(puzzle.startHex);
    cpp_int pEnd   = hexToInt(puzzle.endHex);
    cpp_int pRange = pEnd - pStart;
    SQLite::Statement cv(db_.raw(),
        "SELECT id, status, worker_name, start_hex, end_hex, alloc_generation FROM chunks WHERE puzzle_id = ? AND is_test = 0 ORDER BY id ASC");
    cv.bind(1, puzzle.id);
    while (cv.executeStep()) {
        cpp_int cs = hexToInt(cv.getColumn(3).getString()) - pStart;
        cpp_int ce = hexToInt(cv.getColumn(4).getString()) - pStart;
        long double s = cs.convert_to<long double>() / pRange.convert_to<long double>();
        long double e = ce.convert_to<long double>() / pRange.convert_to<long double>();
        chunksVis.push_back({
            {"id", cv.getColumn(0).getInt64()},
            {"st", cv.getColumn(1).getString()},
            {"w",  cv.isColumnNull(2) ? json(nullptr) : json(cv.getColumn(2).getString())},
            {"g",  cv.isColumnNull(5) ? json(nullptr) : json(cv.getColumn(5).getString())},
            {"s",  static_cast<double>(s)},
            {"e",  static_cast<double>(e)}
        });
    }
    out["chunks_vis"] = chunksVis;

    json virtualTotalJ     = nullptr;
    json virtualStartedJ   = json(0);
    json virtualCompletedJ = json(0);
    json blockedCountJ     = json("0");
    std::string strategy = puzzle.allocStrategy.empty() ? cfg_.allocStrategyLegacy : puzzle.allocStrategy;
    if (strategy == cfg_.allocStrategyVChunks) {
        virtualTotalJ = bigToDec(puzzle.virtualChunkCount);
        cpp_int started = 0, completed = 0;
        SQLite::Statement vc(db_.raw(), R"SQL(
            SELECT vchunk_start_hex, vchunk_end_hex, status
            FROM chunks
            WHERE puzzle_id = ? AND is_test = 0
              AND vchunk_start_hex IS NOT NULL AND vchunk_end_hex IS NOT NULL
        )SQL");
        vc.bind(1, puzzle.id);
        while (vc.executeStep()) {
            cpp_int s = hexToInt(vc.getColumn(0).getString());
            cpp_int e = hexToInt(vc.getColumn(1).getString());
            if (e > s) {
                started += (e - s);
                std::string st2 = vc.getColumn(2).getString();
                if (st2 == "completed" || st2 == "FOUND") completed += (e - s);
            }
        }
        virtualStartedJ   = bigToDec(started);
        virtualCompletedJ = bigToDec(completed);

        // Blocked vchunk count and vis entries — merge across sources first so
        // overlapping imports from different sources are counted as union coverage.
        cpp_int blockedTotal = 0;
        if (!puzzle.virtualChunkSizeKeys.empty()) {
            cpp_int vchunkSize;
            try { vchunkSize = cpp_int(puzzle.virtualChunkSizeKeys); } catch (...) {}
            if (vchunkSize > 0) {
                // Read all rows sorted by start; ORDER BY is lexicographic on 64-char hex (correct)
                SQLite::Statement bq(db_.raw(),
                    "SELECT start_vchunk, end_vchunk FROM blocked_vchunk_ranges WHERE puzzle_id = ? ORDER BY start_vchunk ASC");
                bq.bind(1, puzzle.id);
                std::vector<std::pair<cpp_int, cpp_int>> rawBlocked;
                while (bq.executeStep()) {
                    cpp_int vs = hexToInt(bq.getColumn(0).getString());
                    cpp_int ve = hexToInt(bq.getColumn(1).getString());
                    if (ve > vs) rawBlocked.emplace_back(vs, ve);
                }
                // Merge overlapping/adjacent intervals across all sources
                std::vector<std::pair<cpp_int, cpp_int>> mergedBlocked;
                for (auto& [vs, ve] : rawBlocked) {
                    if (mergedBlocked.empty() || vs > mergedBlocked.back().second)
                        mergedBlocked.emplace_back(vs, ve);
                    else
                        mergedBlocked.back().second = maxBig(mergedBlocked.back().second, ve);
                }
                // Sum merged intervals and emit one vis entry each
                int64_t blockedId = -1;
                for (auto& [vs, ve] : mergedBlocked) {
                    blockedTotal += (ve - vs);
                    long double sNorm = (vs * vchunkSize).convert_to<long double>() / pRange.convert_to<long double>();
                    long double eNorm = (ve * vchunkSize).convert_to<long double>() / pRange.convert_to<long double>();
                    if (eNorm > 1.0L) eNorm = 1.0L;
                    chunksVis.push_back({
                        {"id", blockedId--},
                        {"st", "blocked"},
                        {"w",  nullptr},
                        {"g",  nullptr},
                        {"s",  static_cast<double>(sNorm)},
                        {"e",  static_cast<double>(eNorm)}
                    });
                }
            }
        }
        blockedCountJ = bigToDec(blockedTotal);
    } else {
        virtualTotalJ     = scalarCount("SELECT COUNT(*) FROM sectors WHERE puzzle_id = ?");
        virtualStartedJ   = scalarCount("SELECT COUNT(DISTINCT sector_id) FROM chunks WHERE puzzle_id = ? AND is_test = 0 AND sector_id IS NOT NULL");
        virtualCompletedJ = scalarCount("SELECT COUNT(*) FROM sectors WHERE puzzle_id = ? AND status = 'done'");
    }
    json vchunkSizeJ = puzzle.virtualChunkSizeKeys.empty() ? json(nullptr) : json(puzzle.virtualChunkSizeKeys);
    out["virtual_chunks"] = {{"total", virtualTotalJ}, {"started_vchunks", virtualStartedJ}, {"completed_vchunks", virtualCompletedJ}, {"virtual_chunk_size_keys", vchunkSizeJ}, {"blocked_vchunk_count", blockedCountJ}};
    out["shards"]         = out["virtual_chunks"];

    json generations = {{"legacy", 0}, {"affine", 0}, {"feistel", 0}};
    SQLite::Statement gq(db_.raw(),
        "SELECT alloc_generation, COUNT(*) FROM chunks WHERE puzzle_id = ? AND is_test = 0 GROUP BY alloc_generation ORDER BY alloc_generation");
    gq.bind(1, puzzle.id);
    while (gq.executeStep()) {
        std::string gen = gq.isColumnNull(0) ? "legacy" : gq.getColumn(0).getString();
        if (generations.contains(gen)) generations[gen] = gq.getColumn(1).getInt64();
    }
    out["alloc_generations"] = generations;

    return out;
}

} // namespace puzzpool
