#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "TradePresetText.h"

namespace gwdash {
    struct TradePreset {
        std::string name;
        std::string kind; // "wtb" | "wts" | "wtt"
        std::string message;
        bool is_default = false;
    };

    /** True when the character is in an outpost (trade chat is usable). */
    [[nodiscard]] bool CanSendTradeChat();

    /**
     * Local store of trade chat presets. Persistence is plugins/GWDash/presets.json.
     * Send goes through GW::Chat::SendChat('$', ...) and must run on the game thread.
     */
    class TradePresets {
    public:
        [[nodiscard]] const std::vector<TradePreset>& List() const { return presets_; }
        [[nodiscard]] bool LoadOk() const { return load_ok_; }
        [[nodiscard]] const std::string& LoadError() const { return load_error_; }

        bool Load();
        bool Save() const;
        void Reset();

        [[nodiscard]] const TradePreset* Find(std::string_view name) const;
        [[nodiscard]] const TradePreset* DefaultPreset() const;

        bool Upsert(std::string_view name, std::string_view kind, std::string_view message,
                    std::string& error);
        bool Remove(std::string_view name);
        bool Rename(std::string_view from, std::string_view to, std::string& error);
        bool Duplicate(std::string_view name, std::string& error);
        bool Move(std::string_view name, int delta, std::string& error);
        bool SetDefault(std::string_view name, std::string& error);

        bool ExportToFile(const std::filesystem::path& path, std::string& error) const;
        bool ImportFromFile(const std::filesystem::path& path, std::string& error);

        bool TrySend(const TradePreset& preset, std::string& error);
        bool TrySendByName(std::string_view name, std::string& error);

    private:
        bool WritePayload(const std::filesystem::path& path, std::string& error) const;
        bool ReadPayload(const std::string& json, std::vector<TradePreset>& out,
                         std::string& error) const;

        std::vector<TradePreset> presets_;
        int64_t last_send_ms_ = 0;
        bool load_ok_ = true;
        std::string load_error_;
    };
}
