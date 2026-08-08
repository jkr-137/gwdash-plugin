#include "GWDashPlugin.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Utilities/Hook.h>

#include <PluginUtils.h>

#include "Paths.h"
#include "PriceSnapshot.h"
#include "TimeUtil.h"
#include "Version.h"

namespace {
    GW::HookEntry chat_command_hook;

    constexpr char ICON_COINS[] = "\xEF\x94\x9E";

    constexpr std::array<int, 3> REFRESH_CHOICES{60, 120, 300};
    constexpr float UNFOCUSED_IDLE_SECONDS = 600.0f;
    constexpr int64_t STALE_AFTER_MS = 6 * 60 * 60 * 1000;

    constexpr const char* KIND_LABELS[] = {"wtb", "wts", "wtt"};
    constexpr const char* KIND_COMBO = "wtb\0wts\0wtt\0";

    const ImVec4 COLOR_STALE{1.0f, 0.82f, 0.35f, 1.0f};
    const ImVec4 COLOR_MISSING{0.6f, 0.6f, 0.6f, 1.0f};
    const ImVec4 COLOR_ERROR{1.0f, 0.42f, 0.42f, 1.0f};

    int KindIndex(const std::string& kind)
    {
        if (kind == "wtb") {
            return 0;
        }
        if (kind == "wtt") {
            return 2;
        }
        return 1;
    }

    const char* KindLabel(const int index)
    {
        if (index < 0 || index > 2) {
            return KIND_LABELS[1];
        }
        return KIND_LABELS[index];
    }

    void WriteChat(const std::string& message)
    {
        const std::wstring wide = PluginUtils::StringToWString(message);
        GW::Chat::WriteChat(GW::Chat::Channel::CHANNEL_GWCA2, wide.c_str(), L"GWDash", true);
    }

    bool GuildWarsHasFocus()
    {
        const HWND window = GW::MemoryMgr::GetGWWindowHandle();
        return window == nullptr || GetForegroundWindow() == window;
    }

    std::filesystem::path PickJsonFile(const bool save_dialog)
    {
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GW::MemoryMgr::GetGWWindowHandle();
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (save_dialog) {
            ofn.Flags |= OFN_OVERWRITEPROMPT;
            ofn.lpstrDefExt = L"json";
            if (!GetSaveFileNameW(&ofn)) {
                return {};
            }
        }
        else {
            ofn.Flags |= OFN_FILEMUSTEXIST;
            if (!GetOpenFileNameW(&ofn)) {
                return {};
            }
        }
        return file;
    }

    void DrawValueRow(const char* label,
                      const std::string& value,
                      const ImVec4& value_color,
                      const std::string& age,
                      const char* tooltip)
    {
        ImGui::BeginGroup();

        const float row_start = ImGui::GetCursorPosX();
        const float row_width = ImGui::GetContentRegionAvail().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;

        ImGui::TextUnformatted(label);

        const float value_width = ImGui::CalcTextSize(value.c_str()).x;
        const float age_width = age.empty() ? 0.0f : ImGui::CalcTextSize(age.c_str()).x + spacing;

        ImGui::SameLine(row_start + row_width - value_width - age_width);
        ImGui::TextColored(value_color, "%s", value.c_str());

        if (!age.empty()) {
            ImGui::SameLine(0.0f, spacing);
            ImGui::TextDisabled("%s", age.c_str());
        }

        ImGui::EndGroup();

        if (tooltip && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
    }

    std::string ChatPriceTooltip(const char* title, const gwdash::ChatPrice& price, const bool ready)
    {
        if (!ready) {
            return std::string(title) + " - waiting for trade-chat mentions";
        }
        return std::string(title) + " - recency-weighted median (n=" + std::to_string(price.n) + ")";
    }
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static GWDashPlugin instance;
    return &instance;
}

namespace {
    GWDashPlugin* Plugin()
    {
        return static_cast<GWDashPlugin*>(ToolboxPluginInstance());
    }

