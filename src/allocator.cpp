#include <puzzpool/allocator.hpp>
#include <puzzpool/hex_bigint.hpp>
#include <puzzpool/hash_utils.hpp>
#include <puzzpool/permutation.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

namespace puzzpool {

Allocator::Allocator(PoolDb& db) : db_(db), cfg_(db.cfg()) {}

void Allocator::ensureAllocatorForPuzzle(int64_t puzzleId) {
    auto p = db_.puzzleById(puzzleId);
    if (!p) throw std::runtime_error("Puzzle not found");
    const std::string strategy = p->allocStrategy.empty() ? cfg_.allocStrategyLegacy : p->allocStrategy;

    if (strategy == cfg_.allocStrategyVChunks) {
        int64_t issued = db_.chunkCountNonTest(puzzleId);
        const auto rn = normalizedRange(p->startHex, p->endHex);
        cpp_int desiredDefault = chooseDefaultVirtualChunkSize(rn.range);
        cpp_int size = desiredDefault;

        if (!p->virtualChunkSizeKeys.empty()) {
            try {
                cpp_int stored(p->virtualChunkSizeKeys);
                cpp_int actual = minBig(stored, rn.range);
                if (actual > 0) size = actual;
            } catch (...) {}
        }

        cpp_int expectedCount = ceilDiv(rn.range, size);
        if (p->virtualChunkCount <= 0 || p->virtualChunkCount != expectedCount) {
            seedVirtualChunks(puzzleId, p->startHex, p->endHex,
                              defaultAllocSeedForPuzzle(*p, cfg_.allocStrategyVChunks), size);
        } else if (cfg_.autoReseedEmptyVChunkPuzzles && issued == 0 && size != desiredDefault) {
            seedVirtualChunks(puzzleId, p->startHex, p->endHex,
                              defaultAllocSeedForPuzzle(*p, cfg_.allocStrategyVChunks), desiredDefault);
        }
    } else {
        if (db_.sectorCount(puzzleId) == 0) {
            seedSectors(puzzleId, p->startHex, p->endHex);
        }
    }
}

cpp_int Allocator::chooseDefaultVirtualChunkSize(const cpp_int& range) const {
    cpp_int size = cfg_.defaultVirtualChunkSizeKeys > 0 ? cfg_.defaultVirtualChunkSizeKeys : cpp_int(1);
    if (size > range) size = range;
    return size;
}

std::string Allocator::defaultAllocSeedForPuzzle(const PuzzleRow& p, const std::string& strategy) const {
    return sha256Hex(p.name + "|" + p.startHex + "|" + p.endHex + "|" + strategy);
}

void Allocator::seedSectors(int64_t puzzleId, const std::string& startHex, const std::string& endHex) {
    const cpp_int minSectorSize = cpp_int("1000000000");
    auto rn = normalizedRange(startHex, endHex);
    cpp_int numSectors = rn.range / minSectorSize;
    if (numSectors < 1) numSectors = 1;
    if (numSectors > cfg_.targetSectors) numSectors = cfg_.targetSectors;

    cpp_int sectorSize = rn.range / numSectors;
    SQLite::Transaction tx(db_.raw());
    SQLite::Statement ins(db_.raw(),
        "INSERT INTO sectors (puzzle_id, start_hex, end_hex, current_hex, status) VALUES (?, ?, ?, ?, 'open')");

    for (cpp_int i = 0; i < numSectors; ++i) {
        cpp_int s = rn.start + i * sectorSize;
        cpp_int e = (i == numSectors - 1) ? rn.end : s + sectorSize;
        ins.reset();
        ins.bind(1, puzzleId);
        ins.bind(2, intToHex(s));
        ins.bind(3, intToHex(e));
        ins.bind(4, intToHex(s));
        ins.exec();
    }
    tx.commit();
}

void Allocator::seedVirtualChunks(int64_t puzzleId, const std::string& startHex, const std::string& endHex,
                                   const std::string& allocSeed, const cpp_int& virtualChunkSizeKeys) {
    auto rn = normalizedRange(startHex, endHex);
    cpp_int chunkSize = minBig(virtualChunkSizeKeys > 0 ? virtualChunkSizeKeys : chooseDefaultVirtualChunkSize(rn.range), rn.range);
    cpp_int chunkCountBig = ceilDiv(rn.range, chunkSize);

    SQLite::Transaction tx(db_.raw());
    SQLite::Statement q(db_.raw(), R"SQL(
        UPDATE puzzles
        SET alloc_strategy = ?,
            alloc_seed = ?,
            alloc_cursor_hex = ?,
            virtual_chunk_size_keys = ?,
            virtual_chunk_count_hex = ?,
            bootstrap_stage = 0
        WHERE id = ?
    )SQL");
    q.bind(1, cfg_.allocStrategyVChunks);
    q.bind(2, allocSeed);
    q.bind(3, intToHex(cpp_int(0), 64));
    q.bind(4, bigToDec(chunkSize));
    q.bind(5, intToHex(chunkCountBig, 64));
    q.bind(6, puzzleId);
    q.exec();
    tx.commit();
}

std::optional<Allocator::WorkAssignResult>
Allocator::assignLegacyRandomChunk(const std::string& worker,
                                    const std::optional<double>& hashrate,
                                    const PuzzleRow& puzzle) {
    cpp_int hashrateBig = normalizeHashrate(hashrate.value_or(static_cast<double>(readWorkerHashrate(worker))));
    cpp_int chunkSize   = hashrateBig * cpp_int(cfg_.targetMinutes * 60);
    bool isFirst = db_.chunkCountNonTest(puzzle.id) == 0;

    SQLite::Transaction tx(db_.raw());
    SQLite::Statement openSector(db_.raw(),
        "SELECT id, current_hex, end_hex FROM sectors WHERE puzzle_id = ? AND status = 'open' ORDER BY RANDOM() LIMIT 1");
    SQLite::Statement openSectorAt(db_.raw(),
        "SELECT id, current_hex, end_hex FROM sectors WHERE puzzle_id = ? AND status = 'open' ORDER BY id ASC LIMIT 1 OFFSET ?");
    SQLite::Statement sectorDone(db_.raw(),
        "UPDATE sectors SET current_hex = end_hex, status = 'done' WHERE id = ?");
    SQLite::Statement sectorAdvance(db_.raw(),
        "UPDATE sectors SET current_hex = ? WHERE id = ?");
    SQLite::Statement insertChunk(db_.raw(), R"SQL(
        INSERT INTO chunks (
            puzzle_id, start_hex, end_hex, status,
            worker_name, assigned_at, heartbeat_at, is_test,
            sector_id,
            alloc_generation
        ) VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 0, ?, 'legacy')
    )SQL");

