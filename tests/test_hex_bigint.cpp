#include <puzzpool/hex_bigint.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace puzzpool;

// ── isValidHex ────────────────────────────────────────────────────────────────

TEST_CASE("isValidHex rejects empty and blank strings", "[hex_bigint]") {
    CHECK_FALSE(isValidHex(""));
    CHECK_FALSE(isValidHex("   "));
}

TEST_CASE("isValidHex accepts bare hex and 0x-prefixed", "[hex_bigint]") {
    CHECK(isValidHex("0"));
    CHECK(isValidHex("ff"));
    CHECK(isValidHex("FF"));
    CHECK(isValidHex("0xFF"));
    CHECK(isValidHex("0xdeadbeef"));
    CHECK(isValidHex("DEADBEEF"));
    CHECK(isValidHex("0x0000000000000000000000000000000000000000000000000000000000000001"));
}

TEST_CASE("isValidHex rejects non-hex characters", "[hex_bigint]") {
    CHECK_FALSE(isValidHex("0xgg"));
    CHECK_FALSE(isValidHex("xyz"));
    CHECK_FALSE(isValidHex("0x"));
    CHECK_FALSE(isValidHex("0x "));
}

// ── hexToInt / intToHex round-trip ────────────────────────────────────────────

TEST_CASE("hexToInt parses small values", "[hex_bigint]") {
    CHECK(hexToInt("0") == 0);
    CHECK(hexToInt("1") == 1);
    CHECK(hexToInt("ff") == 255);
    CHECK(hexToInt("FF") == 255);
    CHECK(hexToInt("0xff") == 255);
    CHECK(hexToInt("0x10") == 16);
}

TEST_CASE("hexToInt parses 256-bit boundary value", "[hex_bigint]") {
    // 2^256 - 1
    const std::string max256 = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    cpp_int expected = (cpp_int(1) << 256) - 1;
    CHECK(hexToInt(max256) == expected);
}

TEST_CASE("intToHex round-trips through hexToInt", "[hex_bigint]") {
    for (int v : {0, 1, 255, 256, 65535, 1 << 20}) {
        cpp_int big(v);
        CHECK(hexToInt(intToHex(big)) == big);
    }
}

TEST_CASE("intToHex pads to requested width", "[hex_bigint]") {
    REQUIRE(intToHex(cpp_int(1), 64).size() == 64);
    REQUIRE(intToHex(cpp_int(0), 64).size() == 64);
}

// ── normalizeHex ──────────────────────────────────────────────────────────────
// normalizeHex strips 0x, lowercases, and zero-pads to 64 hex characters (256 bits).

TEST_CASE("normalizeHex strips 0x prefix, lowercases and pads to 64 chars", "[hex_bigint]") {
    const std::string pad = std::string(56, '0'); // 56 leading zeros for 3-byte values
    CHECK(normalizeHex("0xFF")      == std::string(62, '0') + "ff");
    CHECK(normalizeHex("DEADBEEF")  == std::string(56, '0') + "deadbeef");
    CHECK(normalizeHex("0xDEADBEEF") == std::string(56, '0') + "deadbeef");
    CHECK(normalizeHex("0x0")       == std::string(64, '0'));
    (void)pad;
}

TEST_CASE("normalizeHex output is always exactly 64 characters", "[hex_bigint]") {
    CHECK(normalizeHex("0x1").size() == 64);
    CHECK(normalizeHex("0xFFFFFFFFFFFFFFFF").size() == 64);
}

TEST_CASE("normalizeHex is idempotent", "[hex_bigint]") {
    std::string n = normalizeHex("0xABCDEF");
    CHECK(normalizeHex(n) == n);
}

// ── ceilDiv ───────────────────────────────────────────────────────────────────

TEST_CASE("ceilDiv exact division", "[hex_bigint]") {
    CHECK(ceilDiv(cpp_int(10), cpp_int(5)) == 2);
    CHECK(ceilDiv(cpp_int(100), cpp_int(10)) == 10);
}

TEST_CASE("ceilDiv rounds up", "[hex_bigint]") {
    CHECK(ceilDiv(cpp_int(11), cpp_int(5)) == 3);
    CHECK(ceilDiv(cpp_int(1), cpp_int(5)) == 1);
}

// ── minBig / maxBig ──────────────────────────────────────────────────────────

TEST_CASE("minBig and maxBig basic", "[hex_bigint]") {
    cpp_int a(7), b(3);
    CHECK(minBig(a, b) == 3);
    CHECK(maxBig(a, b) == 7);
    CHECK(minBig(a, a) == 7);
}

// ── bigToDec ─────────────────────────────────────────────────────────────────

TEST_CASE("bigToDec produces correct decimal string", "[hex_bigint]") {
    CHECK(bigToDec(cpp_int(0)) == "0");
    CHECK(bigToDec(cpp_int(255)) == "255");
    CHECK(bigToDec(cpp_int(1000000)) == "1000000");
}

// ── normalizedRange ───────────────────────────────────────────────────────────

TEST_CASE("normalizedRange computes correct fields", "[hex_bigint]") {
    auto r = normalizedRange("0x00", "0xff");
    CHECK(r.start == 0);
    CHECK(r.end == 255);
    CHECK(r.range == 255);
}

TEST_CASE("normalizedRange near 256-bit upper boundary", "[hex_bigint]") {
    const std::string penultimate = "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe";
    const std::string maximum     = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    auto r = normalizedRange(penultimate, maximum);
    CHECK(r.range == 1);
}

// ── bitLength ────────────────────────────────────────────────────────────────

TEST_CASE("bitLength returns correct values", "[hex_bigint]") {
    CHECK(bitLength(cpp_int(0)) == 0);
    CHECK(bitLength(cpp_int(1)) == 1);
    CHECK(bitLength(cpp_int(2)) == 2);
    CHECK(bitLength(cpp_int(3)) == 2);
    CHECK(bitLength(cpp_int(4)) == 3);
    CHECK(bitLength(cpp_int(255)) == 8);
    CHECK(bitLength(cpp_int(256)) == 9);
}
