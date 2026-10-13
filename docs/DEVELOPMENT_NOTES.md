# TigerSlicer development notes

Technical notes for each release: which commits changed what, in enough detail to reproduce or port a
change (in particular from the Windows build to the macOS one). For a short list of what changed for
users, see [CHANGELOG.md](../CHANGELOG.md). TigerSlicer is based on OrcaSlicer; "upstream" below means
OrcaSlicer.

---

## 0.0.6 (2026-10-13)

Theme: the filament library needs no installing, so its UI goes.

Commits: `63555a3ba` feat(filaments): list materials directly and drop filament installing;
`147a7dd9b` chore: release 0.0.6.

- `PlaterPresetComboBox::update()`: system filaments get no group, so they are listed directly
  instead of under their `filament_vendor` ("Generic") submenu. User, bundle and project presets keep
  their grouping options.
- The "Add/Remove filament" entry (`LABEL_ITEM_WIZARD_FILAMENTS`) is no longer added to the filament
  combo. The sidebar's "Set filaments to use" button beside "Project Filaments" is created but hidden
  (the title bar's click-to-collapse area falls back to the full width when it is hidden).
- Setup wizard: `guide/21/21.js` `GotoFinishPage()` goes from the printer page to the finish page
  (`guide/5`); the filament page (`guide/22`) is no longer reached. Finishing without it is fine:
  `SaveProfile()` keeps the previous filament selection and every library filament is visible anyway.
- Verified: build, and a background launch on a fresh data folder (no errors). The changed controls
  could not be inspected from the build machine (no screenshots; accessibility scripting sees no
  windows), so the visible result is checked by hand.

### Verification (Windows)
- Clean release build of `abe278153` (MSVC 17.14, `build_release_vs2022.bat slicer`, old
  `build\OrcaSlicer` deleted first): no errors, `TigerSlicer.exe` reports 0.0.6. The package's
  `resources\profiles` has no filament JSON outside `OrcaFilamentLibrary`, whose `filament` folder
  holds the 51 `<material> @System.json` files and `base`. The Setup.exe test install matches the
  build output (9,413 files).
- `libslic3r_tests`: 1,028 test cases, 88,317 assertions, all pass (`[filament_library]`: 4 cases,
  228 assertions).
- In the app with an empty `--datadir`:
  - Wizard with only Klipper: Next on the printer page goes straight to the last page; Finish opens
    the app on Prepare, no Home tab.
  - Filament list: the 51 materials directly under "System presets", no "Generic" submenu, brand
    names or "Add/Remove filament"; PLA selected. The header has only "Add one filament".
  - Select/Remove printers → Elegoo Mars 3: Resin "Generic Standard Resin", Process
    "0.05 mm MSlicer Standard".
  - 20 mm cube, slice (400 layers, 8.02 ml), "Export resin file" proposes `cube20.ctb`: 2,107,421
    bytes, starts `07 01 FD 12`.
  - Cube on the plate, Mars 3 → Klipper → Mars 3: no crash, Process "0.05 mm MSlicer Standard".
  - Klipper: PLA edited and saved as "Test PLA" (inherits `PLA @System`, empty
    `compatible_printers`); after adding a Creality Ender-3 V3 SE, "Test PLA" is in its list.
  - Quit without saving the project: normal exit, no crash log or dump.
- A copy of a real data folder last used with 0.0.3: bundles re-installed (library 02.04.00.05), the
  four user filaments load (parents `Generic PLA/PETG/PLA High Speed @System` and `Bambu PAHT-CF
  @System` resolve to PLA, PETG and PA6-CF), normal exit. No new kinds of log errors.
- Seen, not fixed:
  - The system filament list is not in the table order: PLA, PETG, ABS, TPU 85A/90A/95A come first,
    then the rest alphabetically. `PlaterPresetComboBox::update()` looks up full preset names
    ("PLA @System") in `filament_orders`, which holds aliases, so the table order never applies.
  - Older than 0.0.4: every launch logs "Failed to install bundled TigerSlicer Resin profile" twice
    (`setup_directories()` copies into `<data>/vendor`, which a new data folder lacks); a user
    filament limited to `Prusa CORE One L 0.6 nozzle` is not listed for a user printer inheriting it
    (0.0.3 does the same).

## 0.0.5 (2026-10-13)

Theme: filaments by material. Developed and verified on macOS; nothing here is platform-specific.

Commits, oldest first (all on `main`):

| Commit | Subject |
|---|---|
| `469fa3208` | feat(filaments): one filament profile per material, for every printer |
| `457965b8b` | feat(filaments): offer every material and make new filament profiles work with every printer |
| `b84fa2fb0` | test(filaments): the material library, old-name mapping and all-printer profiles |
| `3c0456f22` | chore: release 0.0.5 |

### The filament library
- `scripts/tigerslicer_filaments.py` holds the material table (base template, filament_id, nozzle,
  bed and chamber temperatures, fan, max volumetric speed, density, softening temperature, flow ratio,
  cost) and writes `resources/profiles/OrcaFilamentLibrary/filament/<material> @System.json`, listed
  in `OrcaFilamentLibrary.json` after the untouched `filament/base/fdm_filament_*` templates. The
  bundle keeps its name because the code treats `ORCA_FILAMENT_LIBRARY` as the printer-independent
  library (always installed, loaded before the vendors). 51 profiles: 15 materials with -CF and -GF
  versions (+10 °C nozzle, ≤40 % fan, 80 % volumetric speed, `required_nozzle_HRC` 40), plus TPU
  95A/90A/85A, PVA, BVOH and HIPS. A name like "PLA @System" shows as "PLA" (the alias drops "@…").
- filament_ids keep the old generic ids where the material matches (OGFL99 PLA, OGFG99 PETG, OGFN98
  PA6-CF…); new materials use TSF* ids. `MoonrakerPrinterAgent::map_filament_type_to_generic_id()`
  (also used by the Snapmaker agent) maps every material and the usual other spellings to them.
- The one-time migration (`--migrate`) deleted every vendor's `filament/` folder (7,662 files across
  140 vendors and the library's brand folders) and emptied each vendor's `filament_list`, rewrote
  `default_filament_profile` in machine presets and `default_materials` in machine models to the new
  names, and bumped every vendor bundle's version so installed copies in the data folder are
  replaced. Each new profile's `renamed_from` lists the 6,477 removed names of its material (by
  resolved `filament_type`, fibre from the type or name), so `find_preset()` resolves old names in
  user presets' `inherits`, projects and printer defaults.

