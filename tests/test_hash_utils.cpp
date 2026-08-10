#include <puzzpool/hash_utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace puzzpool;

// ── RFC 4231 HMAC-SHA-256 vectors ────────────────────────────────────────────
// These pin hmacSha256Hex to values published outside this repository, so the
// whole HMAC path — including its hex formatting — is anchored to something no
// refactor here can move.

// RFC 4231 test case 1 — 20 bytes of 0x0b, data "Hi There".
TEST_CASE("hmacSha256Hex matches RFC 4231 case 1", "[hash][hmac]") {
    CHECK(hmacSha256Hex(std::string(20, '\x0b'), "Hi There") ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// RFC 4231 test case 2 — key "Jefe", data "what do ya want for nothing?".
TEST_CASE("hmacSha256Hex matches RFC 4231 case 2", "[hash][hmac]") {
    CHECK(hmacSha256Hex("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// RFC 4231 test case 3 — 20 bytes of 0xaa, 50 bytes of 0xdd.
TEST_CASE("hmacSha256Hex matches RFC 4231 case 3", "[hash][hmac]") {
    CHECK(hmacSha256Hex(std::string(20, '\xaa'), std::string(50, '\xdd')) ==
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

// RFC 4231 test case 6 — a key longer than the 64-byte block must be hashed first.
TEST_CASE("hmacSha256Hex hashes over-long keys per RFC 2104", "[hash][hmac]") {
    CHECK(hmacSha256Hex(std::string(131, '\xaa'),
                        "Test Using Larger Than Block-Size Key - Hash Key First") ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// An empty key is a legitimate RFC 2104 input, and it is exactly the shape a
// missing SESSION_SIGNING_SECRET would produce. It must still be a well-formed
// 64-character digest rather than anything degenerate — the refusal to sign with
// an empty key lives in session.cpp, not here.
TEST_CASE("hmacSha256Hex handles an empty key and empty message", "[hash][hmac]") {
    CHECK(hmacSha256Hex("", "").size() == 64);
    CHECK(hmacSha256Hex("", "") ==
          "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
}

// ── AC4: the permutation digest is frozen ────────────────────────────────────

// This literal was captured from the pre-change binary before any source edit and
// must never change — ADR-4 allocation determinism depends on it. It is deliberately
// NOT written as `== sha256Hex(key + "\x1f" + msg)`: an expression like that moves
// together with the implementation and so could not detect a change.
TEST_CASE("keyedDigestHex output is byte-for-byte frozen", "[hash][permutation]") {
    CHECK(keyedDigestHex("round-key-0", "12345") ==
          "91d5857db38e9ef3eeb3a9a9f51c5e59d1fc9d0707714cf1247442a4bc27fcc4");
}

// Published NIST FIPS 180-4 vectors. sha256Hex is deliberately left untouched by
// this change; these pin it to values that exist outside this repository so that a
// future refactor of the digest path cannot silently reorder allocation.
TEST_CASE("sha256Hex matches the published NIST vectors", "[hash]") {
    CHECK(sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// Every digest this codebase produces is a 64-character lowercase hex string. A
// dropped zero-pad is the classic hand-rolled-hex bug: it shortens the string,
// changes hexToInt, and silently reorders allocation. Assert the invariant on both
// formatters, over inputs chosen to contain low bytes that must render as "0x".
TEST_CASE("digest strings are 64 lowercase hex characters", "[hash][hex]") {
    for (const std::string probe : {std::string(""), std::string("abc"),
                                    std::string("\x01"), std::string(200, 'z')}) {
        INFO(probe);
        for (const std::string digest : {sha256Hex(probe),
                                         keyedDigestHex("k", probe),
                                         hmacSha256Hex("k", probe)}) {
            CHECK(digest.size() == 64);
            CHECK(digest.find_first_not_of("0123456789abcdef") == std::string::npos);
        }
    }
}

// Sensitivity: proves the rename did not alias the two, i.e. that the RFC 4231
// probes above would actually detect a regression back to the secret-prefix
// construction.
TEST_CASE("keyedDigestHex and hmacSha256Hex are different primitives", "[hash][hmac]") {
    CHECK(keyedDigestHex("k", "m") != hmacSha256Hex("k", "m"));
}

// keyedDigestHex is a secret-prefix hash: it is exactly sha256 of key || 0x1f || msg.
// Stating that here documents the construction the WARNING in the header is about,
// and would fail if someone "upgraded" it to a real MAC and broke ADR-4.
TEST_CASE("keyedDigestHex is the secret-prefix construction, not a MAC", "[hash][permutation]") {
    CHECK(keyedDigestHex("k", "m") == sha256Hex(std::string("k") + "\x1f" + "m"));
}

TEST_CASE("constantTimeEquals matches string equality", "[hash][compare]") {
    CHECK(constantTimeEquals("abc", "abc"));
    CHECK_FALSE(constantTimeEquals("abc", "abd"));
    CHECK_FALSE(constantTimeEquals("abc", "abcd"));
    CHECK_FALSE(constantTimeEquals("", "a"));
    CHECK(constantTimeEquals("", ""));
    // Embedded NULs are compared like any other byte, not treated as terminators.
    CHECK(constantTimeEquals(std::string("a\0b", 3), std::string("a\0b", 3)));
    CHECK_FALSE(constantTimeEquals(std::string("a\0b", 3), std::string("a\0c", 3)));
}
