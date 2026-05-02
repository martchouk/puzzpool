#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <iostream>
#include <optional>
#include <string>

namespace puzzpool {

using json = nlohmann::json;

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
                alloc_strategy, alloc_seed, alloc_cursor_hex,
                virtual_chunk_size_keys, virtual_chunk_count_hex, bootstrap_stage
            )
            VALUES (?, ?, ?, 0, ?, ?, ?, ?, NULL, 0)
        )SQL");
        ins.bind(1, name);
        ins.bind(2, startNorm);
        ins.bind(3, endNorm);
        ins.bind(4, strategy);
        ins.bind(5, seed);
        ins.bind(6, intToHex(cpp_int(0), 64));
        if (virtualChunkSize.has_value()) ins.bind(7, bigToDec(*virtualChunkSize));
        else ins.bind(7);
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
                alloc_strategy, alloc_seed, alloc_cursor_hex,
                virtual_chunk_size_keys, virtual_chunk_count_hex, bootstrap_stage
            )
            VALUES (?, ?, ?, 1, ?, ?, ?, ?, NULL, 0)
        )SQL");
        ins.bind(1, name);
        ins.bind(2, startHex);
        ins.bind(3, endHex);
        ins.bind(4, strategy);
        ins.bind(5, seed);
        ins.bind(6, intToHex(cpp_int(0), 64));
        if (virtualChunkSize.has_value()) ins.bind(7, bigToDec(*virtualChunkSize));
        else ins.bind(7);
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

} // namespace puzzpool