### Code
- `PresetBundle::load_installed_filaments`: every library profile is visible, whatever the
  `[filaments]` section holds.
- `PresetCollection::save_current_preset`: a new filament preset gets empty `compatible_printers` and
  `compatible_printers_condition` (all printers). `clone_filament_for_all_printers()` does the same for
  the Create filament dialog, which now makes one profile named "<vendor> <type> <name>" from the first
  ticked printer's profile instead of one "@printer" copy per ticked printer.
- `ORCA_DEFAULT_FILAMENT` is "PLA @System"; `find_preset2()`'s fallback for "<anything> Generic X" now
  tries "X @System"; unknown AMS material and the object table default to "PLA"; the filament combo
  sorts the library in the table's order.

### Verification (macOS)
- `libslic3r_tests`: 1,028 cases pass, including new `[filament_library]` cases: 51 profiles, all
  compatible with every printer, shown by material, fibre versions need a hardened nozzle; all 51
  visible with an empty `[filaments]` section; old names (Bambu PLA Basic @BBL X1C, Generic PETG
  @System, Bambu PPA-CF @System…) resolve to their material; "Save as" and Create filament make
  all-printer profiles.
- The app on a copy of a real 0.0.4 data folder: vendor bundles re-installed from resources, the four
  user filaments (two on removed parents) load, no new errors or crashes. A fresh Klipper setup picks
  "PLA".
