#include <puzzpool/service.hpp>
#include <puzzpool/puzzle_status.hpp>

#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace puzzpool {

using json = nlohmann::json;

namespace {

std::string joinUrlSegment(const std::string& base, const std::string& segment) {
    if (base.empty()) return segment;
    if (base.back() == '/') return base + segment;
    return base + "/" + segment;
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    out.reserve(value.size() + 2);
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

std::optional<json> fetchAddressStatusJson(const std::string& apiBase, const std::string& address) {
    const std::string url = joinUrlSegment(apiBase, address);
    const std::string cmd = "curl -fsSL --max-time 15 " + shellQuote(url);

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }
    const int rc = pclose(pipe);
    if (rc != 0 || output.empty()) return std::nullopt;

    try {
        return json::parse(output);
    } catch (...) {
        return std::nullopt;
    }
}

std::string currentStatusLink(const Config& cfg, const std::string& targetType, const std::string& targetValue) {
    if (targetType == "address" && !targetValue.empty()) return joinUrlSegment(cfg.blockExplorerUrl, targetValue);
    return "";
}

} // namespace

void PoolService::syncConfiguredPuzzleTargets() {
    SQLite::Statement q(db_.raw(), "SELECT id, name FROM puzzles");
    while (q.executeStep()) {
        const int64_t id = q.getColumn(0).getInt64();
        const std::string puzzleName = q.getColumn(1).getString();
        const auto it = cfg_.puzzleStatusTargets.find(canonicalPuzzleName(puzzleName));

        SQLite::Statement up(db_.raw(), R"SQL(
            UPDATE puzzles
            SET status_target_type = ?,
                status_target_value = ?,
                status_link = ?,
                status_state = CASE WHEN ? = 1 THEN NULL ELSE status_state END,
                status_checked_at = CASE WHEN ? = 1 THEN NULL ELSE status_checked_at END,
                status_note = CASE WHEN ? = 1 THEN NULL ELSE status_note END
            WHERE id = ?
        )SQL");

        const bool clearTarget = (it == cfg_.puzzleStatusTargets.end());
        if (clearTarget) {
            up.bind(1);
            up.bind(2);
            up.bind(3);
        } else {
            const std::string type = puzzleStatusTargetTypeToString(it->second.type);
            up.bind(1, type);
            up.bind(2, it->second.value);
            const std::string link = currentStatusLink(cfg_, type, it->second.value);
            if (link.empty()) up.bind(3);
            else up.bind(3, link);
        }
        up.bind(4, clearTarget ? 1 : 0);
        up.bind(5, clearTarget ? 1 : 0);
        up.bind(6, clearTarget ? 1 : 0);
        up.bind(7, id);
        up.exec();
    }
}

void PoolService::refreshPuzzleStatuses() {
    std::unique_lock lock(mu_);
    refreshPuzzleStatusesLocked();
}

void PoolService::refreshPuzzleStatusesLocked() {
    syncConfiguredPuzzleTargets();

    SQLite::Statement q(db_.raw(), R"SQL(
        SELECT id, name, status_target_type, status_target_value
        FROM puzzles
        WHERE status_target_type IS NOT NULL
          AND status_target_value IS NOT NULL
        ORDER BY id ASC
    )SQL");

    const std::string checkedAt = nowIsoUtc();

    while (q.executeStep()) {
        const int64_t puzzleId = q.getColumn(0).getInt64();
        const std::string puzzleName = q.getColumn(1).getString();
        const std::string targetType = q.getColumn(2).getString();
        const std::string targetValue = q.getColumn(3).getString();

        PuzzleStatusState state = PuzzleStatusState::Unknown;
        std::string note;
        const std::string link = currentStatusLink(cfg_, targetType, targetValue);

        if (targetType == "address") {
            auto response = fetchAddressStatusJson(cfg_.blockExplorerApi, targetValue);
            if (!response) {
                note = "block explorer lookup failed";
            } else {
                state = evaluateAddressTargetStatus(*response);
                if (state == PuzzleStatusState::Unknown) {
                    note = "address is unfunded or unresolved";
                }
            }
        } else if (targetType == "findings_threshold") {
            int64_t threshold = 0;
            try {
                threshold = std::stoll(targetValue);
            } catch (...) {
                threshold = 0;
            }

            SQLite::Statement countQ(db_.raw(), R"SQL(
                SELECT COUNT(DISTINCT f.found_key)
                FROM findings f
                JOIN chunks c ON c.id = f.chunk_id
                WHERE c.puzzle_id = ?
            )SQL");
            countQ.bind(1, puzzleId);
            countQ.executeStep();
            const int64_t distinctFoundKeys = countQ.getColumn(0).getInt64();
            state = evaluateFindingsThresholdStatus(distinctFoundKeys, threshold);
            std::ostringstream noteStream;
            noteStream << distinctFoundKeys << "/" << threshold << " distinct found keys";
            note = noteStream.str();
        } else {
            note = "unsupported target type for " + puzzleName;
        }

        SQLite::Statement up(db_.raw(), R"SQL(
            UPDATE puzzles
            SET status_state = ?,
                status_checked_at = ?,
                status_link = ?,
                status_note = ?
            WHERE id = ?
        )SQL");
        up.bind(1, puzzleStatusStateToString(state));
        up.bind(2, checkedAt);
        if (link.empty()) up.bind(3); else up.bind(3, link);
        if (note.empty()) up.bind(4); else up.bind(4, note);
        up.bind(5, puzzleId);
        up.exec();
    }
}

} // namespace puzzpool
