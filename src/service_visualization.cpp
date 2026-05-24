#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace puzzpool {

using json = nlohmann::json;

namespace {

constexpr int kHeatmapCols = 512;
constexpr int kHeatmapRows = 128;
constexpr int kHilbertN = 256;
constexpr int kGapHistBins = 96;
constexpr int kNormGapHistBins = 72;
constexpr double kNormGapClip = 6.0;
constexpr std::size_t kAllocatorSampleLimit = 2048;

struct VisualPoint {
    int64_t id = 0;
    std::string status;
    std::string generation;
    double s = 0.0;
    double e = 0.0;
};

struct GapMetrics {
    std::size_t n = 0;
    double mean = 0.0;
    double median = 0.0;
    double p95 = 0.0;
    double max = 0.0;
    std::optional<double> cv;
    std::optional<double> maxOverMean;
};

struct RequestedPuzzle {
    std::optional<PuzzleRow> puzzle;
    std::optional<crow::response> error;
};

crow::response makeErrorResponse(int code, const std::string& msg) {
    crow::response r;
    r.code = code;
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    r.body = json({{"error", msg}}).dump();
    return r;
}

double clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

int statusIndex(std::string_view status) {
    if (status == "completed") return 0;
    if (status == "assigned") return 1;
    if (status == "reclaimed") return 2;
    if (status == "FOUND") return 3;
    if (status == "blocked") return 4;
    return -1;
}

int statusCode(std::string_view status) {
    return statusIndex(status);
}

std::string normalizeGeneration(const std::string& generation) {
    if (generation.empty()) return "legacy";
    return generation;
}

std::uint32_t hash32Mix(std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0, std::uint32_t d = 0) {
    std::uint32_t x = a ^ 0x9e3779b9u;
    x = static_cast<std::uint32_t>(std::uint64_t(x ^ b) * 0x85ebca6bu);
    x = static_cast<std::uint32_t>(std::uint64_t(x ^ c) * 0xc2b2ae35u);
    x = static_cast<std::uint32_t>(std::uint64_t(x ^ d) * 0x27d4eb2fu);
    x ^= x >> 15;
    x = static_cast<std::uint32_t>(std::uint64_t(x) * 0x85ebca6bu);
    x ^= x >> 13;
    x = static_cast<std::uint32_t>(std::uint64_t(x) * 0xc2b2ae35u);
    x ^= x >> 16;
    return x;
}

std::size_t heatmapIndex(const VisualPoint& point, std::size_t totalCells) {
    const auto scaledS = static_cast<std::uint32_t>(std::floor(clamp01(point.s) * 2147483647.0));
    const auto scaledE = static_cast<std::uint32_t>(std::floor(clamp01(point.e) * 2147483647.0));
    const auto id = static_cast<std::uint32_t>(point.id & 0xffffffff);
    return hash32Mix(id, scaledS, scaledE, 0x1234abcdu) % totalCells;
}

std::size_t hilbertIndex(const VisualPoint& point) {
    const auto totalCells = static_cast<double>(kHilbertN * kHilbertN);
    const auto idx = static_cast<int>(std::floor(clamp01(point.s) * totalCells));
    return static_cast<std::size_t>(std::clamp(idx, 0, kHilbertN * kHilbertN - 1));
}

double quantileSorted(const std::vector<double>& arr, double q) {
    if (arr.empty()) return 0.0;
    if (q <= 0.0) return arr.front();
    if (q >= 1.0) return arr.back();
    const double pos = (static_cast<double>(arr.size()) - 1.0) * q;
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return arr[lo];
    const double t = pos - static_cast<double>(lo);
    return arr[lo] * (1.0 - t) + arr[hi] * t;
}

GapMetrics computeGapMetrics(const std::vector<double>& startsSortedByS) {
    GapMetrics metrics;
    if (startsSortedByS.size() < 2) return metrics;

    std::vector<double> gaps;
    gaps.reserve(startsSortedByS.size() - 1);
    for (std::size_t i = 1; i < startsSortedByS.size(); ++i) {
        const double gap = startsSortedByS[i] - startsSortedByS[i - 1];
        if (std::isfinite(gap) && gap >= 0.0) gaps.push_back(gap);
    }
    if (gaps.empty()) return metrics;

    std::sort(gaps.begin(), gaps.end());
    metrics.n = gaps.size();
    double sum = 0.0;
    for (double gap : gaps) sum += gap;
    metrics.mean = sum / static_cast<double>(gaps.size());
    metrics.median = quantileSorted(gaps, 0.5);
    metrics.p95 = quantileSorted(gaps, 0.95);
    metrics.max = gaps.back();
    if (metrics.mean > 0.0) {
        double variance = 0.0;
        for (double gap : gaps) {
            const double d = gap - metrics.mean;
            variance += d * d;
        }
        variance /= static_cast<double>(gaps.size());
        metrics.cv = std::sqrt(variance) / metrics.mean;
        metrics.maxOverMean = metrics.max / metrics.mean;
    }
    return metrics;
}

std::vector<double> computeNormalizedGaps(const std::vector<double>& startsSortedByS) {
    if (startsSortedByS.size() < 2) return {};
    std::vector<double> gaps;
    gaps.reserve(startsSortedByS.size() - 1);
    for (std::size_t i = 1; i < startsSortedByS.size(); ++i) {
        const double gap = startsSortedByS[i] - startsSortedByS[i - 1];
        if (std::isfinite(gap) && gap >= 0.0) gaps.push_back(gap);
    }
    if (gaps.empty()) return {};
    double sum = 0.0;
    for (double gap : gaps) sum += gap;
    const double mean = sum / static_cast<double>(gaps.size());
    if (!(mean > 0.0)) return {};
    for (double& gap : gaps) gap /= mean;
    std::sort(gaps.begin(), gaps.end());
    return gaps;
}

std::vector<int> histogramFromValues(const std::vector<double>& values, int bins, double maxValue) {
    std::vector<int> hist(bins, 0);
    if (values.empty()) return hist;
    double safeMax = maxValue;
    if (!(safeMax > 0.0)) safeMax = 1.0;
    for (double value : values) {
        int idx = static_cast<int>(std::floor((value / safeMax) * static_cast<double>(bins - 1)));
        idx = std::clamp(idx, 0, bins - 1);
        hist[static_cast<std::size_t>(idx)] += 1;
    }
    return hist;
}

RequestedPuzzle resolveRequestedPuzzle(PoolDb& db, const crow::request& req) {
    RequestedPuzzle out;
    if (const char* pId = req.url_params.get("puzzle_id")) {
        try {
            std::size_t pos = 0;
            const int64_t id = std::stoll(pId, &pos);
            if (pos != std::string_view(pId).size()) {
                out.error = makeErrorResponse(400, "puzzle_id must be a valid integer");
                return out;
            }
            out.puzzle = db.puzzleById(id);
        } catch (const std::exception&) {
            out.error = makeErrorResponse(400, "puzzle_id must be a valid integer");
            return out;
        }
    } else {
        out.puzzle = db.activePuzzle();
    }
    if (!out.puzzle) out.error = makeErrorResponse(404, "Puzzle not found");
    return out;
}

std::vector<std::pair<cpp_int, cpp_int>> loadMergedBlockedRanges(PoolDb& db, int64_t puzzleId) {
    SQLite::Statement q(db.raw(),
        "SELECT start_vchunk, end_vchunk FROM blocked_vchunk_ranges WHERE puzzle_id = ? ORDER BY start_vchunk ASC");
    q.bind(1, puzzleId);
    std::vector<std::pair<cpp_int, cpp_int>> raw;
    while (q.executeStep()) {
        const cpp_int start = hexToInt(q.getColumn(0).getString());
        const cpp_int end = hexToInt(q.getColumn(1).getString());
        if (end > start) raw.emplace_back(start, end);
    }
    std::vector<std::pair<cpp_int, cpp_int>> merged;
    for (const auto& interval : raw) {
        if (merged.empty() || interval.first > merged.back().second) merged.push_back(interval);
        else merged.back().second = maxBig(merged.back().second, interval.second);
    }
    return merged;
}

std::vector<VisualPoint> loadVisualPoints(PoolDb& db, const PuzzleRow& puzzle, bool includeBlocked) {
    std::vector<VisualPoint> points;
    const cpp_int puzzleStart = hexToInt(puzzle.startHex);
    const cpp_int puzzleEnd = hexToInt(puzzle.endHex);
    const cpp_int puzzleRange = puzzleEnd - puzzleStart;

    SQLite::Statement q(db.raw(), R"SQL(
        SELECT id, status, start_hex, end_hex, alloc_generation
        FROM chunks
        WHERE puzzle_id = ? AND is_test = 0
        ORDER BY id ASC
    )SQL");
    q.bind(1, puzzle.id);
    while (q.executeStep()) {
        const cpp_int start = hexToInt(q.getColumn(2).getString()) - puzzleStart;
        const cpp_int end = hexToInt(q.getColumn(3).getString()) - puzzleStart;
        VisualPoint point;
        point.id = q.getColumn(0).getInt64();
        point.status = q.getColumn(1).getString();
        point.generation = q.isColumnNull(4) ? "legacy" : q.getColumn(4).getString();
        point.s = clamp01(static_cast<double>(start.convert_to<long double>() / puzzleRange.convert_to<long double>()));
        point.e = clamp01(static_cast<double>(end.convert_to<long double>() / puzzleRange.convert_to<long double>()));
        points.push_back(std::move(point));
    }

    if (!includeBlocked || puzzle.virtualChunkSizeKeys.empty()) return points;
    cpp_int vchunkSize = 0;
    try { vchunkSize = cpp_int(puzzle.virtualChunkSizeKeys); } catch (...) { return points; }
    if (vchunkSize <= 0) return points;

    int64_t blockedId = -1;
    for (const auto& [startVchunk, endVchunk] : loadMergedBlockedRanges(db, puzzle.id)) {
        VisualPoint point;
        point.id = blockedId--;
        point.status = "blocked";
        point.generation = "";
        point.s = clamp01(static_cast<double>(((startVchunk * vchunkSize).convert_to<long double>()) / puzzleRange.convert_to<long double>()));
        point.e = clamp01(static_cast<double>(((endVchunk * vchunkSize).convert_to<long double>()) / puzzleRange.convert_to<long double>()));
        points.push_back(std::move(point));
    }
    return points;
}

json countsToCellArray(std::size_t index, const std::array<int, 5>& counts) {
    return json::array({
        index,
        counts[0],
        counts[1],
        counts[2],
        counts[3],
        counts[4],
    });
}

json metricsJson(const GapMetrics& metrics) {
    if (metrics.n == 0) return nullptr;
    json out;
    out["n"] = metrics.n;
    out["mean"] = metrics.mean;
    out["median"] = metrics.median;
    out["p95"] = metrics.p95;
    out["max"] = metrics.max;
    out["cv"] = metrics.cv ? json(*metrics.cv) : json(nullptr);
    out["max_over_mean"] = metrics.maxOverMean ? json(*metrics.maxOverMean) : json(nullptr);
    return out;
}

json buildAllocatorGenerationPayload(const std::vector<VisualPoint>& rows) {
    json out;
    out["total_count"] = rows.size();

    json scatter = json::array();
    if (!rows.empty()) {
        const std::size_t sampleCount = std::min(kAllocatorSampleLimit, rows.size());
        for (std::size_t i = 0; i < sampleCount; ++i) {
            const std::size_t sourceIndex = sampleCount == 1
                ? 0
                : static_cast<std::size_t>(std::llround(
                    static_cast<long double>(i) * static_cast<long double>(rows.size() - 1) /
                    static_cast<long double>(sampleCount - 1)));
            const auto& row = rows[sourceIndex];
            const double x = rows.size() > 1
                ? static_cast<double>(sourceIndex) / static_cast<double>(rows.size() - 1)
                : 0.5;
            scatter.push_back(json::array({x, row.s, statusCode(row.status)}));
        }
    }
    out["scatter"] = std::move(scatter);

    std::vector<double> startsSortedByS;
    startsSortedByS.reserve(rows.size());
    for (const auto& row : rows) {
        if (std::isfinite(row.s)) startsSortedByS.push_back(row.s);
    }
    std::sort(startsSortedByS.begin(), startsSortedByS.end());

    std::vector<double> rawGaps;
    rawGaps.reserve(startsSortedByS.size() > 1 ? startsSortedByS.size() - 1 : 0);
    for (std::size_t i = 1; i < startsSortedByS.size(); ++i) {
        const double gap = startsSortedByS[i] - startsSortedByS[i - 1];
        if (std::isfinite(gap) && gap >= 0.0) rawGaps.push_back(gap);
    }
    double maxGap = 0.0;
    for (double gap : rawGaps) maxGap = std::max(maxGap, gap);

    out["gap_histogram"] = {
        {"bins", histogramFromValues(rawGaps, kGapHistBins, maxGap)},
        {"max_gap", maxGap},
    };

    const auto normalizedGaps = computeNormalizedGaps(startsSortedByS);
    out["norm_gap_histogram"] = {
        {"bins", histogramFromValues(normalizedGaps, kNormGapHistBins, kNormGapClip)},
        {"clip", kNormGapClip},
    };
    out["metrics"] = metricsJson(computeGapMetrics(startsSortedByS));

    return out;
}

} // namespace