- Seen, not caused by this change (0.0.4 does the same): with that copied data folder, a Quit Apple
  Event is answered "User canceled" and the app stays open.

## 0.0.4 (2026-10-12)

Theme: the macOS build of the 0.0.3 resin fixes, and no Home tab.

Commits, oldest first (all on `main`):

| Commit | Subject |
|---|---|
| `aeed37d44` | fix(resin): two more crashes when switching to a resin printer |
| `1c9dd684b` | feat(ui): remove the Home tab |
| `ba0694f5a` | chore: release 0.0.4 |

### A leftover settings page could still crash on a resin switch
The first macOS build of 0.0.3 (`6deebe1d6`) crashed once: first-run wizard with Custom → Klipper,
then "Select/Remove printers (system presets)" → Elegoo Mars 3 → Confirm. SIGSEGV at 0x0 in
`__dynamic_cast` ← `ConfigOptionsGroup::get_config_value` ← `reload_config` ← `TabPrinter::reload_config`
← `Tab::load_current_preset` ← `GUI_App::load_current_presets` ← `GUI_App::run_wizard`. The return
address (`get_config_value+3188`) is right after the call to `DynamicConfig::opt_float()`: an option
group asked the new (resin) printer config for a float option that config does not have, so the
printer tab still had an active page from the FFF page set. Two later attempts, including the same
wizard-first order, did not reproduce it, so the leftover page depends on event timing.
- `ConfigOptionsGroup::reload_config()` skips, and logs at error level, any option missing from the
  group's config (`"<group>" has option <key> that this config doesn't have, skipped`). Every missing
  option crashed in `get_config_value()` before, except `bed_type`, `first_layer_sequence_choice`,
  `other_layers_sequence_choice` and `curr_bed_type`, which it fills in on purpose and which are still
  passed through. The guard is shared code, so it also covers Windows.

### Switching to a resin printer with a model on the plate
- `GLCanvas3D::reload_scene()` ran the filament checks (TPU, filament printable, PLA/PETG mixing)
  when `current_printer_technology()` was FFF. On a printer-combo switch from an FFF printer to a
  resin one, `Tab::load_current_preset()` calls `ObjectList::update_objects_list_filament_column()`,
  which redraws the scene while the background process still reports FFF but `full_config()` is
  already the resin config: `PartPlate::check_mixture_of_pla_and_petg()` read `filament_type` from it
  and crashed (SIGSEGV at 0x8). The checks now go by the edited printer preset's technology, the same
  source `full_config()` uses. Only reachable with an object on the plate, so 0.0.3's Windows check,
  which switched printers before loading the cube, did not hit it.

### No Home tab
- `MainFrame`: the `WebViewPanel` Home page is no longer created or added (`m_webview` stays null);
  `EVT_LOAD_URL` opens the URL in the default browser instead. Every `m_webview->` call is null-checked.
- Startup, "Delete all" (`Plater::reset_with_confirm`) and Ctrl+1 inside a settings field select
  Prepare instead of Home. The tab bar handles a missing Home page already: Prepare and Preview are
  inserted at position 0 when `FindPageByName(TAB_ID_HOME)` fails.
- Preferences: the "Default page" (Home/Prepare) choice is removed; `default_page` in the config is
  ignored.
- The File menu keeps New/Open/Save project and "Recent files".

### Verification (macOS)
- Release build on macOS 15 (Apple Silicon), version 0.0.4.
- A temporary self-test (an environment-variable hook in `GUI_App::post_init`, removed before commit)
  drove the app through the code the buttons use, started in the background with a throwaway
  `--datadir` already on Generic Klipper Printer:
  - No Home tab; the app opens on Prepare.
  - Elegoo Mars 3 added through `GuideFrame::apply_config()` + `GUI_App::load_current_presets()`, as
    "Select/Remove printers" → Confirm does: Generic Standard Resin, process
    `0.05 mm MSlicer Standard`, and the Process box shows it.
  - 20 mm cube, export: proposed `cube20.ctb`, 2,107,421 bytes, starts `07 01 FD 12` (CTB v5).
  - Printer combo Mars 3 → Klipper → Mars 3 (`Tab::select_preset` as the combo calls it): correct
    presets and Process box each time. This step found the crash above; it passes with the fix.
  - Normal Quit (Apple Event) after the whole run: no crash report.
- Not reproduced: two crashes on quit (`ParamsDialog::~ParamsDialog` → `wxWindowDisabler` →
  `EndModalSession`) from test instances that were still running while their app bundle was deleted
  and rebuilt. Unchanged upstream code; watch for it.
- Not run: the guard in `ConfigOptionsGroup::reload_config()` never fired in these runs, because the
  original leftover-page crash did not happen again.

## 0.0.3 (2026-10-08)

Theme: resin printing on Windows. Fixes for the crashes the first Windows build of 0.0.2 hit when a
resin printer was selected at run time or a resin file was exported.

Commits, oldest first (all on `main`):

| Commit | Subject |
|---|---|
| `6104cfd89` | fix(resin): picking a resin printer or exporting no longer crashes |
| `5a4133d2e` | docs: development notes for the resin switch and export fixes |
| `d93371f2f` | chore: release 0.0.3 |

### Switching to a resin printer at run time
The first Windows build crashed as soon as a resin printer was selected, and again on export. The
macOS check had preselected the printer with a startup hook, so no run-time technology switch had
run. Upstream never switches technology at run time either, so these paths were dead code there.
All of it is shared code; the macOS app picks it up on its next build.
- `Tab::load_current_preset`: the PrusaSlicer block that inserts and removes preset tabs in
  `tab_panel()` on a technology change is disabled (`if (false)`), as is the `SetPageImage` call after
  it. In Orca `tab_panel()` is the Home/Prepare/Preview notebook and the preset tabs live in
  `ParamsPanel`, so `FindPage()` returned `wxNOT_FOUND` and `GetPage()` threw `std::out_of_range`
  ("invalid vector subscript"); the app then quit and crashed again tearing down the half-switched
  tabs. `ParamsPanel::switch_printer_technology()` already shows the right tabs.
- `TabPrinter::update_pages` clears `m_active_page` when it swaps the FFF and SLA page sets: a hidden
  printer tab kept a page of the old set and `reload_config()` read options the new config lacks.
- `ParamsPanel::switch_printer_technology` hands the page area over like
  `Tab::tree_sel_change_delayed()` (`clear_pages()` and `unselect_tree_item()` on the outgoing tab).
  Otherwise the FFF print tab kept an active page whose controls were destroyed, and going back from
  a resin printer to an FFF one crashed in `ConfigManipulation::toggle_field()`. If the outgoing tab
  was a plate/object/part/layer settings tab (FFF only), the new technology's print tab is shown.
- Printer combo (`Tab::select_preset`): the SLA rows of the `updates` table are restored, so a resin
  printer picked in the combo reloads its print and resin tabs (the process combo stayed empty).
  `MainFrame::technology_changed()` returns when there is no `wxMenuBar` (only macOS has one) and
  `Sidebar::auto_calc_flushing_volumes()` returns for non-FFF printers (it read filament keys that the
  resin full config does not have); both crashed on a combo switch.
- Export: `MSlicerResinPrint::output_filename()` read `output_filename_format` (the PrusaSlicer name;
  Orca's option is `filename_format`) and crashed. It reads `filename_format` now. A template's own
  extension (`.sl1` from the SLA print default `[input_filename_base].sl1`, `.gcode`, or an MSlicer
  format) is replaced by the archive's; anything else after a dot is part of the name, so a project
  "Bracket v1.2" exports as `Bracket v1.2.ctb`. Tests in `test_mslicer_resin_job.cpp`.

### Verification (Windows)
- Release build (MSVC 17.14) with no errors. `libslic3r_tests` built on Windows for the first time
  (`cmake -DBUILD_TESTS=ON`, target `libslic3r_tests`): 1,024 test cases, 88,087 assertions, all pass.
- In the app with a throwaway `--datadir` (build before the printer-combo fixes): first run with
  Generic Klipper Printer, then "Select/Remove printers (system presets)" → Elegoo Mars 3; 20 mm cube;
  slice (400 layers, 8.02 ml resin); "Export resin file" proposes `cube20.ctb` and writes a
  2,107,421-byte CTB v5 file (`0x12FD0107`).
- Final build, printer combo: Mars 3 → Klipper → Mars 3 (process combo shows `0.05 mm MSlicer
  Standard`), slice and export again (same file size and magic), back to Klipper, then a normal
  close: no crash logs.
- Not re-run after the fixes: the first-run wizard with only a resin printer selected (it applies
  through the same `load_current_presets()` as "Select/Remove printers"), and macOS.

## 0.0.2 (2026-10-08)

Theme: make resin printing reachable and usable. Developed and verified on macOS. The first Windows
build found crashes on paths the macOS check had not run; they are fixed in 0.0.3.

Commits, oldest first (all on `main`):

| Commit | Subject |
|---|---|
| `0cd41bccb` | fix(resin): bundled resin printers use their real bed and real profiles |
| `1b6ba0e2a` | feat(resin): list resin printers in the setup wizard |
| `7265c6026` | feat(resin): resin settings, hollowing and drain holes, layer slider and SDCP upload |
| `93bfd87a2` | chore: release 0.0.2 |

### Resin printers are selectable
- `PresetBundle::TIGERSLICER_RESIN_VENDOR` (`"TigerSlicerResin"`) names the ini-based resin vendor.
  `apply_vendor_config` skips it when installing vendor `.json` bundles (there is none, so selecting a
  resin printer used to fail the whole wizard apply) and in the `@System` filament replacement logic.
- Setup wizard: `GuideFrame::LoadResinProfileFamily()` (`WebGuideDialog.cpp`) adds the vendor's SLA
  models to the printer list with variant `default` (the resin presets' `printer_variant`), technology
  `SLA` and the `TigerSlicerResin/<model>_thumbnail.png` cover. `guide/21/common.js` shows them as
  "Resin printers (MSLA)" right after Custom Printer, labels the card "Type: Resin (MSLA)" instead of a
  nozzle size, and `21.js` skips the filament page when only resin printers are selected.
- `resources/profiles/TigerSlicerResin.ini` (config 1.3.2):
  - `bed_shape`/`max_print_height` (PrusaSlicer names, silently dropped by Orca's `handle_legacy`) are
    now `printable_area`/`printable_height`, so every printer gets its real build area and height.
  - Model ids are the display names (`[printer_model:Elegoo Mars 3]`, `printer_model = Elegoo Mars 3`),
    as in Orca's JSON vendors: the printer combo shows and resolves system printers by `printer_model`.
    Thumbnails were renamed to match.
- `PresetBundle::update_compatible` (SLA branch) replaces a selected `- default -` print/material with
  the printer's `default_sla_print_profile`/`default_sla_material_profile` (or the first compatible
  one): the placeholders are compatible with everything, so they were never replaced, and the default
  print enables `pad_enable`, which the engine refuses. `export_selections`/`load_selections` save and
  restore the resin print and material per printer (`sla_print`, `sla_materials` printer settings).

### Crashes with a resin printer selected
- `BackgroundSlicingProcess` never received the resin print: `set_resin_print(&resin_print)` only
  existed inside a commented-out block in `Plater::priv::priv`. It is registered (with a status
  callback) before the first plate selects its technology.
- `PartPlate::update_slice_context` selected the plate's construction-time technology (always FFF);
  it now uses the selected printer preset's technology, so resin plates slice with the resin print.
- `BackgroundSlicingProcess::apply` read `wxGetApp().mainframe->m_plater` while the main frame is
  still being constructed (resin's first apply reports a change, FFF's does not).
- `PartPlateList::set_default_wipe_tower_pos_for_plate` returns early for non-FFF printers (the SLA
  full config has no wipe tower options).
- `GLCanvas3D::reload_scene` runs the filament checks (TPU, PLA/PETG mixing, nozzle compatibility)
  only for FFF and clears their warnings otherwise.
- `Plater::record_slice_preset` (called by `reslice()`, i.e. Slice plate and switching to Preview)
  returns early for non-FFF printers; it dereferenced the missing filament presets.

### Resin settings UI
- `MainFrame::create_preset_tabs` creates `TabSLAPrint` (sidebar params panel) and `TabSLAMaterial`
  (params dialog, title "Resin Settings"). `ParamsPanel::refresh_tabs` hides tabs for the other
  technology (they are never laid out, so they drew over the panel: the "appearance bug" behind BBS's
  workaround), and `ParamsPanel::switch_printer_technology()` rebuilds both panels from
  `Plater::set_printer_technology`. On macOS `create_layout` detaches `m_page_view` before re-adding it.
- Pages: `TabPrinter::build_sla` (printable space, display, output file, corrections),
  `TabSLAMaterial::build` (resin info, exposure/PWM/wait, normal and bottom lift/retract, corrections),
  `TabSLAPrint::build` (layers, anti-aliasing, hollowing; hollowing fields follow `hollowing_enable`).
- `PrintConfig.cpp`: labels, tooltips, units and limits restored for the legacy SLA options these pages
  show (BBS had commented them out).
- Sidebar: a Resin row (`combo_sla_material` + edit button) in the printer section replaces the
  filament section for resin printers; the nozzle diameter box is hidden. `Sidebar::update_presets`
  refreshes the resin combos.
- The export button and its dropdown read "Export resin file" for resin printers.

### Supports workspace, preview, network upload
- `SupportsView`: Hollowing group and drain holes (place/remove by clicking, diameter and depth spin
  boxes; default 4 mm x 6 mm as in PrusaSlicer's hollowing gizmo). Support presets and "Save print
  settings profile" work now that the SLA print tab exists. Stale "slicing refuses" texts fixed.
- `GLCanvas3D::_render_msla_manual_points()` (ported from the PrusaSlicer-based TigerSlicer) draws
  support points (pink spheres) and drain holes (purple cylinders) in the Supports workspace.
- `ResinRasterPreviewPanel` has a layer slider (`wxSlider`, arrow keys work); Orca's ImGui layer
  slider only exists for FFF previews.
- `PrintHost::get_print_host` returns `MSlicerSDCPHost` when `resin_host_type` is ChiTu SDCP.
  `resin_host_type` is in the FFF printer, SLA printer and physical printer option lists (the dialog
  builds its fields once), the SLA printer options gained the print host keys, and
  `PhysicalPrinterDialog` shows "Resin Host Type" for resin printers, hiding credential fields for SDCP.

### Verification (macOS)
- Launched with a resin printer selected and with a model loaded: no crash, correct printer name,
  bed and resin profiles, resin pages in the sidebar, "Export resin file" button.
- Setup wizard: the generated printer list holds all 67 resin models with existing thumbnails; the
  printer page renders them under "Resin printers (MSLA)" and a resin-only choice skips the filament page.
- New test `[Preset][Bundle][resin]` in `test_preset_bundle_loading.cpp`: the bundled Elegoo Mars 3 loads
  with its build area and height, is listed by name, and selecting it picks the real resin profiles.
  `libslic3r_tests` passes in full (1,022 cases, 88,078 assertions), including all `[resin]` cases.
- End to end in the app (with a temporary startup hook, removed before commit): Elegoo Mars 3 + a
  20 mm cube sliced and exported a 2.1 MB CTB v5 file (`0x12FD0107` magic); Preview showed the raster
  panel at layer 201 of 400 with 8.02 ml resin and the layer slider.

### Known gaps
- The 3D view in Preview is empty for resin plates (Orca removed the SLA preview scene); the raster
  panel beside it is the resin preview.
- The Physical Printer tab does not discover resin printers: the engine only probes an address that
  is entered explicitly (no broadcast). Enter the printer's IP in the printer connection dialog.

## 0.0.1 (2026-10-06)

Theme: make it TigerSlicer, not an OrcaSlicer fork. Cloud features removed,
Orca branding replaced, the remaining green/teal turned pink, new startup screen.

Commits, oldest first (all on `main`):

| Commit | Subject |
|---|---|
| `509f32dfc2` | fix(cloud): our own keychain entry, and no silent sign-in on startup |
| `340f138315` | feat(cloud): sign-in, preset sync and the startup phone-home are gone |
| `e75058f9ed` | feat(splash): a black-and-pink startup card |
| `e5645119a6` | fix(i18n): compile catalogs as TigerSlicer.mo so translations load |
| `e4fa398bcf` | feat(brand): ship TigerSlicer.exe and stop claiming OrcaSlicer's system names |
| `e63c3d1feb` | feat(brand): say TigerSlicer in every translatable string |
| `9ec5f32b24` | feat(brand): rename the untranslated places users still saw Orca |
| `1614e4fdd0` | fix(prefs): stealth mode says what it still does |
| `fd0627978d` | feat(theme): the 3D scene and its gizmos turn pink |
| `0025939531` | feat(theme): dialogs, overlays and widgets lose their teal |
| `fd95203897` | feat(theme): new parts start pink |
| `4d9f875127` | fix(theme): filament swatches show their colour, pink icons stay pink |
| `ccf5350806` | feat(theme): web views lose their teal |
| `4281a5a918` | fix(theme): fresh installs skip the filament colour migration |
| `99467ae37d` | chore(version): 0.0.1 |
| `0247a6f342` | fix(theme): selected pink parts stay pink |

Style used for disabled code: early `return;` or `if (false && ...)` with a
`// TigerSlicer:` comment, keeping upstream code compiled so upstream merges stay easy.
No classes or headers were deleted.

