#pragma once

#include <puzzpool/allocator.hpp>
#include <puzzpool/config.hpp>
#include <puzzpool/db.hpp>
#include <puzzpool/types.hpp>

#include <string>

namespace puzzpool::test {

// Returns a Config wired for in-memory SQLite (no filesystem I/O).
inline Config memConfig() {
    Config cfg;
    cfg.dbPath            = ":memory:";
    cfg.timeoutMinutes    = 15;
    cfg.reactivateMinutes = 15;
    cfg.targetMinutes     = 5;
    cfg.targetSectors     = 64;
    cfg.maxAllocProbes    = 8192;
    cfg.defaultVirtualChunkSizeKeys = cpp_int("1000");
    return cfg;
}

// Inserts a puzzle row and returns its id.
inline int64_t insertPuzzle(PoolDb& db,
    const std::string& start = "0000000000000000000000000000000000000000000000000000000000000001",
    const std::string& end   = "00000000000000000000000000000000000000000000000000000000000f4240",
    const std::string& strategy = "legacy_random_shards_v1") {
    db.exec("UPDATE puzzles SET active = 0");
    SQLite::Statement ins(db.raw(),
        "INSERT INTO puzzles (name, start_hex, end_hex, active, alloc_strategy, alloc_seed, alloc_cursor_hex)"
        " VALUES ('test', ?, ?, 1, ?, 'seed', '0000000000000000000000000000000000000000000000000000000000000000')");
    ins.bind(1, start);
    ins.bind(2, end);
    ins.bind(3, strategy);
    ins.exec();
    return db.raw().getLastInsertRowid();
}

// Inserts a chunk directly; returns the chunk id.
inline int64_t insertChunk(PoolDb& db, int64_t puzzleId,
    const std::string& start, const std::string& end,
    const std::string& status, const std::string& worker = "") {
    SQLite::Statement ins(db.raw(),
        "INSERT INTO chunks (puzzle_id, start_hex, end_hex, status, worker_name, assigned_at, heartbeat_at, is_test)"
        " VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 0)");
    ins.bind(1, puzzleId);
    ins.bind(2, start);
    ins.bind(3, end);
    ins.bind(4, status);
    if (worker.empty()) ins.bind(5); else ins.bind(5, worker);
    ins.exec();
    return db.raw().getLastInsertRowid();
}

} // namespace puzzpool::test