crow::response PoolService::handleHeatmapVisualization(const crow::request& req) {
    std::unique_lock lock(mu_);
    auto requested = resolveRequestedPuzzle(db_, req);
    if (requested.error) return std::move(*requested.error);
    const auto& puzzle = *requested.puzzle;
    const auto revision = visualizationRevisionLocked(puzzle.id);
    auto& cache = visCache_[puzzle.id];
    if (cache.revision != revision || cache.heatmap.is_null()) {
        cache.revision = revision;
        cache.heatmap = buildHeatmapVisualization(puzzle);
    }
    return jsonResponse(cache.heatmap);
}

crow::response PoolService::handleHilbertVisualization(const crow::request& req) {
    std::unique_lock lock(mu_);
    auto requested = resolveRequestedPuzzle(db_, req);
    if (requested.error) return std::move(*requested.error);
    const auto& puzzle = *requested.puzzle;
    const auto revision = visualizationRevisionLocked(puzzle.id);
    auto& cache = visCache_[puzzle.id];
    if (cache.revision != revision || cache.hilbert.is_null()) {
        cache.revision = revision;
        cache.hilbert = buildHilbertVisualization(puzzle);
    }
    return jsonResponse(cache.hilbert);
}

crow::response PoolService::handleAllocatorVisualization(const crow::request& req) {
    std::unique_lock lock(mu_);
    auto requested = resolveRequestedPuzzle(db_, req);
    if (requested.error) return std::move(*requested.error);
    const auto& puzzle = *requested.puzzle;
    const auto revision = visualizationRevisionLocked(puzzle.id);
    auto& cache = visCache_[puzzle.id];
    if (cache.revision != revision || cache.allocator.is_null()) {
        cache.revision = revision;
        cache.allocator = buildAllocatorVisualization(puzzle);
    }
    return jsonResponse(cache.allocator);
}

