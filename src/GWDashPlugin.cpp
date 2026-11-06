#include "GWDashPlugin.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cctype>
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
#include <GWCA/Managers/UIMgr.h>
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
        } else {
            ofn.Flags |= OFN_FILEMUSTEXIST;
            if (!GetOpenFileNameW(&ofn)) {
                return {};
            }
        }
        return file;
    }

    // Prices are the reason the overlay exists, so they get a bigger font and
    // full-white text; labels and ages stay quieter.
    constexpr float PRICE_VALUE_SCALE = 1.25f;
    const ImVec4 COLOR_LABEL{0.82f, 0.80f, 0.74f, 1.0f};
    const ImVec4 COLOR_VALUE{1.0f, 1.0f, 1.0f, 1.0f};
    const ImVec4 COLOR_WTB{0.47f, 0.74f, 1.0f, 1.0f};
    const ImVec4 COLOR_WTS{0.55f, 0.86f, 0.47f, 1.0f};
    const ImVec4 COLOR_WTT{1.0f, 0.78f, 0.38f, 1.0f};

    struct PriceRow {
        const char* label;
        std::string value;
        ImVec4 value_color;
        std::string age;
        std::string tooltip;
        std::string detail; // optional muted line under the row (ecto buy/sell)
    };

    void TextRightAligned(const ImVec4& color, const std::string& text)
    {
        const float width = ImGui::CalcTextSize(text.c_str()).x;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > width) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
        }
        ImGui::TextColored(color, "%s", text.c_str());
    }

    void TooltipIfHovered(const std::string& tooltip)
    {
        if (!tooltip.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
    }

    void DrawPriceTable(const std::vector<PriceRow>& rows, const float value_font_size,
                        const bool with_age)
    {
        // Line height of the enlarged value font, to drop the smaller label/age onto
        // roughly the same baseline.
        const float small_height = ImGui::GetTextLineHeight();
        ImGui::PushFont(nullptr, value_font_size);
        const float big_height = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        const float baseline_offset = std::max(0.0f, (big_height - small_height) * 0.75f);

        constexpr ImGuiTableFlags table_flags =
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
            ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoClip;
        if (!ImGui::BeginTable("gwdash_prices", with_age ? 3 : 2, table_flags)) {
            return;
        }
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed);
        if (with_age) {
            ImGui::TableSetupColumn("age", ImGuiTableColumnFlags_WidthFixed);
        }

        for (const PriceRow& row : rows) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + baseline_offset);
            ImGui::TextColored(COLOR_LABEL, "%s", row.label);
            TooltipIfHovered(row.tooltip);

            ImGui::TableSetColumnIndex(1);
            ImGui::PushFont(nullptr, value_font_size);
            TextRightAligned(row.value_color, row.value);
            ImGui::PopFont();
            TooltipIfHovered(row.tooltip);

            if (with_age) {
                ImGui::TableSetColumnIndex(2);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + baseline_offset);
                if (row.age.empty()) {
                    ImGui::TextUnformatted("");
                } else {
                    TextRightAligned(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], row.age);
                    TooltipIfHovered("Age of this price");
                }
            }

            if (!row.detail.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", row.detail.c_str());
            }
        }

        ImGui::EndTable();
    }

    const ImVec4& KindColor(const std::string& kind)
    {
        if (kind == "wtb") {
            return COLOR_WTB;
        }
        if (kind == "wtt") {
            return COLOR_WTT;
        }
        return COLOR_WTS;
    }

    std::string UpperKind(const std::string& kind)
    {
        std::string out = kind;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return out;
    }

    std::string ChatPriceTooltip(const char* title, const gwdash::ChatPrice& price,
                                 const bool ready)
    {
        if (!ready) {
            return std::string(title) + " - waiting for trade-chat mentions";
        }
        return std::string(title) + " - recency-weighted median (n=" + std::to_string(price.n) +
               ")";
    }
} // namespace

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
} // namespace

