# TigerSlicer changelog

Detailed notes for every TigerSlicer release, written so the changes can be
reproduced or ported (in particular to the macOS build). TigerSlicer is based on
OrcaSlicer; "upstream" below means OrcaSlicer.

---

## 0.0.1 (2026-10-06)

Theme: make it TigerSlicer, not an OrcaSlicer fork. Cloud features removed,
Orca branding replaced, the remaining green/teal turned pink, new startup screen.

Commits, oldest first (all on `main`):

| Commit | Subject |
|---|---|
| `e2f17d562d` | fix(cloud): our own keychain entry, and no silent sign-in on startup |
| `54266f12f9` | feat(cloud): sign-in, preset sync and the startup phone-home are gone |
| `d47de5f80a` | feat(splash): a black-and-pink startup card |
| `bc2b876fad` | fix(i18n): compile catalogs as TigerSlicer.mo so translations load |
| `e0c87eb0ae` | feat(brand): ship TigerSlicer.exe and stop claiming OrcaSlicer's system names |
| `a06dcde36a` | feat(brand): say TigerSlicer in every translatable string |
| `1746d8cba3` | feat(brand): rename the untranslated places users still saw Orca |
| `a9713f82ad` | fix(prefs): stealth mode says what it still does |
| `db2ca258c8` | feat(theme): the 3D scene and its gizmos turn pink |
| `cdf2eeea1e` | feat(theme): dialogs, overlays and widgets lose their teal |
| `b32763ca49` | feat(theme): new parts start pink |
| `b995ef02c3` | fix(theme): filament swatches show their colour, pink icons stay pink |
| `917783b1ce` | feat(theme): web views lose their teal |
| `d3166a8e67` | fix(theme): fresh installs skip the filament colour migration |
| `d98abeffa2` | chore(version): 0.0.1 |
| `a63356fce9` | fix(theme): selected pink parts stay pink |

Style used for disabled code: early `return;` or `if (false && ...)` with a
`// TigerSlicer:` comment, keeping upstream code compiled so upstream merges stay easy.
No classes or headers were deleted.

### 1. Cloud features removed

**Privacy bug fixed first (`e2f17d562d`, `src/slic3r/Utils/OrcaCloudServiceAgent.cpp`).**
TigerSlicer stored its cloud login in the OS keychain under `"OrcaSlicer/Auth"`, the
same entry stock OrcaSlicer uses, and `OrcaCloudServiceAgent::start()` (called on every
launch, even in stealth mode) loaded and refreshed that token against
auth.orcaslicer.com. So TigerSlicer silently signed into the user's real OrcaSlicer
account, and a TigerSlicer logout could delete OrcaSlicer's saved login.
- `SECRET_STORE_SERVICE` is now `"TigerSlicer/Auth"` (wxSecretStore keys by service name
  on Windows, macOS and Linux).
- `start()` only runs `regenerate_pkce()` and returns: no session restore/refresh.
- `connect_server()` returns `BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED` without the
  `/api/v1/health` request.
- The agent object is still created (other code dereferences it without null checks).

**Sign-in and sync (`54266f12f9`).**
- `GUI_App.cpp`: `ShowUserLogin(true)` is a no-op (`ShowUserLogin(false)` still closes a
  dialog), so `request_login`/`check_login` do nothing. `get_login_info()` returns at the
  top (can never call `user_logout()`). `handle_web_request()` ignores all
  `get_*login_info`, `homepage_*login_or_register`, `homepage_*logout`,
  `homepage_modeldepot` and `request_project_download` commands (placed before the
  stealth checks, so the "Quit Stealth Mode to log in" dialog is unreachable).
- Startup: user-preset cloud sync, `set_cloud_user(preset_folder)` and the "logged in →
  switch preset folder / fetch cloud plugins" block are disabled. Local user presets load
  from `user/default`. Note: presets saved while the old bug had TigerSlicer signed in
  live in `data_dir/user/<cloud-user-id>/` and no longer show; copy them by hand if needed.
- Bambu Cloud login: `NetworkAgentFactory.cpp` skips the `BBL_CLOUD_PROVIDER` entry;
  `AppConfig.cpp` migration no longer adds `bbl` for `installed_networking` users.
  The temporary `BBLCloudServiceAgent` wrapper that starts the Bambu network plugin's
  agent is untouched, so **Bambu LAN printing still works** (the printer agent only
  stores the cloud-agent pointer and talks to `BBLNetworkPlugin` directly).