    void OnGwdashCommand(GW::HookStatus*, const wchar_t*, const int argc, const LPWSTR* argv)
    {
        if (const auto plugin = Plugin()) {
            plugin->OnChatCommand(argc, argv);
        }
    }
}

GWDashPlugin::GWDashPlugin()
{
    can_show_in_main_window = true;
    can_close = true;
}

const char* GWDashPlugin::Icon() const
{
    return ICON_COINS;
}

void GWDashPlugin::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, allocator_fns, toolbox_dll);

    GW::Chat::CreateCommand(&chat_command_hook, L"gwdash", OnGwdashCommand);

    ApplyRefreshInterval();
    updater_.SetAutoInstall(auto_update_);
    trade_presets_.Load();
    prices_.Start();
    updater_.Start();
    started_ = true;
}

void GWDashPlugin::SignalTerminate()
{
    ToolboxUIPlugin::SignalTerminate();
    GW::Chat::DeleteCommand(&chat_command_hook, L"gwdash");

    prices_.SignalStop();
    updater_.SignalStop();
}

bool GWDashPlugin::CanTerminate()
{
    return prices_.Finished() && updater_.Finished();
}

void GWDashPlugin::Terminate()
{
    prices_.Join();
    updater_.Join();
    started_ = false;
    ToolboxUIPlugin::Terminate();
}

void GWDashPlugin::ApplyRefreshInterval()
{
    const int index = std::clamp(refresh_index_, 0,
                                 static_cast<int>(REFRESH_CHOICES.size()) - 1);
    prices_.SetIntervalSeconds(REFRESH_CHOICES[static_cast<size_t>(index)]);
}

void GWDashPlugin::PersistPresets()
{
    if (!trade_presets_.LoadOk()) {
        WriteChat("Not saving trade presets - load failed. Reset or fix presets.json.");
        return;
    }
    if (!trade_presets_.Save()) {
        WriteChat("Could not save trade presets to disk.");
    }
}

void GWDashPlugin::RequestSendPreset(const gwdash::TradePreset& preset, const bool confirm)
{
    pending_send_name_ = preset.name;
    pending_send_line_ = gwdash::ComposeTradeLine(preset.kind, preset.message);
    if (confirm) {
        open_send_confirm_ = true;
        return;
    }
    SendPreset(preset);
}

void GWDashPlugin::SendPreset(const gwdash::TradePreset& preset)
{
    std::string error;
    if (trade_presets_.TrySend(preset, error)) {
        WriteChat(std::string("Sent to trade: ") +
                  gwdash::ComposeTradeLine(preset.kind, preset.message));
        return;
    }
    WriteChat(error.empty() ? "Failed to send trade preset." : error);
}

void GWDashPlugin::SendPresetByName(const std::string& name)
{
    if (const gwdash::TradePreset* preset = trade_presets_.Find(name)) {
        SendPreset(*preset);
        return;
    }
    WriteChat("Unknown preset. Type /gwdash presets to list them.");
}

void GWDashPlugin::PollSendHotkey()
{
    if (send_hotkey_vk_ <= 0 || !GuildWarsHasFocus()) {
        hotkey_was_down_ = false;
        return;
    }

    const bool down = (GetAsyncKeyState(send_hotkey_vk_) & 0x8000) != 0;
    const bool edge = down && !hotkey_was_down_;
    hotkey_was_down_ = down;
    if (!edge) {
        return;
    }

    if (const gwdash::TradePreset* preset = trade_presets_.DefaultPreset()) {
        RequestSendPreset(*preset, true);
    }
}

void GWDashPlugin::Update(const float delta)
{
    if (!started_) {
        return;
    }

    if (GuildWarsHasFocus()) {
        seconds_unfocused_ = 0.0f;
    }
    else {
        seconds_unfocused_ += delta;
    }

    const bool idle = throttle_unfocused_ && seconds_unfocused_ > UNFOCUSED_IDLE_SECONDS;
    if (idle != prices_idle_) {
        prices_idle_ = idle;
        prices_.SetIdle(idle);
    }

    PollSendHotkey();

    seconds_since_tick_ += delta;
    if (seconds_since_tick_ < 1.0f) {
        return;
    }
    seconds_since_tick_ = 0.0f;

    for (const std::string& message : updater_.TakeNotifications()) {
        WriteChat(message);
    }
}

