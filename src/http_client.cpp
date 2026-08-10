#include <puzzpool/http_client.hpp>

#include <curl/curl.h>

#include <cstddef>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace puzzpool {

namespace {

struct WriteSink {
    std::string  data;
    std::size_t  limit    = 0;
    bool         overflow = false;
};

std::size_t writeCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* sink = static_cast<WriteSink*>(userdata);
    const std::size_t incoming = size * nmemb;
    if (sink->data.size() + incoming > sink->limit) {
        sink->overflow = true;
        return 0; // aborts the transfer with CURLE_WRITE_ERROR
    }
    sink->data.append(ptr, incoming);
    return incoming;
}

struct CurlEasyDeleter {
    void operator()(CURL* handle) const noexcept {
        if (handle) curl_easy_cleanup(handle);
    }
};

struct CurlSlistDeleter {
    void operator()(curl_slist* list) const noexcept {
        if (list) curl_slist_free_all(list);
    }
};

// curl_easy_init() would initialise libcurl lazily, but that path is not
// thread-safe and auth callbacks are served on Crow's worker threads.
void ensureCurlGlobalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace

std::string urlEncode(const std::string& value) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (const char c : value) {
        const auto uc = static_cast<unsigned char>(c);
        const bool unreserved = (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
                                (uc >= '0' && uc <= '9') || uc == '-' || uc == '_' ||
                                uc == '.' || uc == '~';
        if (unreserved) {
            out << c;
        } else {
            out << '%' << std::setw(2) << static_cast<unsigned>(uc);
        }
    }
    return out.str();
}

HttpClient makeCurlHttpClient() {
    return [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;

        ensureCurlGlobalInit();

        std::unique_ptr<CURL, CurlEasyDeleter> handle(curl_easy_init());
        if (!handle) {
            response.error = "curl_easy_init failed";
            return response;
        }

        WriteSink sink;
        sink.limit = request.maxResponseBytes;

        std::unique_ptr<curl_slist, CurlSlistDeleter> headerList;
        for (const auto& [name, value] : request.headers) {
            // curl_slist_append() returns the list head it was given, so ownership
            // has to leave the unique_ptr for the duration of the call.
            curl_slist* head     = headerList.release();
            curl_slist* appended = curl_slist_append(head, (name + ": " + value).c_str());
            if (!appended) {
                headerList.reset(head);
                response.error = "failed to build request headers";
                return response;
            }
            headerList.reset(appended);
        }

        curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, static_cast<long>(request.timeoutSeconds));
        curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, static_cast<long>(request.timeoutSeconds));
        curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
#if defined(CURL_AT_LEAST_VERSION) && CURL_AT_LEAST_VERSION(7, 85, 0)
        curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
        if (headerList) {
            curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headerList.get());
        }

        if (request.method == "POST") {
            curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
            // POSTFIELDSIZE first: libcurl would otherwise strlen() the buffer.
            curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(request.body.size()));
            curl_easy_setopt(handle.get(), CURLOPT_COPYPOSTFIELDS, request.body.c_str());
        } else if (request.method != "GET") {
            curl_easy_setopt(handle.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str());
        }

        const CURLcode rc = curl_easy_perform(handle.get());
        if (rc != CURLE_OK) {
            // curl_easy_strerror() describes the transport failure only; it never
            // echoes the request body, so no secret can reach this string.
            response.error = sink.overflow ? "response exceeded maximum size"
                                           : curl_easy_strerror(rc);
            return response;
        }

        long status = 0;
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
        response.status = status;
        response.body   = std::move(sink.data);
        return response;
    };
}

} // namespace puzzpool
