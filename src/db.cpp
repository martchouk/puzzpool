#include <puzzpool/db.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <cstdint>
#include <string>

namespace puzzpool {

PoolDb::PoolDb(const Config& cfg)
    : cfg_(cfg), db_(cfg.dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=OFF");
    migrate();
}

SQLite::Database& PoolDb::raw() { return db_; }
const Config&     PoolDb::cfg() const { return cfg_; }

void PoolDb::exec(const std::string& sql) {
    db_.exec(sql);
}

void PoolDb::migrate() {
    exec(R"SQL(
      CREATE TABLE IF NOT EXISTS puzzles (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL,
        start_hex TEXT NOT NULL,
        end_hex TEXT NOT NULL,
        active INTEGER NOT NULL DEFAULT 0,
        test_start_hex TEXT,
        test_end_hex TEXT,
        alloc_strategy TEXT,
        alloc_seed TEXT,
        alloc_cursor_hex TEXT,
        virtual_chunk_size_keys TEXT,
        virtual_chunk_count_hex TEXT,
        bootstrap_stage INTEGER NOT NULL DEFAULT 0
      );

      CREATE TABLE IF NOT EXISTS workers (
        name TEXT PRIMARY KEY,
        hashrate REAL,
        last_seen DATETIME DEFAULT CURRENT_TIMESTAMP,
        version TEXT,
        min_chunk_keys TEXT,
        chunk_quantum_keys TEXT
      );

      CREATE TABLE IF NOT EXISTS chunks (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        puzzle_id INTEGER,
        start_hex TEXT,
        end_hex TEXT,
        status TEXT,
        worker_name TEXT,
        prev_worker_name TEXT,
        assigned_at DATETIME,
        heartbeat_at DATETIME,
        found_key TEXT,
        found_address TEXT,
        is_test INTEGER NOT NULL DEFAULT 0,
        sector_id INTEGER,
        vchunk_start_hex TEXT,
        vchunk_end_hex TEXT,
        alloc_generation TEXT
      );

      CREATE INDEX IF NOT EXISTS idx_chunks_puzzle_status ON chunks (puzzle_id, status);
      CREATE INDEX IF NOT EXISTS idx_chunks_vchunk_hex_span ON chunks (puzzle_id, vchunk_start_hex, vchunk_end_hex, status);

      CREATE TABLE IF NOT EXISTS sectors (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        puzzle_id INTEGER NOT NULL,
        start_hex TEXT NOT NULL,
        end_hex TEXT NOT NULL,
        current_hex TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'open'
      );

      CREATE INDEX IF NOT EXISTS idx_sectors_puzzle_status ON sectors (puzzle_id, status);
      CREATE INDEX IF NOT EXISTS idx_sectors_puzzle_id ON sectors (puzzle_id, id);
      CREATE UNIQUE INDEX IF NOT EXISTS idx_sectors_unique_span ON sectors (puzzle_id, start_hex, end_hex);

      CREATE TABLE IF NOT EXISTS findings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        chunk_id INTEGER NOT NULL,
        worker_name TEXT NOT NULL,
        found_key TEXT NOT NULL,
        found_address TEXT,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
      );

      CREATE UNIQUE INDEX IF NOT EXISTS idx_findings_dedup ON findings (chunk_id, worker_name, found_key);
    )SQL");

    addColumnIfMissing("puzzles", "test_start_hex TEXT");
    addColumnIfMissing("puzzles", "test_end_hex TEXT");
    addColumnIfMissing("puzzles", "alloc_strategy TEXT");
    addColumnIfMissing("puzzles", "alloc_seed TEXT");
    addColumnIfMissing("puzzles", "alloc_cursor_hex TEXT");
    addColumnIfMissing("puzzles", "virtual_chunk_size_keys TEXT");
    addColumnIfMissing("puzzles", "virtual_chunk_count_hex TEXT");
    addColumnIfMissing("puzzles", "bootstrap_stage INTEGER NOT NULL DEFAULT 0");

    addColumnIfMissing("workers", "version TEXT");
    addColumnIfMissing("workers", "min_chunk_keys TEXT");
    addColumnIfMissing("workers", "chunk_quantum_keys TEXT");

    addColumnIfMissing("chunks", "is_test INTEGER NOT NULL DEFAULT 0");
    addColumnIfMissing("chunks", "prev_worker_name TEXT");
    addColumnIfMissing("chunks", "vchunk_start_hex TEXT");
    addColumnIfMissing("chunks", "vchunk_end_hex TEXT");
    addColumnIfMissing("chunks", "heartbeat_at DATETIME");
    addColumnIfMissing("chunks", "alloc_generation TEXT");

