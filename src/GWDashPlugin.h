#pragma once

#include <ToolboxUIPlugin.h>

#include "PriceClient.h"
#include "Updater.h"

/**
 * In-game overlay for the three prices GWDash tracks: Ectoplasm (NPC trader),
 * Armbrace of Truth (Kamadan player trader) and Black Dye (Pre-Searing Ascalon).
 * Values and formatting mirror gwdash.com so both agree at a glance.
 */
class GWDashPlugin final : public ToolboxUIPlugin {
public:
    GWDashPlugin();
    ~GWDashPlugin() override = default;

    [[nodiscard]] const char* Name() const override { return "GWDash"; }
    [[nodiscard]] const char* Icon() const override;
    [[nodiscard]] bool HasSettings() const override { return true; }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;
    void SignalTerminate() override;
    bool CanTerminate() override;
    void Terminate() override;

    void Update(float delta) override;
    void Draw(IDirect3DDevice9* device) override;
    void DrawSettings() override;

    void LoadSettings(const wchar_t* folder) override;
    void SaveSettings(const wchar_t* folder) override;

    /** Handles /gwdash. Runs on the game thread. */
    void OnChatCommand(int argc, const LPWSTR* argv);

private:
    void DrawRows();
    void DrawCompact();
    void DrawStatusLine();
    void WritePricesToChat() const;
    void ApplyRefreshInterval();

    gwdash::PriceClient prices_;
    gwdash::Updater updater_;

    bool started_ = false;

    // Settings, persisted in <plugins>/GWDash.json.
    int refresh_index_ = 1;
    bool compact_ = false;
    bool show_age_ = true;
    bool show_ecto_spread_ = false;
    bool show_status_ = true;
    bool throttle_unfocused_ = true;
    bool auto_update_ = true;
    float background_alpha_ = 0.75f;
    float font_scale_ = 1.0f;

    float seconds_since_tick_ = 0.0f;
    float seconds_unfocused_ = 0.0f;
};