    const int midpoint = static_cast<int>(cfg_.targetSectors / 2);
    for (;;) {
        bool got = false;
        int64_t sectorId = 0;
        std::string currentHex, endHex;

        if (isFirst) {
            openSectorAt.reset();
            openSectorAt.bind(1, puzzle.id);
            openSectorAt.bind(2, midpoint);
            if (openSectorAt.executeStep()) {
                got       = true;
                sectorId  = openSectorAt.getColumn(0).getInt64();
                currentHex = openSectorAt.getColumn(1).getString();
                endHex    = openSectorAt.getColumn(2).getString();
            }
        }
        if (!got) {
            openSector.reset();
            openSector.bind(1, puzzle.id);
            if (!openSector.executeStep()) return std::nullopt;
            sectorId   = openSector.getColumn(0).getInt64();
            currentHex = openSector.getColumn(1).getString();
            endHex     = openSector.getColumn(2).getString();
        }

        cpp_int current      = hexToInt(currentHex);
        cpp_int sectorEndBig = hexToInt(endHex);
        cpp_int effective    = minBig(chunkSize, sectorEndBig - current);
        if (effective <= 0) {
            sectorDone.reset();
            sectorDone.bind(1, sectorId);
            sectorDone.exec();
            continue;
        }

        std::string startOut = intToHex(current);
        std::string endOut   = intToHex(current + effective);

        if (current + effective >= sectorEndBig) {
            sectorDone.reset();
            sectorDone.bind(1, sectorId);
            sectorDone.exec();
        } else {
            sectorAdvance.reset();
            sectorAdvance.bind(1, endOut);
            sectorAdvance.bind(2, sectorId);
            sectorAdvance.exec();
        }

        insertChunk.reset();
        insertChunk.bind(1, puzzle.id);
        insertChunk.bind(2, startOut);
        insertChunk.bind(3, endOut);
        insertChunk.bind(4, worker);
        insertChunk.bind(5, sectorId);
        insertChunk.exec();

        int64_t id = db_.raw().getLastInsertRowid();
        tx.commit();
        return WorkAssignResult{id, startOut, endOut};
    }
}

