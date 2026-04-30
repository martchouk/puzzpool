#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace puzzpool {

using json = nlohmann::json;

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
        if (strategy == cfg_.allocStrategyVChunks)
            result = allocator_.assignVirtualChunkJob(name, hashrate, minChunkKeys, chunkQuantumKeys, *puzzle);
        else
            result = allocator_.assignLegacyRandomChunk(name, hashrate, *puzzle);

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
    c.id             = q.getColumn(0).getInt64();
    c.puzzleId       = q.getColumn(1).getInt64();
    c.startHex       = q.getColumn(2).getString();
    c.endHex         = q.getColumn(3).getString();
    c.status         = q.getColumn(4).getString();
    c.workerName     = q.isColumnNull(5) ? "" : q.getColumn(5).getString();
    c.prevWorkerName = q.isColumnNull(6) ? "" : q.getColumn(6).getString();
    c.assignedAt     = q.isColumnNull(7) ? "" : q.getColumn(7).getString();
    c.heartbeatAt    = q.isColumnNull(8) ? "" : q.getColumn(8).getString();
    c.isTest         = q.isColumnNull(9)  ? 0  : q.getColumn(9).getInt();
    c.sectorId       = q.isColumnNull(10) ? 0  : q.getColumn(10).getInt64();
    c.vchunkStart    = q.isColumnNull(11) ? -1 : q.getColumn(11).getInt64();
    c.vchunkEnd      = q.isColumnNull(12) ? -1 : q.getColumn(12).getInt64();
    c.allocGeneration = q.isColumnNull(13) ? "" : q.getColumn(13).getString();
    return c;
}

} // namespace puzzpool
