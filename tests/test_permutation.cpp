#include <puzzpool/permutation.hpp>
#include <puzzpool/hex_bigint.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

#include <set>
#include <string>

using namespace puzzpool;

// ── permuteIndexFeistel: basic contract ───────────────────────────────────────

TEST_CASE("permuteIndexFeistel output is bounded [0, n)", "[permutation][feistel]") {
    const cpp_int n("1000000");
    const std::string key = "test_seed_a";

    for (int i = 0; i < 1000; ++i) {
        cpp_int out = permuteIndexFeistel(cpp_int(i), n, key);
        REQUIRE(out >= 0);
        REQUIRE(out < n);
    }
}

TEST_CASE("permuteIndexFeistel is deterministic", "[permutation][feistel]") {
    const cpp_int n("999983");
    const std::string key = "determinism_check";

    cpp_int first  = permuteIndexFeistel(cpp_int(42), n, key);
    cpp_int second = permuteIndexFeistel(cpp_int(42), n, key);
    REQUIRE(first == second);
}

TEST_CASE("permuteIndexFeistel different seeds produce different outputs often enough", "[permutation][feistel]") {
    const cpp_int n("100000");
    int mismatches = 0;
    for (int i = 0; i < 100; ++i) {
        cpp_int a = permuteIndexFeistel(cpp_int(i), n, "seed_alpha");
        cpp_int b = permuteIndexFeistel(cpp_int(i), n, "seed_beta");
        if (a != b) ++mismatches;
    }
    // Different seeds should differ on well over half the domain
    REQUIRE(mismatches > 50);
}

TEST_CASE("permuteIndexFeistel: 100k sample is injective (zero duplicate outputs)", "[permutation][feistel]") {
    const int N = 100000;
    const cpp_int n(N);
    const std::string key = "inject_test";

    std::set<cpp_int> seen;
    for (int i = 0; i < N; ++i) {
        cpp_int out = permuteIndexFeistel(cpp_int(i), n, key);
        REQUIRE(out >= 0);
        REQUIRE(out < n);
        auto [_, inserted] = seen.insert(out);
        REQUIRE(inserted); // duplicate would fail here
    }
    REQUIRE(static_cast<int>(seen.size()) == N);
}

// ── permuteIndexFeistel: keyspace edge cases ──────────────────────────────────

TEST_CASE("permuteIndexFeistel works for power-of-two sizes", "[permutation][feistel]") {
    for (int bits = 1; bits <= 20; bits += 4) {
        cpp_int n = cpp_int(1) << bits;
        cpp_int out = permuteIndexFeistel(cpp_int(0), n, "pow2_seed");
        REQUIRE(out >= 0);
        REQUIRE(out < n);
    }
}

TEST_CASE("permuteIndexFeistel works for n=1", "[permutation][feistel]") {
    cpp_int out = permuteIndexFeistel(cpp_int(0), cpp_int(1), "tiny");
    REQUIRE(out == 0);
}

TEST_CASE("permuteIndexFeistel works for near-power-of-two sizes", "[permutation][feistel]") {
    // just below, at, just above a power of two
    for (int base_bits = 4; base_bits <= 20; base_bits += 4) {
        cpp_int base = cpp_int(1) << base_bits;
        for (cpp_int delta : {cpp_int(-1), cpp_int(0), cpp_int(1)}) {
            cpp_int n = base + delta;
            if (n <= 0) continue;
            cpp_int out = permuteIndexFeistel(cpp_int(0), n, "nearpo2");
            REQUIRE(out >= 0);
            REQUIRE(out < n);
        }
    }
}

// ── permuteIndexAffine: basic contract ───────────────────────────────────────

TEST_CASE("permuteIndexAffine output is bounded [0, n)", "[permutation][affine]") {
    const cpp_int n("999983"); // prime
    auto params = deriveAffinePermutationParams("deadbeef", n);

    for (int i = 0; i < 1000; ++i) {
        cpp_int out = permuteIndexAffine(cpp_int(i), n, params.a, params.b);
        REQUIRE(out >= 0);
        REQUIRE(out < n);
    }
}

TEST_CASE("permuteIndexAffine is deterministic", "[permutation][affine]") {
    const cpp_int n("999983");
    auto params = deriveAffinePermutationParams("cafebabe", n);

    cpp_int first  = permuteIndexAffine(cpp_int(77), n, params.a, params.b);
    cpp_int second = permuteIndexAffine(cpp_int(77), n, params.a, params.b);
    REQUIRE(first == second);
}

TEST_CASE("permuteIndexAffine: 10k sample is injective", "[permutation][affine]") {
    const int N = 10000;
    const cpp_int n(N);
    auto params = deriveAffinePermutationParams("affine_inject", n);

    std::set<cpp_int> seen;
    for (int i = 0; i < N; ++i) {
        cpp_int out = permuteIndexAffine(cpp_int(i), n, params.a, params.b);
        REQUIRE(out >= 0);
        REQUIRE(out < n);
        REQUIRE(seen.insert(out).second);
    }
    REQUIRE(static_cast<int>(seen.size()) == N);
}

// ── Benchmarks (informational only — do not gate CI on these) ─────────────────

TEST_CASE("Benchmark: permuteIndexFeistel single call", "[.benchmark][permutation]") {
    const cpp_int n = hexToInt("7fffffffffffffffff");
    const std::string key = "bench_seed";
    cpp_int idx(12345678);
    BENCHMARK("permuteIndexFeistel") {
        return permuteIndexFeistel(idx, n, key);
    };
}

TEST_CASE("Benchmark: permuteIndexAffine single call", "[.benchmark][permutation]") {
    const cpp_int n = hexToInt("7fffffffffffffffff");
    auto params = deriveAffinePermutationParams("bench_affine", n);
    cpp_int idx(12345678);
    BENCHMARK("permuteIndexAffine") {
        return permuteIndexAffine(idx, n, params.a, params.b);
    };
}
