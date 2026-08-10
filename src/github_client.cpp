#include <puzzpool/github_client.hpp>

#include <curl/curl.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace puzzpool {

namespace {

/// Hard ceiling on a provider response we are willing to buffer. GitHub's token and
/// user payloads are a few hundred bytes; anything approaching this is either a
/// wrong endpoint or a hostile one, and neither should be able to grow the heap.
constexpr std::size_t kMaxResponseBytes = 256 * 1024;

void ensureCurlGlobalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t appendBody(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (out->size() + bytes > kMaxResponseBytes) {
        return 0;   // aborts the transfer with CURLE_WRITE_ERROR
    }
    out->append(ptr, bytes);
    return bytes;
}

/// RAII for the header list, so no path — including the error paths — leaks it.
class CurlSlist {
public:
    explicit CurlSlist(const std::vector<std::string>& headers) {
        for (const auto& h : headers) list_ = curl_slist_append(list_, h.c_str());
    }
    ~CurlSlist() { if (list_) curl_slist_free_all(list_); }
    CurlSlist(const CurlSlist&) = delete;
    CurlSlist& operator=(const CurlSlist&) = delete;

    curl_slist* get() const { return list_; }

private:
    curl_slist* list_ = nullptr;
};

/// Options shared by both verbs.
///
/// Deliberately absent: CURLOPT_VERBOSE and any logging of the URL, headers,
/// request body, or response body. A verbose handle would print the bearer token
/// and the client secret straight into the service log (AC22).
void applyCommonOptions(CURL* curl, const std::string& url, std::string& body,
                        curl_slist* headers, int timeoutSeconds) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
}

HttpResult finish(CURL* curl, CURLcode code, std::string body) {
    HttpResult result;
    if (code != CURLE_OK) {
        // Distinguish "we never reached the provider" from "the provider said no",
        // so the caller does not report a network outage as a bad credential.
        result.transportError = true;
        return result;
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    result.status = status;
    result.body = std::move(body);
    return result;
}

} // namespace

GitHubHttpClient makeLibcurlGitHubClient(int timeoutSeconds) {
    ensureCurlGlobalInit();

    GitHubHttpClient client;

    client.postForm = [timeoutSeconds](const std::string& url,
                                       const std::string& formBody,
                                       const std::vector<std::string>& headers) {
        HttpResult failed;
        failed.transportError = true;

        CURL* curl = curl_easy_init();
        if (!curl) return failed;

        std::string body;
        const CurlSlist list(headers);
        applyCommonOptions(curl, url, body, list.get(), timeoutSeconds);
        // The client secret and the authorization code go here, in the request
        // body — never in the URL, never in a header, and never through
        // popen/system. CURLOPT_POSTFIELDSIZE is set explicitly so an embedded NUL
        // could not truncate the form.
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(formBody.size()));

        const CURLcode code = curl_easy_perform(curl);
        HttpResult result = finish(curl, code, std::move(body));
        curl_easy_cleanup(curl);
        return result;
    };

    client.get = [timeoutSeconds](const std::string& url,
                                  const std::vector<std::string>& headers) {
        HttpResult failed;
        failed.transportError = true;

        CURL* curl = curl_easy_init();
        if (!curl) return failed;

        std::string body;
        const CurlSlist list(headers);
        applyCommonOptions(curl, url, body, list.get(), timeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

        const CURLcode code = curl_easy_perform(curl);
        HttpResult result = finish(curl, code, std::move(body));
        curl_easy_cleanup(curl);
        return result;
    };

    return client;
}

} // namespace puzzpool
