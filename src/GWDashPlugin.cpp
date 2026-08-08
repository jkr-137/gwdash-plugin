#include "GWDashPlugin.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>

#include <imgui.h>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Utilities/Hook.h>

#include <PluginUtils.h>

#include "Paths.h"
#include "PriceSnapshot.h"
#include "Version.h"

namespace {
    GW::HookEntry chat_command_hook;

    // Font Awesome 5 "coins" (U+F51E). Spelled out rather than pulling in
    // IconFontCppHeaders just for one glyph; Toolbox merges FA5 into its font.
    constexpr char ICON_COINS[] = "\xEF\x94\x9E";

    constexpr std::array<int, 3> REFRESH_CHOICES{60, 120, 300};
    constexpr float UNFOCUSED_IDLE_SECONDS = 600.0f;
    /** Chat samples older than this are shown as stale rather than current. */
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

    int64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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

    /** label on the left, value (plus optional age) right-aligned. */
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

    // Only signals; the worker threads finish their current request and exit.
    prices_.SignalStop();
    updater_.SignalStop();
}

bool GWDashPlugin::CanTerminate()
{
    // Never unmap this DLL while a worker thread is still inside it.
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
    if (!trade_presets_.Save()) {
        WriteChat("Could not save trade presets to disk.");
    }
}

void GWDashPlugin::SendPreset(const gwdash::TradePreset& preset)
{
    std::string error;
    if (trade_presets_.TrySend(preset, error)) {
        WriteChat(std::string("Sent to trade: ") + preset.message);
        return;
    }
    WriteChat(error.empty() ? "Failed to send trade preset." : error);
}

void GWDashPlugin::SendPresetByName(const std::string& name)
{
    std::string error;
    if (trade_presets_.TrySendByName(name, error)) {
        if (const gwdash::TradePreset* preset = trade_presets_.Find(name)) {
            WriteChat(std::string("Sent to trade: ") + preset->message);
        }
        else {
            WriteChat("Sent to trade chat.");
        }
        return;
    }
    WriteChat(error.empty() ? "Failed to send trade preset." : error);
}

void GWDashPlugin::Update(const float delta)
{
    if (!started_) {
        return;
    }

    // Delta is in seconds despite what the base class header claims.
    if (GuildWarsHasFocus()) {
        seconds_unfocused_ = 0.0f;
    }
    else {
        seconds_unfocused_ += delta;
    }
    prices_.SetIdle(throttle_unfocused_ && seconds_unfocused_ > UNFOCUSED_IDLE_SECONDS);

    seconds_since_tick_ += delta;
    if (seconds_since_tick_ < 1.0f) {
        return;
    }
    seconds_since_tick_ = 0.0f;

    // Updater messages are queued on its worker thread; chat writes have to
    // happen here, on the game thread.
    for (const std::string& message : updater_.TakeNotifications()) {
        WriteChat(message);
    }
}

void GWDashPlugin::DrawRows()
{
    const gwdash::PriceState state = prices_.State();
    const int64_t now = NowMs();

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
    DrawValueRow("Arms", arms_value, color_for(snapshot.armbrace.price, snapshot.armbrace.at),
                 age_for(snapshot.armbrace.at),
                 snapshot.armbrace.price.has_value()
                     ? "Armbrace of Truth - recency-weighted median of Kamadan trade chat, in ecto"
                     : "Armbrace of Truth - waiting for Kamadan trade-chat mentions");

    const std::string dye_value = state.has_data && snapshot.blackdye.price.has_value()
                                      ? gwdash::FormatGold(*snapshot.blackdye.price)
                                      : "--";
    DrawValueRow("Black Dye", dye_value, color_for(snapshot.blackdye.price, snapshot.blackdye.at),
                 age_for(snapshot.blackdye.at),
                 snapshot.blackdye.price.has_value()
                     ? "Black Dye - recency-weighted median of Pre-Searing Ascalon trade chat"
                     : "Black Dye - waiting for Ascalon trade-chat mentions");
}

void GWDashPlugin::DrawCompact()
{
    const gwdash::PriceState state = prices_.State();
    const gwdash::PriceSnapshot& snapshot = state.snapshot;

    const std::string line =
        "Ecto " + (snapshot.ecto.price.has_value() ? gwdash::FormatGold(*snapshot.ecto.price) : "--") +
        " | Arms " + (snapshot.armbrace.price.has_value() ? gwdash::FormatEcto(*snapshot.armbrace.price) : "--") +
        " | BD " + (snapshot.blackdye.price.has_value() ? gwdash::FormatGold(*snapshot.blackdye.price) : "--");

    ImGui::TextUnformatted(line.c_str());
}

