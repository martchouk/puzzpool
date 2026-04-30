#include <puzzpool/service.hpp>

#include <string>

namespace puzzpool {

using json = nlohmann::json;

crow::response PoolService::handleSubmit(const crow::request& req) {
    std::unique_lock lock(mu_);
    try {
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        std::string name   = body.value("name", "");
        int64_t     jobId  = body.value("job_id", 0LL);
        std::string status = body.value("status", "");
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
