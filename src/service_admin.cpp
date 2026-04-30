#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace puzzpool {

using json = nlohmann::json;

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
            if (body.contains("virtual_chunk_size_keys") && !body["virtual_chunk_size_keys"].is_null())
                vchunkSize = minBig(cpp_int(body["virtual_chunk_size_keys"].get<std::string>()), puzzleRange);
            else
                vchunkSize = allocator_.chooseDefaultVirtualChunkSize(puzzleRange);
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
                {"id",                      q.getColumn(0).getInt64()},
                {"name",                    q.getColumn(1).getString()},
                {"active",                  q.getColumn(2).getInt()},
                {"start_hex",               q.getColumn(3).getString()},
                {"end_hex",                 q.getColumn(4).getString()},
                {"alloc_strategy",          q.isColumnNull(5) ? json(nullptr) : json(q.getColumn(5).getString())},
                {"alloc_seed",              q.isColumnNull(6) ? json(nullptr) : json(q.getColumn(6).getString())},
                {"alloc_cursor",            q.isColumnNull(7) ? json(nullptr) : json(q.getColumn(7).getInt64())},
                {"virtual_chunk_size_keys", q.isColumnNull(8) ? json(nullptr) : json(q.getColumn(8).getString())},
                {"virtual_chunk_count",     q.isColumnNull(9) ? json(nullptr) : json(q.getColumn(9).getInt64())},
                {"bootstrap_stage",         q.isColumnNull(10) ? 0 : q.getColumn(10).getInt()}
            });
        }
        return jsonResponse({{"puzzles", arr}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

} // namespace puzzpool