void GWDashPlugin::DrawStatusLine()
{
    const gwdash::PriceState state = prices_.State();
    const gwdash::UpdateState update = updater_.State();
    const int64_t now = NowMs();

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
    else if (state.received_at > 0) {
        ImGui::TextDisabled("updated %s ago", gwdash::FormatAge(state.received_at, now).c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Source: %s", state.source.empty() ? "unknown" : state.source.c_str());
        }
    }

    if (update.status == gwdash::UpdateStatus::Staged) {
        ImGui::TextDisabled("update ready - restart");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", update.message.c_str());
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

    if (ImGui::Begin(Name(), show_closebutton ? GetVisiblePtr() : nullptr,
                     GetWinFlags(ImGuiWindowFlags_NoScrollbar))) {
        const bool scaled = font_scale_ < 0.99f || font_scale_ > 1.01f;
        if (scaled) {
            // SetWindowFontScale is compiled out by Toolbox's
            // IMGUI_DISABLE_OBSOLETE_FUNCTIONS, so scale the font explicitly.
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * font_scale_);
        }

        if (compact_) {
            DrawCompact();
        }
        else {
            DrawRows();
        }

        if (show_trade_presets_ && !trade_presets_.List().empty()) {
            ImGui::Separator();
            DrawTradePresetButtons();
        }

        if (show_status_) {
            ImGui::Separator();
            DrawStatusLine();
        }

        if (scaled) {
            ImGui::PopFont();
        }
    }
    ImGui::End();
}

void GWDashPlugin::DrawTradePresetButtons()
{
    for (const gwdash::TradePreset& preset : trade_presets_.List()) {
        ImGui::PushID(preset.name.c_str());

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("[%s]", preset.kind.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(preset.name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", preset.message.c_str());
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Send")) {
            SendPreset(preset);
        }

        ImGui::PopID();
    }
}

void GWDashPlugin::DrawTradePresetSettings()
{
    ImGui::TextUnformatted("Trade presets");
    ImGui::TextDisabled("Saved locally. Send is manual only and needs a trade district.");
    ImGui::Checkbox("Show presets on overlay", &show_trade_presets_);

    if (!presets_ui_error_.empty()) {
        ImGui::TextColored(COLOR_ERROR, "%s", presets_ui_error_.c_str());
    }

    // Snapshot so Delete during the loop does not invalidate iteration.
    const std::vector<gwdash::TradePreset> listed = trade_presets_.List();
    for (const gwdash::TradePreset& preset : listed) {
        ImGui::PushID(preset.name.c_str());
        ImGui::Separator();

        ImGui::TextUnformatted(preset.name.c_str());

        auto [it, inserted] = preset_edit_drafts_.try_emplace(preset.name);
        PresetEditDraft& draft = it->second;
        if (inserted) {
            draft.kind_index = KindIndex(preset.kind);
            draft.message.fill('\0');
            std::strncpy(draft.message.data(), preset.message.c_str(), draft.message.size() - 1);
        }

        ImGui::Combo("Kind", &draft.kind_index, KIND_COMBO);
        ImGui::InputText("Message", draft.message.data(), draft.message.size());

        if (ImGui::Button("Save")) {
            std::string error;
            if (trade_presets_.Upsert(preset.name, KindLabel(draft.kind_index), draft.message.data(),
                                      error)) {
                PersistPresets();
                presets_ui_error_.clear();
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
            SendPreset(pending);
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(draft.message.data());
            WriteChat("Copied trade preset to clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            if (trade_presets_.Remove(preset.name)) {
                preset_edit_drafts_.erase(preset.name);
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
    ImGui::InputText("Message##new", draft_message_.data(), draft_message_.size());

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

    refresh_index_ = std::clamp(refresh_index_, 0, static_cast<int>(REFRESH_CHOICES.size()) - 1);
    background_alpha_ = std::clamp(background_alpha_, 0.0f, 1.0f);
    font_scale_ = std::clamp(font_scale_, 0.7f, 2.0f);

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
            WriteChat("  /gwdash send " + preset.name + "  [" + preset.kind + "] " + preset.message);
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
