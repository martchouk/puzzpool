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

struct RefreshTarget {
    int64_t id = 0;
    std::string name;
    std::string targetType;
    std::string targetValue;
    int64_t thresholdDistinctFoundKeys = 0;
};

struct RefreshResult {
    int64_t puzzleId = 0;
    PuzzleStatusState state = PuzzleStatusState::Unknown;
    std::string link;
    std::string note;
};

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

} // namespace

std::optional<json> PoolService::fetchAddressStatusJson(const std::string& apiBase, const std::string& address) {
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

namespace {

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
    std::vector<RefreshTarget> targets;
    const std::string checkedAt = nowIsoUtc();
    {
        std::unique_lock lock(mu_);
        syncConfiguredPuzzleTargets();

        SQLite::Statement q(db_.raw(), R"SQL(
            SELECT id, name, status_target_type, status_target_value
            FROM puzzles
            WHERE status_target_type IS NOT NULL
              AND status_target_value IS NOT NULL
            ORDER BY id ASC
        )SQL");

        while (q.executeStep()) {
            RefreshTarget target;
            target.id = q.getColumn(0).getInt64();
            target.name = q.getColumn(1).getString();
            target.targetType = q.getColumn(2).getString();
            target.targetValue = q.getColumn(3).getString();

            if (target.targetType == "findings_threshold") {
                SQLite::Statement countQ(db_.raw(), R"SQL(
                    SELECT COUNT(DISTINCT f.found_key)
                    FROM findings f
                    JOIN chunks c ON c.id = f.chunk_id
                    WHERE c.puzzle_id = ?
                )SQL");
                countQ.bind(1, target.id);
                countQ.executeStep();
                target.thresholdDistinctFoundKeys = countQ.getColumn(0).getInt64();
            }
            targets.push_back(std::move(target));
        }
    }

    std::vector<RefreshResult> results;
    results.reserve(targets.size());
    for (const auto& target : targets) {
        RefreshResult result;
        result.puzzleId = target.id;
        result.link = currentStatusLink(cfg_, target.targetType, target.targetValue);

        if (target.targetType == "address") {
            auto response = fetchAddressStatus_(cfg_.blockExplorerApi, target.targetValue);
            if (!response) {
                result.note = "block explorer lookup failed";
            } else {
                result.state = evaluateAddressTargetStatus(*response);
                if (result.state == PuzzleStatusState::Unknown) {
                    result.note = "address is unfunded or unresolved";
                }
            }
        } else if (target.targetType == "findings_threshold") {
            int64_t threshold = 0;
            try {
                threshold = std::stoll(target.targetValue);
            } catch (...) {
                threshold = 0;
            }
            result.state = evaluateFindingsThresholdStatus(target.thresholdDistinctFoundKeys, threshold);
            std::ostringstream noteStream;
            noteStream << target.thresholdDistinctFoundKeys << "/" << threshold << " distinct found keys";
            result.note = noteStream.str();
        } else {
            result.note = "unsupported target type for " + target.name;
        }
        results.push_back(std::move(result));
    }

    std::unique_lock lock(mu_);
    for (const auto& result : results) {
        SQLite::Statement up(db_.raw(), R"SQL(
            UPDATE puzzles
            SET status_state = ?,
                status_checked_at = ?,
                status_link = ?,
                status_note = ?
            WHERE id = ?
        )SQL");
        up.bind(1, puzzleStatusStateToString(result.state));
        up.bind(2, checkedAt);
        if (result.link.empty()) up.bind(3); else up.bind(3, result.link);
        if (result.note.empty()) up.bind(4); else up.bind(4, result.note);
        up.bind(5, result.puzzleId);
        up.exec();
    }
}

} // namespace puzzpool
