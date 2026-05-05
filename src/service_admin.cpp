#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <cstdint>
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
            if (body.contains("virtual_chunk_size_keys") && !body["virtual_chunk_size_keys"].is_null()) {
                if (!body["virtual_chunk_size_keys"].is_string())
                    return errorResponse(400, "virtual_chunk_size_keys must be a decimal string");
                std::string vcsStr = body["virtual_chunk_size_keys"].get<std::string>();
                if (vcsStr.empty() || vcsStr.find_first_not_of("0123456789") != std::string::npos)
                    return errorResponse(400, "virtual_chunk_size_keys must be a positive decimal integer");
                cpp_int vcs = cpp_int(vcsStr);
                if (vcs <= 0)
                    return errorResponse(400, "virtual_chunk_size_keys must be a positive decimal integer");
                vchunkSize = minBig(vcs, puzzleRange);
            }
            else
                vchunkSize = allocator_.chooseDefaultVirtualChunkSize(puzzleRange);
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
            tx.commit();
            allocator_.ensureAllocatorForPuzzle(puzzleId);
        } else {
            SQLite::Statement ins(db_.raw(), R"SQL(
                INSERT INTO puzzles (
                    name, start_hex, end_hex, active,
                    alloc_strategy, alloc_seed, alloc_cursor_hex,
                    virtual_chunk_size_keys, virtual_chunk_count_hex, bootstrap_stage
                ) VALUES (?, ?, ?, 1, ?, ?, ?, ?, NULL, 0)
            )SQL");
            ins.bind(1, name);
            ins.bind(2, startNorm);
            ins.bind(3, endNorm);
            ins.bind(4, strategy);
            ins.bind(5, seed);
            ins.bind(6, intToHex(cpp_int(0), 64));
            if (vchunkSize) ins.bind(7, bigToDec(*vchunkSize)); else ins.bind(7);
            ins.exec();
            puzzleId = db_.raw().getLastInsertRowid();
            tx.commit();
            if (strategy == cfg_.allocStrategyVChunks)
                allocator_.seedVirtualChunks(puzzleId, startNorm, endNorm, seed, *vchunkSize);
            else
                allocator_.seedSectors(puzzleId, startNorm, endNorm);
        }
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
        std::string startHex;
        if (body.contains("start_hex") && body["start_hex"].is_string())
            startHex = body["start_hex"].get<std::string>();
        std::string endHex;
        if (body.contains("end_hex") && body["end_hex"].is_string())
            endHex = body["end_hex"].get<std::string>();

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
    std::shared_lock lock(mu_);
    try {
        SQLite::Statement q(db_.raw(), R"SQL(
            SELECT id, name, active, start_hex, end_hex,
                   alloc_strategy, alloc_seed,
                   alloc_cursor_hex,
                   virtual_chunk_size_keys,
                   virtual_chunk_count_hex,
                   bootstrap_stage
            FROM puzzles
            ORDER BY id ASC
        )SQL");
        json arr = json::array();
        while (q.executeStep()) {
            json cursorJ = q.isColumnNull(7)
                ? json(nullptr)
                : json(bigToDec(hexToInt(q.getColumn(7).getString())));
            json countJ = q.isColumnNull(9)
                ? json(nullptr)
                : json(bigToDec(hexToInt(q.getColumn(9).getString())));
            arr.push_back({
                {"id",                      q.getColumn(0).getInt64()},
                {"name",                    q.getColumn(1).getString()},
                {"active",                  q.getColumn(2).getInt()},
                {"start_hex",               q.getColumn(3).getString()},
                {"end_hex",                 q.getColumn(4).getString()},
                {"alloc_strategy",          q.isColumnNull(5) ? json(nullptr) : json(q.getColumn(5).getString())},
                {"alloc_seed",              q.isColumnNull(6) ? json(nullptr) : json(q.getColumn(6).getString())},
                {"alloc_cursor",            cursorJ},
                {"virtual_chunk_size_keys", q.isColumnNull(8) ? json(nullptr) : json(q.getColumn(8).getString())},
                {"virtual_chunk_count",     countJ},
                {"bootstrap_stage",         q.isColumnNull(10) ? 0 : q.getColumn(10).getInt()}
            });
        }
        return jsonResponse({{"puzzles", arr}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

crow::response PoolService::handleAdminReclaim() {
    try {
        int count = reclaimTimedOutChunks();
        return jsonResponse({{"ok", true}, {"reclaimed", count}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

crow::response PoolService::handleImportRanges(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto body = json::parse(req.body.empty() ? "{}" : req.body);

        if (!body.contains("puzzle_id") || !body["puzzle_id"].is_number_integer())
            return errorResponse(400, "Missing or invalid puzzle_id");
        int64_t puzzleId = body["puzzle_id"].get<int64_t>();

        if (!body.contains("source") || !body["source"].is_string())
            return errorResponse(400, "Missing source");
        std::string source = body["source"].get<std::string>();

        if (!body.contains("base_hex") || !body["base_hex"].is_string())
            return errorResponse(400, "Missing base_hex");
        std::string baseHex = body["base_hex"].get<std::string>();
        if (!isValidHex(baseHex))
            return errorResponse(400, "base_hex must be a valid hex string");
        cpp_int base = hexToInt(normalizeHex(baseHex));

        if (!body.contains("step"))
            return errorResponse(400, "Missing step");
        cpp_int step;
        try {
            if (body["step"].is_string())
                step = cpp_int(body["step"].get<std::string>());
            else if (body["step"].is_number_integer())
                step = cpp_int(body["step"].get<int64_t>());
            else
                return errorResponse(400, "step must be a decimal integer or string");
        } catch (...) {
            return errorResponse(400, "Invalid step value");
        }
        if (step <= 0) return errorResponse(400, "step must be positive");

        if (!body.contains("range_ids") || !body["range_ids"].is_array())
            return errorResponse(400, "Missing range_ids array");

        auto puzzle = db_.puzzleById(puzzleId);
        if (!puzzle) return errorResponse(404, "Puzzle not found");
        if (puzzle->allocStrategy != cfg_.allocStrategyVChunks)
            return errorResponse(400, "Puzzle does not use virtual_random_chunks_v1 strategy");
        if (puzzle->virtualChunkSizeKeys.empty())
            return errorResponse(400, "Puzzle has no virtual_chunk_size_keys");

        cpp_int vchunkSize;
        try { vchunkSize = cpp_int(puzzle->virtualChunkSizeKeys); } catch (...) {
            return errorResponse(500, "Invalid virtual_chunk_size_keys on puzzle");
        }
        if (vchunkSize <= 0) return errorResponse(500, "virtual_chunk_size_keys must be positive");

        cpp_int puzzleStart  = hexToInt(puzzle->startHex);
        cpp_int puzzleEnd    = hexToInt(puzzle->endHex);
        cpp_int totalChunks  = puzzle->virtualChunkCount;
        if (totalChunks <= 0) return errorResponse(400, "Puzzle has no virtual chunk count");

        int64_t inserted = 0, alreadyBlocked = 0, invalid = 0;
        json errors = json::array();

        SQLite::Transaction tx(db_.raw());

        for (const auto& ridJ : body["range_ids"]) {
            cpp_int rangeId;
            try {
                if (ridJ.is_string())
                    rangeId = cpp_int(ridJ.get<std::string>());
                else if (ridJ.is_number_integer())
                    rangeId = cpp_int(ridJ.get<int64_t>());
                else { ++invalid; continue; }
            } catch (...) { ++invalid; continue; }

            cpp_int keyStart = base + rangeId * step;
            cpp_int keyEnd   = keyStart + step;

            if (keyEnd <= puzzleStart || keyStart >= puzzleEnd) { ++invalid; continue; }

            cpp_int clampedStart = maxBig(keyStart, puzzleStart);
            cpp_int clampedEnd   = minBig(keyEnd,   puzzleEnd);

            cpp_int vStart = (clampedStart - puzzleStart) / vchunkSize;
            cpp_int vEnd   = ceilDiv(clampedEnd - puzzleStart, vchunkSize);
            vStart = maxBig(vStart, cpp_int(0));
            vEnd   = minBig(vEnd,   totalChunks);

            if (vEnd <= vStart) { ++invalid; continue; }

            std::string vStartHex = intToHex(vStart, 64);
            std::string vEndHex   = intToHex(vEnd,   64);

            try {
                SQLite::Statement ins(db_.raw(), R"SQL(
                    INSERT OR IGNORE INTO blocked_vchunk_ranges
                        (puzzle_id, start_vchunk, end_vchunk, source)
                    VALUES (?, ?, ?, ?)
                )SQL");
                ins.bind(1, puzzleId);
                ins.bind(2, vStartHex);
                ins.bind(3, vEndHex);
                ins.bind(4, source);
                ins.exec();
                if (db_.raw().getChanges() > 0) ++inserted;
                else ++alreadyBlocked;
            } catch (const std::exception& e) {
                ++invalid;
                errors.push_back(std::string(e.what()));
            }
        }

        tx.commit();
        allocator_.loadBlockedRanges(puzzleId);

        return jsonResponse({
            {"ok",              true},
            {"inserted_ranges", inserted},
            {"already_blocked", alreadyBlocked},
            {"invalid",         invalid},
            {"errors",          errors}
        });
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

} // namespace puzzpool
