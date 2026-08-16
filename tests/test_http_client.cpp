#include <puzzpool/http_client.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

using namespace puzzpool;

// ── urlEncode ────────────────────────────────────────────────────────────────
//
// urlEncode() builds the token-exchange body that carries the client secret and
// the authorization code. If it stopped escaping `&` or `=`, a value could split
// itself into extra parameters, so these cases are pinned directly rather than
// left to the indirect coverage in test_auth_routes.cpp.

TEST_CASE("urlEncode leaves the RFC 3986 unreserved set alone", "[http][urlencode]") {
    const std::string unreserved =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    CHECK(urlEncode(unreserved) == unreserved);
    CHECK(urlEncode("") == "");
}

TEST_CASE("urlEncode escapes every character that could split a parameter",
          "[http][urlencode]") {
    // The four that matter most: unescaped, any of these would let an injected
    // value add or terminate a parameter in the exchange body.
    CHECK(urlEncode("&") == "%26");
    CHECK(urlEncode("=") == "%3D");
    CHECK(urlEncode("?") == "%3F");
    CHECK(urlEncode("#") == "%23");

    CHECK(urlEncode("%") == "%25");
    CHECK(urlEncode("+") == "%2B");
    CHECK(urlEncode("/") == "%2F");
    CHECK(urlEncode(":") == "%3A");
    // Space becomes %20, not '+': correct in both a query string and a body.
    CHECK(urlEncode(" ") == "%20");
}

TEST_CASE("urlEncode percent-encodes each byte of a multi-byte character",
          "[http][urlencode]") {
    // U+00FC (ü) is 0xC3 0xBC in UTF-8; each byte is encoded on its own.
    CHECK(urlEncode("\xC3\xBC") == "%C3%BC");
    // High bytes keep two hex digits — std::setw applies per insertion, so a
    // regression that set the width once would produce "%C3%BC" wrongly padded.
    CHECK(urlEncode("\x01\x0F\x7F\x80\xFF") == "%01%0F%7F%80%FF");
}

TEST_CASE("urlEncode escapes an injected parameter rather than passing it through",
          "[http][urlencode]") {
    const std::string injected = "abc&client_secret=stolen";
    const std::string encoded  = urlEncode(injected);

    // Sensitivity: the literal text survives, so this is escaping and not the
    // function silently dropping characters.
    CHECK(encoded.find("abc") == 0);
    CHECK(encoded.find("client_secret") != std::string::npos);
    CHECK(encoded.find("stolen") != std::string::npos);
    // But neither delimiter remains, so the value stays one parameter.
    CHECK(encoded.find('&') == std::string::npos);
    CHECK(encoded.find('=') == std::string::npos);
}

// ── makeCurlHttpClient ───────────────────────────────────────────────────────
//
// The client is never pointed at the real provider by any test. These cases use
// requests that fail before or without a network round trip, which is enough to
// reach the branches that are otherwise unverified.

TEST_CASE("the curl client refuses a non-HTTPS URL", "[http][curl]") {
    const HttpClient client = makeCurlHttpClient();

    HttpRequest request;
    request.url            = "http://127.0.0.1:1/never-reached";
    request.timeoutSeconds = 2;

    const HttpResponse response = client(request);
    // CURLOPT_PROTOCOLS_STR restricts the client to https, so this is rejected
    // by libcurl itself rather than attempted.
    CHECK_FALSE(response.error.empty());
    CHECK(response.status == 0);
    CHECK(response.body.empty());
}

TEST_CASE("the curl client reports a transport failure without echoing the request",
          "[http][curl]") {
    const HttpClient client = makeCurlHttpClient();

    HttpRequest request;
    request.method         = "POST";
    request.url            = "https://127.0.0.1:1/token";
    request.body           = "client_secret=super-secret-value";
    request.timeoutSeconds = 2;

    const HttpResponse response = client(request);
    REQUIRE_FALSE(response.error.empty());
    CHECK(response.status == 0);
    // The transport description never carries the body, so no secret can reach
    // a caller that logs the error.
    CHECK(response.error.find("super-secret-value") == std::string::npos);
}

// ── the response-size cap ────────────────────────────────────────────────────

TEST_CASE("responseBudgetExceeded accepts a body up to the limit", "[http][budget]") {
    CHECK_FALSE(responseBudgetExceeded(0, 0, 0));
    CHECK_FALSE(responseBudgetExceeded(0, 10, 10)); // exactly the limit fits
    CHECK_FALSE(responseBudgetExceeded(9, 1, 10));  // and so does the last byte
    CHECK_FALSE(responseBudgetExceeded(0, 0, 10));
}

TEST_CASE("responseBudgetExceeded rejects the first byte past the limit", "[http][budget]") {
    CHECK(responseBudgetExceeded(0, 11, 10));
    CHECK(responseBudgetExceeded(10, 1, 10));
    CHECK(responseBudgetExceeded(0, 1, 0)); // a zero budget accepts nothing
    // Already over budget stays over, whatever arrives next.
    CHECK(responseBudgetExceeded(11, 0, 10));
}

TEST_CASE("responseBudgetExceeded cannot be wrapped around by a huge chunk",
          "[http][budget]") {
    // A `currentSize + incoming > limit` formulation would overflow to a small
    // sum here and wrongly admit the chunk.
    constexpr std::size_t kMax = static_cast<std::size_t>(-1);
    CHECK(responseBudgetExceeded(1, kMax, 256 * 1024));
    CHECK(responseBudgetExceeded(256 * 1024, kMax, 256 * 1024));
}