void GWDashPlugin::DrawRows(const gwdash::PriceState& state)
{
    const int64_t now = gwdash::NowMs();

    const auto age_for = [&](const std::optional<int64_t>& at) -> std::string {
        if (!show_age_ || !at.has_value() || *at <= 0) {
            return {};
        }
        return gwdash::FormatAge(*at, now);
    };

    const auto color_for = [&](const std::optional<double>& price, const std::optional<int64_t>& at) {
        if (!price.has_value()) {
            return COLOR_MISSING;
        }
        if (at.has_value() && *at > 0 && now - *at > STALE_AFTER_MS) {
            return COLOR_STALE;
        }
        return ImGui::GetStyle().Colors[ImGuiCol_Text];
    };

    const gwdash::PriceSnapshot& snapshot = state.snapshot;

    const std::string ecto_value = state.has_data && snapshot.ecto.price.has_value()
                                       ? gwdash::FormatGold(*snapshot.ecto.price)
                                       : "--";
    DrawValueRow("Ecto", ecto_value, color_for(snapshot.ecto.price, snapshot.ecto.at),
                 age_for(snapshot.ecto.at), "Ectoplasm - average of the NPC trader's buy/sell price");

    if (show_ecto_spread_ && snapshot.ecto.buy.has_value() && snapshot.ecto.sell.has_value()) {
        ImGui::TextDisabled("  buy %s / sell %s",
                            gwdash::FormatGold(*snapshot.ecto.buy).c_str(),
                            gwdash::FormatGold(*snapshot.ecto.sell).c_str());
    }

    const std::string arms_value = state.has_data && snapshot.armbrace.price.has_value()
                                       ? gwdash::FormatEcto(*snapshot.armbrace.price)
                                       : "--";
    const std::string arms_tip = ChatPriceTooltip(
        "Armbrace of Truth", snapshot.armbrace, snapshot.armbrace.price.has_value());
    DrawValueRow("Arms", arms_value, color_for(snapshot.armbrace.price, snapshot.armbrace.at),
                 age_for(snapshot.armbrace.at), arms_tip.c_str());

    const std::string dye_value = state.has_data && snapshot.blackdye.price.has_value()
                                      ? gwdash::FormatGold(*snapshot.blackdye.price)
                                      : "--";
    const std::string dye_tip = ChatPriceTooltip(
        "Black Dye", snapshot.blackdye, snapshot.blackdye.price.has_value());
    DrawValueRow("Black Dye", dye_value, color_for(snapshot.blackdye.price, snapshot.blackdye.at),
                 age_for(snapshot.blackdye.at), dye_tip.c_str());
}

void GWDashPlugin::DrawCompact(const gwdash::PriceState& state)
{
    const gwdash::PriceSnapshot& snapshot = state.snapshot;

    const std::string line =
        "Ecto " + (snapshot.ecto.price.has_value() ? gwdash::FormatGold(*snapshot.ecto.price) : "--") +
        " | Arms " + (snapshot.armbrace.price.has_value() ? gwdash::FormatEcto(*snapshot.armbrace.price) : "--") +
        " | BD " + (snapshot.blackdye.price.has_value() ? gwdash::FormatGold(*snapshot.blackdye.price) : "--");

    ImGui::TextUnformatted(line.c_str());
}

void GWDashPlugin::DrawStatusLine(const gwdash::PriceState& state)
{
    const gwdash::UpdateState update = updater_.State();
    const int64_t now = gwdash::NowMs();

    if (!state.has_data && !state.error.empty()) {
        ImGui::TextColored(COLOR_ERROR, "offline");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", state.error.c_str());
        }
        return;
    }

    if (state.consecutive_failures > 0) {
        ImGui::TextColored(COLOR_STALE, "reconnecting");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", state.error.empty() ? "no connection" : state.error.c_str());
        }
    }
    else {
        const int64_t age_at = state.snapshot.at > 0 ? state.snapshot.at : state.received_at;
        if (age_at > 0) {
            if (state.from_cache) {
                ImGui::TextDisabled("updated %s ago (cached)", gwdash::FormatAge(age_at, now).c_str());
            }
            else {
                ImGui::TextDisabled("updated %s ago", gwdash::FormatAge(age_at, now).c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Source: %s", state.source.empty() ? "unknown" : state.source.c_str());
            }
        }
    }

    if (update.status == gwdash::UpdateStatus::Staged) {
        ImGui::TextDisabled("update ready - restart");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", update.message.c_str());
        }
    }
}

