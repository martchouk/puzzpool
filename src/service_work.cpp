#include <puzzpool/service.hpp>

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

        auto result = ws_.assignWork(name, hashrate, version, minChunkKeys, chunkQuantumKeys);
        if (!result.ok) return errorResponse(result.errorCode, result.error);
        return jsonResponse({
            {"job_id",    result.jobId},
            {"start_key", result.startHex},
            {"end_key",   result.endHex}
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
        ws_.heartbeat(name, jobId);
        return jsonResponse({{"ok", true}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

} // namespace puzzpool
