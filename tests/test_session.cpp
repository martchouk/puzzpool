#include <puzzpool/session.hpp>

#include <puzzpool/base64.hpp>
#include <puzzpool/secure_random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace puzzpool;

namespace {
constexpr const char* kSecret = "test-signing-secret";
constexpr int64_t kNow = 1'770'000'000;
}

// ── round trip ───────────────────────────────────────────────────────────────

TEST_CASE("a session token round-trips login and id", "[session]") {
    const auto token = issueSessionToken(kSecret, {"Alice", "4242"}, kNow + 600);
    const auto v = verifySessionToken(kSecret, token, kNow);
    REQUIRE(v.error == SessionError::None);
    CHECK(v.identity.login == "Alice");
    CHECK(v.identity.githubId == "4242");
    CHECK(v.expiresAt == kNow + 600);
}

// ── forgery ──────────────────────────────────────────────────────────────────

TEST_CASE("a tampered login is rejected (AC3)", "[session][security]") {
    const auto token  = issueSessionToken(kSecret, {"alice", "1"}, kNow + 600);
    const auto forged = issueSessionToken("attacker-secret", {"root", "1"}, kNow + 600);
    // Swap in the attacker's payload but keep the legitimate signature.
    const auto payloadEnd = forged.rfind('.');
    const auto sigStart   = token.rfind('.');
    const std::string spliced = forged.substr(0, payloadEnd) + token.substr(sigStart);
    CHECK(verifySessionToken(kSecret, spliced, kNow).error == SessionError::BadSignature);
}

TEST_CASE("a flipped signature byte is rejected", "[session][security]") {
    auto token = issueSessionToken(kSecret, {"alice", "1"}, kNow + 600);
    token.back() = (token.back() == 'a') ? 'b' : 'a';
    CHECK(verifySessionToken(kSecret, token, kNow).error == SessionError::BadSignature);
}

TEST_CASE("a token signed with a different secret is rejected", "[session][security]") {
    const auto token = issueSessionToken("other-secret", {"alice", "1"}, kNow + 600);
    CHECK(verifySessionToken(kSecret, token, kNow).error == SessionError::BadSignature);
}

TEST_CASE("an empty signing secret never verifies anything (AC13)", "[session][security]") {
    const auto token = issueSessionToken(kSecret, {"alice", "1"}, kNow + 600);
    CHECK(verifySessionToken("", token, kNow).error == SessionError::BadSignature);
    // Not even a blob that was itself "signed" with the empty key.
    const auto selfSigned = issueSessionToken("", {"alice", "1"}, kNow + 600);
    CHECK(verifySessionToken("", selfSigned, kNow).error == SessionError::BadSignature);
}

// ── expiry ───────────────────────────────────────────────────────────────────

TEST_CASE("expiry is absolute and checked against the supplied clock", "[session]") {
    const auto token = issueSessionToken(kSecret, {"alice", "1"}, kNow + 60);
    CHECK(verifySessionToken(kSecret, token, kNow + 59).error == SessionError::None);
    CHECK(verifySessionToken(kSecret, token, kNow + 60).error == SessionError::None);
    CHECK(verifySessionToken(kSecret, token, kNow + 61).error == SessionError::Expired);
}

// ── shape ────────────────────────────────────────────────────────────────────

TEST_CASE("malformed tokens are rejected without throwing", "[session][security]") {
    for (const std::string bad : {std::string(""), std::string("v1"),
                                  std::string("v1.session.a.b.c"),
                                  std::string("v1.session.a.b.c.d.e"),
                                  std::string("v2.session.YWxpY2U.MQ.1770000600.deadbeef"),
                                  std::string("v1.session.!!!.MQ.1770000600.deadbeef"),
                                  std::string("v1.session.YWxpY2U.MQ.not-a-number.deadbeef")}) {
        INFO(bad);
        CHECK(verifySessionToken(kSecret, bad, kNow).error == SessionError::Malformed);
    }
}

TEST_CASE("a login containing the delimiter cannot forge a field boundary",
          "[session][security]") {
    const auto token = issueSessionToken(kSecret, {"ali.ce.9999999999", "1"}, kNow + 600);
    const auto v = verifySessionToken(kSecret, token, kNow);
    REQUIRE(v.error == SessionError::None);
    CHECK(v.identity.login == "ali.ce.9999999999");
    CHECK(v.identity.githubId == "1");
}

