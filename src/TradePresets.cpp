#include "TradePresets.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <string_view>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/MapMgr.h>

#include <PluginUtils.h>
#include <glaze/glaze.hpp>

#include "Paths.h"
#include "TimeUtil.h"

namespace gwdash {
    struct PresetsFilePayload {
        int v = 1;
        std::vector<TradePreset> presets;
    };
}

namespace {
    constexpr std::size_t MAX_PRESETS_FILE_BYTES = 64 * 1024;

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

    std::string UniqueCopyName(const std::vector<gwdash::TradePreset>& presets,
                               std::string_view base)
    {
        std::string candidate = gwdash::SanitizePresetName(std::string(base) + "_copy");
        if (candidate.empty()) {
            candidate = "copy";
        }
        if (candidate.size() > gwdash::MAX_PRESET_NAME_LEN) {
            candidate.resize(gwdash::MAX_PRESET_NAME_LEN);
        }
        int suffix = 2;
        for (;;) {
            const bool taken = std::any_of(
                presets.begin(), presets.end(),
                [&](const gwdash::TradePreset& p) { return NamesEqual(p.name, candidate); });
            if (!taken) {
                return candidate;
            }
            const std::string numbered = gwdash::SanitizePresetName(
                std::string(base) + "_" + std::to_string(suffix++));
            candidate = numbered.empty() ? ("copy" + std::to_string(suffix)) : numbered;
            if (candidate.size() > gwdash::MAX_PRESET_NAME_LEN) {
                candidate.resize(gwdash::MAX_PRESET_NAME_LEN);
            }
        }
    }
}

template <>
struct glz::meta<gwdash::TradePreset> {
    using T = gwdash::TradePreset;
    static constexpr auto value = object(
        "name", &T::name,
        "kind", &T::kind,
        "message", &T::message,
        "default", &T::is_default);
};

template <>
struct glz::meta<gwdash::PresetsFilePayload> {
    using T = gwdash::PresetsFilePayload;
    static constexpr auto value = object(
        "v", &T::v,
        "presets", &T::presets);
};

namespace gwdash {
    bool CanSendTradeChat()
    {
        return GW::Map::GetInstanceType() == GW::Constants::InstanceType::Outpost;
    }