std::optional<Allocator::WorkAssignResult>
Allocator::assignVirtualChunkJob(const std::string& worker,
                                  const std::optional<double>& hashrate,
                                  const std::optional<std::string>& minChunkKeys,
                                  const std::optional<std::string>& chunkQuantumKeys,
                                  const PuzzleRow& puzzle) {
    if (puzzle.virtualChunkSizeKeys.empty() || puzzle.virtualChunkCount <= 0) return std::nullopt;

    const cpp_int totalChunks = puzzle.virtualChunkCount;
    cpp_int hashrateBig = normalizeHashrate(hashrate.value_or(static_cast<double>(readWorkerHashrate(worker))));
    cpp_int minKeys  = minChunkKeys    ? parsePositiveBigInt(*minChunkKeys, 0)    : cpp_int(0);
    cpp_int quantum  = chunkQuantumKeys ? parsePositiveBigInt(*chunkQuantumKeys, 1) : cpp_int(1);
    cpp_int requestedKeys = computeWorkerRequestedKeys(hashrateBig, minKeys, quantum);
    cpp_int vchunkSize(puzzle.virtualChunkSizeKeys);
    cpp_int neededChunks = ceilDiv(requestedKeys, vchunkSize);
    if (neededChunks < 1) neededChunks = 1;
    if (neededChunks > totalChunks) neededChunks = totalChunks;

    int freshChunkCount = static_cast<int>(db_.chunkCountNonTest(puzzle.id));
    int stage = puzzle.bootstrapStage;

    SQLite::Transaction tx(db_.raw());

    if (freshChunkCount < 3 && stage < 3) {
        if (auto r = assignBootstrap(worker, puzzle, totalChunks, neededChunks, stage)) {
            tx.commit();
            return r;
        }
        advanceBootstrapStage(puzzle.id, stage + 1);
    }

    const std::string permKey = puzzle.allocSeed.empty()
        ? defaultAllocSeedForPuzzle(puzzle, cfg_.allocStrategyVChunks)
        : puzzle.allocSeed;
    const cpp_int rawCursorBig = puzzle.allocCursor;
    int64_t probeLimit = static_cast<int64_t>(std::min(totalChunks, cpp_int(cfg_.maxAllocProbes)));

    std::optional<AffineParams> affine;
    if (cfg_.permutationMode == "affine") affine = deriveAffinePermutationParams(permKey, totalChunks);

    std::set<cpp_int> triedStarts;
    for (int64_t offset = 0; offset < probeLimit; ++offset) {
        cpp_int orderIndex    = (rawCursorBig + offset) % totalChunks;
        cpp_int candidateIndex = (cfg_.permutationMode == "affine")
            ? permuteIndexAffine(orderIndex, totalChunks, affine->a, affine->b)
            : permuteIndexFeistel(orderIndex, totalChunks, permKey);
        cpp_int runStart = normalizeRunStartForCandidate(candidateIndex, neededChunks, totalChunks);
        if (!triedStarts.insert(runStart).second) continue;
        if (rangeIsFree(puzzle.id, runStart, runStart + neededChunks)) {
            cpp_int nextCursor = (orderIndex + 1) % totalChunks;
            setAllocCursor(puzzle.id, nextCursor);
            auto res = assignVirtualChunkRun(worker, puzzle, runStart, neededChunks, cfg_.permutationMode);
            if (stage < 3) advanceBootstrapStage(puzzle.id, 3);
            tx.commit();
            return res;
        }
    }

    for (cpp_int fallback = neededChunks - 1; fallback >= 1; --fallback) {
        triedStarts.clear();
        for (int64_t offset = 0; offset < probeLimit; ++offset) {
            cpp_int orderIndex    = (rawCursorBig + offset) % totalChunks;
            cpp_int candidateIndex = (cfg_.permutationMode == "affine")
                ? permuteIndexAffine(orderIndex, totalChunks, affine->a, affine->b)
                : permuteIndexFeistel(orderIndex, totalChunks, permKey);
            cpp_int runStart = normalizeRunStartForCandidate(candidateIndex, fallback, totalChunks);
            if (!triedStarts.insert(runStart).second) continue;
            if (rangeIsFree(puzzle.id, runStart, runStart + fallback)) {
                cpp_int nextCursor = (orderIndex + 1) % totalChunks;
                setAllocCursor(puzzle.id, nextCursor);
                auto res = assignVirtualChunkRun(worker, puzzle, runStart, fallback, cfg_.permutationMode);
                if (stage < 3) advanceBootstrapStage(puzzle.id, 3);
                tx.commit();
                return res;
            }
        }
    }

    return std::nullopt;
}

cpp_int Allocator::normalizeHashrate(double v) const {
    if (!std::isfinite(v) || v <= 0) return 1000000;
    return static_cast<uint64_t>(std::max(1.0, std::floor(v)));
}