### 1. Cloud features removed

**Privacy bug fixed first (`509f32dfc2`, `src/slic3r/Utils/OrcaCloudServiceAgent.cpp`).**
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

**Sign-in and sync (`340f138315`).**
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

**Stealth mode tooltip (`1614e4fdd0`).** Rewritten to what it still does: keeps the Bambu
network plugin from checking for updates/connecting to Bambu's servers and stops online
printer error descriptions. New msgid, untranslated (English fallback).

### 2. "TigerSlicer", not Orca

**Translations never loaded (`e5645119a6`).** The build wrote
`resources/i18n/<lang>/OrcaSlicer.mo` but the app loads catalog `SLIC3R_APP_KEY` =
`TigerSlicer`. Now `CMakeLists.txt` writes `${SLIC3R_APP_KEY}.mo`, and
`scripts/run_gettext.bat` / `scripts/run_gettext.sh` use `mo_name="TigerSlicer"`
(must match `version.inc`); `.github/workflows/check_locale.yml` too. Source catalogs keep
their names (`localization/i18n/OrcaSlicer.pot`, `<lang>/OrcaSlicer_<lang>.po`).
macOS needs nothing extra (`build_release_macos.sh` calls the fixed `run_gettext.sh`).

**Windows names and OS registration (`e4fa398bcf`, Windows only unless noted).**
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

