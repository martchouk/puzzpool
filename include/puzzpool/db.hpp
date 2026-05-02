#pragma once

#include <puzzpool/config.hpp>
#include <puzzpool/types.hpp>

#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace puzzpool {

class PoolDb {
public:
    explicit PoolDb(const Config& cfg);

    SQLite::Database& raw();
    const Config&     cfg() const;

    void exec(const std::string& sql);
    void migrate();
    void addColumnIfMissing(const std::string& table, const std::string& spec);

    std::optional<PuzzleRow>             activePuzzle();
    std::optional<PuzzleRow>             puzzleById(int64_t id);
    std::optional<PuzzleRow>             puzzleByName(const std::string& name);
    std::vector<nlohmann::json>          listPuzzles();
    int64_t                              chunkCountNonTest(int64_t puzzleId);
    int64_t                              sectorCount(int64_t puzzleId);

private:
    PuzzleRow readPuzzle(SQLite::Statement& q);

    const Config&    cfg_;
    SQLite::Database db_;
};

} // namespace puzzpool