GWDashPlugin::GWDashPlugin()
{
    can_show_in_main_window = true;
    can_close = true;
}

const char* GWDashPlugin::Icon() const
{
    return ICON_COINS;
}

void GWDashPlugin::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns,
                              const HMODULE toolbox_dll)
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
    const int index = std::clamp(refresh_index_, 0, static_cast<int>(REFRESH_CHOICES.size()) - 1);
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

void GWDashPlugin::SendAndCopyPreset(const gwdash::TradePreset& preset)
{
    const std::string line = gwdash::ComposeTradeLine(preset.kind, preset.message);
    if (line.empty()) {
        WriteChat("Preset message is empty.");
        return;
    }

    // Always copy first: even when sending is not possible right now (explorable,
    // cooldown) the line is ready for Ctrl+V in the chat box.
    ImGui::SetClipboardText(line.c_str());

    if (!gwdash::CanSendTradeChat()) {
        WriteChat("Copied (trade chat only works in an outpost): " + line);
        return;
    }

    if (send_via_chat_box_) {
        if (!FillChatBox(line)) {
            WriteChat("Could not open the chat box. Copied to clipboard instead.");
        }
        return;
    }

    std::string error;
    if (trade_presets_.TrySend(preset, error)) {
        WriteChat("Sent to trade + copied: " + line);
        return;
    }
    WriteChat((error.empty() ? std::string("Failed to send trade preset.") : error) +
              " Copied to clipboard instead.");
}

bool GWDashPlugin::FillChatBox(const std::string& line)
{
    // Same route GWToolbox's market window uses to pre-fill a whisper: open the chat
    // box (unless the player is already typing) and append the text. The player
    // posts it with Enter, so it goes through Guild Wars' own input and history.
    std::wstring wide = PluginUtils::StringToWString(line);
    if (!GW::Chat::GetIsTyping()) {
        GW::UI::Keypress(GW::UI::ControlAction_OpenChat);
    }
    if (!GW::UI::SendUIMessage(GW::UI::UIMessage::kAppendMessageToChat, wide.data())) {
        return false;
    }
    if (GW::Chat::GetCurrentChatChannel() != GW::Chat::CHANNEL_TRADE) {
        WriteChat("Chat box is not on the trade channel - switch to trade ($) before Enter.");
    }
    return true;
}

GWDashPlugin::PresetEditDraft& GWDashPlugin::DraftFor(const gwdash::TradePreset& preset)
{
    auto [it, inserted] = preset_edit_drafts_.try_emplace(preset.name);
    PresetEditDraft& draft = it->second;
    if (inserted) {
        draft.kind_index = KindIndex(preset.kind);
        draft.rename.fill('\0');
        draft.message.fill('\0');
        strncpy_s(draft.rename.data(), draft.rename.size(), preset.name.c_str(), _TRUNCATE);
        strncpy_s(draft.message.data(), draft.message.size(), preset.message.c_str(), _TRUNCATE);
    }
    return draft;
}

bool GWDashPlugin::SavePresetDraft(const gwdash::TradePreset& preset, PresetEditDraft& draft)
{
    // Copy the name: `preset` may point into the list that Rename() rewrites.
    const std::string old_name = preset.name;
    std::string error;
    if (!trade_presets_.Upsert(old_name, KindLabel(draft.kind_index), draft.message.data(),
                               error)) {
        presets_ui_error_ = error;
        return false;
    }

    const std::string new_name = gwdash::SanitizePresetName(draft.rename.data());
    if (draft.rename[0] != '\0' && new_name != old_name) {
        if (!trade_presets_.Rename(old_name, draft.rename.data(), error)) {
            PersistPresets(); // kind/message were saved already
            presets_ui_error_ = error;
            return false;
        }
        preset_edit_drafts_.erase(old_name);
        if (open_preset_editor_ == old_name) {
            open_preset_editor_ = new_name;
        }
        if (overlay_editing_ == old_name) {
            overlay_editing_ = new_name;
        }
    }

    PersistPresets();
    presets_ui_error_.clear();
    return true;
}