- Home page (`resources/web/homepage/index.html`, `css/home.css`, `js/home.js`):
  removed the whole `#LoginArea` (Tiger Cloud + Bambu Cloud sign-in and the
  network-plugin tip) and the TigerCloud shortcut; `#LeftBoard` now defaults to
  `display:block` (it used to wait for a `cloud_providers_info` message); login JS removed.
  `WebViewDialog.cpp`: the 2-second login poll timer is not created.
- Menus (`MainFrame.cpp`): "Sync Presets" removed from the Windows/Linux top menu **and
  the macOS File menu (inside the `#else` branch)**; Help > "Check for Updates" removed
  on all platforms (the checker was already a no-op); the "Synchronize from Orca Cloud?"
  dialog (`on_select_default_preset`) returns early.
- Preferences (`Preferences.cpp`): removed Show shared profiles notification, Hide login
  side panel, Cloud Providers / Enable Bambu Cloud, Check for stable updates only, Auto
  sync user presets, Update built-in presets, Use encrypted file for token storage.
  Kept: Login region (selects the Bambu plugin download host), Stealth mode, Network test,
  Bambu network plugin enable/version.
- Plugins dialog: `open_plugin_hub()`/`open_plugin_on_cloud()` return early; default
  install action is `install-local`; hub/"View on TigerCloud" links removed from
  `resources/web/dialog/PluginsDialog/*`. Missing-plugin notices in `Plater.cpp` are kept
  (slicing stays blocked until resolved) but their button is now "Open Plugins".
- Preset bundles: `PresetBundleDialog::OpenBundleOnCloud()` returns early.
  Export Preset dialog: "Export to TigerCloud" removed. `CreatePresetsDialog.cpp`: no
  "sync user presets" offer. Setup wizard: `guide/22/22.js` goes straight to `guide/5`
  (the cloud/stealth page `guide/4orca` is no longer reachable).
- Shared-profiles notification: default `show_shared_profiles_notification=false` and
  the notification is gated off in `Plater.cpp`.

**No more phone-home on startup.**
- `AppConfig::profile_update_url()` returns `""` → per-vendor profile checks against
  check-version.orcaslicer.com stop. To keep bundled vendor profiles installing exactly
  as before, `PresetUpdater` forces the existing compare-versions path
  (`if (true || enabled_config_update)`).
- `PresetUpdater::sync()` no longer calls `sync_printer_config()` (the Bambu printer
  config download on every launch); the bundled `resources/printers` copy is still used.
- Left on purpose: the Bambu network plugin and its own update check (only when the
  plugin is loaded and not in stealth mode), online HMS printer-error descriptions, the
  WebView2 runtime download, physical-printer host OAuth (SimplyPrint/Obico/3DPrinterOS/
  Flashforge), and the user-initiated network test.

**Stealth mode tooltip (`a9713f82ad`).** Rewritten to what it still does: keeps the Bambu
network plugin from checking for updates/connecting to Bambu's servers and stops online
printer error descriptions. New msgid, untranslated (English fallback).

### 2. "TigerSlicer", not Orca

**Translations never loaded (`bc2b876fad`).** The build wrote
`resources/i18n/<lang>/OrcaSlicer.mo` but the app loads catalog `SLIC3R_APP_KEY` =
`TigerSlicer`. Now `CMakeLists.txt` writes `${SLIC3R_APP_KEY}.mo`, and
`scripts/run_gettext.bat` / `scripts/run_gettext.sh` use `mo_name="TigerSlicer"`
(must match `version.inc`); `.github/workflows/check_locale.yml` too. Source catalogs keep
their names (`localization/i18n/OrcaSlicer.pot`, `<lang>/OrcaSlicer_<lang>.po`).
macOS needs nothing extra (`build_release_macos.sh` calls the fixed `run_gettext.sh`).

**Windows names and OS registration (`e0c87eb0ae`, Windows only unless noted).**
- `src/CMakeLists.txt`: launcher outputs `TigerSlicer.exe`, GUI DLL `TigerSlicer.dll`
  (CMake target names unchanged; DLL gets `PDB_NAME`/`ARCHIVE_OUTPUT_NAME`
  `TigerSlicer_dll` so it doesn't collide with the exe's .pdb/.lib).
  `src/OrcaSlicer_app_msvc.cpp` loads `TigerSlicer.dll` (export `orcaslicer_main` kept);
  `Process.cpp` spawns `TigerSlicer.exe`; `BBLNetworkPlugin.cpp` exe-name check uses
  `TigerSlicer.exe` with a computed length (the old fallback compared against a wrong
  hard-coded length and never fired). `OrcaSlicer.rc.in` OriginalFilename
  `TigerSlicer.exe`; `OrcaSlicer-gcodeviewer.rc.in` CompanyName "Tiger".
