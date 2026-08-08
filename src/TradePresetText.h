#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gwdash {
    constexpr std::size_t MAX_TRADE_PRESETS = 12;
    constexpr std::size_t MAX_PRESET_NAME_LEN = 24;
    constexpr std::size_t MAX_PRESET_MESSAGE_LEN = 120;
    constexpr int64_t SEND_COOLDOWN_MS = 3000;

    std::string SanitizePresetName(std::string_view raw);
    std::string SanitizePresetKind(std::string_view raw);
    std::string SanitizePresetMessage(std::string_view raw);
    std::string ComposeTradeLine(std::string_view kind, std::string_view message);
}