    exec("CREATE INDEX IF NOT EXISTS idx_chunks_vchunk_hex_span ON chunks (puzzle_id, vchunk_start_hex, vchunk_end_hex, status)");
    exec("UPDATE chunks SET heartbeat_at = assigned_at WHERE heartbeat_at IS NULL AND assigned_at IS NOT NULL");
    exec("UPDATE puzzles SET alloc_strategy = 'legacy_random_shards_v1' WHERE alloc_strategy IS NULL");
    exec(R"SQL(
        UPDATE chunks SET is_test = 1
        WHERE is_test = 0
          AND EXISTS (
            SELECT 1 FROM puzzles p
            WHERE p.id = chunks.puzzle_id
              AND p.test_start_hex IS NOT NULL
              AND chunks.start_hex = p.test_start_hex
              AND chunks.end_hex = p.test_end_hex
          )
    )SQL");
}

void PoolDb::addColumnIfMissing(const std::string& table, const std::string& spec) {
    auto col = spec.substr(0, spec.find(' '));
    SQLite::Statement q(db_, "PRAGMA table_info(" + table + ")");
    while (q.executeStep()) {
        if (q.getColumn(1).getString() == col) return;
    }
    exec("ALTER TABLE " + table + " ADD COLUMN " + spec);
}

std::optional<PuzzleRow> PoolDb::activePuzzle() {
    SQLite::Statement q(db_, "SELECT * FROM puzzles WHERE active = 1 LIMIT 1");
    if (!q.executeStep()) return std::nullopt;
    return readPuzzle(q);
}

std::optional<PuzzleRow> PoolDb::puzzleById(int64_t id) {
    SQLite::Statement q(db_, "SELECT * FROM puzzles WHERE id = ?");
    q.bind(1, id);
    if (!q.executeStep()) return std::nullopt;
    return readPuzzle(q);
}

std::optional<PuzzleRow> PoolDb::puzzleByName(const std::string& name) {
    SQLite::Statement q(db_, "SELECT * FROM puzzles WHERE name = ? LIMIT 1");
    q.bind(1, name);
    if (!q.executeStep()) return std::nullopt;
    return readPuzzle(q);
}

std::vector<nlohmann::json> PoolDb::listPuzzles() {
    std::vector<nlohmann::json> out;
    SQLite::Statement q(db_, "SELECT id, name, active FROM puzzles ORDER BY id ASC");
    while (q.executeStep()) {
        out.push_back({
            {"id",     q.getColumn(0).getInt64()},
            {"name",   q.getColumn(1).getString()},
            {"active", q.getColumn(2).getInt()}
        });
    }
    return out;
}

int64_t PoolDb::chunkCountNonTest(int64_t puzzleId) {
    SQLite::Statement q(db_, "SELECT COUNT(*) FROM chunks WHERE puzzle_id = ? AND is_test = 0");
    q.bind(1, puzzleId);
    q.executeStep();
    return q.getColumn(0).getInt64();
}

int64_t PoolDb::sectorCount(int64_t puzzleId) {
    SQLite::Statement q(db_, "SELECT COUNT(*) FROM sectors WHERE puzzle_id = ?");
    q.bind(1, puzzleId);
    q.executeStep();
    return q.getColumn(0).getInt64();
}

PuzzleRow PoolDb::readPuzzle(SQLite::Statement& q) {
    auto getStr = [&](const char* name) -> std::string {
        int idx = q.getColumnIndex(name);
        if (idx < 0 || q.isColumnNull(idx)) return "";
        return q.getColumn(idx).getString();
    };
    auto getI64 = [&](const char* name) -> int64_t {
        int idx = q.getColumnIndex(name);
        if (idx < 0 || q.isColumnNull(idx)) return 0;
        return q.getColumn(idx).getInt64();
    };
    auto getI = [&](const char* name) -> int {
        int idx = q.getColumnIndex(name);
        if (idx < 0 || q.isColumnNull(idx)) return 0;
        return q.getColumn(idx).getInt();
    };

    PuzzleRow p;
    p.id                  = getI64("id");
    p.name                = getStr("name");
    p.startHex            = getStr("start_hex");
    p.endHex              = getStr("end_hex");
    p.active              = getI("active");
    p.testStartHex        = getStr("test_start_hex");
    p.testEndHex          = getStr("test_end_hex");
    p.allocStrategy       = getStr("alloc_strategy");
    p.allocSeed           = getStr("alloc_seed");
    std::string cursorHex = getStr("alloc_cursor_hex");
    p.allocCursor = cursorHex.empty() ? cpp_int(0) : hexToInt(cursorHex);

    p.virtualChunkSizeKeys = getStr("virtual_chunk_size_keys");

    std::string countHex = getStr("virtual_chunk_count_hex");
    p.virtualChunkCount = countHex.empty() ? cpp_int(0) : hexToInt(countHex);
    p.bootstrapStage      = getI("bootstrap_stage");
    return p;
}

} // namespace puzzpool