- File association ProgID `" Orca.Slicer.1"` → `" Tiger.Slicer.1"` (3 places in
  `GUI_App.cpp`, leading space kept) so it no longer collides with real OrcaSlicer.
- URL scheme: registers `tigerslicer://` instead of hijacking `orcaslicer://`; incoming
  links accept both (`is_tigerslicer_open()` in `Utils.hpp`, regexes in `Downloader.cpp`
  and `InstanceCheck.cpp`). Old `orcaslicer` registry keys from earlier builds are not removed.
- CPack/NSIS (`CMakeLists.txt`): name, install dir, uninstall key, shortcuts → TigerSlicer;
  vendor "Tiger"; new WiX upgrade GUID `609736df-9965-4297-8877-6072556c6015` (the old
  settings could have uninstalled a real OrcaSlicer). MSIX manifest + `build_msix.ps1` too.
- Cross-platform text: Linux `.desktop` `Name=TigerSlicer` + `x-scheme-handler/tigerslicer`;
  macOS `cmake/modules/MacOSXBundleInfo.plist.in` registers the `tigerslicer` URL scheme.

**Translatable strings (`a06dcde36a`).** 98 of the 117 Orca msgids changed, in the C++
source, `OrcaSlicer.pot` and all 23 `.po` files together (so translations keep matching);
all catalogs pass `tools/msgfmt.exe --check-format`. Rules (applied only inside gettext
literals / msgid / msgstr): `Orca Slicer` → `TigerSlicer`; word `OrcaSlicer` (not part of
an identifier, path or URL) → `TigerSlicer`; standalone `Orca` → `TigerSlicer` except
`Orca Cloud/Arena/Cube/Badge/Tolerance/String/Plug`. Kept as Orca: cloud/login strings
(now unreachable), strings about a file's origin ("generated by an old OrcaSlicer
version", "created with an OrcaSlicer 2.3.1-alpha", "painted using a newer version of
OrcaSlicer"), and the About history paragraph. Translator transliterations were fixed
(zh_CN 逆戟鲸, ja オルカ, ko 오르카, th ออร์ก้า, tr "Orca Dilimleyici"); new/changed
translations are marked `# AI Translated`. hints.ini tips (8) updated in sync.

**Hand edits (`1746d8cba3`).**
- About dialog: "TigerSlicer is based on OrcaSlicer by SoftFever and contributors, which
  builds on Bambu Studio and PrusaSlicer." Copyright "TigerSlicer" plus "Portions
  Copyright (C) 2026 OrcaSlicer Pte Ltd" (AGPL attribution kept). Link →
  github.com/tiggy0808/TigerSlicer.
- `src/OrcaSlicer.cpp`: CLI errors and the DLL-injection warning say TigerSlicer; `--help`
  usage says TigerSlicer on Windows, `orca-slicer` elsewhere (`#ifdef _WIN32`).
- Window titles (setup wizard, plugin download, Python plugin dialogs) use
  `SLIC3R_APP_FULL_NAME`. Export filter "TigerSlicer Preset Bundle (*.orca_bundle)"
  (extension kept). Printer-agent dropdown shows "TigerSlicer" (id stays `orca`).
  Moonraker client name TigerSlicer. Troubleshoot zips `TigerSlicer_Logs_*` /
  `TigerSlicer_PackedDebugInfo_*`; commit/new-issue links → tiggy0808/TigerSlicer.
  Network test checks `https://github.com`. Calibration names "TigerSlicer YOLO Flow
  Calibration", `TigerSlicer-PA-Calibration-Test`.
- Handy models carry OrcaSlicer branding in their geometry, so their menu items got
  neutral names: Calibration Cube, Whale Combo, Badge, Tolerance Test, String Hell
  (file names unchanged).