// ── domain separation between the two blob types ─────────────────────────────

TEST_CASE("a state token never verifies as a session token", "[session][security]") {
    const auto state = issueStateToken(kSecret, "the-nonce", kNow + 600);
    CHECK(verifySessionToken(kSecret, state, kNow).error == SessionError::WrongPurpose);
}

TEST_CASE("a session token never verifies as a state token", "[session][security]") {
    const auto session = issueSessionToken(kSecret, {"alice", "1"}, kNow + 600);
    CHECK(verifyStateToken(kSecret, session, kNow).error == SessionError::WrongPurpose);
}

// Sensitivity for the two cases above: each blob still verifies under its own
// purpose, so WrongPurpose is detecting the label and not a blanket rejection.
TEST_CASE("each token still verifies under its own purpose", "[session]") {
    const auto state = issueStateToken(kSecret, "the-nonce", kNow + 600);
    const auto verified = verifyStateToken(kSecret, state, kNow);
    REQUIRE(verified.error == SessionError::None);
    CHECK(verified.identity.login == "the-nonce");
    CHECK(verifySessionToken(kSecret,
                             issueSessionToken(kSecret, {"alice", "1"}, kNow + 600),
                             kNow).error == SessionError::None);
}

// The purpose is covered by the MAC, not merely compared after the fact: rewriting
// the label in an otherwise valid blob must fail on the signature.
TEST_CASE("the purpose label is inside the MAC", "[session][security]") {
    const auto state = issueStateToken(kSecret, "the-nonce", kNow + 600);
    const std::string relabelled = "v1.session" + state.substr(state.find(".state.") + 6);
    CHECK(verifySessionToken(kSecret, relabelled, kNow).error == SessionError::BadSignature);
}

// ── cookie parsing ───────────────────────────────────────────────────────────

TEST_CASE("cookieValue extracts the right cookie", "[session][cookie]") {
    CHECK(cookieValue("pp_session=abc", "pp_session") == "abc");
    CHECK(cookieValue("a=1; pp_session=abc; b=2", "pp_session") == "abc");
    CHECK(cookieValue("a=1;pp_session=abc", "pp_session") == "abc");
    CHECK(cookieValue("pp_session=abc; pp_oauth_state=xyz", "pp_oauth_state") == "xyz");
    CHECK_FALSE(cookieValue("xpp_session=abc", "pp_session").has_value());
    CHECK_FALSE(cookieValue("pp_session_extra=abc", "pp_session").has_value());
    CHECK_FALSE(cookieValue("", "pp_session").has_value());
    CHECK(cookieValue("pp_session=", "pp_session") == "");
}

// ── primitives the token format rests on ─────────────────────────────────────

TEST_CASE("secureRandomBytes returns distinct full-length buffers", "[session][random]") {
    const auto a = secureRandomBytes(32);
    const auto b = secureRandomBytes(32);
    CHECK(a.size() == 32);
    CHECK(b.size() == 32);
    CHECK(a != b);
}

TEST_CASE("base64url round-trips binary and omits padding", "[session][base64]") {
    // Adjacent literals, not "\x00\xff\x10binary": C++ hex escapes are maximal-munch,
    // so \x10b would parse as one escape whose value does not fit in a char.
    const std::string raw("\x00\xff\x10" "binary", 9);
    const auto encoded = base64UrlEncode(raw);
    CHECK(encoded.find('=') == std::string::npos);
    CHECK(encoded.find('+') == std::string::npos);
    CHECK(encoded.find('/') == std::string::npos);
    CHECK(base64UrlDecode(encoded) == raw);
    CHECK_FALSE(base64UrlDecode("!!!").has_value());
}

TEST_CASE("base64url round-trips every payload length", "[session][base64]") {
    std::string raw;
    for (int i = 0; i < 40; ++i) {
        INFO("length " << raw.size());
        const auto encoded = base64UrlEncode(raw);
        REQUIRE(base64UrlDecode(encoded).has_value());
        CHECK(*base64UrlDecode(encoded) == raw);
        raw.push_back(static_cast<char>(i * 7));
    }
}
