#include "TradePresets.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/MapMgr.h>

#include <PluginUtils.h>
#include <glaze/glaze.hpp>

#include "Paths.h"

namespace gwdash {
    struct PresetsFilePayload {
        int v = 1;
        std::vector<TradePreset> presets;
    };
}

namespace {
    constexpr std::size_t MAX_PRESETS_FILE_BYTES = 64 * 1024;

    int64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    std::string ToLowerAscii(std::string_view raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (const unsigned char ch : raw) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
        return out;
    }

    bool NamesEqual(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }
}

template <>
struct glz::meta<gwdash::TradePreset> {
    using T = gwdash::TradePreset;
    static constexpr auto value = object(
        "name", &T::name,
        "kind", &T::kind,
        "message", &T::message);
};

template <>
struct glz::meta<gwdash::PresetsFilePayload> {
    using T = gwdash::PresetsFilePayload;
    static constexpr auto value = object(
        "v", &T::v,
        "presets", &T::presets);
};

namespace gwdash {
    std::string SanitizePresetName(const std::string_view raw)
    {
        std::string out;
        out.reserve(std::min(raw.size(), MAX_PRESET_NAME_LEN));
        for (const unsigned char ch : raw) {
            if (out.size() >= MAX_PRESET_NAME_LEN) {
                break;
            }
            if (std::isalnum(ch) || ch == '_' || ch == '-') {
                out.push_back(static_cast<char>(std::tolower(ch)));
            }
        }
        return out;
    }

    std::string SanitizePresetKind(const std::string_view raw)
    {
        const std::string lower = ToLowerAscii(raw);
        if (lower == "wtb" || lower == "wts" || lower == "wtt") {
            return lower;
        }
        return "wts";
    }

    std::string SanitizePresetMessage(const std::string_view raw)
    {
        std::string out;
        out.reserve(std::min(raw.size(), MAX_PRESET_MESSAGE_LEN));

        // Trim leading whitespace.
        std::size_t start = 0;
        while (start < raw.size() &&
               std::isspace(static_cast<unsigned char>(raw[start]))) {
            ++start;
        }

        for (std::size_t i = start; i < raw.size() && out.size() < MAX_PRESET_MESSAGE_LEN; ++i) {
            const unsigned char ch = static_cast<unsigned char>(raw[i]);
            if (ch == '\r' || ch == '\n') {
                continue;
            }
            out.push_back(static_cast<char>(ch));
        }

        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
            out.pop_back();
        }
        return out;
    }

    bool TradePresets::Load()
    {
        const std::string json = ReadFile(PresetsFile(), MAX_PRESETS_FILE_BYTES);
        if (json.empty()) {
            presets_.clear();
            return true;
        }

        constexpr glz::opts lenient{.error_on_unknown_keys = false};
        gwdash::PresetsFilePayload staged{};
        if (glz::read<lenient>(staged, json)) {
            return false;
        }

        std::vector<TradePreset> cleaned;
        cleaned.reserve(std::min(staged.presets.size(), MAX_TRADE_PRESETS));
        for (const TradePreset& entry : staged.presets) {
            if (cleaned.size() >= MAX_TRADE_PRESETS) {
                break;
            }
            TradePreset next{
                .name = SanitizePresetName(entry.name),
                .kind = SanitizePresetKind(entry.kind),
                .message = SanitizePresetMessage(entry.message),
            };
            if (next.name.empty() || next.message.empty()) {
                continue;
            }
            const bool duplicate = std::any_of(
                cleaned.begin(), cleaned.end(),
                [&](const TradePreset& existing) { return NamesEqual(existing.name, next.name); });
            if (duplicate) {
                continue;
            }
            cleaned.push_back(std::move(next));
        }

        presets_ = std::move(cleaned);
        return true;
    }

    bool TradePresets::Save() const
    {
        const gwdash::PresetsFilePayload payload{.v = 1, .presets = presets_};
        std::string json;
        if (glz::write_json(payload, json)) {
            return false;
        }
        return WriteFileAtomic(PresetsFile(), json);
    }

    const TradePreset* TradePresets::Find(const std::string_view name) const
    {
        const std::string key = SanitizePresetName(name);
        if (key.empty()) {
            return nullptr;
        }
        for (const TradePreset& preset : presets_) {
            if (NamesEqual(preset.name, key)) {
                return &preset;
            }
        }
        return nullptr;
    }

    bool TradePresets::Upsert(const std::string_view name, const std::string_view kind,
                              const std::string_view message, std::string& error)
    {
        TradePreset next{
            .name = SanitizePresetName(name),
            .kind = SanitizePresetKind(kind),
            .message = SanitizePresetMessage(message),
        };
        if (next.name.empty()) {
            error = "Preset name must use letters, numbers, _ or -.";
            return false;
        }
        if (next.message.empty()) {
            error = "Preset message is empty.";
            return false;
        }

        for (TradePreset& existing : presets_) {
            if (NamesEqual(existing.name, next.name)) {
                existing = std::move(next);
                error.clear();
                return true;
            }
        }

        if (presets_.size() >= MAX_TRADE_PRESETS) {
            error = "At most 12 trade presets.";
            return false;
        }

        presets_.push_back(std::move(next));
        error.clear();
        return true;
    }

    bool TradePresets::Remove(const std::string_view name)
    {
        const std::string key = SanitizePresetName(name);
        if (key.empty()) {
            return false;
        }
        const auto it = std::remove_if(
            presets_.begin(), presets_.end(),
            [&](const TradePreset& preset) { return NamesEqual(preset.name, key); });
        if (it == presets_.end()) {
            return false;
        }
        presets_.erase(it, presets_.end());
        return true;
    }

    bool TradePresets::TrySend(const TradePreset& preset, std::string& error)
    {
        const std::string message = SanitizePresetMessage(preset.message);
        if (message.empty()) {
            error = "Preset message is empty.";
            return false;
        }

        if (GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading) {
            error = "Cannot send while the map is loading.";
            return false;
        }

        const int64_t now = NowMs();
        if (last_send_ms_ > 0 && now - last_send_ms_ < SEND_COOLDOWN_MS) {
            error = "Wait a moment before sending again.";
            return false;
        }

        const std::wstring wide = PluginUtils::StringToWString(message);
        if (!GW::Chat::SendChat('$', wide.c_str())) {
            error = "Failed to send to trade chat (are you in a trade district?).";
            return false;
        }

        last_send_ms_ = now;
        error.clear();
        return true;
    }

    bool TradePresets::TrySendByName(const std::string_view name, std::string& error)
    {
        const TradePreset* preset = Find(name);
        if (!preset) {
            error = "Unknown preset. Type /gwdash presets to list them.";
            return false;
        }
        return TrySend(*preset, error);
    }
}
