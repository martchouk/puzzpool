#pragma once

#include <puzzpool/db.hpp>
#include <puzzpool/types.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace puzzpool {

class SubmissionService {
public:
    explicit SubmissionService(PoolDb& db);

    struct SubmitResult {
        bool        accepted  = false;
        bool        hasError  = false;
        int         errorCode = 0;
        std::string error;
    };

    SubmitResult submitDone(const std::string& name, int64_t jobId, const nlohmann::json& body);
    SubmitResult submitFound(const std::string& name, int64_t jobId, const nlohmann::json& body);

    void clearTestChunkIfNeeded(int64_t jobId);

private:
    PoolDb& db_;
};

} // namespace puzzpool
