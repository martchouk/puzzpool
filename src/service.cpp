#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>
#include <puzzpool/hash_utils.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace puzzpool {

using json = nlohmann::json;

PoolService::PoolService(const Config& cfg)
    : cfg_(cfg), db_(cfg), allocator_(db_) {
    seedConfiguredKeyspaces();
    ensureSingleActivePuzzle();
    ensureAllocators();
}

// ─── public handlers ─────────────────────────────────────────────────────────

crow::response PoolService::handleStats(const crow::request& req) {
    try {
        std::optional<PuzzleRow> puzzle;
        if (req.url_params.get("puzzle_id")) {
            puzzle = db_.puzzleById(std::stoll(req.url_params.get("puzzle_id")));
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
            out["puzzle"]                = nullptr;
            out["active_workers_count"]  = 0;
            out["inactive_workers_count"] = 0;
            out["total_hashrate"]        = 0;
            out["completed_chunks"]      = 0;
            out["reclaimed_chunks"]      = 0;
            out["total_keys_completed"]  = "0";
            out["virtual_chunks"] = {{"total", 0}, {"started", 0}, {"completed", 0}};
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

crow::response PoolService::handleWork(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        std::string name = body.value("name", "");
        if (name.empty()) return errorResponse(400, "Missing name");

        std::optional<double> hashrate;
        if (body.contains("hashrate") && !body["hashrate"].is_null())
            hashrate = body["hashrate"].get<double>();
        std::string version = body.value("version", "");
        std::optional<std::string> minChunkKeys;
        if (body.contains("min_chunk_keys") && !body["min_chunk_keys"].is_null())
            minChunkKeys = body["min_chunk_keys"].get<std::string>();
        std::optional<std::string> chunkQuantumKeys;
        if (body.contains("chunk_quantum_keys") && !body["chunk_quantum_keys"].is_null())
            chunkQuantumKeys = body["chunk_quantum_keys"].get<std::string>();

        bool isReactivating = upsertWorkerAndDetectReactivation(name, hashrate.value_or(1000000), version, minChunkKeys, chunkQuantumKeys);
        if (isReactivating) {
            SQLite::Statement q(db_.raw(), R"SQL(
                UPDATE chunks
                SET status = 'reclaimed', prev_worker_name = worker_name, worker_name = NULL,
                    assigned_at = NULL, heartbeat_at = NULL
                WHERE worker_name = ? AND status = 'assigned'
            )SQL");
            q.bind(1, name);
            q.exec();
        }

        auto puzzle = db_.activePuzzle();
        if (!puzzle) return errorResponse(503, "No active puzzle configured");
        allocator_.ensureAllocatorForPuzzle(puzzle->id);
        puzzle = db_.activePuzzle();

        if (auto existing = existingAssignedChunk(name, puzzle->id)) {
            return jsonResponse({
                {"job_id",    existing->id},
                {"start_key", existing->startHex},
                {"end_key",   existing->endHex}
            });
        }

        if (!puzzle->testStartHex.empty() && !puzzle->testEndHex.empty()) {
            if (auto claimed = claimTestChunk(name, *puzzle)) {
                return jsonResponse({
                    {"job_id",    claimed->id},
                    {"start_key", claimed->startHex},
                    {"end_key",   claimed->endHex}
                });
            }
        }

        if (!isReactivating) {
            if (auto reclaimed = reclaimChunk(name, puzzle->id)) {
                return jsonResponse({
                    {"job_id",    reclaimed->id},
                    {"start_key", reclaimed->startHex},
                    {"end_key",   reclaimed->endHex}
                });
            }
        }

        std::optional<Allocator::WorkAssignResult> result;
        std::string strategy = puzzle->allocStrategy.empty() ? cfg_.allocStrategyLegacy : puzzle->allocStrategy;
        if (strategy == cfg_.allocStrategyVChunks) {
            result = allocator_.assignVirtualChunkJob(name, hashrate, minChunkKeys, chunkQuantumKeys, *puzzle);
        } else {
            result = allocator_.assignLegacyRandomChunk(name, hashrate, *puzzle);
        }

        if (!result) return errorResponse(503, "All keyspace has been assigned");
        return jsonResponse({
            {"job_id",    result->chunkId},
            {"start_key", result->startHex},
            {"end_key",   result->endHex}
        });
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

crow::response PoolService::handleHeartbeat(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        std::string name = body.value("name", "");
        if (name.empty() || !body.contains("job_id")) return errorResponse(400, "Missing name or job_id");
        int64_t jobId = body["job_id"].get<int64_t>();

        SQLite::Statement ins(db_.raw(), R"SQL(
            INSERT INTO workers (name, hashrate, last_seen)
            VALUES (?, 0, CURRENT_TIMESTAMP)
            ON CONFLICT(name) DO UPDATE SET last_seen = CURRENT_TIMESTAMP
        )SQL");
        ins.bind(1, name);
        ins.exec();

        SQLite::Statement q(db_.raw(), R"SQL(
            UPDATE chunks
            SET heartbeat_at = CURRENT_TIMESTAMP
            WHERE id = ? AND worker_name = ? AND status = 'assigned'
        )SQL");
        q.bind(1, jobId);
        q.bind(2, name);
        q.exec();
        return jsonResponse({{"ok", true}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

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

crow::response PoolService::handleActivatePuzzle(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        if (!body.contains("id")) return errorResponse(400, "Missing id");
        int64_t id = body["id"].get<int64_t>();
        auto target = db_.puzzleById(id);
        if (!target) return errorResponse(404, "Puzzle not found");

        SQLite::Transaction tx(db_.raw());
        db_.exec("UPDATE puzzles SET active = 0");
        SQLite::Statement q(db_.raw(), "UPDATE puzzles SET active = 1 WHERE id = ?");
        q.bind(1, id);
        q.exec();
        tx.commit();

        allocator_.ensureAllocatorForPuzzle(id);
        auto p = db_.puzzleById(id);
        return jsonResponse({{"ok", true}, {"puzzle", puzzleJson(*p)}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

crow::response PoolService::handleSetPuzzle(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        std::string name     = body.value("name", "");
        std::string startHex = body.value("start_hex", "");
        std::string endHex   = body.value("end_hex", "");
        if (name.empty() || startHex.empty() || endHex.empty())
            return errorResponse(400, "Missing name, start_hex, or end_hex");
        if (!isValidHex(startHex) || !isValidHex(endHex))
            return errorResponse(400, "start_hex and end_hex must be valid hex strings");

        std::string strategy = body.value("alloc_strategy", cfg_.defaultAllocStrategy);
        if (strategy != cfg_.allocStrategyLegacy && strategy != cfg_.allocStrategyVChunks)
            return errorResponse(400, "alloc_strategy must be legacy_random_shards_v1 or virtual_random_chunks_v1");

        std::string startNorm = normalizeHex(startHex);
        std::string endNorm   = normalizeHex(endHex);
        if (hexToInt(endNorm) <= hexToInt(startNorm))
            return errorResponse(400, "end_hex must be greater than start_hex");
        cpp_int puzzleRange = hexToInt(endNorm) - hexToInt(startNorm);

        std::optional<cpp_int> vchunkSize;
        if (strategy == cfg_.allocStrategyVChunks) {
            if (body.contains("virtual_chunk_size_keys") && !body["virtual_chunk_size_keys"].is_null()) {
                vchunkSize = minBig(cpp_int(body["virtual_chunk_size_keys"].get<std::string>()), puzzleRange);
            } else {
                vchunkSize = allocator_.chooseDefaultVirtualChunkSize(puzzleRange);
            }
            if (ceilDiv(puzzleRange, *vchunkSize) > std::numeric_limits<int64_t>::max())
                return errorResponse(400, "virtual chunk count exceeds current prod-compatible DB schema");
        }

        SQLite::Transaction tx(db_.raw());
        db_.exec("UPDATE puzzles SET active = 0");
        auto existing = db_.puzzleByName(name);
        int64_t puzzleId = 0;

        std::string seed = body.value("alloc_seed", std::string());
        if (seed.empty()) {
            PuzzleRow temp; temp.name = name; temp.startHex = startNorm; temp.endHex = endNorm;
            seed = allocator_.defaultAllocSeedForPuzzle(temp, strategy);
        }

        bool reuse = false;
        if (existing) {
            bool sameSeed = existing->allocSeed == seed;
            bool sameSize = !existing->virtualChunkSizeKeys.empty()
                ? existing->virtualChunkSizeKeys == (vchunkSize ? bigToDec(*vchunkSize) : "")
                : true;
            reuse = existing->startHex == startNorm && existing->endHex == endNorm &&
                    (existing->allocStrategy.empty() ? cfg_.allocStrategyLegacy : existing->allocStrategy) == strategy &&
                    sameSeed && sameSize;
        }

        if (reuse) {
            SQLite::Statement q(db_.raw(), "UPDATE puzzles SET active = 1 WHERE id = ?");
            q.bind(1, existing->id);
            q.exec();
            puzzleId = existing->id;
            allocator_.ensureAllocatorForPuzzle(puzzleId);
        } else {
            SQLite::Statement ins(db_.raw(), R"SQL(
                INSERT INTO puzzles (
                    name, start_hex, end_hex, active,
                    alloc_strategy, alloc_seed, alloc_cursor,
                    virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
                ) VALUES (?, ?, ?, 1, ?, ?, 0, ?, NULL, 0)
            )SQL");
            ins.bind(1, name);
            ins.bind(2, startNorm);
            ins.bind(3, endNorm);
            ins.bind(4, strategy);
            ins.bind(5, seed);
            if (vchunkSize) ins.bind(6, bigToDec(*vchunkSize)); else ins.bind(6);
            ins.exec();
            puzzleId = db_.raw().getLastInsertRowid();
            if (strategy == cfg_.allocStrategyVChunks)
                allocator_.seedVirtualChunks(puzzleId, startNorm, endNorm, seed, *vchunkSize);
            else
                allocator_.seedSectors(puzzleId, startNorm, endNorm);
        }
        tx.commit();
        auto p = db_.activePuzzle();
        return jsonResponse({{"ok", true}, {"puzzle", puzzleJson(*p)}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

crow::response PoolService::handleSetTestChunk(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto puzzle = db_.activePuzzle();
        if (!puzzle) return errorResponse(503, "No active puzzle");
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        std::string startHex = body.value("start_hex", "");
        std::string endHex   = body.value("end_hex", "");

        if (startHex.empty()) {
            SQLite::Statement q(db_.raw(), "UPDATE puzzles SET test_start_hex = NULL, test_end_hex = NULL WHERE id = ?");
            q.bind(1, puzzle->id);
            q.exec();
            return jsonResponse({{"ok", true}, {"test_chunk", nullptr}});
        }
        if (!isValidHex(startHex)) return errorResponse(400, "start_hex must be a valid hex string");
        std::string startNorm = normalizeHex(startHex);
        cpp_int ts = hexToInt(startNorm);

        std::string endNorm;
        if (!endHex.empty()) {
            if (!isValidHex(endHex)) return errorResponse(400, "end_hex must be a valid hex string");
            endNorm = normalizeHex(endHex);
        } else {
            cpp_int max256 = (cpp_int(1) << 256) - 1;
            if (ts + cfg_.gpuBatchKeys > max256)
                return errorResponse(400, "auto-resolved end_hex exceeds 256-bit range");
            endNorm = intToHex(ts + cfg_.gpuBatchKeys);
        }
        if (hexToInt(endNorm) <= ts)
            return errorResponse(400, "end_hex must be greater than start_hex");

        SQLite::Statement q(db_.raw(), "UPDATE puzzles SET test_start_hex = ?, test_end_hex = ? WHERE id = ?");
        q.bind(1, startNorm);
        q.bind(2, endNorm);
        q.bind(3, puzzle->id);
        q.exec();
        return jsonResponse({{"ok", true}, {"test_chunk", {{"start_hex", startNorm}, {"end_hex", endNorm}}}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

crow::response PoolService::handleAdminPuzzles() {
    try {
        SQLite::Statement q(db_.raw(), R"SQL(
            SELECT id, name, active, start_hex, end_hex,
                   alloc_strategy, alloc_seed, alloc_cursor,
                   virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
            FROM puzzles
            ORDER BY id ASC
        )SQL");
        json arr = json::array();
        while (q.executeStep()) {
            arr.push_back({
                {"id",                     q.getColumn(0).getInt64()},
                {"name",                   q.getColumn(1).getString()},
                {"active",                 q.getColumn(2).getInt()},
                {"start_hex",              q.getColumn(3).getString()},
                {"end_hex",                q.getColumn(4).getString()},
                {"alloc_strategy",         q.isColumnNull(5) ? json(nullptr) : json(q.getColumn(5).getString())},
                {"alloc_seed",             q.isColumnNull(6) ? json(nullptr) : json(q.getColumn(6).getString())},
                {"alloc_cursor",           q.isColumnNull(7) ? json(nullptr) : json(q.getColumn(7).getInt64())},
                {"virtual_chunk_size_keys", q.isColumnNull(8) ? json(nullptr) : json(q.getColumn(8).getString())},
                {"virtual_chunk_count",    q.isColumnNull(9) ? json(nullptr) : json(q.getColumn(9).getInt64())},
                {"bootstrap_stage",        q.isColumnNull(10) ? 0 : q.getColumn(10).getInt()}
            });
        }
        return jsonResponse({{"puzzles", arr}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

void PoolService::reclaimTimedOutChunks() {
    std::unique_lock lock(mu_);
    SQLite::Statement q(db_.raw(), R"SQL(
        UPDATE chunks
        SET status = 'reclaimed',
            prev_worker_name = worker_name,
            worker_name = NULL,
            assigned_at = NULL,
            heartbeat_at = NULL
        WHERE status = 'assigned'
          AND COALESCE(heartbeat_at, assigned_at) < datetime('now', ?)
    )SQL");
    q.bind(1, "-" + std::to_string(cfg_.timeoutMinutes) + " minutes");
    q.exec();
}

// ─── private helpers ──────────────────────────────────────────────────────────

void PoolService::seedConfiguredKeyspaces() {
    for (const auto& [name, rangePair] : cfg_.keyspaces) {
        const std::string& startRaw = rangePair.first;
        const std::string& endRaw   = rangePair.second;

        if (!isValidHex(startRaw) || !isValidHex(endRaw)) {
            std::cerr << "[Config] Skipping invalid keyspace " << name
                      << " — expected valid start/end hex\n";
            continue;
        }

        const std::string startNorm = normalizeHex(startRaw);
        const std::string endNorm   = normalizeHex(endRaw);

        if (hexToInt(endNorm) <= hexToInt(startNorm)) {
            std::cerr << "[Config] Skipping " << name << " — end_hex must be greater than start_hex\n";
            continue;
        }

        auto existing = db_.puzzleByName(name);
        if (existing) continue;

        const std::string strategy = cfg_.defaultAllocStrategy;
        const std::string seed = allocator_.defaultAllocSeedForPuzzle(
            PuzzleRow{0, name, startNorm, endNorm, 0, "", "", strategy, "", 0, "", 0, 0},
            strategy
        );

        std::optional<cpp_int> virtualChunkSize;
        if (strategy == cfg_.allocStrategyVChunks) {
            const auto rn = normalizedRange(startNorm, endNorm);
            virtualChunkSize = allocator_.chooseDefaultVirtualChunkSize(rn.range);
        }

        SQLite::Statement ins(db_.raw(), R"SQL(
            INSERT INTO puzzles (
                name, start_hex, end_hex, active,
                alloc_strategy, alloc_seed, alloc_cursor,
                virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
            )
            VALUES (?, ?, ?, 0, ?, ?, 0, ?, NULL, 0)
        )SQL");

        ins.bind(1, name);
        ins.bind(2, startNorm);
        ins.bind(3, endNorm);
        ins.bind(4, strategy);
        ins.bind(5, seed);
        if (virtualChunkSize.has_value()) ins.bind(6, bigToDec(*virtualChunkSize));
        else ins.bind(6);
        ins.exec();
        const int64_t puzzleId = db_.raw().getLastInsertRowid();

        std::cerr << "[Config] Seeded keyspace: " << name << "\n";

        if (strategy == cfg_.allocStrategyVChunks)
            allocator_.seedVirtualChunks(puzzleId, startNorm, endNorm, seed, *virtualChunkSize);
        else
            allocator_.seedSectors(puzzleId, startNorm, endNorm);
    }

    SQLite::Statement countQ(db_.raw(), "SELECT COUNT(*) FROM puzzles");
    countQ.executeStep();
    const int64_t puzzleCount = countQ.getColumn(0).getInt64();

    if (puzzleCount == 0) {
        const std::string name     = "Puzzle #71";
        const std::string startHex = normalizeHex("0400000000000000000");
        const std::string endHex   = normalizeHex("07fffffffffffffffff");
        const std::string strategy = cfg_.defaultAllocStrategy;
        const std::string seed = allocator_.defaultAllocSeedForPuzzle(
            PuzzleRow{0, name, startHex, endHex, 1, "", "", strategy, "", 0, "", 0, 0},
            strategy
        );

        std::optional<cpp_int> virtualChunkSize;
        if (strategy == cfg_.allocStrategyVChunks) {
            const auto rn = normalizedRange(startHex, endHex);
            virtualChunkSize = allocator_.chooseDefaultVirtualChunkSize(rn.range);
        }

        SQLite::Statement ins(db_.raw(), R"SQL(
            INSERT INTO puzzles (
                name, start_hex, end_hex, active,
                alloc_strategy, alloc_seed, alloc_cursor,
                virtual_chunk_size_keys, virtual_chunk_count, bootstrap_stage
            )
            VALUES (?, ?, ?, 1, ?, ?, 0, ?, NULL, 0)
        )SQL");
        ins.bind(1, name);
        ins.bind(2, startHex);
        ins.bind(3, endHex);
        ins.bind(4, strategy);
        ins.bind(5, seed);
        if (virtualChunkSize.has_value()) ins.bind(6, bigToDec(*virtualChunkSize));
        else ins.bind(6);
        ins.exec();
        const int64_t puzzleId = db_.raw().getLastInsertRowid();

        std::cerr << "[Init] Seeded Puzzle #71 as active puzzle.\n";

        if (strategy == cfg_.allocStrategyVChunks)
            allocator_.seedVirtualChunks(puzzleId, startHex, endHex, seed, *virtualChunkSize);
        else
            allocator_.seedSectors(puzzleId, startHex, endHex);
    }

    SQLite::Statement activeCountQ(db_.raw(), "SELECT COUNT(*) FROM puzzles WHERE active = 1");
    activeCountQ.executeStep();
    const int64_t activeCount = activeCountQ.getColumn(0).getInt64();

    if (activeCount == 0) {
        db_.exec("UPDATE puzzles SET active = 1 WHERE id = (SELECT MIN(id) FROM puzzles)");
        std::cerr << "[Init] No active puzzle found — activated the first one.\n";
    } else if (activeCount > 1) {
        db_.exec("UPDATE puzzles SET active = 0 WHERE id != (SELECT MAX(id) FROM puzzles WHERE active = 1)");
        std::cerr << "[Init] Multiple active puzzles found — deactivated all but the latest.\n";
    }

    SQLite::Statement allQ(db_.raw(), "SELECT id FROM puzzles ORDER BY id ASC");
    while (allQ.executeStep()) {
        allocator_.ensureAllocatorForPuzzle(allQ.getColumn(0).getInt64());
    }
}

void PoolService::ensureSingleActivePuzzle() {
    SQLite::Statement q(db_.raw(), "SELECT COUNT(*) FROM puzzles WHERE active = 1");
    q.executeStep();
    int64_t activeCount = q.getColumn(0).getInt64();
    if (activeCount == 0) {
        db_.exec("UPDATE puzzles SET active = 1 WHERE id = (SELECT MIN(id) FROM puzzles)");
    } else if (activeCount > 1) {
        db_.exec("UPDATE puzzles SET active = 0 WHERE id != (SELECT MAX(id) FROM puzzles WHERE active = 1)");
    }
}

void PoolService::ensureAllocators() {
    SQLite::Statement q(db_.raw(), "SELECT id FROM puzzles");
    while (q.executeStep()) {
        allocator_.ensureAllocatorForPuzzle(q.getColumn(0).getInt64());
    }
}

bool PoolService::upsertWorkerAndDetectReactivation(
    const std::string& name, double hashrate, const std::string& version,
    const std::optional<std::string>& minChunkKeys,
    const std::optional<std::string>& chunkQuantumKeys) {
    SQLite::Statement prev(db_.raw(),
        "SELECT CASE WHEN last_seen < datetime('now', ?) THEN 1 ELSE 0 END FROM workers WHERE name = ?");
    prev.bind(1, "-" + std::to_string(cfg_.reactivateMinutes) + " minutes");
    prev.bind(2, name);
    bool inactive = prev.executeStep() && prev.getColumn(0).getInt() == 1;

    SQLite::Statement up(db_.raw(), R"SQL(
        INSERT INTO workers (name, hashrate, last_seen, version, min_chunk_keys, chunk_quantum_keys)
        VALUES (?, ?, CURRENT_TIMESTAMP, ?, ?, ?)
        ON CONFLICT(name) DO UPDATE SET
            hashrate = excluded.hashrate,
            last_seen = CURRENT_TIMESTAMP,
            version = COALESCE(excluded.version, workers.version),
            min_chunk_keys = COALESCE(excluded.min_chunk_keys, workers.min_chunk_keys),
            chunk_quantum_keys = COALESCE(excluded.chunk_quantum_keys, workers.chunk_quantum_keys)
    )SQL");
    up.bind(1, name);
    up.bind(2, hashrate);
    if (!version.empty()) up.bind(3, version); else up.bind(3);
    if (minChunkKeys)     up.bind(4, *minChunkKeys);     else up.bind(4);
    if (chunkQuantumKeys) up.bind(5, *chunkQuantumKeys); else up.bind(5);
    up.exec();
    return inactive;
}

std::optional<ChunkRow> PoolService::existingAssignedChunk(const std::string& name, int64_t puzzleId) {
    SQLite::Statement q(db_.raw(), R"SQL(
        SELECT id, puzzle_id, start_hex, end_hex, status, worker_name, prev_worker_name,
               assigned_at, heartbeat_at, is_test, sector_id, vchunk_start, vchunk_end, alloc_generation
        FROM chunks WHERE worker_name = ? AND puzzle_id = ? AND status = 'assigned' LIMIT 1
    )SQL");
    q.bind(1, name);
    q.bind(2, puzzleId);
    if (!q.executeStep()) return std::nullopt;
    return readChunk(q);
}

std::optional<ChunkRow> PoolService::reclaimChunk(const std::string& name, int64_t puzzleId) {
    SQLite::Statement q(db_.raw(), R"SQL(
        UPDATE chunks
        SET status = 'assigned', worker_name = ?, assigned_at = CURRENT_TIMESTAMP, heartbeat_at = CURRENT_TIMESTAMP
        WHERE id = (
            SELECT id FROM chunks WHERE status = 'reclaimed' AND puzzle_id = ? AND is_test = 0 ORDER BY id ASC LIMIT 1
        ) RETURNING id, puzzle_id, start_hex, end_hex, status, worker_name, prev_worker_name,
                    assigned_at, heartbeat_at, is_test, sector_id, vchunk_start, vchunk_end, alloc_generation
    )SQL");
    q.bind(1, name);
    q.bind(2, puzzleId);
    if (!q.executeStep()) return std::nullopt;
    return readChunk(q);
}

std::optional<ChunkRow> PoolService::claimTestChunk(const std::string& name, const PuzzleRow& puzzle) {
    SQLite::Statement taken(db_.raw(),
        "SELECT id FROM chunks WHERE puzzle_id = ? AND start_hex = ? AND end_hex = ? AND is_test = 1 AND status = 'assigned' LIMIT 1");
    taken.bind(1, puzzle.id);
    taken.bind(2, puzzle.testStartHex);
    taken.bind(3, puzzle.testEndHex);
    if (taken.executeStep()) return std::nullopt;

    SQLite::Statement rec(db_.raw(), R"SQL(
        UPDATE chunks
        SET status = 'assigned', worker_name = ?, assigned_at = CURRENT_TIMESTAMP, heartbeat_at = CURRENT_TIMESTAMP
        WHERE id = (
            SELECT id FROM chunks
            WHERE puzzle_id = ? AND start_hex = ? AND end_hex = ? AND is_test = 1 AND status = 'reclaimed'
            LIMIT 1
        ) RETURNING id, puzzle_id, start_hex, end_hex, status, worker_name, prev_worker_name,
                    assigned_at, heartbeat_at, is_test, sector_id, vchunk_start, vchunk_end, alloc_generation
    )SQL");
    rec.bind(1, name);
    rec.bind(2, puzzle.id);
    rec.bind(3, puzzle.testStartHex);
    rec.bind(4, puzzle.testEndHex);
    if (rec.executeStep()) return readChunk(rec);

    SQLite::Statement ins(db_.raw(), R"SQL(
        INSERT INTO chunks (
            puzzle_id, start_hex, end_hex, status, worker_name,
            assigned_at, heartbeat_at, is_test, alloc_generation
        ) VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 1, 'test')
    )SQL");
    ins.bind(1, puzzle.id);
    ins.bind(2, puzzle.testStartHex);
    ins.bind(3, puzzle.testEndHex);
    ins.bind(4, name);
    ins.exec();

    SQLite::Statement fetch(db_.raw(), R"SQL(
        SELECT id, puzzle_id, start_hex, end_hex, status, worker_name, prev_worker_name,
               assigned_at, heartbeat_at, is_test, sector_id, vchunk_start, vchunk_end, alloc_generation
        FROM chunks WHERE id = ?
    )SQL");
    fetch.bind(1, db_.raw().getLastInsertRowid());
    fetch.executeStep();
    return readChunk(fetch);
}

ChunkRow PoolService::readChunk(SQLite::Statement& q) {
    ChunkRow c;
    c.id            = q.getColumn(0).getInt64();
    c.puzzleId      = q.getColumn(1).getInt64();
    c.startHex      = q.getColumn(2).getString();
    c.endHex        = q.getColumn(3).getString();
    c.status        = q.getColumn(4).getString();
    c.workerName    = q.isColumnNull(5) ? "" : q.getColumn(5).getString();
    c.prevWorkerName = q.isColumnNull(6) ? "" : q.getColumn(6).getString();
    c.assignedAt    = q.isColumnNull(7) ? "" : q.getColumn(7).getString();
    c.heartbeatAt   = q.isColumnNull(8) ? "" : q.getColumn(8).getString();
    c.isTest        = q.isColumnNull(9) ? 0  : q.getColumn(9).getInt();
    c.sectorId      = q.isColumnNull(10) ? 0 : q.getColumn(10).getInt64();
    c.vchunkStart   = q.isColumnNull(11) ? -1 : q.getColumn(11).getInt64();
    c.vchunkEnd     = q.isColumnNull(12) ? -1 : q.getColumn(12).getInt64();
    c.allocGeneration = q.isColumnNull(13) ? "" : q.getColumn(13).getString();
    return c;
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
    wq.bind(1, "-" + formatDouble(cfg_.activeMinutes) + " minutes");
    wq.bind(2, puzzle.id);
    wq.bind(3, "-" + formatDouble(cfg_.activeMinutes) + " minutes");
    wq.bind(4, puzzle.id);
    wq.bind(5, "-" + std::to_string(cfg_.timeoutMinutes) + " minutes");
    wq.bind(6, puzzle.id);

    std::map<std::string, json> workerAssignedMap;
    SQLite::Statement aq(db_.raw(), R"SQL(
        SELECT id, worker_name, vchunk_start, vchunk_end, assigned_at, heartbeat_at, start_hex, end_hex
        FROM chunks WHERE status = 'assigned' AND puzzle_id = ? AND is_test = 0
    )SQL");
    aq.bind(1, puzzle.id);
    while (aq.executeStep()) {
        std::string worker = aq.getColumn(1).getString();
        json j;
        j["current_chunk"] = aq.getColumn(0).getInt64();
        if (!aq.isColumnNull(2) && !aq.isColumnNull(3)) {
            auto s = aq.getColumn(2).getInt64();
            auto e = aq.getColumn(3).getInt64();
            j["current_vchunk_run"]       = std::to_string(s) + ".." + std::to_string(e - 1);
            j["current_vchunk_run_start"] = s;
            j["current_vchunk_run_end"]   = e;
        } else {
            j["current_vchunk_run"]       = nullptr;
            j["current_vchunk_run_start"] = nullptr;
            j["current_vchunk_run_end"]   = nullptr;
        }
        j["assigned_at"]         = aq.isColumnNull(4) ? json(nullptr) : json(aq.getColumn(4).getString());
        j["heartbeat_at"]        = aq.isColumnNull(5) ? json(nullptr) : json(aq.getColumn(5).getString());
        j["current_job_start_hex"] = aq.isColumnNull(6) ? json(nullptr) : json(aq.getColumn(6).getString());
        j["current_job_end_hex"]   = aq.isColumnNull(7) ? json(nullptr) : json(aq.getColumn(7).getString());
        if (!aq.isColumnNull(6) && !aq.isColumnNull(7)) {
            j["current_job_keys"] = bigToDec(hexToInt(aq.getColumn(7).getString()) - hexToInt(aq.getColumn(6).getString()));
        } else {
            j["current_job_keys"] = nullptr;
        }
        workerAssignedMap[worker] = j;
    }

    int activeWorkers   = 0;
    int inactiveWorkers = 0;
    while (wq.executeStep()) {
        json w;
        std::string name = wq.getColumn(0).getString();
        w["name"]              = name;
        w["hashrate"]          = wq.isColumnNull(1) ? 0.0 : wq.getColumn(1).getDouble();
        w["last_seen"]         = wq.isColumnNull(2) ? json(nullptr) : json(wq.getColumn(2).getString());
        w["version"]           = wq.isColumnNull(3) ? json(nullptr) : json(wq.getColumn(3).getString());
        w["min_chunk_keys"]    = wq.isColumnNull(4) ? json(nullptr) : json(wq.getColumn(4).getString());
        w["chunk_quantum_keys"] = wq.isColumnNull(5) ? json(nullptr) : json(wq.getColumn(5).getString());
        w["fresh"]             = wq.getColumn(6).getInt() == 1;
        w["assigned_here"]     = wq.getColumn(7).getInt() == 1;
        w["active"]            = wq.getColumn(8).getInt() == 1;
        if (w["active"].get<bool>()) {
            totalHashrate += w["hashrate"].get<double>();
            activeWorkers++;
        } else {
            inactiveWorkers++;
        }
        if (workerAssignedMap.count(name)) {
            for (auto& [k, v] : workerAssignedMap[name].items()) w[k] = v;
        } else {
            w["current_chunk"]         = nullptr;
            w["current_vchunk_run"]    = nullptr;
            w["current_vchunk_run_start"] = nullptr;
            w["current_vchunk_run_end"]   = nullptr;
            w["assigned_at"]           = nullptr;
            w["heartbeat_at"]          = nullptr;
            w["current_job_start_hex"] = nullptr;
            w["current_job_end_hex"]   = nullptr;
            w["current_job_keys"]      = nullptr;
        }
        visibleWorkers.push_back(w);
    }

    out["workers"]               = visibleWorkers;
    out["active_workers_count"]  = activeWorkers;
    out["inactive_workers_count"] = inactiveWorkers;
    out["total_hashrate"]        = totalHashrate;

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
               c.id AS chunk_global,
               c.vchunk_start, c.vchunk_end
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
            {"vchunk_start", fq.isColumnNull(5) ? json(nullptr) : json(fq.getColumn(5).getInt64())},
            {"vchunk_end",   fq.isColumnNull(6) ? json(nullptr) : json(fq.getColumn(6).getInt64())}
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

    int64_t virtualTotal     = 0;
    int64_t virtualStarted   = 0;
    int64_t virtualCompleted = 0;
    std::string strategy = puzzle.allocStrategy.empty() ? cfg_.allocStrategyLegacy : puzzle.allocStrategy;
    if (strategy == cfg_.allocStrategyVChunks) {
        virtualTotal = puzzle.virtualChunkCount;
        SQLite::Statement st(db_.raw(),
            "SELECT COALESCE(SUM(vchunk_end - vchunk_start), 0) FROM chunks WHERE puzzle_id = ? AND is_test = 0 AND vchunk_start IS NOT NULL AND vchunk_end IS NOT NULL");
        st.bind(1, puzzle.id);
        st.executeStep();
        virtualStarted = st.getColumn(0).getInt64();
        SQLite::Statement ct(db_.raw(),
            "SELECT COALESCE(SUM(vchunk_end - vchunk_start), 0) FROM chunks WHERE puzzle_id = ? AND is_test = 0 AND status IN ('completed', 'FOUND') AND vchunk_start IS NOT NULL AND vchunk_end IS NOT NULL");
        ct.bind(1, puzzle.id);
        ct.executeStep();
        virtualCompleted = ct.getColumn(0).getInt64();
    } else {
        virtualTotal     = scalarCount("SELECT COUNT(*) FROM sectors WHERE puzzle_id = ?");
        virtualStarted   = scalarCount("SELECT COUNT(DISTINCT sector_id) FROM chunks WHERE puzzle_id = ? AND is_test = 0 AND sector_id IS NOT NULL");
        virtualCompleted = scalarCount("SELECT COUNT(*) FROM sectors WHERE puzzle_id = ? AND status = 'done'");
    }
    out["virtual_chunks"] = {{"total", virtualTotal}, {"started", virtualStarted}, {"completed", virtualCompleted}};
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

json PoolService::puzzleJson(const PuzzleRow& p) {
    json j;
    j["id"]       = p.id;
    j["name"]     = p.name;
    j["start_hex"] = p.startHex;
    j["end_hex"]   = p.endHex;
    j["active"]    = p.active;
    j["total_keys"] = bigToDec(hexToInt(p.endHex) - hexToInt(p.startHex));
    if (!p.testStartHex.empty())
        j["test_chunk"] = {{"start_hex", p.testStartHex}, {"end_hex", p.testEndHex}};
    else
        j["test_chunk"] = nullptr;
    j["alloc_strategy"]          = p.allocStrategy.empty() ? cfg_.allocStrategyLegacy : p.allocStrategy;
    j["alloc_cursor"]            = p.allocCursor;
    j["virtual_chunk_size_keys"] = p.virtualChunkSizeKeys.empty() ? json(nullptr) : json(p.virtualChunkSizeKeys);
    j["virtual_chunk_count"]     = p.virtualChunkCount > 0 ? json(p.virtualChunkCount) : json(nullptr);
    j["bootstrap_stage"]         = p.bootstrapStage;
    return j;
}

std::string PoolService::formatDouble(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << v;
    std::string s = oss.str();
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

std::string PoolService::nowIsoUtc() {
    auto now  = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

crow::response PoolService::jsonResponse(const json& j, int code) {
    crow::response r;
    r.code = code;
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    r.body = j.dump();
    return r;
}

crow::response PoolService::errorJsonResponse(int code, const json& j) {
    return jsonResponse(j, code);
}

crow::response PoolService::errorResponse(int code, const std::string& msg) {
    return jsonResponse({{"error", msg}}, code);
}

} // namespace puzzpool
