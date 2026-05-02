#include <puzzpool/service.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace puzzpool {

using json = nlohmann::json;

json PoolService::puzzleJson(const PuzzleRow& p) {
    json j;
    j["id"]        = p.id;
    j["name"]      = p.name;
    j["start_hex"] = p.startHex;
    j["end_hex"]   = p.endHex;
    j["active"]    = p.active;
    j["total_keys"] = bigToDec(hexToInt(p.endHex) - hexToInt(p.startHex));
    if (!p.testStartHex.empty())
        j["test_chunk"] = {{"start_hex", p.testStartHex}, {"end_hex", p.testEndHex}};
    else
        j["test_chunk"] = nullptr;
    j["alloc_strategy"]          = p.allocStrategy.empty() ? cfg_.allocStrategyLegacy : p.allocStrategy;
    j["alloc_cursor"]            = bigToDec(p.allocCursor);
    j["virtual_chunk_size_keys"] = p.virtualChunkSizeKeys.empty() ? json(nullptr) : json(p.virtualChunkSizeKeys);
    j["virtual_chunk_count"]     = p.virtualChunkCount > 0 ? json(bigToDec(p.virtualChunkCount)) : json(nullptr);
    j["bootstrap_stage"]         = p.bootstrapStage;
    return j;
}

std::string PoolService::formatDouble(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << v;
    std::string s = oss.str();
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

std::string PoolService::nowIsoUtc() {
    auto now  = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

crow::response PoolService::jsonResponse(const json& j, int code) {
    crow::response r;
    r.code = code;
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    r.body = j.dump();
    return r;
}

crow::response PoolService::errorJsonResponse(int code, const json& j) {
    return jsonResponse(j, code);
}

crow::response PoolService::errorResponse(int code, const std::string& msg) {
    return jsonResponse({{"error", msg}}, code);
}

} // namespace puzzpool