json PoolService::buildHeatmapVisualization(const PuzzleRow& puzzle) {
    const auto totalCells = static_cast<std::size_t>(kHeatmapCols * kHeatmapRows);
    std::vector<std::array<int, 5>> counts(totalCells, {0, 0, 0, 0, 0});
    for (const auto& point : loadVisualPoints(db_, puzzle, true)) {
        const int idx = statusIndex(point.status);
        if (idx < 0) continue;
        counts[heatmapIndex(point, totalCells)][static_cast<std::size_t>(idx)] += 1;
    }

    json cells = json::array();
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const auto& cell = counts[i];
        if (std::all_of(cell.begin(), cell.end(), [](int value) { return value == 0; })) continue;
        cells.push_back(countsToCellArray(i, cell));
    }

    return {
        {"puzzle_id", puzzle.id},
        {"loaded_at", nowIsoUtc()},
        {"cells", std::move(cells)},
    };
}

json PoolService::buildHilbertVisualization(const PuzzleRow& puzzle) {
    const auto totalCells = static_cast<std::size_t>(kHilbertN * kHilbertN);
    std::vector<std::array<int, 5>> counts(totalCells, {0, 0, 0, 0, 0});
    for (const auto& point : loadVisualPoints(db_, puzzle, true)) {
        const int idx = statusIndex(point.status);
        if (idx < 0) continue;
        counts[hilbertIndex(point)][static_cast<std::size_t>(idx)] += 1;
    }

    json cells = json::array();
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const auto& cell = counts[i];
        if (std::all_of(cell.begin(), cell.end(), [](int value) { return value == 0; })) continue;
        cells.push_back(countsToCellArray(i, cell));
    }

    return {
        {"puzzle_id", puzzle.id},
        {"loaded_at", nowIsoUtc()},
        {"cells", std::move(cells)},
    };
}

