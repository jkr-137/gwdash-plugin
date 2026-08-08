// GWDash loader stub.
//
// Toolbox sees this DLL, keeps it in its enabled_plugins list by filename, and
// holds a Windows file lock on it for as long as it is loaded. That lock is the
// reason the actual plugin lives one directory down:
//
//   plugins/GWDash.dll                    <- this file, filename never changes
//   plugins/GWDash/GWDash.core.dll        <- the payload we forward to
//   plugins/GWDash/pending/GWDash.core.dll<- staged update, swapped in below
//
// Toolbox scans the plugins folder non-recursively, so it never sees the
// payload, and nothing holds a lock on it before we load it. That lets the
// in-game updater download a new payload into pending/ and have it take effect
// on the next Guild Wars start without any user interaction.
//
// This file must stay boring. It is the one part users cannot auto-update.

#include <Windows.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>

#include <ToolboxPlugin.h>

#include <Version.h>

namespace fs = std::filesystem;

namespace {
    constexpr wchar_t CORE_DIRECTORY[] = L"GWDash";
    constexpr wchar_t CORE_FILENAME[] = L"GWDash.core.dll";
    constexpr wchar_t PENDING_DIRECTORY[] = L"pending";
    constexpr wchar_t PREVIOUS_FILENAME[] = L"GWDash.core.previous.dll";
    constexpr wchar_t VERSION_FILENAME[] = L"version.txt";

    std::string load_error;

    void Debug(const std::string& message)
    {
        OutputDebugStringA(("[GWDash loader] " + message + "\n").c_str());
    }

    /** Directory this DLL sits in, i.e. Toolbox's plugins folder. */
    fs::path OwnDirectory()
    {
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;) {
            const DWORD length =
                GetModuleFileNameW(plugin_handle, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return {};
            }
            if (length < buffer.size()) {
                return fs::path(buffer.data(), buffer.data() + length).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    bool Rename(const fs::path& from, const fs::path& to)
    {
        return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    void Remove(const fs::path& path)
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    /**
     * Moves a staged payload into place, keeping the outgoing one around as
     * rollback material until we know the new one actually loads.
     */
    void ApplyStagedUpdate(const fs::path& core_dir)
    {
        std::error_code ec;
        const fs::path pending_dir = core_dir / PENDING_DIRECTORY;
        const fs::path staged = pending_dir / CORE_FILENAME;
        if (!fs::exists(staged, ec)) {
            return;
        }

        const fs::path current = core_dir / CORE_FILENAME;
        const fs::path previous = core_dir / PREVIOUS_FILENAME;

        Remove(previous);
        const bool had_current = fs::exists(current, ec);
        if (had_current && !Rename(current, previous)) {
            Debug("could not move the running payload aside, keeping it");
            return;
        }

        if (!Rename(staged, current)) {
            Debug("could not move the staged payload into place");
            if (had_current) {
                Rename(previous, current);
            }
            return;
        }

        const fs::path staged_version = pending_dir / VERSION_FILENAME;
        if (fs::exists(staged_version, ec)) {
            Rename(staged_version, core_dir / VERSION_FILENAME);
        }

        Remove(pending_dir);
        Debug("applied staged update");
    }

    HMODULE LoadPayload(const fs::path& core_dir)
    {
        std::error_code ec;
        const fs::path current = core_dir / CORE_FILENAME;
        const fs::path previous = core_dir / PREVIOUS_FILENAME;

        if (!fs::exists(current, ec)) {
            load_error = "GWDash\\GWDash.core.dll is missing. Re-run the installer: "
                         "irm https://gwdash.com/install.ps1 | iex";
            return nullptr;
        }

        if (const HMODULE handle = LoadLibraryW(current.c_str())) {
            Remove(previous);
            return handle;
        }

        const DWORD error = GetLastError();
        Debug("LoadLibraryW failed with " + std::to_string(error));

        // A freshly swapped payload that will not load: put the previous one
        // back and try again, so a bad release cannot brick the plugin.
        if (fs::exists(previous, ec) && Rename(previous, current)) {
            Debug("rolled back to the previous payload");
            if (const HMODULE handle = LoadLibraryW(current.c_str())) {
                return handle;
            }
        }

        load_error = "Windows refused to load GWDash.core.dll (error " + std::to_string(error) +
                     "). If your antivirus quarantined it, allow the file and restart Guild Wars.";
        return nullptr;
    }

    /**
     * Stand-in returned when the payload cannot be loaded. Toolbox dereferences
     * plugin->instance without a null check in PluginModule::SaveSettings, so
     * returning nullptr from ToolboxPluginInstance is not an option.
     */
    class FallbackPlugin final : public ToolboxPlugin {
      public:
        [[nodiscard]] const char* Name() const override { return "GWDash"; }
        [[nodiscard]] bool HasSettings() const override { return true; }

        void Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns,
                        const HMODULE toolbox_dll) override
        {
            ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
            if (!reported) {
                reported = true;
                Debug(load_error.empty() ? "payload failed to load" : load_error);
                // Visible once without needing GWCA WriteChat in the loader.
                MessageBoxA(nullptr,
                            load_error.empty() ? "GWDash.core.dll failed to load."
                                               : load_error.c_str(),
                            "GWDash", MB_OK | MB_ICONWARNING);
            }
        }

        void DrawSettings() override
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "GWDash could not start.");
            ImGui::TextWrapped("%s", load_error.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("Loader version %s", GWDASH_VERSION);
        }

      private:
        bool reported = false;
    };
} // namespace

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static ToolboxPlugin* instance = nullptr;
    if (instance) {
        return instance;
    }

    static FallbackPlugin fallback;
    instance = &fallback;

    const fs::path own_dir = OwnDirectory();
    if (own_dir.empty()) {
        load_error = "Could not determine the plugin folder.";
        return instance;
    }

    const fs::path core_dir = own_dir / CORE_DIRECTORY;
    ApplyStagedUpdate(core_dir);

    const HMODULE payload = LoadPayload(core_dir);
    if (!payload) {
        return instance;
    }

    using InstanceFn = ToolboxPlugin* (*)();
    const auto entry =
        reinterpret_cast<InstanceFn>(GetProcAddress(payload, "ToolboxPluginInstance"));
    if (!entry) {
        load_error =
            "GWDash.core.dll has no ToolboxPluginInstance export - the install looks corrupt.";
        FreeLibrary(payload);
        return instance;
    }

    if (ToolboxPlugin* forwarded = entry()) {
        // Deliberately kept loaded for the rest of the process: FreeLibrary is
        // not safe to call from DllMain, which is the only unload hook we get.
        // The payload therefore has to tolerate repeated Initialize/Terminate
        // cycles if the user unloads and reloads us from the Plugins panel.
        instance = forwarded;
    } else {
        load_error = "GWDash.core.dll returned no plugin instance.";
        FreeLibrary(payload);
    }

    return instance;
}
