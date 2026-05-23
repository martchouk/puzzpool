#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace puzzpool {

enum class PuzzleStatusTargetType {
    Address,
    FindingsThreshold,
};

struct PuzzleStatusTargetConfig {
    PuzzleStatusTargetType type = PuzzleStatusTargetType::Address;
    std::string value;
};

enum class PuzzleStatusState {
    Unknown,
    Unsolved,
    Solved,
};

std::string canonicalPuzzleName(std::string_view raw);
std::string canonicalPuzzleTargetNameFromEnv(std::string_view raw);

std::string puzzleStatusTargetTypeToString(PuzzleStatusTargetType type);
std::optional<PuzzleStatusTargetType> parsePuzzleStatusTargetType(std::string_view raw);

std::string puzzleStatusStateToString(PuzzleStatusState state);
std::optional<PuzzleStatusState> parsePuzzleStatusState(std::string_view raw);

bool isValidBitcoinAddress(std::string_view address);

PuzzleStatusState evaluateAddressTargetStatus(const nlohmann::json& response);
PuzzleStatusState evaluateFindingsThresholdStatus(std::int64_t distinctFoundKeys, std::int64_t threshold);

} // namespace puzzpool
