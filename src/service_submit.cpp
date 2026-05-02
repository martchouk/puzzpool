#include <puzzpool/service.hpp>

#include <cstdint>
#include <limits>
#include <string>

namespace puzzpool {

using json = nlohmann::json;

crow::response PoolService::handleSubmit(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        std::string name;
        if (body.contains("name") && body["name"].is_string())
            name = body["name"].get<std::string>();
        if (!body.contains("job_id") || !body["job_id"].is_number_integer())
            return errorResponse(400, "job_id must be an integer");
        int64_t jobId;
        {
            const auto& jv = body["job_id"];
            if (jv.is_number_unsigned()) {
                uint64_t uv = jv.get<uint64_t>();
                if (uv > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    return errorResponse(400, "job_id out of int64 range");
                jobId = static_cast<int64_t>(uv);
            } else {
                jobId = jv.get<int64_t>();
            }
        }
        std::string status;
        if (body.contains("status") && body["status"].is_string())
            status = body["status"].get<std::string>();
        if (status != "done" && status != "FOUND")
            return errorResponse(400, "status must be \"done\" or \"FOUND\"");

        SubmissionService::SubmitResult result = (status == "FOUND")
            ? ss_.submitFound(name, jobId, body)
            : ss_.submitDone(name, jobId, body);

        if (result.hasError)
            return errorJsonResponse(result.errorCode, {{"accepted", false}, {"error", result.error}});
        return jsonResponse({{"accepted", result.accepted}});
    } catch (const std::exception& e) {
        return errorResponse(500, e.what());
    }
}

} // namespace puzzpool