    bool TradePresets::ReadPayload(const std::string& json, std::vector<TradePreset>& out,
                                   std::string& error) const
    {
        constexpr glz::opts lenient{.error_on_unknown_keys = false};
        PresetsFilePayload staged{};
        if (glz::read<lenient>(staged, json)) {
            error = "Could not parse presets JSON.";
            return false;
        }

        std::vector<TradePreset> cleaned;
        cleaned.reserve(std::min(staged.presets.size(), MAX_TRADE_PRESETS));
        bool saw_default = false;
        for (const TradePreset& entry : staged.presets) {
            if (cleaned.size() >= MAX_TRADE_PRESETS) {
                break;
            }
            TradePreset next{
                .name = SanitizePresetName(entry.name),
                .kind = SanitizePresetKind(entry.kind),
                .message = SanitizePresetMessage(entry.message),
                .is_default = entry.is_default && !saw_default,
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
            if (next.is_default) {
                saw_default = true;
            }
            cleaned.push_back(std::move(next));
        }

        out = std::move(cleaned);
        error.clear();
        return true;
    }

    bool TradePresets::WritePayload(const std::filesystem::path& path, std::string& error) const
    {
        const PresetsFilePayload payload{.v = 1, .presets = presets_};
        std::string json;
        if (glz::write_json(payload, json)) {
            error = "Could not serialize presets.";
            return false;
        }
        if (!WriteFileAtomic(path, json)) {
            error = "Could not write presets file.";
            return false;
        }
        error.clear();
        return true;
    }

    bool TradePresets::Load()
    {
        const std::string json = ReadFile(PresetsFile(), MAX_PRESETS_FILE_BYTES);
        if (json.empty()) {
            presets_.clear();
            load_ok_ = true;
            load_error_.clear();
            return true;
        }

        std::vector<TradePreset> cleaned;
        std::string error;
        if (!ReadPayload(json, cleaned, error)) {
            load_ok_ = false;
            load_error_ = error;
            return false;
        }

        presets_ = std::move(cleaned);
        load_ok_ = true;
        load_error_.clear();
        return true;
    }

    bool TradePresets::Save() const
    {
        if (!load_ok_) {
            return false;
        }
        std::string error;
        return WritePayload(PresetsFile(), error);
    }

    void TradePresets::Reset()
    {
        presets_.clear();
        load_ok_ = true;
        load_error_.clear();
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

    const TradePreset* TradePresets::DefaultPreset() const
    {
        for (const TradePreset& preset : presets_) {
            if (preset.is_default) {
                return &preset;
            }
        }
        return presets_.empty() ? nullptr : &presets_.front();
    }

    bool TradePresets::Upsert(const std::string_view name, const std::string_view kind,
                              const std::string_view message, std::string& error)
    {
        if (!load_ok_) {
            error = "Presets file failed to load. Reset or fix presets.json first.";
            return false;
        }

        TradePreset next{
            .name = SanitizePresetName(name),
            .kind = SanitizePresetKind(kind),
            .message = SanitizePresetMessage(message),
            .is_default = false,
        };
        if (next.name.empty()) {
            error = "Preset name must use letters, numbers, _ or -.";
            return false;
        }
        if (next.message.empty() || ComposeTradeLine(next.kind, next.message).empty()) {
            error = "Preset message is empty.";
            return false;
        }

        for (TradePreset& existing : presets_) {
            if (NamesEqual(existing.name, next.name)) {
                next.is_default = existing.is_default;
                existing = std::move(next);
                error.clear();
                return true;
            }
        }

        if (presets_.size() >= MAX_TRADE_PRESETS) {
            error = "At most 12 trade presets.";
            return false;
        }

        if (presets_.empty()) {
            next.is_default = true;
        }
        presets_.push_back(std::move(next));
        error.clear();
        return true;
    }

    bool TradePresets::Remove(const std::string_view name)
    {
        if (!load_ok_) {
            return false;
        }
        const std::string key = SanitizePresetName(name);
        if (key.empty()) {
            return false;
        }
        const auto it = std::find_if(
            presets_.begin(), presets_.end(),
            [&](const TradePreset& preset) { return NamesEqual(preset.name, key); });
        if (it == presets_.end()) {
            return false;
        }
        const bool was_default = it->is_default;
        presets_.erase(it);
        if (was_default && !presets_.empty()) {
            presets_.front().is_default = true;
        }
        return true;
    }

    bool TradePresets::Rename(const std::string_view from, const std::string_view to,
                              std::string& error)
    {
        if (!load_ok_) {
            error = "Presets file failed to load. Reset or fix presets.json first.";
            return false;
        }
        const std::string new_name = SanitizePresetName(to);
        if (new_name.empty()) {
            error = "Preset name must use letters, numbers, _ or -.";
            return false;
        }

        TradePreset* existing = nullptr;
        for (TradePreset& preset : presets_) {
            if (NamesEqual(preset.name, from)) {
                existing = &preset;
                break;
            }
        }
        if (!existing) {
            error = "Unknown preset.";
            return false;
        }
        if (!NamesEqual(existing->name, new_name) && Find(new_name)) {
            error = "A preset with that name already exists.";
            return false;
        }

        existing->name = new_name;
        error.clear();
        return true;
    }

    bool TradePresets::Duplicate(const std::string_view name, std::string& error)
    {
        if (!load_ok_) {
            error = "Presets file failed to load. Reset or fix presets.json first.";
            return false;
        }
        const TradePreset* existing = Find(name);
        if (!existing) {
            error = "Unknown preset.";
            return false;
        }
        if (presets_.size() >= MAX_TRADE_PRESETS) {
            error = "At most 12 trade presets.";
            return false;
        }
        const std::string copy_name = UniqueCopyName(presets_, existing->name);
        return Upsert(copy_name, existing->kind, existing->message, error);
    }

    bool TradePresets::Move(const std::string_view name, const int delta, std::string& error)
    {
        if (!load_ok_) {
            error = "Presets file failed to load. Reset or fix presets.json first.";
            return false;
        }
        if (delta == 0) {
            error.clear();
            return true;
        }
        const auto it = std::find_if(
            presets_.begin(), presets_.end(),
            [&](const TradePreset& preset) { return NamesEqual(preset.name, name); });
        if (it == presets_.end()) {
            error = "Unknown preset.";
            return false;
        }
        const auto index = static_cast<int>(std::distance(presets_.begin(), it));
        const int target = index + delta;
        if (target < 0 || target >= static_cast<int>(presets_.size())) {
            error.clear();
            return true;
        }
        std::swap(presets_[static_cast<std::size_t>(index)],
                  presets_[static_cast<std::size_t>(target)]);
        error.clear();
        return true;
    }

    bool TradePresets::SetDefault(const std::string_view name, std::string& error)
    {
        if (!load_ok_) {
            error = "Presets file failed to load. Reset or fix presets.json first.";
            return false;
        }
        TradePreset* found = nullptr;
        for (TradePreset& preset : presets_) {
            if (NamesEqual(preset.name, name)) {
                found = &preset;
            }
        }
        if (!found) {
            error = "Unknown preset.";
            return false;
        }
        for (TradePreset& preset : presets_) {
            preset.is_default = false;
        }
        found->is_default = true;
        error.clear();
        return true;
    }

    bool TradePresets::ExportToFile(const std::filesystem::path& path, std::string& error) const
    {
        if (!load_ok_) {
            error = "Presets file failed to load. Reset or fix presets.json first.";
            return false;
        }
        return WritePayload(path, error);
    }

    bool TradePresets::ImportFromFile(const std::filesystem::path& path, std::string& error)
    {
        const std::string json = ReadFile(path, MAX_PRESETS_FILE_BYTES);
        if (json.empty()) {
            error = "Import file is missing or empty.";
            return false;
        }
        std::vector<TradePreset> cleaned;
        if (!ReadPayload(json, cleaned, error)) {
            return false;
        }
        presets_ = std::move(cleaned);
        load_ok_ = true;
        load_error_.clear();
        error.clear();
        return true;
    }

    bool TradePresets::TrySend(const TradePreset& preset, std::string& error)
    {
        const std::string line = ComposeTradeLine(preset.kind, preset.message);
        if (line.empty()) {
            error = "Preset message is empty.";
            return false;
        }

        if (GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading) {
            error = "Cannot send while the map is loading.";
            return false;
        }
        if (!CanSendTradeChat()) {
            error = "Trade chat is only available in an outpost.";
            return false;
        }

        const int64_t now = NowMs();
        if (last_send_ms_ > 0 && now - last_send_ms_ < SEND_COOLDOWN_MS) {
            error = "Wait a moment before sending again.";
            return false;
        }

        const std::wstring wide = PluginUtils::StringToWString(line);
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
