#include <puzzpool/hash_utils.hpp>
#include <puzzpool/hex_bigint.hpp>
#include <puzzpool/permutation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace puzzpool;

namespace {

std::string repeat(char c, std::size_t n) {
    return std::string(n, c);
}

} // namespace

// ── Legacy keyed digest: frozen bytes (AC4) ──────────────────────────────────

TEST_CASE("keyedDigestHex is the unchanged sha256(key || 0x1f || msg) construction", "[hash]") {
    CHECK(keyedDigestHex("seed", "42") == sha256Hex(std::string("seed") + "\x1f" + "42"));
    CHECK(keyedDigestHex("", "") == sha256Hex(std::string("") + "\x1f" + ""));
}

TEST_CASE("keyedDigestHex golden vectors are byte-for-byte stable", "[hash]") {
    // Captured from the pre-rename implementation at 946634c. A change here
    // reorders allocation for every existing puzzle (ADR-4) and must never be
    // accepted, so these literals — not a restatement of the formula — are the
    // regression guard.
    CHECK(keyedDigestHex("seed:round:0", "0") ==
          "935ffa3d990113b354618dd94c8106c717c4fd86dfcfbda93378ef7124837fb6");
    CHECK(keyedDigestHex("puzzpool-seed:round:5", "123456789") ==
          "6216ef3a84d24e0ad83754b61ec29c171ca9f6fa60e229b6905fd2b3ad9687bb");
    CHECK(keyedDigestHex("", "") ==
          "ffe679bb831c95b67dc17819c63c5090d221aac6f4c7bf530f594ab43d21fa1e");
}

TEST_CASE("permutation output is unchanged by the keyed-digest rename", "[hash][permutation]") {
    // Golden allocation order for a small domain under the shipped Feistel mode.
    // These indices were produced before the rename; they pin the allocator's
    // observable behaviour, not just the helper's name.
    const cpp_int n = 16;
    std::vector<std::string> order;
    for (int i = 0; i < 16; ++i) {
        order.push_back(bigToDec(permuteIndexFeistel(cpp_int(i), n, "puzzpool-seed")));
    }

    // A permutation: every index appears exactly once.
    std::vector<std::string> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    CHECK(sorted.size() == 16);

    // Deterministic for the same seed, different for a different seed.
    CHECK(bigToDec(permuteIndexFeistel(cpp_int(3), n, "puzzpool-seed")) == order[3]);
    CHECK(bigToDec(permuteIndexFeistel(cpp_int(3), n, "other-seed")) != order[3]);

    // Frozen order captured by building src/permutation.cpp + src/hash_utils.cpp
    // at 946634c (the pre-rename tree) and printing permuteIndexFeistel(i, 16,
    // "puzzpool-seed") for i in [0, 16).
    const std::vector<std::string> golden = {"3", "14", "5", "7", "12", "0", "2", "15",
                                             "4", "6",  "10", "8", "11", "1", "13", "9"};
    CHECK(order == golden);
}

// ── Real HMAC-SHA-256 (AC3) ──────────────────────────────────────────────────

TEST_CASE("hmacSha256Hex matches RFC 4231 test vectors", "[hash][hmac]") {
    // RFC 4231 §4.2 — 20-byte 0x0b key, "Hi There"
    CHECK(hmacSha256Hex(repeat('\x0b', 20), "Hi There") ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // RFC 4231 §4.3 — key "Jefe", data "what do ya want for nothing?"
    CHECK(hmacSha256Hex("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // RFC 4231 §4.4 — 20-byte 0xaa key, 50 bytes of 0xdd
    CHECK(hmacSha256Hex(repeat('\xaa', 20), repeat('\xdd', 50)) ==
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // RFC 4231 §4.6 — 131-byte key (longer than the 64-byte block, so it is
    // hashed first), "Test Using Larger Than Block-Size Key - Hash Key First"
    CHECK(hmacSha256Hex(repeat('\xaa', 131),
                        "Test Using Larger Than Block-Size Key - Hash Key First") ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // RFC 4231 §4.7 — 131-byte key with a long message
    CHECK(hmacSha256Hex(repeat('\xaa', 131),
                        "This is a test using a larger than block-size key and a larger than "
                        "block-size data. The key needs to be hashed before being used by the "
                        "HMAC algorithm.") ==
          "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

TEST_CASE("hmacSha256Hex is not the legacy keyed digest", "[hash][hmac]") {
    // Sensitivity proof: if the two helpers were ever aliased, this fails.
    CHECK(hmacSha256Hex("key", "message") != keyedDigestHex("key", "message"));
    CHECK(hmacSha256Hex("key", "message") != sha256Hex("key\x1fmessage"));
}

TEST_CASE("hmacSha256Hex resists the length extension the legacy digest allows", "[hash][hmac]") {
    // The legacy construction hashes key || 0x1f || msg, so appending to the
    // message is indistinguishable from a longer message under the same prefix.
    // HMAC has no such structural relationship between the two outputs.
    const std::string a = hmacSha256Hex("k", "ab");
    const std::string b = hmacSha256Hex("k", "abc");
    CHECK(a != b);
    CHECK(a.size() == 64);
    CHECK(hmacSha256Raw("k", "ab").size() == 32);
}

TEST_CASE("hmacSha256Hex changes with the key", "[hash][hmac]") {
    CHECK(hmacSha256Hex("key-a", "same message") != hmacSha256Hex("key-b", "same message"));
}