cpp_int Allocator::computeWorkerRequestedKeys(const cpp_int& hashrateBig,
                                               const cpp_int& minChunkKeys,
                                               const cpp_int& chunkQuantumKeys) const {
    cpp_int targetKeys = hashrateBig * cpp_int(cfg_.targetMinutes * 60);
    cpp_int minKeys    = minChunkKeys > 0 ? minChunkKeys : targetKeys;
    cpp_int raw        = maxBig(targetKeys, minKeys);
    cpp_int quantum    = chunkQuantumKeys > 0 ? chunkQuantumKeys : cpp_int(1);
    return quantum <= 1 ? raw : ceilDiv(raw, quantum) * quantum;
}

// ─── private ─────────────────────────────────────────────────────────────────

int64_t Allocator::readWorkerHashrate(const std::string& worker) {
    SQLite::Statement q(db_.raw(), "SELECT hashrate FROM workers WHERE name = ?");
    q.bind(1, worker);
    if (!q.executeStep() || q.isColumnNull(0)) return 1000000;
    return static_cast<int64_t>(std::max(1.0, q.getColumn(0).getDouble()));
}

void Allocator::setAllocCursor(int64_t puzzleId, const cpp_int& cursor) {
    SQLite::Statement q(db_.raw(), R"SQL(
        UPDATE puzzles SET alloc_cursor_hex = ?
        WHERE id = ?
    )SQL");
    q.bind(1, intToHex(cursor, 64));
    q.bind(2, puzzleId);
    q.exec();
}

void Allocator::advanceBootstrapStage(int64_t puzzleId, int stage) {
    SQLite::Statement q(db_.raw(), "UPDATE puzzles SET bootstrap_stage = ? WHERE id = ?");
    q.bind(1, stage);
    q.bind(2, puzzleId);
    q.exec();
}

bool Allocator::rangeIsFree(int64_t puzzleId, const cpp_int& start, const cpp_int& endExclusive) {
    if (start < 0 || endExclusive <= start) return false;
    std::string startHexPad = intToHex(start, 64);
    std::string endHexPad   = intToHex(endExclusive, 64);
    SQLite::Statement q(db_.raw(), R"SQL(
        SELECT 1
        FROM chunks
        WHERE puzzle_id = ?
          AND is_test = 0
          AND status IN ('assigned', 'reclaimed', 'completed', 'FOUND')
          AND vchunk_start_hex IS NOT NULL
          AND vchunk_end_hex IS NOT NULL
          AND vchunk_start_hex < ?
          AND vchunk_end_hex > ?
        LIMIT 1
    )SQL");
    q.bind(1, puzzleId);
    q.bind(2, endHexPad);
    q.bind(3, startHexPad);
    return !q.executeStep();
}

cpp_int Allocator::normalizeRunStartForCandidate(const cpp_int& candidateIndex, const cpp_int& neededChunks, const cpp_int& totalChunks) {
    if (neededChunks >= totalChunks) return cpp_int(0);
    cpp_int start = candidateIndex;
    if (start + neededChunks > totalChunks) start = totalChunks - neededChunks;
    if (start < 0) start = 0;
    return start;
}

std::optional<Allocator::WorkAssignResult>
Allocator::assignBootstrap(const std::string& worker, const PuzzleRow& puzzle,
                            const cpp_int& totalChunks, const cpp_int& neededChunks, int stage) {
    std::optional<cpp_int> runStart;
    if      (stage == 0) runStart = findMidBootstrapRun(puzzle.id, totalChunks, neededChunks);
    else if (stage == 1) runStart = findBeginBootstrapRun(puzzle.id, totalChunks, neededChunks);
    else if (stage == 2) runStart = findEndBootstrapRun(puzzle.id, totalChunks, neededChunks);

    if (runStart) {
        auto res = assignVirtualChunkRun(worker, puzzle, *runStart, neededChunks, cfg_.permutationMode);
        advanceBootstrapStage(puzzle.id, stage + 1);
        return res;
    }

    for (cpp_int fallback = neededChunks - 1; fallback >= 1; --fallback) {
        if      (stage == 0) runStart = findMidBootstrapRun(puzzle.id, totalChunks, fallback);
        else if (stage == 1) runStart = findBeginBootstrapRun(puzzle.id, totalChunks, fallback);
        else if (stage == 2) runStart = findEndBootstrapRun(puzzle.id, totalChunks, fallback);
        if (runStart) {
            auto res = assignVirtualChunkRun(worker, puzzle, *runStart, fallback, cfg_.permutationMode);
            advanceBootstrapStage(puzzle.id, stage + 1);
            return res;
        }
    }
    return std::nullopt;
}

