#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gwdash {
    constexpr std::size_t MAX_TRADE_PRESETS = 12;
    constexpr std::size_t MAX_PRESET_NAME_LEN = 24;
    constexpr std::size_t MAX_PRESET_MESSAGE_LEN = 120;
    constexpr int64_t SEND_COOLDOWN_MS = 1000;

    struct TradePreset {
        std::string name;
        std::string kind; // "wtb" | "wts" | "wtt"
        std::string message;
    };

    /** Normalize name to [a-z0-9_-]{1,24}; empty if nothing usable remains. */
    std::string SanitizePresetName(std::string_view raw);

    /** Normalize kind to wtb/wts/wtt; defaults to "wts" when unrecognized. */
    std::string SanitizePresetKind(std::string_view raw);

    /** Trim, strip CR/LF, clamp to MAX_PRESET_MESSAGE_LEN. */
    std::string SanitizePresetMessage(std::string_view raw);

    /**
     * Local store of trade chat presets. Persistence is plugins/GWDash/presets.json.
     * Send goes through GW::Chat::SendChat('$', ...) and must run on the game thread.
     */
    class TradePresets {
    public:
        [[nodiscard]] const std::vector<TradePreset>& List() const { return presets_; }

        bool Load();
        bool Save() const;

        /** Case-insensitive name lookup; nullptr when missing. */
        [[nodiscard]] const TradePreset* Find(std::string_view name) const;

        /**
         * Insert or replace by sanitized name. Returns false when the name is
         * empty, the message is empty, or the store would exceed MAX_TRADE_PRESETS
         * for a new name.
         */
        bool Upsert(std::string_view name, std::string_view kind, std::string_view message,
                    std::string& error);

        bool Remove(std::string_view name);

        /**
         * Send one trade-channel message. Refuses while the map is loading or
         * within SEND_COOLDOWN_MS of the previous send.
         */
        bool TrySend(const TradePreset& preset, std::string& error);
        bool TrySendByName(std::string_view name, std::string& error);

    private:
        std::vector<TradePreset> presets_;
        int64_t last_send_ms_ = 0;
    };
}