bool GWDashPlugin::AddPresetFromDraft()
{
    std::string error;
    if (!trade_presets_.Upsert(draft_name_.data(), KindLabel(draft_kind_index_),
                               draft_message_.data(), error)) {
        presets_ui_error_ = error;
        return false;
    }
    PersistPresets();
    draft_name_.fill('\0');
    draft_message_.fill('\0');
    draft_kind_index_ = 1;
    presets_ui_error_.clear();
    return true;
}

void GWDashPlugin::DeletePreset(const std::string& name)
{
    if (!trade_presets_.Remove(name)) {
        return;
    }
    preset_edit_drafts_.erase(name);
    if (open_preset_editor_ == name) {
        open_preset_editor_.clear();
    }
    if (overlay_editing_ == name) {
        overlay_editing_.clear();
    }
    overlay_delete_armed_.clear();
    PersistPresets();
    presets_ui_error_.clear();
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
    } else {
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

    const auto color_for = [&](const std::optional<double>& price,
                               const std::optional<int64_t>& at) {
        if (!price.has_value()) {
            return COLOR_MISSING;
        }
        if (at.has_value() && *at > 0 && now - *at > STALE_AFTER_MS) {
            return COLOR_STALE;
        }
        return COLOR_VALUE;
    };

    const gwdash::PriceSnapshot& snapshot = state.snapshot;
    std::vector<PriceRow> rows;
    rows.reserve(3);

    PriceRow ecto{"Ecto",
                  state.has_data && snapshot.ecto.price.has_value()
                      ? gwdash::FormatGold(*snapshot.ecto.price)
                      : "--",
                  color_for(snapshot.ecto.price, snapshot.ecto.at),
                  age_for(snapshot.ecto.at),
                  "Ectoplasm - average of the NPC trader's buy/sell price",
                  {}};
    if (show_ecto_spread_ && snapshot.ecto.buy.has_value() && snapshot.ecto.sell.has_value()) {
        ecto.detail = "buy " + gwdash::FormatGold(*snapshot.ecto.buy) + " / sell " +
                      gwdash::FormatGold(*snapshot.ecto.sell);
    }
    rows.push_back(std::move(ecto));

    rows.push_back(PriceRow{"Arms",
                            state.has_data && snapshot.armbrace.price.has_value()
                                ? gwdash::FormatEcto(*snapshot.armbrace.price)
                                : "--",
                            color_for(snapshot.armbrace.price, snapshot.armbrace.at),
                            age_for(snapshot.armbrace.at),
                            ChatPriceTooltip("Armbrace of Truth", snapshot.armbrace,
                                             snapshot.armbrace.price.has_value()),
                            {}});

    rows.push_back(PriceRow{
        "Black Dye",
        state.has_data && snapshot.blackdye.price.has_value()
            ? gwdash::FormatGold(*snapshot.blackdye.price)
            : "--",
        color_for(snapshot.blackdye.price, snapshot.blackdye.at),
        age_for(snapshot.blackdye.at),
        ChatPriceTooltip("Black Dye", snapshot.blackdye, snapshot.blackdye.price.has_value()),
        {}});

    DrawPriceTable(rows, ImGui::GetStyle().FontSizeBase * font_scale_ * PRICE_VALUE_SCALE,
                   show_age_);
}