json PoolService::buildAllocatorVisualization(const PuzzleRow& puzzle) {
    std::vector<VisualPoint> allRows;
    std::vector<VisualPoint> legacyRows;
    std::vector<VisualPoint> affineRows;
    std::vector<VisualPoint> feistelRows;

    SQLite::Statement q(db_.raw(), R"SQL(
        SELECT id, status, start_hex, end_hex, alloc_generation
        FROM chunks
        WHERE puzzle_id = ? AND is_test = 0
        ORDER BY id ASC
    )SQL");
    q.bind(1, puzzle.id);
    const cpp_int puzzleStart = hexToInt(puzzle.startHex);
    const cpp_int puzzleEnd = hexToInt(puzzle.endHex);
    const cpp_int puzzleRange = puzzleEnd - puzzleStart;
    while (q.executeStep()) {
        VisualPoint point;
        point.id = q.getColumn(0).getInt64();
        point.status = q.getColumn(1).getString();
        point.generation = normalizeGeneration(q.isColumnNull(4) ? "" : q.getColumn(4).getString());
        const cpp_int start = hexToInt(q.getColumn(2).getString()) - puzzleStart;
        const cpp_int end = hexToInt(q.getColumn(3).getString()) - puzzleStart;
        point.s = clamp01(static_cast<double>(start.convert_to<long double>() / puzzleRange.convert_to<long double>()));
        point.e = clamp01(static_cast<double>(end.convert_to<long double>() / puzzleRange.convert_to<long double>()));
        allRows.push_back(point);
        if (point.generation == "legacy") legacyRows.push_back(point);
        else if (point.generation == "affine") affineRows.push_back(point);
        else if (point.generation == "feistel") feistelRows.push_back(point);
    }

    json generations;
    generations["all"] = buildAllocatorGenerationPayload(allRows);
    generations["legacy"] = buildAllocatorGenerationPayload(legacyRows);
    generations["affine"] = buildAllocatorGenerationPayload(affineRows);
    generations["feistel"] = buildAllocatorGenerationPayload(feistelRows);

    return {
        {"puzzle_id", puzzle.id},
        {"loaded_at", nowIsoUtc()},
        {"generations", std::move(generations)},
    };
}

} // namespace puzzpool
