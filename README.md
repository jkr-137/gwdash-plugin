# GWDash plugin for GWToolbox++

In-game overlay for the three prices [GWDash](https://gwdash.com) tracks: **Ectoplasm**, **Armbrace of Truth** and **Black Dye**. Freely movable, updates itself, installs with one line.

```powershell
irm https://gwdash.com/install.ps1 | iex
```

Then start Guild Wars. If the overlay does not appear, enable it once under **Toolbox → Settings → Plugins → GWDash.dll → Load**.

Toggle with `/gwdash`. Also: `/gwdash refresh`, `/gwdash prices`, `/gwdash update`, `/gwdash version`, `/gwdash presets`, `/gwdash send <name>`.

## What you get

- Three price rows matching the dashboard: `Ecto 5.6k`, `Arms 27e` / `27.4e`, `Black Dye 18.4k`
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

- The chat line is **composed** as `kind` + message (unless the message already starts with `wtb`/`wts`/`wtt`). Preview is shown in Settings.
- **Send** asks for confirmation from the overlay (and optional hotkey), then posts exactly one message to the in-game trade channel (`$`). Requires an **outpost**. There is a **3s** cooldown; nothing is sent on a timer or while AFK.
- `/gwdash send <name>` sends without the confirm dialog (same cooldown / outpost rules).
- **Copy** puts the composed line on the clipboard.
- Import/Export JSON, reorder, duplicate, rename, and mark a default preset for the optional send hotkey (VK code in Settings; `0` = off).
- Compact overlay mode shows a **Presets…** menu instead of the full list.
- If `presets.json` fails to parse, saving is blocked until you **Reset presets**.

## Safety notes

- **ArenaNet does not permit third-party plugins.** Toolbox itself prints that warning the first time any plugin is loaded. Use at your own risk.
- Trade-preset **Send** is a convenience for a single, user-initiated chat line — do not use it to spam or automate trade chat.
- The DLL is currently **unsigned** (Authenticode). Windows Defender occasionally quarantines unsigned Toolbox plugins — if Load fails with a virus error, allow the file and try again.
- While any plugin is loaded, Toolbox refuses to write a crash dump. Do not report Toolbox crashes that happen with GWDash loaded.

## Auto-update

`GWDash.dll` is a tiny loader. The real plugin lives one directory down as `GWDash.core.dll`. On start the loader swaps in anything waiting in `GWDash/pending/`, then forwards to the payload. That is how a new version can install itself without fighting the Windows file lock on the DLL Toolbox has loaded.

The updater only talks HTTPS to an allowlisted set of hosts (`api.github.com`, `github.com`, `objects.githubusercontent.com`, `release-assets.githubusercontent.com`, plus the price CDN hosts). Redirects are followed only while the next host stays on that allowlist. ETags are sanitized before being echoed as `If-None-Match`. Draft/prerelease GitHub releases are ignored.

Each release ships `manifest.json` + `manifest.sig` (ed25519). The plugin verifies the signature against the public key in `src/UpdatePublicKey.h`, then checks the SHA-256 of `GWDash.core.dll` before staging. Releases also attach PDBs for crash triage.

**Trust model:** ed25519 proves the manifest was signed with the release key held as GitHub secret `GWDASH_UPDATE_ED25519_SK`. Generate a keypair with `scripts/generate-update-keys.py`, put the public bytes into `UpdatePublicKey.h`, and store the private seed hex in that secret. Treat a compromised signing key or GitHub account as full RCE in Guild Wars — keep org/2FA/CI secrets locked down.

The loader itself cannot be replaced this way. When a release also changes the loader, the overlay asks you to re-run the installer once.

## Building from source

Needs Visual Studio 2022 (toolset v143+) with the **Desktop development with C++** workload, CMake ≥ 3.29, and [vcpkg](https://vcpkg.io).

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset windows-x86 -DGWDASH_VERSION=0.1.0
cmake --build --preset relwithdebinfo
bin\RelWithDebInfo\gwdash_tests.exe
```

Outputs land in `bin/RelWithDebInfo/`:

| File | Role |
|------|------|
| `GWDash.dll` | Loader stub Toolbox sees |
| `GWDash.core.dll` | Overlay, price client, updater |
| `gwdash_tests.exe` | Pure unit tests (no GWCA) |

The build pulls [GWToolbox++](https://github.com/gwdevhub/GWToolboxpp) at the pinned tag `8.32_Release` via FetchContent and reuses its ImGui (with Toolbox's `imconfig.h` — required, the struct layouts differ from stock ImGui), plugin base sources and prebuilt `gwca.lib`. It does **not** build `GWToolboxdll` itself.

## Project layout

```
cmake/ToolboxSdk.cmake   FetchContent + plugin_base stand-in
compat/                  stubs so we can compile Toolbox sources without linking GWToolboxdll
loader/Loader.cpp        GWDash.dll
src/                     GWDash.core.dll
tests/                   gwdash_tests
vendor/ed25519/          Orson Peters ed25519 (verify/sign)
.github/                 CI build + tagged releases
```

## License

MIT. GWToolbox++ is MIT as well; this plugin is not affiliated with ArenaNet or the GWToolbox++ project. `vendor/ed25519` is zlib (see its LICENSE).