**Translatable strings (`e63c3d1feb`).** 98 of the 117 Orca msgids changed, in the C++
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

**Hand edits (`9ec5f32b24`).**
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

- **3D scene (`fd0627978d`).** `src/libslic3r/Color.hpp` `ColorRGB::ORCA()` /
  `ColorRGBA::ORCA()` (0,150,136) → (246,165,193) — feeds `COL_ORCA` (~45 uses: gizmo
  borders, measure text, sliders, plate list), the selection rectangle and ImGui header/
  slider colours. Plate slicing progress (`GLCanvas3D.cpp`), layer slider `BRAND_COLOR`
  and tick/label tints (`IMSlider.cpp`), scale/mirror plane (`Selection.cpp`), resin pad
  `SLA_PAD_COLOR` → #5C3A47 (`3DScene.cpp`), toolpath **Skirt** role (0,135,110) → pink
  (`libvgcode/src/ViewerImpl.cpp`), G-code viewer grid/hover/links, measure hover, cut
  line, mesh-boolean box/buttons, painter tool buttons (supports, fuzzy skin, MMU, seam),
  text/emboss pressed toggles, skip-part "kept" colour.
- **Dialogs/widgets/icons (`0025939531`).** ImGui hover/dark constants, confirm/combo
  borders, text selection (pink at 0.5 alpha for legibility), notifications, hints,
  slicing progress, daily tips; wx selection/focus tints (object list, dropdown, combobox,
  tab buttons, search, printer panel, capsule buttons, purge mode, multi-nozzle sync),
  `StepCtrl`, `ProgressBar`, HTML links, thermal-preconditioning OK button; icons
  `canvas_menu_hover`/`canvas_zoom_hover` (+dark), `pipe_of_*`, `dev_ams_upgrading`;
  flatpak metainfo brand colours.