**Deliberately still Orca (backward compatibility).** 3MF tags (`OrcaSlicer` metadata,
`OrcaSlicer-Version`), G-code producer matchers, `OrcaFilamentLibrary` / Orca Arena
vendor and preset names, `.orca_*` extensions, `orca_*` config keys, printer-agent id
`orca`, Python plugin API (`orca` module, `window.orca`), registered OAuth client ids
(Obico, SimplyPrint), image/model/calibration file names, CMake `ORCA_*` variables,
comments, log messages, orcaslicer.com wiki links (no TigerSlicer docs exist).

### 3. Pink instead of green

Palette: main `#F6A5C1`, hover/pressed `#D98BA8`, dark-mode `#9B5874`; new tints
`#F2D3DE` (selection, dark partner `#3C2233`) and `#FBEAF0` (focus, dark `#3D2D35`),
both added to `Widgets/StateColor.cpp` `gDarkColors` (old teal keys removed).

- **3D scene (`db2ca258c8`).** `src/libslic3r/Color.hpp` `ColorRGB::ORCA()` /
  `ColorRGBA::ORCA()` (0,150,136) → (246,165,193) — feeds `COL_ORCA` (~45 uses: gizmo
  borders, measure text, sliders, plate list), the selection rectangle and ImGui header/
  slider colours. Plate slicing progress (`GLCanvas3D.cpp`), layer slider `BRAND_COLOR`
  and tick/label tints (`IMSlider.cpp`), scale/mirror plane (`Selection.cpp`), resin pad
  `SLA_PAD_COLOR` → #5C3A47 (`3DScene.cpp`), toolpath **Skirt** role (0,135,110) → pink
  (`libvgcode/src/ViewerImpl.cpp`), G-code viewer grid/hover/links, measure hover, cut
  line, mesh-boolean box/buttons, painter tool buttons (supports, fuzzy skin, MMU, seam),
  text/emboss pressed toggles, skip-part "kept" colour.
- **Dialogs/widgets/icons (`cdf2eeea1e`).** ImGui hover/dark constants, confirm/combo
  borders, text selection (pink at 0.5 alpha for legibility), notifications, hints,
  slicing progress, daily tips; wx selection/focus tints (object list, dropdown, combobox,
  tab buttons, search, printer panel, capsule buttons, purge mode, multi-nozzle sync),
  `StepCtrl`, `ProgressBar`, HTML links, thermal-preconditioning OK button; icons
  `canvas_menu_hover`/`canvas_zoom_hover` (+dark), `pipe_of_*`, `dev_ams_upgrading`;
  flatpak metainfo brand colours.
- **New parts start pink (`b32763ca49`, `d3166a8e67`).** The green part colour was the
  default filament colour `#26A69A` (`PresetBundle.cpp` `update_selections` /
  `load_selections`), now `AppConfig::DEFAULT_FILAMENT_COLOR = "#F6A5C1"`. One-time
  migration in `AppConfig::load()` (`migrate_default_filament_colors`, JSON config,
  editor mode, guarded by app key `default_filament_color_migrated`): for every printer
  in `orca_presets`, single entries of `filament_colors` / `filament_multi_colors` equal
  to `#26A69A` (case-insensitive) become `#F6A5C1`; multi-colour packs that merely contain
  the teal are kept. Fresh installs set the marker immediately (`GUI_App::init_app_config`)
  so a teal chosen in the first session isn't rewritten. 3MF colours are never touched.
  SLA `material_colour` default `#29B2B2` → `#F6A5C1`. Tests added to
  `tests/libslic3r/test_appconfig.cpp` (`[AppConfig]`).
- **Selected parts stay pink (`a63356fce9`).** `GLVolume::brighten_color()`
  (`src/slic3r/GUI/3DScene.cpp`) highlights a selected object by adding 0.25 to its HSL
  lightness. The old teal (L ≈0.40) stayed teal, but the pink default (L ≈0.81) hit 1.0
  and every selected part rendered pure white. Now `l = max(l, min(l + 0.25, 0.9))`:
  dark/mid colours (L ≤ 0.65) are unchanged, light colours keep their hue. Measured on
  Windows: selected pink cube (209,209,209) before → (205,171,183) after.
- **Bug fixes (`b995ef02c3`).** Filament swatches were stuck pink because
  `BitmapCache.cpp` still searched for teal `0xFF889600` (nanosvg ABGR) after
  `outlined_rect.svg` became pink → now `0xFFC1A5F6`. Some icons (completed_2, leaf,
  shield, placeholder_excel) turned red in dark mode because the earlier sweep rewrote a
  placeholder key `"#00FF00"` to `"#D98BA8"` → restored to `"#00FF00"`.