void GWDashPlugin::DrawSendConfirmPopup()
{
    if (open_send_confirm_) {
        ImGui::OpenPopup("Send trade preset?");
        open_send_confirm_ = false;
    }

    if (ImGui::BeginPopupModal("Send trade preset?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Send to trade chat?\n\n%s", pending_send_line_.c_str());
        if (!gwdash::CanSendTradeChat()) {
            ImGui::TextColored(COLOR_ERROR, "Not in an outpost.");
        }
        if (ImGui::Button("Send", ImVec2(120, 0))) {
            if (const gwdash::TradePreset* preset = trade_presets_.Find(pending_send_name_)) {
                SendPreset(*preset);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GWDashPlugin::DrawTradePresetPopup()
{
    if (ImGui::BeginPopup("gwdash_presets_popup")) {
        const bool can_send = gwdash::CanSendTradeChat();
        for (const gwdash::TradePreset& preset : trade_presets_.List()) {
            ImGui::PushID(preset.name.c_str());
            const std::string line = gwdash::ComposeTradeLine(preset.kind, preset.message);
            if (!can_send) {
                ImGui::BeginDisabled();
            }
            if (ImGui::MenuItem(preset.name.c_str())) {
                RequestSendPreset(preset, true);
            }
            if (!can_send) {
                ImGui::EndDisabled();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", line.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
}

void GWDashPlugin::DrawTradePresetButtons()
{
    if (compact_) {
        if (ImGui::SmallButton("Presets...")) {
            ImGui::OpenPopup("gwdash_presets_popup");
        }
        DrawTradePresetPopup();
        return;
    }

    if (!ImGui::BeginChild("trade_presets", ImVec2(0.0f, 140.0f), true)) {
        ImGui::EndChild();
        return;
    }

    const bool can_send = gwdash::CanSendTradeChat();
    for (const gwdash::TradePreset& preset : trade_presets_.List()) {
        ImGui::PushID(preset.name.c_str());
        const std::string line = gwdash::ComposeTradeLine(preset.kind, preset.message);

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("[%s]", preset.kind.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(preset.name.c_str());
        if (preset.is_default) {
            ImGui::SameLine();
            ImGui::TextDisabled("*");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", line.c_str());
        }

        ImGui::SameLine();
        if (!can_send) {
            ImGui::BeginDisabled();
        }
        if (ImGui::SmallButton("Send")) {
            RequestSendPreset(preset, true);
        }
        if (!can_send) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Trade chat is only available in an outpost.");
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(line.c_str());
            WriteChat("Copied trade preset to clipboard.");
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
}

void GWDashPlugin::DrawTradePresetSettings()
{
    ImGui::TextUnformatted("Trade presets");
    ImGui::TextDisabled("Saved locally. Send is manual, outpost-only, with a 3s cooldown.");
    ImGui::Checkbox("Show presets on overlay", &show_trade_presets_);

    ImGui::InputInt("Send hotkey VK (0=off)", &send_hotkey_vk_);
    send_hotkey_vk_ = std::clamp(send_hotkey_vk_, 0, 255);
    ImGui::TextDisabled("Hotkey sends the default preset after a confirm dialog.");

    if (!trade_presets_.LoadOk()) {
        ImGui::TextColored(COLOR_ERROR, "%s",
                           trade_presets_.LoadError().empty()
                               ? "Failed to load presets.json"
                               : trade_presets_.LoadError().c_str());
        if (ImGui::Button("Reset presets")) {
            trade_presets_.Reset();
            preset_edit_drafts_.clear();
            PersistPresets();
            presets_ui_error_.clear();
        }
    }

    if (!presets_ui_error_.empty()) {
        ImGui::TextColored(COLOR_ERROR, "%s", presets_ui_error_.c_str());
    }

    if (ImGui::Button("Export JSON")) {
        const auto path = PickJsonFile(true);
        if (!path.empty()) {
            std::string error;
            if (trade_presets_.ExportToFile(path, error)) {
                WriteChat("Exported trade presets.");
                presets_ui_error_.clear();
            }
            else {
                presets_ui_error_ = error;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import JSON")) {
        const auto path = PickJsonFile(false);
        if (!path.empty()) {
            std::string error;
            if (trade_presets_.ImportFromFile(path, error)) {
                preset_edit_drafts_.clear();
                PersistPresets();
                WriteChat("Imported trade presets.");
                presets_ui_error_.clear();
            }
            else {
                presets_ui_error_ = error;
            }
        }
    }

    const std::vector<gwdash::TradePreset> listed = trade_presets_.List();
    for (const gwdash::TradePreset& preset : listed) {
        ImGui::PushID(preset.name.c_str());

        const bool open = open_preset_editor_ == preset.name;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_CollapsingHeader;
        if (open) {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }
        const std::string header = preset.name + (preset.is_default ? "  (default)" : "");
        const bool expanded = ImGui::CollapsingHeader(header.c_str(), flags);
        if (expanded && open_preset_editor_ != preset.name) {
            open_preset_editor_ = preset.name;
        }
        else if (!expanded && open_preset_editor_ == preset.name) {
            open_preset_editor_.clear();
        }

        if (!expanded) {
            ImGui::PopID();
            continue;
        }

        // Keep only one editor open at a time.
        if (open_preset_editor_ != preset.name) {
            open_preset_editor_ = preset.name;
        }

        auto [it, inserted] = preset_edit_drafts_.try_emplace(preset.name);
        PresetEditDraft& draft = it->second;
        if (inserted) {
            draft.kind_index = KindIndex(preset.kind);
            draft.rename.fill('\0');
            draft.message.fill('\0');
            strncpy_s(draft.rename.data(), draft.rename.size(), preset.name.c_str(), _TRUNCATE);
            strncpy_s(draft.message.data(), draft.message.size(), preset.message.c_str(), _TRUNCATE);
        }

        ImGui::Combo("Kind", &draft.kind_index, KIND_COMBO);
        ImGui::InputText("Message", draft.message.data(), draft.message.size());
        ImGui::InputText("Rename", draft.rename.data(), draft.rename.size());

        const std::string preview =
            gwdash::ComposeTradeLine(KindLabel(draft.kind_index), draft.message.data());
        ImGui::TextDisabled("Preview: %s", preview.c_str());

        if (ImGui::Button("Save")) {
            std::string error;
            if (trade_presets_.Upsert(preset.name, KindLabel(draft.kind_index), draft.message.data(),
                                      error)) {
                if (draft.rename[0] != '\0' &&
                    gwdash::SanitizePresetName(draft.rename.data()) != preset.name) {
                    if (!trade_presets_.Rename(preset.name, draft.rename.data(), error)) {
                        presets_ui_error_ = error;
                    }
                    else {
                        preset_edit_drafts_.erase(preset.name);
                        open_preset_editor_ = gwdash::SanitizePresetName(draft.rename.data());
                        PersistPresets();
                        presets_ui_error_.clear();
                    }
                }
                else {
                    PersistPresets();
                    presets_ui_error_.clear();
                }
            }
            else {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Send")) {
            gwdash::TradePreset pending = preset;
            pending.kind = KindLabel(draft.kind_index);
            pending.message = gwdash::SanitizePresetMessage(draft.message.data());
            RequestSendPreset(pending, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(preview.c_str());
            WriteChat("Copied trade preset to clipboard.");
        }

        if (ImGui::Button("Up")) {
            std::string error;
            if (trade_presets_.Move(preset.name, -1, error)) {
                PersistPresets();
            }
            else if (!error.empty()) {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Down")) {
            std::string error;
            if (trade_presets_.Move(preset.name, 1, error)) {
                PersistPresets();
            }
            else if (!error.empty()) {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            std::string error;
            if (trade_presets_.Duplicate(preset.name, error)) {
                PersistPresets();
                presets_ui_error_.clear();
            }
            else {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Default")) {
            std::string error;
            if (trade_presets_.SetDefault(preset.name, error)) {
                PersistPresets();
                presets_ui_error_.clear();
            }
            else {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            if (trade_presets_.Remove(preset.name)) {
                preset_edit_drafts_.erase(preset.name);
                if (open_preset_editor_ == preset.name) {
                    open_preset_editor_.clear();
                }
                PersistPresets();
                presets_ui_error_.clear();
            }
        }

        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Add preset");
    ImGui::InputText("Name", draft_name_.data(), draft_name_.size());
    ImGui::Combo("Kind##new", &draft_kind_index_, KIND_COMBO);
    ImGui::InputTextWithHint("Message##new", "e.g. 27e arms", draft_message_.data(),
                             draft_message_.size());
    ImGui::TextDisabled("Preview: %s",
                        gwdash::ComposeTradeLine(KindLabel(draft_kind_index_), draft_message_.data())
                            .c_str());

    if (trade_presets_.List().size() >= gwdash::MAX_TRADE_PRESETS) {
        ImGui::TextDisabled("At most %zu presets.", gwdash::MAX_TRADE_PRESETS);
    }
    else if (ImGui::Button("Add")) {
        std::string error;
        if (trade_presets_.Upsert(draft_name_.data(), KindLabel(draft_kind_index_),
                                  draft_message_.data(), error)) {
            PersistPresets();
            draft_name_.fill('\0');
            draft_message_.fill('\0');
            draft_kind_index_ = 1;
            presets_ui_error_.clear();
        }
        else {
            presets_ui_error_ = error;
        }
    }
}

void GWDashPlugin::Draw(IDirect3DDevice9*)
{
    if (GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(210.0f, 108.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(background_alpha_);

    const bool show_presets = show_trade_presets_ && !trade_presets_.List().empty();
    ImGuiWindowFlags flags = show_presets ? 0 : ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin(Name(), show_closebutton ? GetVisiblePtr() : nullptr, GetWinFlags(flags))) {
        const bool scaled = font_scale_ < 0.99f || font_scale_ > 1.01f;
        if (scaled) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * font_scale_);
        }

        const gwdash::PriceState state = prices_.State();

        if (compact_) {
            DrawCompact(state);
        }
        else {
            DrawRows(state);
        }

        if (show_presets) {
            ImGui::Separator();
            DrawTradePresetButtons();
        }

        if (show_status_) {
            ImGui::Separator();
            DrawStatusLine(state);
        }

        DrawSendConfirmPopup();

        if (scaled) {
            ImGui::PopFont();
        }
    }
    ImGui::End();
}

void GWDashPlugin::DrawSettings()
{
    ToolboxUIPlugin::DrawSettings();

    ImGui::Separator();

    if (ImGui::Combo("Refresh every", &refresh_index_, "60 seconds\0" "2 minutes\0" "5 minutes\0")) {
        ApplyRefreshInterval();
    }
    ImGui::Checkbox("Slow down while Guild Wars is in the background", &throttle_unfocused_);
    ImGui::Checkbox("Single line", &compact_);
    ImGui::Checkbox("Show data age", &show_age_);
    ImGui::Checkbox("Show ecto buy/sell", &show_ecto_spread_);
    ImGui::Checkbox("Show status line", &show_status_);
    ImGui::SliderFloat("Background", &background_alpha_, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Font scale", &font_scale_, 0.7f, 2.0f, "%.2f");

    ImGui::Separator();

    const gwdash::PriceState state = prices_.State();
    const gwdash::UpdateState update = updater_.State();

    if (ImGui::Button("Refresh prices")) {
        prices_.RequestRefresh();
    }
    ImGui::SameLine();
    if (ImGui::Button("Check for updates")) {
        updater_.RequestCheck();
    }

    if (ImGui::Checkbox("Install updates automatically", &auto_update_)) {
        updater_.SetAutoInstall(auto_update_);
    }

    if (!state.error.empty()) {
        ImGui::TextColored(COLOR_ERROR, "Prices: %s", state.error.c_str());
    }
    if (!update.message.empty()) {
        ImGui::TextDisabled("Updates: %s", update.message.c_str());
    }
    if (update.loader_outdated) {
        ImGui::TextColored(COLOR_STALE, "%s",
                           "This release also updates GWDash.dll. Re-run the installer to pick it up.");
    }

    ImGui::TextDisabled("Version %s | source %s", GWDASH_VERSION,
                        state.source.empty() ? "connecting" : state.source.c_str());
    ImGui::TextDisabled("Type /gwdash in chat to toggle the overlay.");

    ImGui::Separator();
    DrawTradePresetSettings();
    DrawSendConfirmPopup();
}

void GWDashPlugin::LoadSettings(const wchar_t* folder)
{
    ToolboxUIPlugin::LoadSettings(folder);

    LoadSetting("refresh_index", refresh_index_);
    LoadSetting("compact", compact_);
    LoadSetting("show_age", show_age_);
    LoadSetting("show_ecto_spread", show_ecto_spread_);
    LoadSetting("show_status", show_status_);
    LoadSetting("show_trade_presets", show_trade_presets_);
    LoadSetting("throttle_unfocused", throttle_unfocused_);
    LoadSetting("auto_update", auto_update_);
    LoadSetting("background_alpha", background_alpha_);
    LoadSetting("font_scale", font_scale_);
    LoadSetting("send_hotkey_vk", send_hotkey_vk_);

    refresh_index_ = std::clamp(refresh_index_, 0, static_cast<int>(REFRESH_CHOICES.size()) - 1);
    background_alpha_ = std::clamp(background_alpha_, 0.0f, 1.0f);
    font_scale_ = std::clamp(font_scale_, 0.7f, 2.0f);
    send_hotkey_vk_ = std::clamp(send_hotkey_vk_, 0, 255);

    ApplyRefreshInterval();
    updater_.SetAutoInstall(auto_update_);
    trade_presets_.Load();
    preset_edit_drafts_.clear();
}

void GWDashPlugin::SaveSettings(const wchar_t* folder)
{
    SaveSetting("refresh_index", refresh_index_);
    SaveSetting("compact", compact_);
    SaveSetting("show_age", show_age_);
    SaveSetting("show_ecto_spread", show_ecto_spread_);
    SaveSetting("show_status", show_status_);
    SaveSetting("show_trade_presets", show_trade_presets_);
    SaveSetting("throttle_unfocused", throttle_unfocused_);
    SaveSetting("auto_update", auto_update_);
    SaveSetting("background_alpha", background_alpha_);
    SaveSetting("font_scale", font_scale_);
    SaveSetting("send_hotkey_vk", send_hotkey_vk_);

    ToolboxUIPlugin::SaveSettings(folder);
    PersistPresets();
}

void GWDashPlugin::WritePricesToChat() const
{
    const gwdash::PriceState state = prices_.State();
    if (!state.has_data) {
        WriteChat("No prices yet - still waiting for the first update.");
        return;
    }

    const gwdash::PriceSnapshot& snapshot = state.snapshot;
    std::string line = "Ecto ";
    line += snapshot.ecto.price.has_value() ? gwdash::FormatGold(*snapshot.ecto.price) : "--";
    line += " | Armbrace ";
    line += snapshot.armbrace.price.has_value() ? gwdash::FormatEcto(*snapshot.armbrace.price) : "--";
    line += " | Black Dye ";
    line += snapshot.blackdye.price.has_value() ? gwdash::FormatGold(*snapshot.blackdye.price) : "--";
    WriteChat(line);
}

void GWDashPlugin::OnChatCommand(const int argc, const LPWSTR* argv)
{
    bool* visible = GetVisiblePtr();

    if (argc < 2) {
        *visible = !*visible;
        return;
    }

    const std::wstring argument = PluginUtils::ToLower(argv[1]);

    if (argument == L"show") {
        *visible = true;
    }
    else if (argument == L"hide") {
        *visible = false;
    }
    else if (argument == L"toggle") {
        *visible = !*visible;
    }
    else if (argument == L"refresh") {
        prices_.RequestRefresh();
        WriteChat("Refreshing prices...");
    }
    else if (argument == L"prices") {
        WritePricesToChat();
    }
    else if (argument == L"update") {
        updater_.RequestCheck();
        WriteChat("Checking for updates...");
    }
    else if (argument == L"version") {
        WriteChat(std::string("GWDash ") + GWDASH_VERSION);
    }
    else if (argument == L"presets") {
        const auto& presets = trade_presets_.List();
        if (presets.empty()) {
            WriteChat("No trade presets. Add some under Toolbox → Settings → GWDash.");
            return;
        }
        WriteChat(std::string("Trade presets (") + std::to_string(presets.size()) + "):");
        for (const gwdash::TradePreset& preset : presets) {
            const std::string line = gwdash::ComposeTradeLine(preset.kind, preset.message);
            WriteChat("  /gwdash send " + preset.name + (preset.is_default ? " *" : "") +
                      "  " + line);
        }
    }
    else if (argument == L"send") {
        if (argc < 3) {
            WriteChat("Usage: /gwdash send <name>");
            return;
        }
        SendPresetByName(PluginUtils::WStringToString(argv[2]));
    }
    else {
        WriteChat("Usage: /gwdash [show|hide|toggle|refresh|prices|update|version|presets|send <name>]");
    }
}