- **New parts start pink (`fd95203897`, `4281a5a918`).** The green part colour was the
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
- **Selected parts stay pink (`0247a6f342`).** `GLVolume::brighten_color()`
  (`src/slic3r/GUI/3DScene.cpp`) highlights a selected object by adding 0.25 to its HSL
  lightness. The old teal (L ≈0.40) stayed teal, but the pink default (L ≈0.81) hit 1.0
  and every selected part rendered pure white. Now `l = max(l, min(l + 0.25, 0.9))`:
  dark/mid colours (L ≤ 0.65) are unchanged, light colours keep their hue. Measured on
  Windows: selected pink cube (209,209,209) before → (205,171,183) after.
- **Bug fixes (`4d9f875127`).** Filament swatches were stuck pink because
  `BitmapCache.cpp` still searched for teal `0xFF889600` (nanosvg ABGR) after
  `outlined_rect.svg` became pink → now `0xFFC1A5F6`. Some icons (completed_2, leaf,
  shield, placeholder_excel) turned red in dark mode because the earlier sweep rewrote a
  placeholder key `"#00FF00"` to `"#D98BA8"` → restored to `"#00FF00"`.
- **Web views (`ccf5350806`).** `include/global.css` dark main/hover → #9B5874, focus
  tints; homepage `dark.css`/`home.css`; guide pages; dialog `theme.css`, PluginsDialog
  styles (badges, update button, run icon, focus rings), SpeedDial.