- **Web views (`917783b1ce`).** `include/global.css` dark main/hover → #9B5874, focus
  tints; homepage `dark.css`/`home.css`; guide pages; dialog `theme.css`, PluginsDialog
  styles (badges, update button, run icon, focus rings), SpeedDial.
- **Left green on purpose:** support enforcer (green) vs blocker (red), scale grabbers,
  Y axis, heat maps, other toolpath role colours, status-OK greens, palettes of distinct
  colours, real filament colour data (e.g. Bambu Green), PNG images.

### 4. New startup screen (`d47de5f80a`)

- 560×340 DIP borderless card, always dark `#0E0E10` (independent of light/dark mode).
- Artwork `resources/images/splash_tigerslicer.svg`, generated by the new `splash_svg()`
  in `resources/icons/branding/generate_tigerslicer_branding.py` (helpers `mix()`,
  `lockup_group()`, constants `SPLASH_*`): soft pink halo (gradient pre-mixed onto the
  background — nanosvg left a ring with transparency), the mark at 120 DIP, "tiger" pink /
  "slicer" `#c9c9ce` wordmark, centred lockup. Colours avoid `BitmapCache::load_svg`'s
  swap list.
- Text drawn at runtime: "Version x.y.z" (`_L("Version")` + `format_display_version()`,
  `Label::Body_13`, `#8A8A90`), status line (`Label::Body_12`, `#B4B4BA`, ellipsized),
  4 DIP rounded progress bar (track `#2A2A2F`, fill `#F6A5C1`).
- `GUI_App.cpp` `SplashScreen`: `wxAutoBufferedPaintDC` + `wxGCDC` (double-buffered,
  anti-aliased on Windows), SVG loaded per window size with a null check, fixed colours.
  Kept: `FilterEvent` no-op, `wxWeakRef`/`Destroy`, macOS `wxSTAY_ON_TOP` + `wxYield()` in
  `SetText`, Wayland titlebar removal, never-backwards progress, `show_splash_screen`.
- macOS: buffered DC is a plain `wxPaintDC` there and `wxGCDC` uses CoreGraphics;
  `load_svg` already renders at the Retina scale. Old `splash_logo*.svg` are still
  generated but unused.

### 5. Version

`version.inc` `SoftFever_VERSION` 0.0.1 (shown in About, splash, title, saved files).
`SLIC3R_VERSION` stays `02.00.00.00` (Bambu compatibility).

### macOS porting checklist

Done for macOS in this release (shared code): cloud removal (incl. the macOS File menu),
string/translation rename, `TigerSlicer.mo`, pink colours, splash, `tigerslicer://` scheme
in `cmake/modules/MacOSXBundleInfo.plist.in`.

Done for macOS after the Windows release (`2f193c845`, build(macos)):
- The bundle is `TigerSlicer.app/Contents/MacOS/TigerSlicer`: `OUTPUT_NAME "TigerSlicer"`
  on Apple in `src/CMakeLists.txt` (CMake target still `OrcaSlicer`), with the build-tree
  paths, `MACOSX_BUNDLE_BUNDLE_NAME "TigerSlicer"` (menu-bar app name) and the copyright
  string following.
- `CFBundleIdentifier` is `com.tigerslicer.TigerSlicer` (`cmake/modules/MacOSXBundleInfo.plist.in`
  and `CMAKE_XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER`), so macOS no longer confuses
  TigerSlicer with a real OrcaSlicer install.
- `build_release_macos.sh` copies, relocates and verifies `TigerSlicer.app` (otool checks
  on `MacOS/TigerSlicer`, universal step too). The `build/<arch>/OrcaSlicer/` staging
  folder keeps its name.
- `build_orca.yml` signs and packages `TigerSlicer.app` into a dmg whose volume is named
  `TigerSlicer`, and `TroubleshootDialog` detects `/Volumes/TigerSlicer` to match.
  The dmg and artifact *file* names are still `OrcaSlicer_Mac_universal_*`.
- `Mouse3DHandlerMac.mm` registers `"\013TigerSlicer"`.
- Profile validator: name, bundle id and copyright say TigerSlicer; its target and
  bundle are still named `OrcaSlicer_profile_validator`.
