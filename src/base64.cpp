#include <puzzpool/base64.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace puzzpool {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/// Reverse lookup table; 0xff marks every byte outside the base64url alphabet.
constexpr std::array<std::uint8_t, 256> makeDecodeTable() {
    std::array<std::uint8_t, 256> table{};
    for (auto& slot : table) slot = 0xff;
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::uint8_t>(i);
    }
    return table;
}

constexpr std::array<std::uint8_t, 256> kDecodeTable = makeDecodeTable();

} // namespace

std::string base64UrlEncode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);

    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3f]);
        out.push_back(kAlphabet[triple & 0x3f]);
    }

    const std::size_t remaining = bytes.size() - i;
    if (remaining == 1) {
        const std::uint32_t v =
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
    } else if (remaining == 2) {
        const std::uint32_t v =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(kAlphabet[(v >> 6) & 0x3f]);
    }
    return out;
}

std::optional<std::string> base64UrlDecode(std::string_view text) {
    // A length of 1 (mod 4) cannot be produced by the encoder above: one leftover
    // input byte yields two characters, two yield three. Rejecting it here keeps
    // decode a strict inverse rather than silently accepting a truncated token.
    if (text.size() % 4 == 1) return std::nullopt;

    std::string out;
    out.reserve(text.size() / 4 * 3 + 2);

    std::uint32_t buffer = 0;
    int bits = 0;
    for (char ch : text) {
        const std::uint8_t value = kDecodeTable[static_cast<unsigned char>(ch)];
        if (value == 0xff) return std::nullopt;   // includes '=' — padding is rejected
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    // Any leftover bits must be zero padding produced by the encoder; a non-zero
    // remainder means the text was not produced by base64UrlEncode.
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) return std::nullopt;
    return out;
}

} // namespace puzzpool
