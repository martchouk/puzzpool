#include <puzzpool/log_redaction.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace puzzpool;

// The OAuth callback carries the GitHub authorization code and the state nonce
// in its query string, and Crow logs the raw request target for every response.
// These cases pin the redaction that keeps those values out of the process log.

TEST_CASE("redactQueryStrings removes the OAuth callback query string", "[log]") {
    const std::string line =
        "Response: 0x7f9c1 /api/v1/auth/github/callback?code=gho_liveauthcode&state=abc123 302 0";

    const std::string redacted = redactQueryStrings(line);

    CHECK(redacted.find("gho_liveauthcode") == std::string::npos);
    CHECK(redacted.find("abc123") == std::string::npos);
    CHECK(redacted.find("code=") == std::string::npos);
    CHECK(redacted.find("state=") == std::string::npos);

    // Sensitivity: everything an operator reads a request log for survives, so
    // the assertions above show redaction and not wholesale deletion.
    CHECK(redacted ==
          "Response: 0x7f9c1 /api/v1/auth/github/callback?<redacted> 302 0");
}

TEST_CASE("redactQueryStrings leaves a line with no query string untouched", "[log]") {
    const std::string line = "Response: 0x7f9c1 /api/v1/stats 200 0";
    CHECK(redactQueryStrings(line) == line);

    CHECK(redactQueryStrings("") == "");
    CHECK(redactQueryStrings("[puzzpool-cpp] server running on http://127.0.0.1:8888") ==
          "[puzzpool-cpp] server running on http://127.0.0.1:8888");
}

TEST_CASE("redactQueryStrings redacts every query string on a line", "[log]") {
    CHECK(redactQueryStrings("GET /a?x=1 then /b?y=2 done") ==
          "GET /a?<redacted> then /b?<redacted> done");
}

TEST_CASE("redactQueryStrings redacts a query string at the end of a line", "[log]") {
    // No trailing whitespace to stop at — the scan must run to the end rather
    // than leave the tail in place.
    CHECK(redactQueryStrings("Request: GET /api/v1/auth/github/callback?code=secret") ==
          "Request: GET /api/v1/auth/github/callback?<redacted>");
    CHECK(redactQueryStrings("?code=secret") == "?<redacted>");
    CHECK(redactQueryStrings("?") == "?<redacted>");
}

TEST_CASE("redactQueryStrings stops at the first whitespace, not the last", "[log]") {
    // A tab or newline ends the URL token just as a space does; a redactor that
    // only recognised ' ' would swallow the rest of a multi-line message.
    CHECK(redactQueryStrings("/a?k=v\t200") == "/a?<redacted>\t200");
    CHECK(redactQueryStrings("/a?k=v\n/b?k=v\n") == "/a?<redacted>\n/b?<redacted>\n");
}

TEST_CASE("redactQueryStrings keeps the request path and status", "[log]") {
    const std::string redacted = redactQueryStrings(
        "Response: 0x1 /api/v1/auth/github/callback?code=x&state=y 400 1");
    // The path is what makes the log useful for debugging a failing callback.
    CHECK(redacted.find("/api/v1/auth/github/callback") != std::string::npos);
    CHECK(redacted.find(" 400 1") != std::string::npos);
}