- Rounded-square Mac app icon (`2f9adb838`): `macos_app_icon()` in
  `generate_tigerslicer_branding.py` draws the pink mark on Apple's icon grid (824 px
  superellipse body on a 1024 px canvas, near-black gradient, soft shadow). It feeds only
  `resources/Icon.icns`, the Dock icon `images/OrcaSlicer-mac_256px.ico`, and the
  `*-mac_128px.png` / branding `*.icns` files; every other icon keeps the round mark.

Still Orca on macOS: the unused `src/dev-utils/platform/osx/Info.plist.in`, the staging
folder and CI dmg/artifact names above, and the profile validator's file name.

#### Building the Mac app

On Apple Silicon with Homebrew and only the Command Line Tools (no full Xcode):

```bash
brew install cmake ninja gettext nasm autoconf automake libtool texinfo pkg-config
export PATH="$(brew --prefix libtool)/libexec/gnubin:$(brew --prefix texinfo)/bin:$PATH"
export CMAKE_POLICY_VERSION_MINIMUM=3.5   # CMake 4 rejects some dependencies' old minimums
./build_release_macos.sh -a arm64 -x      # first run builds the dependencies too; -s = app only
```

`-x` uses Ninja (the default Xcode generator needs full Xcode). The app lands in
`build/arm64/OrcaSlicer/TigerSlicer.app`. To package the release dmg (ad-hoc signed, as
nothing is notarized yet):

```bash
mkdir -p dmg && ditto build/arm64/OrcaSlicer/TigerSlicer.app dmg/TigerSlicer.app
codesign --force --deep --sign - dmg/TigerSlicer.app
ln -s /Applications dmg/Applications
hdiutil create -volname TigerSlicer -srcfolder dmg -format UDZO TigerSlicer-0.0.1-macos-arm64.dmg
```

Keep the volume name `TigerSlicer` (TroubleshootDialog checks for it).

Also on Linux: binary `orca-slicer`, `com.orcaslicer.OrcaSlicer*.desktop` file names,
flatpak metainfo text, DBus names still Orca.

### Build note for existing checkouts

Earlier builds generated `resources/i18n/<lang>/OrcaSlicer.mo` (git-ignored). The build
now generates `TigerSlicer.mo`, and the install step copies the whole `resources/` tree,
so delete the stale `OrcaSlicer.mo` files (and any old build install folder) before
packaging, or both names end up in the app. A fresh clone is unaffected.

### Verification (Windows, this release)

- Full Release build (MSVC 17.14) with no errors; outputs `TigerSlicer.exe`,
  `TigerSlicer.dll`, 23 × `TigerSlicer.mo`; exe file version 0.0.1.
- Launched the build with throwaway data dirs (`--datadir`) and captured the windows:
  new splash shows "Version 0.0.1" with the progress bar; first-run wizard shows
  "Welcome to TigerSlicer"; home page has no cloud sign-in panel and the left "Recent"
  nav still shows; UI accents pink.
- Translations load: with `language = de_DE` the window title is "Unbenannt -
  TigerSlicer" and the tabs read Datei / Vorbereiten / Vorschau / Gerät / Drucker.
- Filament colour migration on a copy of a real config: 8 × `#26A69A` → `#F6A5C1`,
  marker `default_filament_color_migrated = "1"`, clean exit.
- 3D view: a loaded cube shows the pink filament slot; selected part renders light pink.
- Not yet run: the Linux build, the new `[AppConfig]` Catch2 tests (the Windows
  release script doesn't build tests), Bambu LAN printing with the network plugin.

### Verification (macOS, this release)

- `build_release_macos.sh -a arm64 -x -s` on macOS 15.6 (Apple clang 17, Command Line
  Tools only) with no errors; outputs `TigerSlicer.app` with `CFBundleExecutable`
  `TigerSlicer`, `CFBundleIdentifier` `com.tigerslicer.TigerSlicer`,
  `CFBundleShortVersionString` 0.0.1, 23 × `TigerSlicer.mo` and no `OrcaSlicer.mo`.
- Installed from `TigerSlicer-0.0.1-macos-arm64.dmg` and launched with a throwaway
  `--datadir`: the log reports version 0.0.1 build `b5e74f81c`, the new config records
  0.0.1, no fatal errors.
- Known harmless log noise on a fresh data dir (all platforms): "Failed to install bundled
  TigerSlicer Resin profile" — `PresetBundle::setup_directories()` copies the resin ini/idx
  into `data_dir/vendor`, which nothing creates or reads; the resin bundle is loaded from
  the app's own `resources/profiles` instead.