void GWDashPlugin::DrawCompact(const gwdash::PriceState& state)
{
    const gwdash::PriceSnapshot& snapshot = state.snapshot;

    const std::string line =
        "Ecto " +
        (snapshot.ecto.price.has_value() ? gwdash::FormatGold(*snapshot.ecto.price) : "--") +
        " | Arms " +
        (snapshot.armbrace.price.has_value() ? gwdash::FormatEcto(*snapshot.armbrace.price)
                                             : "--") +
        " | BD " +
        (snapshot.blackdye.price.has_value() ? gwdash::FormatGold(*snapshot.blackdye.price) : "--");

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
    } else {
        const int64_t age_at = state.snapshot.at > 0 ? state.snapshot.at : state.received_at;
        if (age_at > 0) {
            if (state.from_cache) {
                ImGui::TextDisabled("updated %s ago (cached)",
                                    gwdash::FormatAge(age_at, now).c_str());
            } else {
                ImGui::TextDisabled("updated %s ago", gwdash::FormatAge(age_at, now).c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Source: %s",
                                  state.source.empty() ? "unknown" : state.source.c_str());
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

    if (ImGui::BeginPopupModal("Send trade preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Send to trade chat?\n\n%s", pending_send_line_.c_str());
        if (!gwdash::CanSendTradeChat()) {
            ImGui::TextColored(COLOR_ERROR, "Not in an outpost.");
        }
        if (ImGui::Button("Send", ImVec2(120, 0))) {
            if (const gwdash::TradePreset* preset = trade_presets_.Find(pending_send_name_)) {
                SendAndCopyPreset(*preset);
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
        if (trade_presets_.List().empty()) {
            ImGui::TextDisabled("No presets yet. Add some in the full view or Settings.");
        }
        const bool can_send = gwdash::CanSendTradeChat();
        for (const gwdash::TradePreset& preset : trade_presets_.List()) {
            ImGui::PushID(preset.name.c_str());
            const std::string line = gwdash::ComposeTradeLine(preset.kind, preset.message);
            if (ImGui::MenuItem(preset.name.c_str(), can_send ? "send" : "copy")) {
                SendAndCopyPreset(preset);
            }
            if (ImGui::IsItemHovered()) {
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

    DrawPresetsHeader();
    if (overlay_adding_) {
        DrawOverlayAddForm();
    }
    if (!presets_ui_error_.empty() && (overlay_adding_ || !overlay_editing_.empty())) {
        ImGui::TextColored(COLOR_ERROR, "%s", presets_ui_error_.c_str());
    }

    // Copy: saving, renaming or deleting below rewrites the stored list.
    const std::vector<gwdash::TradePreset> listed = trade_presets_.List();
    if (listed.empty() && !overlay_adding_) {
        ImGui::TextDisabled("No presets yet - click + to add one.");
        return;
    }

    // One action per preset: send once to trade chat and copy the line. Outside an
    // outpost the same button only copies, so its label says so.
    const bool can_send = gwdash::CanSendTradeChat();
    const char* action_label = can_send ? "Send" : "Copy";
    const ImGuiStyle& style = ImGui::GetStyle();
    const float action_width =
        std::max(ImGui::CalcTextSize("Send").x, ImGui::CalcTextSize("Copy").x) +
        style.FramePadding.x * 2.0f;

    // The table is closed around an open editor so the editor gets the full width;
    // reopening it with the same ID keeps the columns in sync.
    const auto begin_table = [&]() {
        constexpr ImGuiTableFlags table_flags = ImGuiTableFlags_SizingFixedFit |
                                                ImGuiTableFlags_NoSavedSettings |
                                                ImGuiTableFlags_NoPadOuterX;
        if (!ImGui::BeginTable("gwdash_presets", 3, table_flags)) {
            return false;
        }
        ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, action_width);
        return true;
    };

    bool table_open = begin_table();
    for (const gwdash::TradePreset& preset : listed) {
        if (!table_open) {
            break;
        }
        ImGui::PushID(preset.name.c_str());
        const std::string line = gwdash::ComposeTradeLine(preset.kind, preset.message);
        const bool editing = overlay_editing_ == preset.name;
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(KindColor(preset.kind), "%s", UpperKind(preset.kind).c_str());
        TooltipIfHovered(line);

        ImGui::TableSetColumnIndex(1);
        const std::string label = preset.is_default ? preset.name + " *" : preset.name;
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Selectable(label.c_str(), editing, ImGuiSelectableFlags_None,
                              ImVec2(0.0f, ImGui::GetFrameHeight()))) {
            if (editing) {
                overlay_editing_.clear();
            } else {
                // Start from the stored values, not a stale Settings draft.
                preset_edit_drafts_.erase(preset.name);
                overlay_editing_ = preset.name;
                overlay_adding_ = false;
            }
            presets_ui_error_.clear();
            overlay_delete_armed_.clear();
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n(click to edit)", line.c_str());
        }

        ImGui::TableSetColumnIndex(2);
        if (ImGui::Button(action_label, ImVec2(action_width, 0.0f))) {
            SendAndCopyPreset(preset);
        }
        if (ImGui::IsItemHovered()) {
            if (!can_send) {
                ImGui::SetTooltip("Copy to clipboard (trade chat only works in an outpost):\n%s",
                                  line.c_str());
            } else if (send_via_chat_box_) {
                ImGui::SetTooltip("Put into the chat box (press Enter) and copy:\n%s",
                                  line.c_str());
            } else {
                ImGui::SetTooltip("Send to trade chat and copy:\n%s", line.c_str());
            }
        }

        if (editing) {
            ImGui::EndTable();
            DrawOverlayPresetEditor(preset);
            table_open = begin_table();
        }
        ImGui::PopID();
    }
    if (table_open) {
        ImGui::EndTable();
    }
}

void GWDashPlugin::DrawPresetsHeader()
{
    // "Trade presets ------------ [+]": a separator line with the add button on the right.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float button_size = ImGui::GetFrameHeight();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Trade presets");
    ImGui::SameLine();

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float line_width = ImGui::GetContentRegionAvail().x - button_size - style.ItemSpacing.x;
    if (line_width > 0.0f) {
        const float y = start.y + button_size * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x, y), ImVec2(start.x + line_width, y),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(line_width, button_size));
        ImGui::SameLine();
    }

    const bool full = trade_presets_.List().size() >= gwdash::MAX_TRADE_PRESETS;
    const bool disabled = full && !overlay_adding_;
    if (disabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(overlay_adding_ ? "x##add" : "+##add", ImVec2(button_size, button_size))) {
        overlay_adding_ = !overlay_adding_;
        overlay_editing_.clear();
        overlay_delete_armed_.clear();
        presets_ui_error_.clear();
    }
    if (disabled) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (full && !overlay_adding_) {
            ImGui::SetTooltip("At most %zu presets.", gwdash::MAX_TRADE_PRESETS);
        } else {
            ImGui::SetTooltip("%s", overlay_adding_ ? "Cancel" : "Add a preset");
        }
    }
}

namespace {
    // Kind combo + message on one line, name on the next: fits a narrow overlay.
    void DrawPresetFields(int& kind_index, char* message, const size_t message_size, char* name,
                          const size_t name_size)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float combo_width =
            ImGui::CalcTextSize("wtb").x + ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(combo_width);
        ImGui::Combo("##kind", &kind_index, KIND_COMBO);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##message", "message, e.g. 27e arms", message, message_size);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##name", "name", name, name_size);
        ImGui::TextDisabled("%s", gwdash::ComposeTradeLine(KindLabel(kind_index), message).c_str());
    }
} // namespace

void GWDashPlugin::DrawOverlayAddForm()
{
    ImGui::PushID("gwdash_add_preset");
    DrawPresetFields(draft_kind_index_, draft_message_.data(), draft_message_.size(),
                     draft_name_.data(), draft_name_.size());
    if (ImGui::Button("Add")) {
        if (AddPresetFromDraft()) {
            overlay_adding_ = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        overlay_adding_ = false;
        presets_ui_error_.clear();
    }
    ImGui::PopID();
    ImGui::Spacing();
}

void GWDashPlugin::DrawOverlayPresetEditor(const gwdash::TradePreset& preset)
{
    PresetEditDraft& draft = DraftFor(preset);
    const std::string name = preset.name;

    ImGui::Indent();
    DrawPresetFields(draft.kind_index, draft.message.data(), draft.message.size(),
                     draft.rename.data(), draft.rename.size());

    if (ImGui::Button("Save")) {
        if (SavePresetDraft(preset, draft)) {
            overlay_editing_.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        preset_edit_drafts_.erase(name);
        overlay_editing_.clear();
        overlay_delete_armed_.clear();
        presets_ui_error_.clear();
    }

    // "-" deletes on a second click within 3s: no modal, but no one-click accident.
    const bool armed =
        overlay_delete_armed_ == name && ImGui::GetTime() < overlay_delete_armed_until_;
    const char* delete_label = armed ? "Delete?" : "-";
    const float delete_width =
        armed ? ImGui::CalcTextSize(delete_label).x + ImGui::GetStyle().FramePadding.x * 2.0f
              : ImGui::GetFrameHeight();
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > delete_width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - delete_width);
    }
    if (armed) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.2f, 0.2f, 1.0f));
    }
    if (ImGui::Button(delete_label, ImVec2(delete_width, 0.0f))) {
        if (armed) {
            DeletePreset(name);
        } else {
            overlay_delete_armed_ = name;
            overlay_delete_armed_until_ = ImGui::GetTime() + 3.0;
        }
    }
    if (armed) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", armed ? "Click again to delete" : "Delete preset");
    }

    ImGui::Unindent();
    ImGui::Spacing();
}