- **Left green on purpose:** support enforcer (green) vs blocker (red), scale grabbers,
  Y axis, heat maps, other toolpath role colours, status-OK greens, palettes of distinct
  colours, real filament colour data (e.g. Bambu Green), PNG images.

### 4. New startup screen (`e75058f9ed`)

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

Done for macOS after the Windows release (`8c7560f43`, build(macos)):
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
- Rounded-square Mac app icon (`302c3bdaa`): `macos_app_icon()` in
  `generate_tigerslicer_branding.py` draws the pink mark on Apple's icon grid (824 px
  superellipse body on a 1024 px canvas, near-black gradient, soft shadow). It feeds only
  `resources/Icon.icns`, the Dock icon `images/OrcaSlicer-mac_256px.ico`, and the
  `*-mac_128px.png` / branding `*.icns` files; every other icon keeps the round mark.

Still Orca on macOS: the unused `src/dev-utils/platform/osx/Info.plist.in`, the staging
folder and CI dmg/artifact names above, and the profile validator's file name.

#### Building the Mac app

(Also in [BUILDING.md](BUILDING.md).)

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
  `--datadir`: the log reports version 0.0.1 build `507a595c8`, the new config records
  0.0.1, no fatal errors.
- Known harmless log noise on a fresh data dir (all platforms): "Failed to install bundled
  TigerSlicer Resin profile" — `PresetBundle::setup_directories()` copies the resin ini/idx
  into `data_dir/vendor`, which nothing creates or reads; the resin bundle is loaded from
  the app's own `resources/profiles` instead.
