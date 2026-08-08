# GWDash plugin for GWToolbox++

In-game overlay for the three prices [GWDash](https://gwdash.com) tracks: **Ectoplasm**, **Armbrace of Truth** and **Black Dye**. Freely movable, updates itself, installs with one line.

```powershell
irm https://gwdash.com/install.ps1 | iex
```

Then start Guild Wars. If the overlay does not appear, enable it once under **Toolbox → Settings → Plugins → GWDash.dll → Load**.

Toggle with `/gwdash`. Also: `/gwdash refresh`, `/gwdash prices`, `/gwdash update`, `/gwdash version`, `/gwdash presets`, `/gwdash send <name>`.

## What you get

- Three price rows matching the dashboard: `Ecto 5.6k`, `Arms 27e`, `Black Dye 18.4k`
- Compact single-line mode, adjustable font scale and background
- Position and size remembered by Toolbox (drag the window)
- Trade presets: save WTB / WTS / WTT lines locally and send one message to trade chat via the overlay, Settings, or `/gwdash send <name>` (manual only — no auto-repost)
- Auto-update: a new release is downloaded in the background and activates the next time you start Guild Wars

## Requirements

- Windows, 32-bit Guild Wars (the only kind that exists)
- [GWToolbox++](https://www.gwtoolbox.com/) with the **Plugins** module enabled (Settings → *Enable the following features*)
- Built and tested against Toolbox `8.32_Release`

## Manual install

1. Download `GWDash.dll` and `GWDash.core.dll` from the [latest release](https://github.com/jkr-137/gwdash-plugin/releases/latest).
2. Put them here:

   ```
   %USERPROFILE%\Documents\GWToolboxpp\<COMPUTERNAME>\plugins\GWDash.dll
   %USERPROFILE%\Documents\GWToolboxpp\<COMPUTERNAME>\plugins\GWDash\GWDash.core.dll
   ```

   `scripts/open-plugins-folder.bat` opens that folder for you.
3. Close Guild Wars first if it is running — Windows locks a loaded DLL.
4. Start the game, then **Settings → Plugins → Load**.

Uninstall:

```powershell
iex "& { $(irm https://gwdash.com/install.ps1) } -Uninstall"
```

## Where the prices come from

The plugin polls a static JSON snapshot at `https://data.gwdash.com/prices.json`. That file is written by the GWDash Cloudflare Worker every two minutes into a public R2 bucket and served from Cloudflare's CDN. The request never hits a Worker and never touches D1, so plugin traffic does not consume the free-tier request budget.

If the CDN is unreachable, the plugin falls back to `https://gwdash.com/api/plugin/prices` (edge-cached, ETag).

## Trade presets

Under **Toolbox → Settings → Plugins → GWDash** you can add up to 12 named lines (`wtb` / `wts` / `wtt`). They are stored in `plugins/GWDash/presets.json` on your machine.

- **Send** posts exactly one message to the in-game trade channel (`$`). You need to be in a trade district (e.g. Kamadan). There is a 1s cooldown; nothing is sent on a timer or while AFK.
- **Copy** puts the message on the clipboard if you would rather paste it yourself.
- Overlay quick-send buttons can be toggled with *Show presets on overlay*.
- Chat: `/gwdash presets` lists names; `/gwdash send arms` (or whatever name you chose) sends that preset.

## Safety notes

- **ArenaNet does not permit third-party plugins.** Toolbox itself prints that warning the first time any plugin is loaded. Use at your own risk.
- Trade-preset **Send** is a convenience for a single, user-initiated chat line — do not use it to spam or automate trade chat.
- The DLL is currently **unsigned**. Windows Defender occasionally quarantines unsigned Toolbox plugins — if Load fails with a virus error, allow the file and try again.
- While any plugin is loaded, Toolbox refuses to write a crash dump. Do not report Toolbox crashes that happen with GWDash loaded.

## Auto-update

`GWDash.dll` is a tiny loader. The real plugin lives one directory down as `GWDash.core.dll`. On start the loader swaps in anything waiting in `GWDash/pending/`, then forwards to the payload. That is how a new version can install itself without fighting the Windows file lock on the DLL Toolbox has loaded.

The updater only talks HTTPS to an allowlisted set of hosts (`api.github.com`, `github.com`, `objects.githubusercontent.com`, `release-assets.githubusercontent.com`, plus the price CDN hosts). ETags from the price API are sanitized before being echoed back as `If-None-Match` (no CR/LF injection). Downloads are SHA-256-checked against `SHA256SUMS` from the same release.

**Trust model today:** checksums prove integrity against bit-flip/CDN corruption, not authenticity against a compromised GitHub account/CI (the SUMS file and the DLL share the same publisher). **Planned hardening:** Authenticode-sign both DLLs and verify the signature before staging/`LoadLibrary`, or ship an ed25519-signed update manifest with a public key baked into the binary. Until then, treat a compromised release repo as full RCE in Guild Wars — keep the GitHub org/2FA/CI secrets locked down.

The loader itself cannot be replaced this way. When a release also changes the loader, the overlay asks you to re-run the installer once.

## Building from source

Needs Visual Studio 2022 (toolset v143+) with the **Desktop development with C++** workload, CMake ≥ 3.29, and [vcpkg](https://vcpkg.io).

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset windows-x86 -DGWDASH_VERSION=0.1.0
cmake --build --preset relwithdebinfo
```

Outputs land in `bin/RelWithDebInfo/`:

| File | Role |
|------|------|
| `GWDash.dll` | Loader stub Toolbox sees |
| `GWDash.core.dll` | Overlay, price client, updater |

The build pulls [GWToolbox++](https://github.com/gwdevhub/GWToolboxpp) at the pinned tag `8.32_Release` via FetchContent and reuses its ImGui (with Toolbox's `imconfig.h` — required, the struct layouts differ from stock ImGui), plugin base sources and prebuilt `gwca.lib`. It does **not** build `GWToolboxdll` itself.

## Project layout

```
cmake/ToolboxSdk.cmake   FetchContent + plugin_base stand-in
compat/                  stubs so we can compile Toolbox sources without linking GWToolboxdll
loader/Loader.cpp        GWDash.dll
src/                     GWDash.core.dll
.github/workflows/       CI build + tagged releases
```

## License

MIT. GWToolbox++ is MIT as well; this plugin is not affiliated with ArenaNet or the GWToolbox++ project.