std::optional<cpp_int> Allocator::findBeginBootstrapRun(int64_t puzzleId, const cpp_int& totalChunks, const cpp_int& neededChunks) {
    if (neededChunks > totalChunks) return std::nullopt;
    cpp_int maxStart = totalChunks - neededChunks;
    int64_t probes = static_cast<int64_t>(minBig(cpp_int(maxStart + 1), cpp_int(cfg_.maxAllocProbes)));
    for (int64_t i = 0; i < probes; ++i) {
        cpp_int start = cpp_int(i);
        if (rangeIsFree(puzzleId, start, start + neededChunks)) return start;
    }
    return std::nullopt;
}

std::optional<cpp_int> Allocator::findEndBootstrapRun(int64_t puzzleId, const cpp_int& totalChunks, const cpp_int& neededChunks) {
    if (neededChunks > totalChunks) return std::nullopt;
    cpp_int maxStart = totalChunks - neededChunks;
    int64_t probes = static_cast<int64_t>(minBig(cpp_int(maxStart + 1), cpp_int(cfg_.maxAllocProbes)));
    for (int64_t i = 0; i < probes; ++i) {
        cpp_int start = maxStart - i;
        if (rangeIsFree(puzzleId, start, start + neededChunks)) return start;
    }
    return std::nullopt;
}

std::optional<cpp_int> Allocator::findMidBootstrapRun(int64_t puzzleId, const cpp_int& totalChunks, const cpp_int& neededChunks) {
    if (totalChunks <= 0) return std::nullopt;
    if (neededChunks > totalChunks) return std::nullopt;
    cpp_int anchor   = totalChunks / 2;
    cpp_int maxStart = totalChunks - neededChunks;
    std::set<cpp_int> tried;
    int64_t probes = static_cast<int64_t>(minBig(totalChunks, cpp_int(cfg_.maxAllocProbes)));
    for (int64_t dist = 0; dist < probes; ++dist) {
        cpp_int cdist(dist);
        if (anchor >= cdist) {
            cpp_int left  = anchor - cdist;
            cpp_int start = normalizeRunStartForCandidate(left, neededChunks, totalChunks);
            if (tried.insert(start).second && rangeIsFree(puzzleId, start, start + neededChunks)) return start;
        }
        if (dist == 0) continue;
        cpp_int right = anchor + cdist;
        if (right < totalChunks) {
            cpp_int start = normalizeRunStartForCandidate(right, neededChunks, totalChunks);
            if (tried.insert(start).second && rangeIsFree(puzzleId, start, start + neededChunks)) return start;
        }
    }
    return std::nullopt;
}

Allocator::WorkAssignResult
Allocator::assignVirtualChunkRun(const std::string& worker, const PuzzleRow& puzzle,
                                  const cpp_int& runStart, const cpp_int& runCount, const std::string& generation) {
    cpp_int runEnd = runStart + runCount;
    auto span = virtualChunkRangeToHex(puzzle, runStart, runEnd);

    SQLite::Statement ins(db_.raw(), R"SQL(
        INSERT INTO chunks (
            puzzle_id, start_hex, end_hex, status,
            worker_name, assigned_at, heartbeat_at, is_test,
            sector_id,
            vchunk_start_hex, vchunk_end_hex,
            alloc_generation
        ) VALUES (?, ?, ?, 'assigned', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 0, NULL, ?, ?, ?)
    )SQL");
    ins.bind(1, puzzle.id);
    ins.bind(2, span.first);
    ins.bind(3, span.second);
    ins.bind(4, worker);
    ins.bind(5, intToHex(runStart, 64));
    ins.bind(6, intToHex(runEnd, 64));
    ins.bind(7, generation);
    ins.exec();
    return WorkAssignResult{db_.raw().getLastInsertRowid(), span.first, span.second};
}

std::pair<std::string, std::string>
Allocator::virtualChunkRangeToHex(const PuzzleRow& puzzle, const cpp_int& start, const cpp_int& endExclusive) {
    cpp_int pStart = hexToInt(puzzle.startHex);
    cpp_int pEnd   = hexToInt(puzzle.endHex);
    cpp_int size(puzzle.virtualChunkSizeKeys);
    cpp_int absStart = pStart + start * size;
    cpp_int absEnd   = minBig(pStart + endExclusive * size, pEnd);
    return {intToHex(absStart), intToHex(absEnd)};
}

} // namespace puzzpool
