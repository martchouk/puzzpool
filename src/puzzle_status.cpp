#include <puzzpool/puzzle_status.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif __has_include(<openssl/sha.h>)
#include <openssl/sha.h>
#else
#error "No SHA-256 implementation available."
#endif

namespace puzzpool {

namespace {

constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr std::string_view kBech32Charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr uint32_t kBech32Const = 1;
constexpr uint32_t kBech32mConst = 0x2bc830a3;

std::string trimAndUpper(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    bool inSpace = true;
    for (char rawCh : raw) {
        const auto ch = static_cast<unsigned char>(rawCh);
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::toupper(ch)));
            inSpace = false;
        } else if (!inSpace) {
            out.push_back(' ');
            inSpace = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool isDigitsOnly(std::string_view raw) {
    if (raw.empty()) return false;
    return std::all_of(raw.begin(), raw.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

int64_t statValue(const nlohmann::json& stats, const char* key) {
    if (!stats.is_object()) return 0;
    auto it = stats.find(key);
    if (it == stats.end() || !it->is_number_integer()) return 0;
    return it->get<int64_t>();
}

std::vector<unsigned char> decodeBase58(std::string_view input) {
    std::vector<unsigned char> bytes(1, 0);
    for (char rawCh : input) {
        const auto ch = static_cast<unsigned char>(rawCh);
        const auto pos = kBase58Alphabet.find(static_cast<char>(ch));
        if (pos == std::string_view::npos) return {};
        int carry = static_cast<int>(pos);
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
            carry += 58 * static_cast<int>(*it);
            *it = static_cast<unsigned char>(carry & 0xff);
            carry >>= 8;
        }
        while (carry > 0) {
            bytes.insert(bytes.begin(), static_cast<unsigned char>(carry & 0xff));
            carry >>= 8;
        }
    }

    std::size_t leadingZeros = 0;
    while (leadingZeros < input.size() && input[leadingZeros] == '1') ++leadingZeros;

    std::vector<unsigned char> out(leadingZeros, 0);
    auto firstNonZero = std::find_if(bytes.begin(), bytes.end(), [](unsigned char b) { return b != 0; });
    out.insert(out.end(), firstNonZero, bytes.end());
    return out;
}

std::array<unsigned char, 32> sha256Raw(const unsigned char* data, std::size_t size) {
    std::array<unsigned char, 32> digest{};
#if defined(__APPLE__)
    CC_SHA256(data, static_cast<CC_LONG>(size), digest.data());
#else
    SHA256(data, size, digest.data());
#endif
    return digest;
}

bool hasValidBase58Checksum(std::string_view input) {
    const auto decoded = decodeBase58(input);
    if (decoded.size() < 5) return false;
    const std::vector<unsigned char> payload(decoded.begin(), decoded.end() - 4);
    const auto first = sha256Raw(payload.data(), payload.size());
    const auto second = sha256Raw(first.data(), first.size());
    return std::equal(decoded.end() - 4, decoded.end(), second.begin());
}

std::vector<int> hrpExpand(std::string_view hrp) {
    std::vector<int> values;
    values.reserve(hrp.size() * 2 + 1);
    for (char rawCh : hrp) values.push_back(static_cast<unsigned char>(rawCh) >> 5);
    values.push_back(0);
    for (char rawCh : hrp) values.push_back(static_cast<unsigned char>(rawCh) & 31);
    return values;
}

uint32_t bech32Polymod(const std::vector<int>& values) {
    uint32_t chk = 1;
    constexpr uint32_t gen[5] = {
        0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3
    };
    for (int v : values) {
        const uint32_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ static_cast<uint32_t>(v);
        for (int i = 0; i < 5; ++i) {
            if ((top >> i) & 1U) chk ^= gen[i];
        }
    }
    return chk;
}

bool decodeBech32(std::string_view input, std::string& hrp, std::vector<int>& data, uint32_t& encodingConst) {
    if (input.size() < 14 || input.size() > 90) return false;

    bool hasLower = false;
    bool hasUpper = false;
    for (char rawCh : input) {
        const auto ch = static_cast<unsigned char>(rawCh);
        if (ch < 33 || ch > 126) return false;
        hasLower = hasLower || (std::islower(ch) != 0);
        hasUpper = hasUpper || (std::isupper(ch) != 0);
    }
    if (hasLower && hasUpper) return false;

    std::string normalized(input);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    const auto pos = normalized.rfind('1');
    if (pos == std::string::npos || pos == 0 || pos + 7 > normalized.size()) return false;

    hrp = normalized.substr(0, pos);
    data.clear();
    data.reserve(normalized.size() - pos - 1);
    for (std::size_t i = pos + 1; i < normalized.size(); ++i) {
        const auto idx = kBech32Charset.find(normalized[i]);
        if (idx == std::string_view::npos) return false;
        data.push_back(static_cast<int>(idx));
    }

    auto check = hrpExpand(hrp);
    check.insert(check.end(), data.begin(), data.end());
    const uint32_t polymod = bech32Polymod(check);
    if (polymod == kBech32Const) {
        encodingConst = kBech32Const;
        return true;
    }
    if (polymod == kBech32mConst) {
        encodingConst = kBech32mConst;
        return true;
    }
    return false;
}

bool convertBits(const std::vector<int>& input, int fromBits, int toBits, bool pad, std::vector<unsigned char>& out) {
    int acc = 0;
    int bits = 0;
    const int maxv = (1 << toBits) - 1;
    const int maxAcc = (1 << (fromBits + toBits - 1)) - 1;
    out.clear();
    for (int value : input) {
        if (value < 0 || (value >> fromBits) != 0) return false;
        acc = ((acc << fromBits) | value) & maxAcc;
        bits += fromBits;
        while (bits >= toBits) {
            bits -= toBits;
            out.push_back(static_cast<unsigned char>((acc >> bits) & maxv));
        }
    }
    if (pad) {
        if (bits != 0) out.push_back(static_cast<unsigned char>((acc << (toBits - bits)) & maxv));
    } else if (bits >= fromBits || ((acc << (toBits - bits)) & maxv) != 0) {
        return false;
    }
    return true;
}

bool isValidBech32Address(std::string_view input) {
    std::string hrp;
    std::vector<int> data;
    uint32_t encodingConst = 0;
    if (!decodeBech32(input, hrp, data, encodingConst)) return false;
    if (hrp != "bc" && hrp != "tb") return false;
    if (data.size() < 7) return false;

    const int witnessVersion = data.front();
    if (witnessVersion < 0 || witnessVersion > 16) return false;

    std::vector<int> payload(data.begin() + 1, data.end() - 6);
    std::vector<unsigned char> program;
    if (!convertBits(payload, 5, 8, false, program)) return false;
    if (program.size() < 2 || program.size() > 40) return false;
    if (witnessVersion == 0) {
        if (encodingConst != kBech32Const) return false;
        if (program.size() != 20 && program.size() != 32) return false;
    } else if (encodingConst != kBech32mConst) {
        return false;
    }
    return true;
}

} // namespace

std::string canonicalPuzzleName(std::string_view raw) {
    return trimAndUpper(raw);
}

std::string canonicalPuzzleTargetNameFromEnv(std::string_view raw) {
    std::string canonical = canonicalPuzzleName(raw);
    if (isDigitsOnly(canonical)) return "PUZZLE " + canonical;
    return canonical;
}

std::string puzzleStatusTargetTypeToString(PuzzleStatusTargetType type) {
    switch (type) {
        case PuzzleStatusTargetType::Address: return "address";
        case PuzzleStatusTargetType::FindingsThreshold: return "findings_threshold";
    }
    return "address";
}

std::optional<PuzzleStatusTargetType> parsePuzzleStatusTargetType(std::string_view raw) {
    if (raw == "address") return PuzzleStatusTargetType::Address;
    if (raw == "findings_threshold") return PuzzleStatusTargetType::FindingsThreshold;
    return std::nullopt;
}

std::string puzzleStatusStateToString(PuzzleStatusState state) {
    switch (state) {
        case PuzzleStatusState::Unknown: return "unknown";
        case PuzzleStatusState::Unsolved: return "unsolved";
        case PuzzleStatusState::Solved: return "solved";
    }
    return "unknown";
}

std::optional<PuzzleStatusState> parsePuzzleStatusState(std::string_view raw) {
    if (raw == "unknown") return PuzzleStatusState::Unknown;
    if (raw == "unsolved") return PuzzleStatusState::Unsolved;
    if (raw == "solved") return PuzzleStatusState::Solved;
    return std::nullopt;
}

bool isValidBitcoinAddress(std::string_view address) {
    if (address.empty()) return false;
    if (address.rfind("bc1", 0) == 0 || address.rfind("BC1", 0) == 0 ||
        address.rfind("tb1", 0) == 0 || address.rfind("TB1", 0) == 0) {
        return isValidBech32Address(address);
    }
    return hasValidBase58Checksum(address);
}

PuzzleStatusState evaluateAddressTargetStatus(const nlohmann::json& response) {
    const auto chainStats = response.value("chain_stats", nlohmann::json::object());
    const auto mempoolStats = response.value("mempool_stats", nlohmann::json::object());

    const bool spent =
        statValue(chainStats, "spent_txo_count") > 0 ||
        statValue(chainStats, "spent_txo_sum") > 0 ||
        statValue(mempoolStats, "spent_txo_count") > 0 ||
        statValue(mempoolStats, "spent_txo_sum") > 0;
    if (spent) return PuzzleStatusState::Solved;

    const int64_t fundedSum =
        statValue(chainStats, "funded_txo_sum") +
        statValue(mempoolStats, "funded_txo_sum");
    if (fundedSum > 0) return PuzzleStatusState::Unsolved;
    return PuzzleStatusState::Unknown;
}

PuzzleStatusState evaluateFindingsThresholdStatus(std::int64_t distinctFoundKeys, std::int64_t threshold) {
    if (threshold <= 0) return PuzzleStatusState::Unknown;
    return distinctFoundKeys >= threshold ? PuzzleStatusState::Solved : PuzzleStatusState::Unsolved;
}

} // namespace puzzpool