void GWDashPlugin::DrawTradePresetSettings()
{
    ImGui::TextUnformatted("Trade presets");
    ImGui::TextDisabled("Saved locally. Send posts one line to trade chat (outpost only, 3s");
    ImGui::TextDisabled("cooldown) and copies it to the clipboard.");
    ImGui::Checkbox("Show presets on overlay", &show_trade_presets_);
    ImGui::Checkbox("Send via chat box (press Enter to post, keeps arrow-up history)",
                    &send_via_chat_box_);
    ImGui::TextDisabled("Experimental. Off: Send posts directly (not in the arrow-up history).");

    ImGui::InputInt("Send hotkey VK (0=off)", &send_hotkey_vk_);
    send_hotkey_vk_ = std::clamp(send_hotkey_vk_, 0, 255);
    ImGui::TextDisabled("Hotkey sends the default preset after a confirm dialog.");

    if (!trade_presets_.LoadOk()) {
        ImGui::TextColored(COLOR_ERROR, "%s",
                           trade_presets_.LoadError().empty() ? "Failed to load presets.json"
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
            } else {
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
            } else {
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
        } else if (!expanded && open_preset_editor_ == preset.name) {
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

        PresetEditDraft& draft = DraftFor(preset);

        ImGui::Combo("Kind", &draft.kind_index, KIND_COMBO);
        ImGui::InputText("Message", draft.message.data(), draft.message.size());
        ImGui::InputText("Rename", draft.rename.data(), draft.rename.size());

        const std::string preview =
            gwdash::ComposeTradeLine(KindLabel(draft.kind_index), draft.message.data());
        ImGui::TextDisabled("Preview: %s", preview.c_str());

        if (ImGui::Button("Save")) {
            SavePresetDraft(preset, draft);
        }
        ImGui::SameLine();
        if (ImGui::Button("Send")) {
            gwdash::TradePreset pending = preset;
            pending.kind = KindLabel(draft.kind_index);
            pending.message = gwdash::SanitizePresetMessage(draft.message.data());
            SendAndCopyPreset(pending);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Send to trade chat (outpost only) and copy to the clipboard.");
        }

        if (ImGui::Button("Up")) {
            std::string error;
            if (trade_presets_.Move(preset.name, -1, error)) {
                PersistPresets();
            } else if (!error.empty()) {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Down")) {
            std::string error;
            if (trade_presets_.Move(preset.name, 1, error)) {
                PersistPresets();
            } else if (!error.empty()) {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            std::string error;
            if (trade_presets_.Duplicate(preset.name, error)) {
                PersistPresets();
                presets_ui_error_.clear();
            } else {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Default")) {
            std::string error;
            if (trade_presets_.SetDefault(preset.name, error)) {
                PersistPresets();
                presets_ui_error_.clear();
            } else {
                presets_ui_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            DeletePreset(preset.name);
        }

        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Add preset");
    ImGui::InputText("Name", draft_name_.data(), draft_name_.size());
    ImGui::Combo("Kind##new", &draft_kind_index_, KIND_COMBO);
    ImGui::InputTextWithHint("Message##new", "e.g. 27e arms", draft_message_.data(),
                             draft_message_.size());
    ImGui::TextDisabled(
        "Preview: %s",
        gwdash::ComposeTradeLine(KindLabel(draft_kind_index_), draft_message_.data()).c_str());

    if (trade_presets_.List().size() >= gwdash::MAX_TRADE_PRESETS) {
        ImGui::TextDisabled("At most %zu presets.", gwdash::MAX_TRADE_PRESETS);
    } else if (ImGui::Button("Add")) {
        AddPresetFromDraft();
    }
}

void GWDashPlugin::Draw(IDirect3DDevice9*)
{
    if (GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(210.0f, 108.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(background_alpha_);

    // Shown even when empty in the full view, so presets can be added with "+".
    const bool show_presets = show_trade_presets_ && (!compact_ || !trade_presets_.List().empty());
    ImGuiWindowFlags flags = show_presets ? 0 : ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin(Name(), show_closebutton ? GetVisiblePtr() : nullptr, GetWinFlags(flags))) {
        const bool scaled = font_scale_ < 0.99f || font_scale_ > 1.01f;
        if (scaled) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * font_scale_);
        }

        const gwdash::PriceState state = prices_.State();

        if (compact_) {
            DrawCompact(state);
        } else {
            DrawRows(state);
        }

        if (show_presets) {
            if (compact_) {
                ImGui::Separator();
            } else {
                ImGui::Spacing();
            }
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

    if (ImGui::Combo("Refresh every", &refresh_index_,
                     "60 seconds\0"
                     "2 minutes\0"
                     "5 minutes\0")) {
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
        ImGui::TextColored(
            COLOR_STALE, "%s",
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
    LoadSetting("send_via_chat_box", send_via_chat_box_);

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
    SaveSetting("send_via_chat_box", send_via_chat_box_);

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
    line +=
        snapshot.armbrace.price.has_value() ? gwdash::FormatEcto(*snapshot.armbrace.price) : "--";
    line += " | Black Dye ";
    line +=
        snapshot.blackdye.price.has_value() ? gwdash::FormatGold(*snapshot.blackdye.price) : "--";
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
    } else if (argument == L"hide") {
        *visible = false;
    } else if (argument == L"toggle") {
        *visible = !*visible;
    } else if (argument == L"refresh") {
        prices_.RequestRefresh();
        WriteChat("Refreshing prices...");
    } else if (argument == L"prices") {
        WritePricesToChat();
    } else if (argument == L"update") {
        updater_.RequestCheck();
        WriteChat("Checking for updates...");
    } else if (argument == L"version") {
        WriteChat(std::string("GWDash ") + GWDASH_VERSION);
    } else if (argument == L"presets") {
        const auto& presets = trade_presets_.List();
        if (presets.empty()) {
            WriteChat("No trade presets. Add some under Toolbox → Settings → GWDash.");
            return;
        }
        WriteChat(std::string("Trade presets (") + std::to_string(presets.size()) + "):");
        for (const gwdash::TradePreset& preset : presets) {
            const std::string line = gwdash::ComposeTradeLine(preset.kind, preset.message);
            WriteChat("  /gwdash send " + preset.name + (preset.is_default ? " *" : "") + "  " +
                      line);
        }
    } else if (argument == L"send") {
        if (argc < 3) {
            WriteChat("Usage: /gwdash send <name>");
            return;
        }
        SendPresetByName(PluginUtils::WStringToString(argv[2]));
    } else {
        WriteChat(
            "Usage: /gwdash [show|hide|toggle|refresh|prices|update|version|presets|send <name>]");
    }
}
