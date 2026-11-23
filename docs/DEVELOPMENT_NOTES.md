# TigerSlicer development notes

Technical notes for each release: which commits changed what, in enough detail to reproduce or port a
change (in particular from the Windows build to the macOS one). For a short list of what changed for
users, see [CHANGELOG.md](../CHANGELOG.md). TigerSlicer is based on OrcaSlicer; "upstream" below means
OrcaSlicer. These are the maintainer's working notes, terse and written for someone who knows the code.

---

## 0.1.0 (2026-11-22)

Theme: TigerSlicer moves from OrcaSlicer's code of August 2026 to OrcaSlicer 2.5.2. The version goes to
0.1.0 for the size of the change. Merged and verified on macOS; released for macOS first.

### Merge of OrcaSlicer 2.5.2
TigerSlicer was built on OrcaSlicer `main` of 2026-08-19 (`ba2297391`, between 2.4.2 and the 2.5 alpha).
`36d6dc5bcf` merges the tag `v2.5.2` (`f92a7c065c`, 2,928 commits ahead, about 1,600 source files). The
owner's rule for the merge: use OrcaSlicer's code where both sides have the same thing; keep what only
TigerSlicer has, what it did on purpose, and what it does better.

- **How it was done.** A plain `git merge` on the branch `merge-orca-2.5.2`: 7,143 conflicted files.
  - 6,908 were profiles. Printers, processes and vendor files are OrcaSlicer's. The brand filament
    profiles stay removed: the 0.0.5 migration (`scripts/tigerslicer_filaments.py --migrate`) was run again
    on the new tree with the names it had recorded before, so the library profiles' `renamed_from` now maps
    7,726 old names (1,324 of them new in 2.5). The Orca Arena test vendor stays removed.
  - About 77 source and test files had 207 conflict hunks, resolved by hand, hunk by hunk: OrcaSlicer's
    new code first, TigerSlicer's change put back on top.
  - Translations are OrcaSlicer's catalogs with TigerSlicer's wording edits replayed line by line.
  - Web pages and icons: OrcaSlicer's new versions with the pink palette, the TigerSlicer name and the
    cloud removals put back. 92 new icons were recoloured from `#009688` to `#F6A5C1`.
- **OrcaSlicer's implementation replaced TigerSlicer's** for: object label ids
  (`assign_object_label_ids()`), the print action menu and its default button, the setup wizard's profile
  loading, the custom camera source dialog, the keyboard shortcuts dialog, and colour mixing (see below).
- **Kept from TigerSlicer, where OrcaSlicer went another way:**
  - No cloud: no sign-in, no preset sync, no plugin hub (`open_plugin_hub()` and the cloud health check
    are stubs), no "Sync Presets" menu. Plugins install from a file.
  - No Home tab. OrcaSlicer's lazy pages return null for it.
  - The calibration builder: `calib_dlg.*` stays deleted, `MainFrame::run_calibration()` opens the builder,
    and the Speed Dial has one "Calibration builder" action instead of nine calibrations.
  - One bed temperature, the by-material filament library (`MoonrakerPrinterAgent` keeps TigerSlicer's
    filament id table, as OrcaSlicer's looks up "Generic X @System" presets), the experimental switch
    (the settings search filter moved to `SettingsIndex.cpp`), resin mode and its guards.
  - `NonPlanar::object_mesh()` stays (it also fixes mirrored facets) but takes the print object's own
    first instance, as OrcaSlicer's fix for Z contouring with several copies does.
  - OrcaSlicer's startup prompt to migrate filament presets is off (`check_legacy_user_filament_presets()`
    only runs from the Help menu): TigerSlicer's filaments fit every printer.
- **Colour mixing is OrcaSlicer's mixed filaments; Full Spectrum is removed.** Both mix two or three
  loaded filaments by ratio into extra filament slots, with gradients, so by the owner's rule
  OrcaSlicer's stays.
  - Deleted: `Feature/FullSpectrum/`, `FullSpectrumDialog`, `VirtualExtruderListBox` and the dialog's
    helpers (`ColorUtils`, `BarycentricRatioPicker`, `GradientRatioBar`, `LayerSequenceBar`, `UIColors.hpp`),
    `src/prusa_fdm_mixer/`, `TriangleSelector::remap_states(std::map)`, `Print::num_physical_extruders()`,
    the sidebar's "Color mixing" button and the View menu item.
  - Back to OrcaSlicer's form: `get_create_region()`, `Print::object_extruders()`,
    `update_shrinkage_compensation()`, `PrintRegion::operator==`, `collect_object_printing_extruders()`,
    `clamp_feature_filament_to_valid()`. `MultiMaterialSegmentation.cpp` and `Format/bbs_3mf.cpp` are
    identical to OrcaSlicer's again.
  - An old project keeps opening: its `Metadata/tiger_slicer_full_spectrum.json` is copied like any other
    metadata file and not read. Extruder ids above the filament count become filament 1 at slice time,
    and by reading the code areas painted with a virtual extruder print as unpainted; no real file was
    tried. The mixes have to be made again as mixed filaments.
  - The check that kept independent tool layer heights and Full Spectrum apart went with it; nothing
    checks independent tool layer heights against mixed filaments yet.
- **Prusa profiles** are OrcaSlicer's updated ones (MMU3, INDX, pressure advance), with the 0.0.13
  filament-family rules for the bed levelling temperature applied again. The INDX start G-code keeps its
  own, lower probing temperatures and only gets the family matching.
- **New in the build:** OrcaSlicer 2.5 needs rebuilt dependencies (`build_release_macos.sh -d`): the SLVS
  constraint solver for the Design tab, a pinned EXPAT, newer wxWidgets and Python. A build directory
  from before the merge needs its `CMakeCache.txt` deleted, or configure finds the SDK's older EXPAT.
- **After-merge fixes** (`74b68a1021` and later): `AppConfig`'s per-printer settings are a string map
  now (the default filament colour migration follows); Clipper's join and end types are `Slic3r::` enums;
  G-code written by OrcaSlicer is accepted as a configuration source; `apply_filament_shrinkage` is in
  `s_Preset_print_options`; the new strings of 2.5 say TigerSlicer in the source and the catalogs.
- **Tests adjusted to TigerSlicer's choices** (`tests/slic3rutils`): the Speed Dial catalog expects the
  calibration builder and the neutral handy model keys; the plugin audit expects `tigerslicer.conf`
  (this one already failed before the merge); seven Orca Cloud health check cases are skipped, as the
  health check never runs.
- **Verified on macOS (arm64):**
  - The app builds with rebuilt dependencies and starts with a throwaway data folder: first run, a Prusa
    CORE One L selected, and an Elegoo Mars 3 (resin) selected. No fatal log lines, no crash report.
  - `ctest` (every case in its own process, as OrcaSlicer's CI runs them): 3,998 of 4,002 pass. The 4 that
    fail are the compiler-dependent reference hashes that failed before the merge (`[Regression]` in
    test_print.cpp and three `[NonPlanar]` cases). Their reference values come from Windows and will
    need regenerating there, since OrcaSlicer 2.5 changes the toolpaths they hash.
  - Run directly, `slic3rutils_tests` crashes in its Python cases when another case started Python first.
    That is a documented limit of OrcaSlicer's suite (`python_test_support.hpp`), so it is run with ctest.
- **Not verified:** the Windows build; printing; the new 2.5 features by hand (Design tab, plugins, Speed
  Dial, texture displacement, mixed filaments, belt and IDEX modes); TigerSlicer's tools in the GUI after
  the merge (only their automated tests ran); an old project with Full Spectrum data.
- **Left for later:** OrcaSlicer's belt handling for calibration prints (the calibration builder has
  none); the wizard's "Filament printers" / "Resin printers" tab labels are not translatable under
  OrcaSlicer's new web translation system; the Deviation view cannot be chosen as the default preview
  view; brick layers together with mixed-filament sub-layers keep the lifted travel height for the
  whole layer.

### Verification (Windows)
- Windows builds with `build_win.bat` now (OrcaSlicer's script; `build_release_vs2022.bat` and
  `build_release_vs.bat` went with the merge). docs/BUILDING.md is updated.
- Dependencies: `build_win.bat -d` into a new `deps\build`, with Visual Studio 2022 (MSVC 19.44): 21 min
  50 s on 16 cores, no errors. The tree from before the merge does not fit (GLEW and OpenCSG are gone,
  SLVS and wxInspector are new, OCCT, OpenCV, OpenSSL, PNG, Python and wxWidgets changed); on the
  release PC it was renamed to `deps\build-pre-2.5`, and the old `build` to `build-pre-2.5`.
- App: `build_win.bat -s -i` from the release commit (`4ae34f102`, tag `v0.1.0`) in a new `build`: 9 min
  22 s, no errors. The merge needed no change for MSVC. It installed 9,822 files (515 MB).
- Tests: `build_win.bat -s --tests --no-gettext`, then `ctest --test-dir build/tests -C Release -j 12`:
  4,003 cases, 18 of them skipped by the suite itself (symlink, cloud health check and Python buffer
  cases).
  - At the release commit 4 fail, the same 4 as on macOS: the reference hashes in `[Print][Regression]`
    (six values) and three `[NonPlanar]` cases (two values). OrcaSlicer 2.5 changes the toolpaths they
    hash.
  - The values were recorded again with MSVC (the commit after the release): each was the same in five
    runs, and Z contouring only and On with experimental features off still give one and the same hash.
    With them all 4,003 pass.
  - `[Regression]` is on 167 fff_print cases since the merge, so run those four by name.
- Packaged with `%TEMP%\tsrel-0.1.0`, made by `%LOCALAPPDATA%\TigerSlicer-release-kit\make_kit.py`.
- Release assets, added to the existing `v0.1.0` release on 2026-11-22 (the tag was not moved):
  - `TigerSlicer-0.1.0-win64-Setup.exe`, 198,197,248 B, sha256 `b947f87b…d34d3d`.
  - `TigerSlicer-0.1.0-win64.zip`, 204,705,581 B, sha256 `016765f9…7a6346`.
  - Both digests were checked on GitHub after upload.
- The Setup.exe test install (`TIGERSLICER_INSTALL_DIR`) has 9,823 files. Its `TigerSlicer.exe` and
  `TigerSlicer.dll` match the build.
- Smoke tests of the test install, started minimized, no input, log only:
  - On a copy of a real data folder last used by 0.0.12 (without `log` and `cache`): "Version 0.1.0
    build 4ae34f102c"; the folder's own printer and process presets are selected again, as 0.0.13
    selects them; responding after about 55 s; no crash log. The `[error]` lines are the two
    `calc_exclude_triangles` and "Unsupported printer type: Prusa CORE One L" that 0.0.13 logs too.
  - On a new, empty data folder (first run): the same version line, responding after about 45 s, no
    crash log, and a `user_backup-v0.1.0` folder beside the new configuration.
- Not looked at on screen on Windows: anything. The new 2.5 features, TigerSlicer's own tools after the
  merge and the first-run wizard were not tried by hand, and nothing was printed.

---

## 0.0.13 (2026-11-21)

Theme: shrinkage calibration (experimental), a thin feature warning that only shows when something is
left out, and two fixes for Prusa printers (filament names and the bed levelling temperature).
Developed and verified on macOS; released for macOS first.

Commits, oldest first (all on `main`; the release commit follows them):

| Commit | Subject |
|---|---|
| `172189682` | feat: objects can opt out of filament shrinkage compensation |
| `77b6eb5ba` | feat(calibration): shrinkage calibration (experimental) |
| `9940f0796` | fix: warn about thin features only when the print leaves them out |
| `3be585f8c` | fix(prusa): firmware filament names and a bed levelling temperature by filament type |

### Shrinkage calibration (experimental)
- Base (OrcaSlicer): `filament_shrink` (XY, one value) and `filament_shrinkage_compensation_z` are
  filament percentages, 100 = off, applied as the per-axis scale `Print::shrinkage_compensation()` on
  each instance transform in `PrintApply` (`print_objects_from_model_object`), print-wide from the
  first used filament and only when all used filaments agree. `xy_contour_compensation`,
  `xy_hole_compensation` and `elefant_foot_compensation` are `PrintObjectConfig`, `hole_to_polyhole`
  `PrintRegionConfig`; they offset the slices in `PrintObjectSlice.cpp` after the scale. Separate X and
  Y would only need the `Vec3d`; kept as one XY value by choice (the analysis reports X and Y and uses
  the mean).
- New object option `apply_filament_shrinkage` (bool, default true, category Quality, not in any print
  preset page). `Print::shrinkage_compensation(const ModelObject&)` returns ones when an object turns
  it off; used in `PrintApply`, `PrintObject::update_slicing_parameters`, the build height check in
  `Print::validate` and the variable layer height editor. In `invalidate_state_by_config_options` it
  invalidates `posSlice`.
- `libslic3r/ShrinkageCalibration.{hpp,cpp}` (pure): `fit_line()` (least squares, RMS and max
  residual), `analyse()` (per axis percent = slope × 100, intercept = constant offset; XY mean,
  anisotropy > 0.1 pp, offset > 0.2 mm flagged, residual > 0.05 mm flagged; contour suggestion
  −offset/2, hole suggestion −(hole error)/2 after the XY scale; wall checked against the fit),
  `out_of_range()` (> 5 % or missing), `corrected_percent()` (new = printed-with × measured / 100, so a
  verification print refines the current value), `nominals_for_bed()` and `make_test_part()`.
- Test part: 14 mm arms, each three 2 mm tiers on a 2 mm base inset 1 mm from every measured face, so
  each length runs across opposite-facing faces (a constant offset appears in the intercept) and none
  touches the first layers. Z staircase from z = 0; holes in a 6 mm boss; 10 mm tall wall; X and Y
  engraved 0.6 mm into the exposed top of each arm's bottom tier. Prisms via
  `Emboss::polygons2model`. Arms (and Z) scale down, to 0.5× at most, when the bed (or height) is small.
- GUI: `ShrinkageCalibrationDialog` (Calibration menu, both the Windows title bar and the menu bar,
  enabled while experimental features are shown). `Plater::add_shrinkage_test_part()` adds the part
  with the overrides above (none for verification) and lists the profiles' non-zero compensation in a
  notification. Measurements live for the session; the last part's nominals are kept in the app
  config (`shrinkage_calibration_nominals`). Apply writes the two percentages and a dated record
  (condition, results, measurements) to `filament_notes` of the edited filament through
  `Tab::load_config()`, so it is an unsaved change and a system preset is never overwritten.
- Tests: libslic3r `[ShrinkageCalibration]` (13 cases: pure scale, pure offset, both, noisy,
  inconsistent, anisotropy, suggestions, large offset, rejection, verification correction, bed
  scaling, part size); fff_print "An object can opt out of filament shrinkage compensation" slices a
  20 mm cube at 95/98 % (21.05 × 20.41 mm with the option on, 20 × 20 with it off).
- Verified on macOS: all suites built and run. libslic3r 1,078, sla_print, libnest2d and
  filament_group pass. fff_print (207 of 211 cases pass) and slic3rutils (68 pass, 1 fails, 33
  skipped without numpy) fail the same way on main without this change: the compiler-dependent motion-hash tests (`[Regression]` in
  test_print.cpp, two `[NonPlanar]` regression cases and "With experimental features off, On prints
  as Z contouring only") and the plugin audit's `orcaslicer.conf` names. In the app, with a temporary
  startup hook: filament at 95/98 %, the test part's print object scale was 1/1/1 and the
  verification part's 1.0526/1.0526/1.0204; the overrides were set on the test part only and the
  filament became modified. The dialog itself was not opened from the build machine.

### Fewer "below printable width" warnings (experimental)
- Before: `PrintObject::warn_features_below_printable_width()` counted every `features_below_width()`
  piece, whatever happened to it. It fired for:
  - thin features the walls print anyway (Arachne prints a 0.2 mm fin with a 0.4 mm nozzle);
  - fins the small feature policy prints (Balanced, Outside);
  - sharp corners (a 45° or 25° point, classic or Arachne): the miter opening only gives back corners
    blunter than its miter limit;
  - features only a layer or two tall.
- Now a feature counts only if `SmallFeatures::is_left_unprinted()` holds. The part of it that
  `printed_coverage(layer)` misses, opened by half of `kDeviationTolerance`, must have a piece of at least
  min width × nozzle mm² (a minimum-width line one nozzle long). Coverage is computed once per layer,
  and only on layers that have a feature.
  - It runs before `set_done(posPerimeters)`, so coverage holds walls, gap fill and small feature lines;
    the fill area counts as covered.
- The layers that have an unprinted feature must add up to `kMinUnprintedHeight` (0.6 mm), or there is
  no warning.
- New wording: "N thin features will not print with a X mm nozzle (narrower than Y mm)". When every
  region's policy is Drop, it adds "A small feature policy other than Drop can print them."
  - A policy can't always help, so the hint is left out otherwise. Every policy skips pieces narrower
    than half the minimum width. Inside also skips those narrower than the minimum width, since its line
    would cross the edge.
- Tests (fff_print `[SmallFeatures]`), all with a 0.4 mm nozzle:
  - Warns: classic walls with a 0.15 mm fin, with the hint, and only with experimental features on.
  - Still warns, without the hint: 0.08 mm with Balanced or Outside, and 0.15 mm with Inside.
  - No warning: Arachne with a 0.2 mm fin; Balanced or Outside with a 0.15 mm fin; 25°, 45° and 90°
    wedges with both wall generators; a fin 0.4 mm tall.
  - Against the old warning code, the corner, Arachne, policy and short-fin cases fail. The others fail
    only on the new wording.
- Verified on macOS: fff_print 212 of 216 cases pass, and libslic3r passes all 1,078. The 4 fff_print
  failures are the compiler-dependent regression tests that also fail on main (`[Regression]` in
  test_print.cpp, and test_non_planar.cpp 220, 228 and 307). The app was built, but the warning was not
  checked in the GUI.

### Prusa firmware: filament names and the bed levelling temperature
Reported on a CORE One L: PPA loaded, PPA sliced, and the printer says the wrong filament is loaded.
- Cause, from the firmware source (Prusa-Firmware-Buddy):
  - `GCodeInfo::parse_comment()` takes `; filament_type = a;b;c`, one name per tool, cut to 7 characters.
  - `gcode_compatibility.cpp` sets `VirtualToolCheck::filament_type` ("Wrong filament type") when that
    name `!=` the loaded filament's name. There is no other matching.
  - The preset names (`PresetFilamentType`) are PLA, PETG, ASA, PC, PVB, ABS, HIPS, PP, FLEX, PA, PVA and
    BVOH, plus PPS and PPA with the high temperature hotend. TigerSlicer's types are PPA-CF, PPA-GF, PA-CF,
    TPU and so on, which never match. PrusaSlicer's own profiles use the base name (its Siraya Tech PPA-CF
    has `filament_type = PPA`).
  - The plain-text reader scans the comments before the first G-code and then the last 75,000 bytes; a
    later `filament_type` replaces an earlier one.
- Fix: `GCode/PrusaFirmware.{hpp,cpp}`.
  - `filament_name()`: a preset name stays; TPU*/TPE* become FLEX; otherwise the part before the first
    `-` if it is a preset name; PA6, PA11, PA12, PA612 and PAHT become PA; anything else is unchanged (for
    a filament the user added to the printer under that name).
  - `filament_type_line()` gives `; filament_type = ...` with those names, only when the start G-code has
    `M862.3` (the firmware's printer model check) and a name differs from its type.
  - `GCode::_do_export()` writes it after `; CONFIG_BLOCK_END`, so it is the last one in the file. The
    configuration block keeps the slicer's types: `load_from_gcode_file()` only reads the block.
  - Not handled: a single-tool printer whose print starts with the project's second filament. The
    firmware assigns the names to tools in order, as with PrusaSlicer.
- Bed levelling ("standby") temperature. The Prusa start G-codes compared `filament_type[0]` to exactly
  "PC", "PA" and "FLEX", so every other type waited at 170 °C, and TPU missed the FLEX retraction.
  - The comparisons are now regular expressions (the parser's `=~` is a full match):
    `/^(PC|PC-.*|PA|PA-.*|PA[0-9]+.*|PAHT.*)$/` (not PCTG) and `/^(FLEX|TPU.*|TPE.*)$/`.
  - New branch in front of them, from PrusaSlicer's "CORE One L HT Hotend" start G-code: PEEK* 330,
    PEKK* 310, and `/^(PPA|PPS|PSU|PES|PEI|PI|TPI).*/` at `min(first layer - 40, 310)`.
  - The `MBL160` and `HT_MBL10` filament notes still win. The 170 °C idle temperature while the chamber
    heats is unchanged.
  - In the single-tool profiles `filament_type[0]`, `filament_notes[0]` and `first_layer_temperature[0]`
    became `[initial_tool]`. The XL files keep `[0]` where they address tool 0.
  - 11 machine files changed (CORE One, CORE One L, MK3.5, MK4, MK4S, XL); `Prusa.json` is 02.04.00.05
    so installed system profiles update. User printer presets keep their own start G-code.
- Tests:
  - libslic3r `[PrusaFirmware]`: the name table (32 types) and when the line is written.
  - fff_print `[PrusaStartGCode]` slices a cube with the shipped CORE One L start G-code: both waits and
    the purge temperature for 21 types, the flexible retraction, and the last `filament_type` of the file
    (PPA, FLEX; unchanged for PLA and for a start G-code without `M862.3`).
- Verified on macOS: fff_print 215 of 219 cases and libslic3r all 1,080; the 4 failures are the same
  compiler-dependent regression tests as on main. Not verified on a printer.

### Verification (Windows)
- Tests (MSVC), from the release commit (`a8e202c53`, tag `v0.0.13`): fff_print 219 of 219 and libslic3r
  1,080 of 1,080, including `[PrusaStartGCode]`, `[PrusaFirmware]` and the regression hash tests, whose
  reference values are MSVC's.
- Release build from the same commit with a clean `build\OrcaSlicer` and `BUILD_TESTS` off, against the
  pre-2.5 dependencies: no errors. It installed 9,323 files, as 0.0.12 did.
- Packaged with `%TEMP%\tsrel013`, made by `%LOCALAPPDATA%\TigerSlicer-release-kit\make_kit.py`.
- Release assets, added to the existing `v0.0.13` release on 2026-11-22 (the tag was not moved):
  - `TigerSlicer-0.0.13-win64-Setup.exe`, 229,666,816 B, sha256 `8ef0846d…2222ec`.
  - `TigerSlicer-0.0.13-win64.zip`, 236,056,760 B, sha256 `a908e2c9…71233b`.
  - Both digests were checked on GitHub after upload.
- The Setup.exe test install (`TIGERSLICER_INSTALL_DIR`) has 9,324 files. Its `TigerSlicer.exe` and
  `TigerSlicer.dll` match the build.
- Smoke test of the test install, started minimized on a copy of the real data folder (without `log`
  and `cache`), no input: "Version 0.0.13 build a8e202c535", responding after about 25 s, no crash log.
  The `[error]` lines are two `calc_exclude_triangles` and "Unsupported printer type: Prusa CORE One
  L", as in 0.0.12.
- Not looked at on screen: the shrinkage calibration window and the Prusa start G-code on a printer.

---

## 0.0.12 (2026-11-15)

Theme: a screw hole tool and a fillet/chamfer tool (the only CAD in the slicer: the Sketch tool was
removed before release), non-planar top layers (experimental), and the object label id fix. Developed
and verified on Windows.

Commits, oldest first (all on `main`; the release commit follows them):

| Commit | Subject |
|---|---|
| `3295b1d06` | feat: Sketch - draw shapes and extrude them into a new object, a part or a cut |
| `f3d8a08ff` | feat: non-planar top layers (Off / Z contouring only / On) |
| `a38576924` | chore: remove the Sketch tool |
| `0428fa3b8` | fix: number objects in G-code labels without exclude objects |
| `48cf88b9c` | feat: screw hole and fillet/chamfer tools |
| `58e33bcbf` | docs: hole and fillet/chamfer tools and the object label fix |

### Sketch (removed before release)
- Added in 3295b1d06 (Sketch window, `libslic3r/Sketch`, `GLGizmoSVG::create_volume_from_sketch`, Edit menu
  and Add part / Negative part items), then reverted whole before any release: CAD in the slicer is kept to
  the hole and edge tools below.

### Screw hole and fillet/chamfer tools
- **Approach.** Neither tool edits a mesh. Each adds volumes to the object (`add_tool_volumes()` in
  `GUI/Gizmos/GLGizmoModelTools`): the mesh is built in world coordinates of the selected instance, mapped
  to object coordinates by the inverse instance matrix (`its_transform(..., fix_left_handed = true)` keeps
  mirrored instances outward-facing), added with `ModelObject::add_volume()` (which centers it), named,
  `extruder` 0 for negative volumes and the object's extruder for parts. One `Plater::TakeSnapshot`, then
  `ObjectList::reorder_volumes_and_get_selection(obj_idx)`, which lists the volumes and reloads the scene,
  and `notify_instance_updated(obj_idx)` for the plates. An instance-mode selection takes the new volumes in (`Selection::instances_changed`), so the gizmo stays
  open. Slicing does the rest: negative volumes are subtracted, parts unioned.
- **Picking.** `SurfacePicker` keeps one `MeshRaycaster` per model part of the selected instance, rebuilt
  only when the object, instance matrix, part meshes or part matrices change. `pick()` returns the hit
  closest to the camera with the world normal (inverse-transpose). Both gizmos need only
  `CommonGizmosDataID::SelectionInfo` and are activable on a single full instance. They are appended at
  the end of `GLGizmosManager::EType` (`Hole`, `EdgeBlend`), with icons `toolbar_hole*.svg` and
  `toolbar_edge_blend*.svg`. No keyboard shortcuts.
- **Screw hole** (`GLGizmoHole`, geometry in `libslic3r/Feature/ModelTools/ScrewHole`). M2-M8 table:
  pilot = ISO tap drill (M3 2.5), clearance = ISO 273 normal (M3 3.4). `hole_mesh()` is a 64-sided
  circumscribed cylinder (flats on the circle) from 0.5 mm over the surface to the depth under it, along
  the hit normal. "All the way through" uses `SurfacePicker::depth_through()`: the farthest
  `AABBMesh::query_ray_hits()` exit along -normal over all parts, plus 1 mm. The volume is named
  "M3 through hole" etc., with the diameter added when it was typed in.
- **Fillet/chamfer** (`GLGizmoEdgeBlend`, geometry in `libslic3r/Feature/ModelTools/EdgeBlend`).
  - `sharp_edges()`: edges whose face normals differ by more than 30°, from `its_face_neighbors()`
    (needs consistent orientation; open edges and degenerate faces skipped). Convex if the right face's
    centroid lies under the left face's plane. Chained through vertices with exactly two sharp edges of
    the same convexity that turn by at most 45°; what remains after walking from the other vertices is
    closed loops. Each segment records the face holding it in the chain's direction (`face_left`).
  - `edge_blend_mesh()`: per chain vertex, a profile in the plane square to the tangent: the faces'
    in-surface directions `ul = nl x t`, `ur = t x nr`, angle between them 20°-160°. Fillet: 20-segment
    arc of radius r tangent to both faces at `r / tan(a/2)`; chamfer: the chord between points `size`
    along each face. The profile is closed 0.2 mm past the faces (outward for a convex edge, into the
    material for a concave one), with the corner at `p + 0.2 (nl + nr) / (1 + nl.nr)`. Profiles are
    mitered at turns (stretched by `1/cos(turn/2)` along the turn), swept into rings, capped with fans from
    the edge point (star-shaped profile) on open chains, and flipped if the volume comes out negative.
    Open convex chains reach 0.05 mm past their ends; concave ones stop at the ends. At a turn, a profile
    reaching more than 0.9 of the local curve radius (`segment / (2 sin(turn/2))`) toward the centre gives
    an empty mesh, so a fillet wider than a small rim is refused instead of folding over itself. Corners
    where chains meet are not blended (each chain ends square), and the radius is not checked against
    the width of the faces.
  - The gizmo computes chains on each part's mesh in world coordinates, so radii are true mm under
    instance scaling. Hover: nearest chain segment to the hit point, accepted within 12 px on screen; off
    the model, the nearest segment on screen if `SurfacePicker::is_visible()` (a ray from the camera to it
    meets no part first), so outline edges can be picked from just outside.
    Picked chains are drawn as tubes 2.5 px thick, with a see-through preview of the cut (pink) and fill
    (green). Convex chains become negative volumes, concave ones parts, named "Fillet 2 mm" etc.
  - Previews and edge tubes are blue (hover yellow, fills green), to stand out on pink and most filament
    colors. `GLModel::init_from()` silently ignores an already-initialized model in release builds, so
    both tools `reset()` their preview models before rebuilding them.
- **Tests.** `tests/libslic3r/test_model_tools.cpp` (`[ModelTools]`): sizes, hole mesh closed and wide
  enough, 12 edges on a cube, two closed rims on a cylinder, one concave edge in an L prism, and the volume
  a fillet or chamfer removes (CGAL difference against the analytic value: cube edge, cylinder rim by
  Pappus) or adds (L corner), no blend for nearly flat faces or a fillet too big for a 3 mm boss. `tests/fff_print/test_model_tools.cpp`:
  a cube sliced with a fillet cutter steps back along the circle near the top, and with an M3 hole has a
  hole of the right area in every layer.

### Object label ids
- `PrintObject::m_id` was never initialized and only set by `GCode::set_object_info()`, which runs only
  with exclude_object. The `; printing object <name> id:N copy M` labels (`gcode_label_objects`) printed
  it anyway, so ids were garbage and could collide. That made the `[CalibrationBuilder]` test "Layers
  above the tower print without its overrides" fail about two runs in three (the cube's tally took the
  tower's plastic too, halving the ratio). `m_id` now starts at 0, and `_do_export()` numbers objects
  and instances before the exclude-object block, the same way `set_object_info()` does.

### Non-planar top layers
- **Setting.** `non_planar_mode` (enum `NonPlanarMode`: `off` / `z_contouring` / `on`, region config,
  default `off`) replaces upstream's `zaa_enabled`. `handle_legacy()` maps `zaa_enabled` 1/true to
  `z_contouring` and anything else to `off`. Every upstream `zaa_enabled` check is now
  `non_planar_mode != npmOff`: ContourZ, BrickLayers, Fill (3 places), Preset's key list,
  ConfigManipulation and `compute_slice_z`. So Off and Z contouring only print exactly what upstream
  printed. The `[Regression]` hash tests pin this; the hashes are MSVC's.
- Two more settings, `non_planar_max_angle` (20°) and `non_planar_max_height` (3 mm). Both are
  `experimental`, shown only when the mode is `on`. All three are in the GUI's Non-planar group and
  in the per-object/part settings.
- **Experimental values.** `ConfigOptionDef::experimental_values` + `experimental_fallback` make one
  enum value experimental (here `on` → `z_contouring`).
  - `experimental_options_used()` / `reset_experimental_options()` and the Model helpers cover
    them, and `experimental_values_used()` / `model_experimental_values_used()` list them.
  - `Print::apply` records them in `experimental_values_ignored()` before resetting.
  - `_do_export` adds a notice that the value was sliced as the fallback.
- **Zones** (`PrintObject::find_non_planar_zones()`, called from `slice()` after the polyholes).
  `Feature/NonPlanar/NonPlanar.cpp`.
  - The mesh is `NonPlanar::object_mesh()`: raw mesh × instance 0, without the center offset,
    `fix_left_handed`. It is shared with `contour_z()`.
  - Mesh Z = object Z + `ground`; print Z = object Z + `object_print_z_min`.
  - Top facets: normal within `max_angle` of up, and nothing above the centroid (a ray up).
  - Each union island of the projected facets is a candidate. Its top is the highest corner.
    `bottom = max(lowest corner, top − reach, first layer top + thickness)`:
    - `thickness = shells × layer_height`, and `shells = max(top_shell_layers,
      ceil(top_shell_thickness / layer_height))`.
    - `reach = max_height − thickness`.
  - It is a zone if `top − bottom ≥ layer_height`; smaller rises are left to Z contouring.
  - Footprint: the island's facets clipped above `bottom`, then projected. It is dropped if the
    object is less than `thickness + layer_height` thick anywhere under it (ray samples every mm).
  - The zone takes the settings of the region that holds its largest piece halfway up
    (`SmallFeatures::region_of`). It must be one of the `on` regions. The footprint is then clipped
    to that region's slices at the base layer (the highest layer top ≤ `bottom − thickness`). That
    keeps it off other regions and off holes that negative volumes cut.
  - `zone.layer` is the first layer whose top reaches the zone's top. The shells are printed there.
  - Copies that share another object's layers (`m_shared_object`) also share its zones:
    `non_planar_zones()` forwards to it.
- **Trim.** For each layer from `bottom − thickness` up to the zone's top, the slices lose
  `footprint \ section(layer top + thickness − 1 µm)` in every region. The section is `slice_mesh_ex`
  of the mesh. So the flat layers stop `thickness` under the surface. The 1 µm keeps a flat top
  exactly at the plane.
- **Cover.** `cover_non_planar_tops()`, in `prepare_infill` after `detect_surfaces_type()`. In the
  same range, including the layer the lowest shell lies on, `stTop` surfaces within the footprint
  become `stInternalSolid`. No flat top skin goes under the shells.
- **Shells.** `add_non_planar_shells()`, at the end of `contour_z()` (posContouring, after
  ironing). They are added to the zone layer's region `fills` as one `no_sort` collection, bottom
  up. The G-code writer extrudes a fill collection's direct paths only, and must not reorder them.
  - Each shell is a loop half a wall width inside the footprint, then rectilinear lines at
    45°/135°, alternating per shell. All one role: `erTopSolidInfill` for the top shell and
    `erSolidInfill` below it. One role per collection keeps one extruder.
  - Every 0.2 mm, a ray from above gives the surface. The point is `(shells − 1 − j) × h` under it,
    stored as a Z offset from the layer's `print_z` (like ZAA). A missed ray keeps the last height;
    a path that starts with misses drops those points. Points in line within 1 µm (at the same flow)
    are merged.
  - The paths set `z_contoured` and the new `ExtrusionPath::non_planar` flag. That flag is copied in
    every constructor and assignment and in the loop `split_at`. `z_contoured` already keeps
    `simplify()` off them.
- **Lowest shell flow.** The flat layers under it end on 0.2 mm steps, so the gap under the lowest
  shell is from 0 to `h`. `ExtrusionPath::point_flow` holds a ratio per point:
  `(h + g) / h`, where `g` is the curved bottom − the highest flat top under it, clamped to [0, h].
  `reverse()` reverses it. The G-code writer uses the segment's average, and only when there is one
  ratio per point. `print_flow_ratio` and `top_solid_infill_flow_ratio` still apply by role.
- **G-code** (`GCode.cpp`):
  - A `non_planar` path is reached at `m_nominal_z + path.height`. The zone's top may be up to that
    height over the layer's Z. The existing "set Z for contouring" move takes it straight down.
  - After the path, `extrude_path` lifts straight up to the same height ("lift after non-planar
    path") and skips the wipe.
  - ZAA's `(h + dz) / h` extrusion ratio is not applied to it (it would go negative a layer down).
  - `_extrude`'s empty-layer check allows `top − bottom + thickness` more gap on a zone's layer, since
    the layers under it are trimmed.
- **Limits** (v1):
  - Rectilinear lines only, and no walls in the shells beyond the edge loop.
  - No collision model beyond `max_height` / `max_angle`.
  - One instance's transform (instance 0) for the mesh.
  - Not with spiral vase.
  - The curved paths lift after each path; fills are mostly connected, so that is few lifts.
- Tests: fff_print `[NonPlanar]`:
  - Off and Z contouring hashes, and the legacy mapping.
  - On on a 30 mm block with a 2 mm dome: one zone 3–5 mm, 3 shells bottom up, each point a layer
    apart under the surface, no flat top skin, no low travels, no CRITICAL warning.
  - Experimental off → the Z contouring hash, plus the ignored list.
  - Steep slopes stay flat; reach caps a tall dome.
  - Lowest-shell flow ratios.
  - Mirrored, raft, copies, top shell thickness, and modifiers on and off.
- Verified on Windows:
  - fff_print 208 test cases and libslic3r 1,054 pass, and the app builds.
  - The dev build, run on a copy of the user's data folder (Core One L, 0.6 mm nozzle, 0.32 mm
    layers):
    - The dropdown lists the three choices, and the two maximums show only with On and experimental
      features on.
    - The dome slices with no error. With these 0.32 mm layers the zone is the dome's upper disc,
      and the rest of the top prints flat with Z contouring.
    - The unsaved-changes dialog reads "Off → On (experimental)".
  - The preview's layer slider labels the zone's layer by its low extrusions (3.79 here, nominal
    5.0). That is cosmetic.
  - "On (experimental)" stays in the dropdown with experimental features off. It slices as Z
    contouring only, with the notice.

### Verification (Windows)
- Tests (MSVC), from `58e33bcbf` (the release commit only changes the version): fff_print 210 of 210,
  libslic3r 1,065 of 1,065, including `[ModelTools]` (11 + 2 cases) and `[NonPlanar]` (13). The
  `[CalibrationBuilder]` case that failed two runs in three before the label id fix passed 5 runs of 5.
- Release build from the release commit (`de8e30ffa`, tag `v0.0.12`), with a clean `build\OrcaSlicer`
  and `BUILD_TESTS` off: no errors. It installed 9,323 files: 0.0.11's 9,319 plus the four new
  toolbar icons.
- Packaged with `%TEMP%\tsrel012`, made by `%LOCALAPPDATA%\TigerSlicer-release-kit\make_kit.py`: the
  earlier kits' setup.sed and install.ps1 had been deleted by Windows temp cleanup and were rebuilt
  from the session logs (only `$version` and the kit paths ever changed).
- Release assets:
  - `TigerSlicer-0.0.12-win64-Setup.exe`, 229,617,664 B, sha256 `45fe957c…63f727`.
  - `TigerSlicer-0.0.12-win64.zip`, 236,005,453 B, sha256 `e2f88192…ee9032`.
  - Both digests were checked on GitHub after upload.
- The Setup.exe test install (`TIGERSLICER_INSTALL_DIR`) has 9,324 files. Its `TigerSlicer.exe` and
  `TigerSlicer.dll` match the build.
- Smoke test of the test install, started minimized on a copy of the real data folder (without `log`
  and `cache`), no input: "Version 0.0.12 build de8e30ffa7", responding after about 20 s, no crash log.
  The `[error]` lines are two `calc_exclude_triangles` and "Unsupported printer type: Prusa CORE One
  L", which earlier releases log too.
- In the app (dev build, before the preview fix): both toolbar icons show and the Screw hole panel
  opens with its controls. The hover preview was not seen; that led to the `GLModel::reset()` and
  color fixes above. Neither tool was used on a model in the app: they are covered by the
  `[ModelTools]` tests only.

---

## 0.0.11 (2026-10-23)

Theme: one bed temperature for every build plate, and the experimental features switch where people
see it. Developed and verified on macOS; nothing here is platform-specific.

Commits, oldest first (all on `main`; the release commit follows them):

| Commit | Subject |
|---|---|
| `571c42a85` | feat(ui): turn experimental features on and off from the process settings and the View menu |
| `b6bbbdb15` | feat(filaments): one bed temperature for every build plate |

### One bed temperature for every build plate
- `get_bed_temp_key()` / `get_bed_temp_1st_layer_key()` (PrintConfig.hpp) return the `hot_plate_temp`
  pair for every plate type, so everything that looks up a plate's temperature (G-code, the bed checks,
  calibration, arrange) gets the one value. `curr_bed_type` is still chosen and still reaches the
  G-code (`bed_type_to_gcode_string`, start G-code conditionals).
- `unify_bed_temperatures()` copies the `hot_plate_temp` pair over the other five plates' keys;
  `PresetBundle::full_fff_config()` calls it, so custom G-code naming e.g. `[cool_plate_temp]` and the
  config saved into G-code and 3MF agree. (No bundled printer's G-code names a plate other than
  `hot_plate_temp`.)
- Filament settings: the Bed temperature group has one "Bed" line (first layer, other layers) on the
  `hot_plate_temp` keys; the per-plate lines and the code that showed only the current plate's line
  are gone. The tooltips no longer say 0 means "not supported on this plate".
- `scripts/tigerslicer_filaments.py` writes only the `hot_plate_temp` pair (the cool-plate column is
  gone); the library's other plate keys now come from the base templates and are overwritten when
  slicing.
- Tests: fff_print `[BedTemperature]` slices on all six plate types with the other plates set to
  31/33 °C and checks for `M190 S73` / `M140 S71` from the one setting; plus `unify_bed_temperatures()`.
  macOS: fff_print 194 of 195 pass (the failure is the known compiler-dependent `[Regression]` hash
  test), libslic3r 1,054 pass.

### Experimental features switch outside Preferences
- `GUI_App::show_experimental_features()` / `set_show_experimental_features(bool)`: the one place that
  writes `show_experimental_features`, calls `update_mode()` and schedules a reslice. The Preferences
  checkbox now calls it too.
- `ParamsPanel`: `m_experimental_btn`, a `ScalableButton` before the mode icon in the process header,
  `experimental.svg` (pink flask) when on and `experimental_off.svg` (empty flask) when off.
  `update_experimental_button()` sets the icon and tooltip; `update_mode()` calls it, so the button
  follows every other way of changing the setting.
- View menu: "Show Experimental Features", a check item after "Show 3D Navigator", checked from the
  config on every menu update.
- Verified on macOS: build, and background launches with the setting off and on (no new `[error]`
  lines, no crash). The button and menu item were not looked at on screen.

### Verification (Windows)
- Release build from the release commit (`5682da746`, tag `v0.0.11`), with a clean `build\OrcaSlicer`
  and `BUILD_TESTS` off: no errors.
  - It installed 9,319 files: the files of the shipped 0.0.10 zip, without its `uninstall.ps1`, plus
    `resources/images/experimental.svg` and `experimental_off.svg`.
  - Packaged with the `%TEMP%\tsrel011` kit: copied from tsrel010 with only the version and paths
    changed.
- Release assets:
  - `TigerSlicer-0.0.11-win64-Setup.exe`, 229,531,648 B, sha256 `4ed9d528…5ba562`.
  - `TigerSlicer-0.0.11-win64.zip`, 235,916,527 B, sha256 `80fd3b04…b40ed5`.
  - Both digests were checked on GitHub after upload.
- Tests (MSVC), fff_print:
  - `[BedTemperature]` passes: 1 case, 38 assertions.
  - `[Print]` passes: 19 cases, 114 assertions. That includes the `[Regression]` hash test, whose
    hashes are MSVC's.
- The Setup.exe test install (`TIGERSLICER_INSTALL_DIR`) has 9,320 files. Its `TigerSlicer.exe` and
  `TigerSlicer.dll` match the build.
- Smoke test of the build (the same binaries), with a new, empty data folder:
  - The first-run wizard, with only Klipper chosen and no plug-in, ends on Generic Klipper Printer
    (0.4 mm nozzle, PLA, 0.20mm Standard @MyKlipper).
  - The flask button turned experimental features on (pink), and View > Show Experimental Features
    was then checked.
  - Unchecking the menu item turned the flask grey, and Preferences > General then showed the box
    clear. Ticking it there turned the flask pink and checked the menu item again.
  - PLA's filament settings show one "Bed" line: first layer 60 °C, other layers 60 °C.
  - A 20 mm cube, sliced and exported, has `M190 S60` in the start G-code,
    `PRINT_START ... BED=60`, and `M140 S60` for the other layers. Every plate key in the config
    block is 60.
  - The log reads "Version 0.0.11 build 5682da7460".
  - Quit from the File menu, without saving the cube project: the log ends with the normal exit,
    and there is no crash log. The only `[error]` lines are four `calc_exclude_triangles`, which
    earlier releases log too.

### Verification (macOS)
- Release build on macOS 15.6.1 (Apple Silicon), Apple clang 17.0.0, from the release commit:
  `./build_release_macos.sh -a arm64 -x -s` with `BUILD_TESTS` off.
- Tests (from `b6bbbdb15`): fff_print 194 of 195 pass, including `[BedTemperature]`; the failure is the
  compiler-dependent `[Regression]` hash test (see 0.0.10). libslic3r 1,054 pass.
- Background launches with throwaway data folders on a Generic Klipper printer, a Bambu Lab X1 Carbon
  and a copy of a real 0.0.10 data folder: no new `[error]` lines, no crash. The new button, menu item
  and Bed line were not looked at on screen.

---

## 0.0.10 (2026-10-23)

Theme: small feature accuracy, all of it experimental and off unless "Show experimental features" is
on. With it off, default output is unchanged; the `[Regression]` test proves this on the moves.

Commits, oldest first (all on `main`; the release commit follows them):

| Commit | Subject |
|---|---|
| `39f94c5dd` | feat: experimental features switch |
| `a994e2d6e` | feat: deviation preview and the below-printable-width warning |
| `f61a66208` | feat: small feature policy and out-and-back tip lines |
| `1054d0fc6` | fix: small feature lines on free-standing thin walls, and none on corners |
| `595664ae7` | feat: keep narrow gaps open |
| `a7d5bba8a` | feat: small feature settings per object and per modifier |

### The experimental switch (phase 0)
- `ConfigOptionDef::experimental` (`Config.hpp`). The helpers are `experimental_options_used()`
  (the keys set to something other than their default, sorted) and `reset_experimental_options()`
  in `PrintConfig`, plus `model_has_experimental_options()` and `remove_experimental_options()` in
  `Model` (object, volume and layer range configs).
- `Print::set_experimental_features_enabled()` defaults to true, for the CLI and the tests.
  - The GUI sets it from AppConfig `show_experimental_features` (default 0) in
    `BackgroundSlicingProcess::apply()`.
  - When it is off, `Print::apply()` resets the experimental keys of the full config. It also
    applies a copy of the model with them removed, keeping the object and volume IDs.
  - A change of the flag invalidates every print and object step.
- GUI:
  - Preferences > General > "Show experimental features". Toggling it calls `update_mode()` and
    reslices.
  - `experimental_option_shown()` and `option_label()` (the "(Experimental)" badge) are in
    `Field`.
  - `OptionsGroup` keeps `m_options_experimental` parallel to `m_options_mode`. `OG_CustomCtrl`,
    `Search` and `SettingsFactory` (the object list's settings menus) hide the options with the
    switch off.
  - The tooltip gets an "Experimental: ..." line.
- `GCode::_do_export()` writes `; experimental_features_used = <list>` after "generated by" (or in
  the BTT thumbnail branch) and adds a notice.
- Tests:
  - `ExperimentalTestDef` in `test_config.cpp`.
  - In `test_print.cpp`:
    - A `MarkExperimental` RAII marks an existing option for the test.
    - The header line.
    - The forced defaults with the switch off.
    - "Default settings print the same moves as before the small feature options" `[Regression]`,
      an FNV-1a hash over the G0-G3 lines, comments stripped, for `gt2_teeth`, `overhang` and
      `cube_with_hole`, each with classic and Arachne walls.

### Deviation and the printable-width warning (phase 1)
- New module `Feature/SmallFeatures/SmallFeatures.{hpp,cpp}` (in libslic3r's CMakeLists, after
  FuzzySkin).
- `printed_coverage()` buffers every path by half its width with round ends (`etOpenRound`).
  - Arc-fitted paths are expanded back into segments by `path_points()`, from `fitting_result`.
  - `fill_expolygons` counts as covered, since sparse infill leaves gaps on purpose.
  - Paths without flow cover nothing.
- `layer_deviation()` sets undershoot = slices - covered and overshoot = covered - slices.
  - An area is dropped if an opening by 0.01 mm removes it, or if its magnitude (2·area/perimeter,
    the width of a strip) is under the 0.02 mm `kDeviationTolerance`. The magnitude rule drops the
    square corners a round bead end leaves.
- `print_deviation()` runs over the layers with tbb and merges layers by print_z. It is stored in
  `GCodeProcessorResult::deviation` at export, with experimental features on only.
- The preview's `libvgcode::EViewType::Deviation` (added before COUNT) draws the toolpaths in grey.
  `GCodeViewer::render_deviation()` draws red and blue `GLModel`s, triangulated, at the top visible
  layer, in 5 shades with limits 0.05/0.1/0.2/0.4 mm. The view list has Deviation only with the
  switch on.
- `small_feature_min_width` (FloatOrPercent, 60% of `nozzle_diameter`).
  - `PrintObject::warn_features_below_printable_width()` runs before `set_done(posPerimeters)`.
  - It counts `features_below_width()`: slices minus their miter opening, without the slivers
    shorter than they are wide. The count is from the layer with the most.
- Test data in `tests/data/small_features/`:
  - The 22-tooth GT2 pulley STEP (0.27 mm flat tips at r = 6.749).
  - The classic-wall Creality Print slice whose config the tests use.
  - The hand-made "spike teeth" G-code: 0.30 mm lines from the cusp (r 6.32) to r 6.649, out and
    back at F900.
  - None of them has a host, key or user path.

### Small feature lines (phase 2, and the fix after it)
- `PrintRegionConfig` options:
  - `small_feature_policy` (`SmallFeaturePolicy` drop/inside/balanced/outside, default drop).
  - `tip_overshoot` (0.05).
  - `thin_wall_speed` (15).
  - `thin_wall_flow_ratio` (1).
  - These are in Tab > Strength > Small features. `ConfigManipulation` shows `tip_overshoot` for
    outside only, and the speed and flow when the policy isn't drop.
- `add_small_feature_lines()` runs after the walls in `LayerRegion::make_perimeters()`, but not in
  spiral mode.
  - Uncovered = slices - (walls + gap fill + fill area), opened by 0.01 mm.
  - Only parts of the model narrower than two outer walls count: slices minus their miter opening
    by the outer wall width. More than half of an uncovered piece must lie there. This leaves out the
    corners walls round off (a miter opening gives every corner back) and the slivers between
    walls. Before the fix, the outer-wall check let corners through.
- The line for a piece:
  - **Wall line**, when the nearest wall loop to the piece's centroid is close: the line enters the
    piece within outer width + 0.02 mm of the loop. It runs straight from the loop's closest point
    through the centroid to where it leaves the piece. On a tooth that's from the cusp out to the tip.
    Its widths come from chords across the line every 20 um: the piece's for max and mean, the
    model's for the narrowest.
  - **Center line** otherwise: the longest `Geometry::MedialAxis` polyline of the piece, simplified
    by 5 um first because round bead ends break the Voronoi diagram. Failures are logged and skipped.
    Free ends are extended to the outline from themselves. Before the fix, a two-point line was
    extended from its other end, which collapsed it.
  - Why not the medial axis everywhere? A tooth tip stub is about as long as it is wide, so its medial
    axis forks to the corners, and the longest branch ran corner to corner.
- Width:
  - inside: the model's narrowest chord.
  - balanced: the mean chord.
  - outside: the widest chord.
  - Each is at least `small_feature_min_width`. A piece narrower than half of that is skipped.
- Free end:
  - inside: pulled back w/2 + 0.01.
  - balanced: pulled back 0.25·sqrt(3)·w.
  - outside: pulled back w/2 - `tip_overshoot`.
- Flow is Flow(w, h, nozzle) × `thin_wall_flow_ratio`.
- A wall line becomes the loop's `ExtrusionLoop::dead_ends` entry, which `polygons_covered_by_width`
  covers. Other lines are open paths, appended to the perimeters.
- `GCode::extrude_loop()` splices dead ends after the seam split and clipping:
  - At the nearest loop point within 0.05 mm, it prints out, then back
    (`set_force_no_extrusion`, `mm3_per_mm` 0), both at `thin_wall_speed`.
  - With a sloped seam, or when the seam gap clipped the attach point, it prints them after the loop
    with `m_wipe.reset_path()`.
- On the pulley with classic walls, outside gives 0.29 mm lines from r 6.32 to 6.65. The hand-made
  reference uses 0.30 mm lines ending at 6.649.

### Narrow gaps (phase 3)
- `narrow_gap_policy` (`NarrowGapPolicy` allow_fill/keep_open, default allow_fill) is in the Small
  features group.
- `keep_narrow_gaps_open()` runs after the small feature lines, on the walls and gap fill.
  - Gaps are `closing(lslices, line) - lslices`, where line is the larger of the outer and inner
    wall widths. That is every gap narrower than two lines.
  - A bead may be no wider than twice its distance from the gaps (exact segment-to-outline distance,
    with `AABBTreeLines::LinesDistancer`), floored to 5 um, down to `small_feature_min_width`.
    Stretches near a gap are cut every 50 um. A narrowed stretch becomes its own `ExtrusionPath`
    piece, with its flow scaled by the rounded-rectangle area.
  - Points never move, so loops stay closed and dead ends stay on their loops. A dead end stays one
    path at its narrowest width.
  - An earlier version also shifted beads away from the gap. Near slot corners the side was ambiguous,
    and shifted round ends spilled into the slot.
- Test block: 18 x 8 mm, with 0.3/0.5/0.8 mm slots and 0.25 mm fins between them. Arachne prints the
  fins at 0.38 mm by default, spilling into the slots; with keep_open, none of it spills.

### Per object and per modifier (phase 4)
- The options are `PrintRegionConfig`, so objects and modifiers already gave them regions. But
  `Layer::make_perimeters()` generates walls once for all perimeter-compatible regions, with one
  region's config (`layerm_config`). The settings must not split walls: a modifier on the teeth
  would otherwise add a wall around itself.
  - So `add_small_feature_lines()` and `keep_narrow_gaps_open()` take the compatible regions. Each
    piece (by `region_of()`: the region holding its centroid, else the one it overlaps most) and
    each narrowed path (by its middle point) uses the settings of the region it lies in.
  - `thin_wall_speed` joins `Layer::is_perimeter_compatible()`, because `GCode` reads it from the
    region that owns the walls, as it does the other wall speeds.
- `warn_features_below_printable_width()` counts each feature against the minimum width of the
  region it lies in. When the regions disagree, the message names no width.
- `SettingsFactory::PART_CATEGORY_SETTINGS` lists the six options under Quality for the object
  list's settings menu. `get_visible_options()` and `get_all_visible_options()` drop experimental
  ones with the switch off.
- Painted regions aren't supported. The paint tools set supports, seams, fuzzy skin and colours, not
  arbitrary settings; modifiers cover the same need.
- Tests:
  - The pulley with a cylinder modifier (r 7.5, the teeth) and a second one (r 4, the bore), both
    ways round. The teeth follow their modifier, nothing spills into the bore, and the layer has as
    many wall loops as without modifiers.
  - Two objects, each a block with a 0.25 mm fin: outside on one object prints its fin, the default
    on the other leaves it.

### Notes for macOS
- `far` and `near` are empty macros in the Windows headers. The Windows build broke on locals with
  those names. On macOS they're fine, but avoid them.
- No new dependencies. The module is plain libslic3r (Clipper, MedialAxis, AABBTreeLines, tbb).

### Verification (Windows)
- Tests on `a7d5bba8a`: libslic3r 1,054 cases and fff_print 194, all passing.
  - That includes `[SmallFeatures]`, 13 cases with 19,001 assertions, and `[Print]`, 18 cases with the
    regression hashes.
- The calibration builder tests are flaky, with no experimental option involved.
  - Run alone, "Layers above the tower print without its overrides" failed 2 runs in 6.
  - "Grid copies print with their own value" failed 1 run in 4.
  - One full run had a failure in the first. They are not fixed.
- Each phase commit was staged from the final files minus the later phases' changes, and checked
  for stray content. Only the tip of `main` was built and tested, not each commit on its own.
- App build (tests off): no errors.
- Checked in the dev build, on a copy of the data folder: Core One L, 0.6 mm nozzle, the pulley STEP.
  - With the switch off, the Strength page has no Small features group.
  - With it on, the group appears, each label marked "(Experimental)". Tip overshoot and the line
    speed and flow appear only once the policy prints lines.
  - Slicing with Outside gives the notices "22T_pulley.step: 30 features below printable width for a
    0.6 mm nozzle (narrower than 0.36 mm)." and "This print uses experimental features:
    small_feature_policy".
  - In the Deviation view at 5.00 mm, the toolpaths are grey and each of the 22 tips has a small blue
    overshoot, with no red. Red is left only on slivers between wall loops inside the model.
  - Not checked in the app: the object list's settings menu and modifiers, which are covered by tests
    only.
- Release build from `790cb6bee`, with a clean `build\OrcaSlicer`: no errors.
  - It installed the same 9,317 files as 0.0.9, which ships 9,318 with `uninstall.ps1`.
  - Packaged with the `%TEMP%\tsrel010` kit: copied from tsrel009 with only the version and paths
    changed.
- Release assets:
  - `TigerSlicer-0.0.10-win64-Setup.exe`, 229,535,744 B, sha256 `9766646f…f881cc`.
  - `TigerSlicer-0.0.10-win64.zip`, 235,916,518 B, sha256 `d16d8e34…3380c7`.
  - Both digests were checked on GitHub after upload.
- Smoke test: the Setup.exe test install (`TIGERSLICER_INSTALL_DIR`) on a copy of a real data folder.
  - Its `TigerSlicer.dll` matches the build.
  - Its log reads "Version 0.0.10 build 790cb6bee2".
  - It ran for 45 s with no crash log and no "can not find parent". The only errors are ones 0.0.9 also
    logs on that folder: `calc_exclude_triangles` and an unsupported printer type.
  - This run was log-only. The GUI was checked on the dev build of the same code, as above.
- Shipped Windows-only. The notes point Mac users to 0.0.9.

### Verification (macOS)
- Built from tag `v0.0.10` (`790cb6bee`) on macOS 15.6.1 (Apple Silicon) with Apple clang 17.0.0
  (clang-1700.3.19.1) and cargo 1.97.1: `./build_release_macos.sh -a arm64 -x -s` (dependencies
  unchanged since 0.0.7), after deleting `build/arm64/OrcaSlicer`, with `BUILD_TESTS` off. Clang
  compiled `Feature/SmallFeatures` and the libvgcode Deviation view without changes. Bundle
  `com.tigerslicer.TigerSlicer`, version 0.0.10.
- `TigerSlicer-0.0.10-macos-arm64.dmg`: 292,995,919 bytes, sha256
  `aa21f07606a9b580b6e209e1745921f14ad80080c84d653d09acc7b3d9f7ec8c`; packaged as in
  [BUILDING.md](BUILDING.md) and uploaded to the release afterwards (digest checked).
- The app copied out of the mounted dmg and started with a throwaway `--datadir` logs
  "Current OrcaSlicer Version 0.0.10 build 790cb6bee". No crash log. `[error]` lines are the same as
  0.0.9's: the missing 3Dconnexion driver and "Unable to create exclude triangles" (twice). The
  Preferences switch and the Small features group were not checked by hand (no access to the window
  from the build machine).
- Tests (built from the tag with `BUILD_TESTS` on): libslic3r 1,054 cases pass; fff_print
  `[SmallFeatures]` 13 cases pass (18,978 assertions); `[Print]` 17 of 18 pass. The failing case is
  "Default settings print the same moves as before the small feature options" `[Regression]`: all six
  hashes differ from the stored ones on macOS. Built on macOS at `39f94c5dd`, which has the same test
  and hashes but none of the small feature code, the test gives the same six hashes as `v0.0.10`:

  | Mesh | Walls | macOS hash |
  |---|---|---|
  | gt2_teeth | classic | `0xe9464f6fc4ba073c` |
  | gt2_teeth | arachne | `0xb8802272c06a4761` |
  | overhang | classic | `0x56f88ec6dc0eb6dc` |
  | overhang | arachne | `0xc5d6a48e2c6b8f73` |
  | cube_with_hole | classic | `0xf0a9332a354e9f24` |
  | cube_with_hole | arachne | `0x0b6a5898c900e2a5` |

  So default moves are unchanged on macOS too; the stored hashes are MSVC's, and clang's
  floating-point results (fused multiply-add on arm64) differ in the last digits. The test needs a
  hash per compiler (or a tolerance) to pass on both.

---

## 0.0.9 (2026-10-22)

Theme: fin supports that hold a tilted part the way support fins should, and more settings per model.

Commits, oldest first (all on `main`; the release commit follows them):

| Commit | Subject |
|---|---|
| `de875917b` | docs: Windows verification for 0.0.8 |
| `9e477a03e` | feat: set more process settings per model |
| `a9a9e8fa4` | feat: Fins (manual), fins along the lean, and fin spacing in mm or % |
| `02aab9934` | fix: the Auto slice button is a whole pill |

### Fins (manual)
- `SupportType` gains `stFinsManual`, saved as `fins(manual)` and appended after `stFins`. `fins`
  stays the key of Fins (auto), so existing presets and projects load as before. `is_fins()` covers
  both. Every former `== stFins` check now uses it: `ModelArrange`, `Print::validate`,
  `PrintObject::_generate_support_material`, `SupportParameters` and `ConfigManipulation`.
- `fin_fallback_support_type()` (`PrintConfig.hpp`) gives the supplemental supports' type. They are
  `stTree`/`stNormal` (manual) with Fins (manual), so they only support enforced overhangs. It is
  used by the generator and by the GUI's field toggles.
- `generate_fin_support()`, when manual:
  - The enforcers are `slice_support_enforcers()` plus the painted ENFORCER facets. With none it
    returns at once.
  - Each layer's fin region is intersected with the union of the enforcers at or above it, which is
    their projection to the bed.
  - The uncovered overhangs handed to the supplemental supports are only the enforced ones.
  - Lean side only is skipped (and hidden in the GUI), because the painted area picks the side.
- Fix, seen in the app with Fins (manual) and painted enforcers:
  - Tree support (`TreeSupport3D::generate_overhangs`) subtracts blockers from its overhangs, then
    adds the enforced overhangs back. Normal support also lets enforcers win.
  - So the fin columns, which reach the supplemental generator as blockers, did not stop it on
    enforced areas, and trees grew where the fins were. The same happened with Fins (auto) and
    painted enforcers.
  - While `m_fin_support_reserved` is set, `PrintObject::slice_support_enforcers()` (moved out of
    line) and the ENFORCER branch of `project_and_append_custom_facets()` now subtract the reserved
    columns.
- Tests (`[Fins]` in `test_support_material.cpp`):
  - An enforcer volume over one side of the support table, with no supplemental supports and with
    tree.
  - A painted underside face of the tipped cube, with none and with tree (the latter failed
    before the fix).
  - No enforcer gives no support.
  - Fins (auto) ignores enforcers.

### Fins run the way the part leans
- Seen in the app on a hex bar tilted along its length: the fins stood across the bar, two short
  walls side by side, instead of one wall down its length. `fin_angle()` ran the fins across the
  long axis of the fin regions. That suits a block tipped onto an edge, whose regions are long along
  the edge, but the shadow of a long tilted part is long along the lean, so the fins came out across
  it, where a fin is weakest against tipping.
- `fin_angle()` now runs the fins along the lean, from the center of the bed contact to the center
  of mass (`object_lean()`, shared with `keep_lean_side()`).
  - This applies only when the part would tip over: its center of mass is outside the convex hull of
    its bed contact (`FinLean::tips_over`), and at least `kMinLean` (0.5 mm) from the contact's center.
  - A part that stands on its own keeps the long-axis rule. That covers a big part slightly off
    center and a block tipped exactly onto an edge, which are unchanged.
- The strip grid used to be anchored at the origin, so where the fins fell across a part was luck.
  `fin_strips_offset()` puts one strip through the area centroid of the lowest fin region (which
  holds every column), and the rest every pitch either side. A part narrower than two pitches gets
  one fin down its middle, where a round or tilted part hangs lowest. (Choosing between one fin in
  the middle and two either side by the region's width was tried first. It gave the 14 mm pulley two
  fins under its sloping sides, because the shadow of a round part is as wide as the part.) Cross
  fins are placed the same way.
- With tree or normal supports filling in (the default), the pulley's one fin was still cut into
  pieces. The columns kept free for them (the "uncovered" overhangs) took any layer reaching more
  than half a support line past the one below. On a part tilted 40 degrees every slope does that
  (0.24 mm per 0.2 mm layer), the roof of its bore included, so columns were cut through the fin
  for overhangs the tree support, at a 30 degree threshold, then did not support at all.
  `overhang_threshold_offset()` now uses the supplemental supports' own lower layer offset: the
  threshold angle (+1 degree, as they do), or the threshold overlap when the angle is 0.
  - Half a line is kept wherever tree and normal supports ignore the threshold: in Fins (manual), on
    enforced overhangs (enforcers are now read in auto mode too, when there are supplemental
    supports), and in the first `enforce_support_layers` layers.
  - Without the enforced case, a painted enforcer inside the part (the roof of a bore) was dropped:
    the reserved fin columns covered it, and `slice_support_enforcers()` subtracts them.
- The per-island fallback fin (a column the grid misses) is now also added when a grid fin only
  grazes the column, narrower than `min_width`, since `opening()` would remove that graze.
- A column kept for an overhang inside the part still runs straight down to the bed, through the
  part and any fin under it, because `m_fin_support_reserved` is one set of polygons for every
  layer. Stopping it at the part would need that set per layer.
- Tests:
  - A 50 x 8 x 8 mm bar tilted 35 degrees along its length, along X and turned 30 degrees, gets one
    fin, along the bar and through its center line.
  - The same bar with a 4 x 4 mm bore, tilted 35 degrees, with tree fallback at a 30 degree
    threshold, keeps one whole fin above its end face (two pieces before the threshold change).
  - With the bore's roof painted as an enforcer and normal fallback, there is support inside the bore.

### More settings per object
- The Objects view (`TabPrintObject`) shows every print option that is in `PrintObjectConfig` or
  `PrintRegionConfig`. About 300 of the ~390 process options already were. Of the rest, these move
  from `PrintConfig` to `PrintObjectConfig`:
  - `initial_layer_speed` and `initial_layer_infill_speed`
  - `initial_layer_line_width`
  - `slow_down_layers`
  - `reduce_crossing_wall` and `max_travel_detour_distance`
  - `reduce_infill_retraction`
- Everything else in `PrintConfig`/`GCodeConfig` is shared by the whole plate, so it stays global:
  - Print sequence and order, first layer height, skirt and draft shield.
  - Prime/wipe tower, spiral vase, timelapse, travel speeds, pressure equalizer, arc fitting.
  - G-code output: labels, comments, filename, post-processing, notes.
- How each moved option reaches the object:
  - The G-code options (speeds, slow layers, avoid crossing walls, infill retraction) are read from
    `GCode::m_config`, which already applies each object's config before printing it.
  - First layer width: `PrintRegion::flow()`, `support_material_1st_layer_flow()`, `ToolOrdering` and the
    G-code header read the object's config. The skirt and the brim (`Print::skirt_flow()`,
    `brim_flow()`, `object_skirt_offset()`) are shared, so they use `m_default_object_config`.
  - The wipe towers take the global first layer speed: `WipeTower::set_first_layer_speed()`, and a
    `default_object_config` argument to `WipeTower2`'s constructor.
- `GCode::process_layer()` now applies an object's config before its skirt group and brim, not after.
  Before, the per-object skirt and brim printed with whatever the previous object left in `m_config`.
  That was already wrong for `support_speed`, which the brim uses.
- Invalidation moved from `Print::invalidate_state_by_config_options()` to `PrintObject`'s with the same
  steps:
  - `initial_layer_line_width`: perimeters, infill, support and skirt/brim.
  - The two speeds: wipe tower, skirt/brim and G-code.
  - The rest: G-code export only.
- Changing the global first layer width or speed when every object overrides it changes no object's config,
  so no object diff invalidates anything. But the skirt, brim and wipe tower use the global value, so
  `Print::apply()` invalidates `psSkirtBrim` and `psWipeTower` whenever either is in `object_diff`. Any
  global change already invalidates G-code export.
- The combined skirt goes round every object. It now prints with the default object config, applied just
  for it: first layer speed and slow layers are global.
  - Before, it took them from `layer.object()`, or on later extruders from whichever object was printed
    last.
  - A brim or skirt group shared by several objects takes the first-printed object's settings.
- `Print::validate()` checks each object's `initial_layer_line_width` against the first layer height and
  the nozzle, like `line_width`. `PrintConfigDef::validate()` only sees the global value.
- `print_flow_ratio` is a region option. An object's walls, infill and surfaces use its value. Its
  support, brim and skirt print with the default region config, so they use the global one (1 unless
  changed).
- `print_flow_ratio` (a `PrintRegionConfig` multiplier on the filament flow ratio, hidden in the
  global tab) is added to the Objects view's Frequent page. Its tooltip now says what it is.
- Tests (`[PrintObject]`):
  - The moved options are object options, and plate-wide ones are still `PrintConfig`.
  - Two cubes side by side, one overridden: per-object first layer speed and slow layers in the
    G-code, and the per-object first layer width in `PrintRegion::flow()`.
  - Both cubes overridden: changing the global first layer width invalidates and redoes the skirt.
- Reviewed. No critical or high findings. The two medium ones are fixed above: the stale
  skirt/brim/wipe tower, and the combined skirt's settings. Range check and the flow ratio note
  came from the low ones.

### Fin and tine spacing in mm or %
- `support_fin_spacing` and `support_fin_tine_spacing` are `coFloatOrPercent` ("mm or %", like
  `support_threshold_overlap`). A number without % still reads as mm, so presets and 3MF projects
  that saved 8 or 5 load unchanged.
- Fin spacing as a percentage is a density: `fin_pitch()` sets the center-to-center distance to
  thickness / density, using the thickness after rounding to whole loops. 100% is solid.
- Tine spacing as a percentage is the share of the fin's height in tine rows: `tine_row_spacing()`
  returns `layer_height` / density, so 4% at 0.2 mm layers is a row every 5 mm. Thinner layers give
  more, thinner tines, for about the same bonded area.
- Defaults: 15% (about 10 mm apart for 1.6 mm fins, 9.5 mm for the 1.43 mm they round to with a
  0.4 mm line) and 4%. `max_literal` 50 keeps mm values up to 50 from asking "is it % or mm?".
- Test: 30% gives about twice the fin area of 15% on the support table, and 4% tines at 0.2 mm
  layers are the same rows as 5 mm.

### Verification (Windows)
- Tests before the release commit: libslic3r 1,053 cases, fff_print 178 (all `[Fins]` and
  `[PrintObject]` cases included), all passing.
  - "Grid copies print with their own value" (`test_calibration_builder.cpp`, from 0.0.8) failed 1 run
    in 5 when run alone during this work, with no fin code involved: `extruded.size()` was 1, not 2.
    It is not fixed. It uses `init_print` with arrange on an infinite bed.
- Fins in the app were checked by slicing the reported part (a 34 mm hex pulley with a bore, tilted
  40 degrees) in a throwaway test and drawing the fins:
  - Before, the bed view showed two short fins across the part, plus trees.
  - Now there is one fin down its length, in one piece with tree fallback on, and with tines along the
    sloped edge.
- Release build from `01e59c542`, with a clean `build\OrcaSlicer`: no errors.
  - Packaged with the `%TEMP%\tsrel009` kit (from tsrel008, only the version and paths changed).
  - 9,318 files.
- Release assets:
  - `TigerSlicer-0.0.9-win64-Setup.exe`, 229,474,304 B, sha256 `32076354…0750e3`.
  - `TigerSlicer-0.0.9-win64.zip`, 235,858,345 B, sha256 `eef28026…758670`.
  - Both digests were checked on GitHub after upload.
- Smoke test: the Setup.exe test install (`TIGERSLICER_INSTALL_DIR`) on a copy of a real data folder.
  - Its log reads "Version 0.0.9 build 01e59c542".
  - It ran for 45 s with no crash log. The only errors are ones 0.0.8 also logs on that folder:
    `calc_exclude_triangles` and an unsupported printer type.
  - This run was log-only, without clicking through.
- Installed over the 0.0.8 files on the maintainer's PC with the Setup.exe. The uninstall entry (which still said 0.0.6) now reads
  0.0.9, and the installed `TigerSlicer.dll` hash matches the build.

### Verification (macOS)
- Built from tag `v0.0.9` (`01e59c542`) on macOS 15.6.1 (Apple Silicon) with Apple clang 17.0.0
  (clang-1700.3.19.1) and cargo 1.97.1: `./build_release_macos.sh -a arm64 -x -s` (dependencies
  unchanged since 0.0.7), after deleting `build/arm64/OrcaSlicer`. Bundle `com.tigerslicer.TigerSlicer`,
  version 0.0.9.
- `TigerSlicer-0.0.9-macos-arm64.dmg`: 291,051,315 bytes, sha256 `176de7efdb0d56f1130322e105ae275c587b598fe93321d4953cbdac0bb56b15`; packaged as in
  [BUILDING.md](BUILDING.md) and uploaded to the release afterwards (digest checked).
- The app copied out of the mounted dmg and started with a throwaway `--datadir` logs
  "Current OrcaSlicer Version 0.0.9 build 01e59c542". No crash log. `[error]` lines: the missing
  3Dconnexion driver and "Unable to create exclude triangles" (twice), both on every launch.

---

## 0.0.8 (2026-10-21)

Theme: one calibration builder instead of the separate calibration tests, and independent tool layer
heights and colour mixing where people can find them.

Commits, oldest first (all on `main`; the release commit follows them):

| Commit | Subject |
|---|---|
| `0f1a32b1b` | docs: Windows verification for 0.0.7 |
| `a5e8f27b5` | chore(github): tidy workflows and issue templates for a public repo |
| `3f7dcf90d` | docs: README, build guide and community files for going public |
| `381d22b18` | chore: remove unused OrcaSlicer leftovers |
| `15ff2a3bc` | feat: show independent tool layer heights and a Color mixing button |
| `6411a2745` | feat: calibration builder replaces the separate calibration tests |
| `92409623a` | fix: calibration builder fixes from review and app testing |
| `019d95d10` | fix: keep a printer maker's profiles that your own presets are based on |

### Unselected printer makers keep their system profiles
- `PresetUpdater::priv::check_installed_vendor_profiles()` runs on every launch. Since 0.0.1 (the
  `if (true || enabled_config_update)` from `340f13831`), it deleted `system/<vendor>.json` and
  `system/<vendor>/` for every vendor in `resources/profiles` that had no model selected in the app
  config's `models`.
- User presets that inherit from that vendor then fail to load ("can not find parent …") and
  disappear.
- Found in the 0.0.8 smoke test, on a copy of a real data folder:
  - One vendor is selected in `models`. A user printer and two process presets were made from
    another vendor's printer, whose `system/` profiles were installed earlier.
  - Copies opened in 0.0.6, 0.0.7 and 0.0.8 all lost that vendor's system profiles.
  - That folder's own 0.0.6 runs never did. Their check took 25 ms and updated nothing, not even
    Custom and Prusa at 2.4.0.3 against 2.4.0.4 in resources. That was not explained.
- Unselected vendors now keep their profiles, and they are not updated. Their printers stay hidden,
  because visibility follows the selected models.

### Independent tool layer heights and colour mixing, easier to find
- The ITLH options already existed in `PrintConfig.cpp` (upstream), but no settings page showed them.
  `Tab.cpp` now does:
  - `independent_tool_layer_heights` in Process → Quality → Layer height.
  - `multi_rate_layer_height_capable` in Printer → Basic information → Advanced.
  - An "Independent layer height" group on each extruder page: `extruder_preferred_layer_height`,
    `extruder_layer_height_ratio` and `extruder_independent_{walls,infill,solid_infill,support}`,
    added as `(key, "", extruder_idx)`.
- These options, and the permission lambda, move from `comExpert` to `comAdvanced`.
- Full Spectrum colour mixing was only in the View menu. The sidebar's Filament header now has a
  "Color mixing" button (`m_color_mixing_btn`, `Plater.cpp`). It calls
  `Plater::open_full_spectrum_dialog()`. `Sidebar::show_SEMM_buttons()` shows it when the printer
  has two or more nozzles: `FullSpectrumDialog` counts `nozzle_diameter`, so a single-nozzle
  multi-material printer (SEMM) only gets "needs at least two physical extruders". It is also
  excluded from the header's click-to-collapse area, like the other header buttons.

### Calibration builder
- The menu calibrations are replaced by one builder. Removed:
  - `GUI/calib_dlg.{hpp,cpp}`.
  - Plater's `calib_pa`, `_calib_pa_pattern*`, `_calib_pa_tower`, `calib_flowrate`, `calib_temp`,
    `calib_max_vol_speed`, `calib_retraction`, `calib_VFA`, `calib_input_shaping_{freq,damp}` and
    `Calib_Cornering`. Also the PA-pattern regeneration in `reslice()`, `set_config_values` and
    `has_junction_deviation`.
  - The models only those used: `calib/cornering/SCV-V2.drc`, `input_shaping/*.drc`,
    `pressure_advance/tower_with_seam.drc`, `temperature_tower/temperature_tower.drc`,
    `filament_flow/Orca-LinearFlow*.3mf` and `pass1.3mf`.
- Kept on purpose: the Bambu device Calibration tab (`CalibUtils`, `CalibrationWizard*`). It still
  uses every old `CalibMode` and the per-layer cases in `GCode::process_layer`, plus the models
  `flowrate-test-pass{1,2}.3mf`, `auto_pa_line_*.3mf`, `pa_pattern.3mf`,
  `pressure_advance_test.drc`, `retraction_tower.drc`, `vfa.drc` and `SpeedTestStructure.drc`.
- `libslic3r/CalibrationBuilder.{hpp,cpp}` (`Slic3r::calib_builder`) has no GUI, so it is tested
  directly:
  - `Setting`: temperature, PA, retraction length and speed, flow ratio, print speed, acceleration,
    jerk, junction deviation, and input shaping frequency and damping. New settings go at the end
    of the enum (`Calib_Params` stores it as an int); `settings()` sets the dialog's order.
  - `info()`: each setting's label, unit, decimals, default range and limits, and whether a grid
    is allowed. Grids are allowed for flow ratio, print speed and acceleration only, because those
    are region or object options; the rest are global and need a tower. The lowest accepted values
    are ones the writer sends: `set_jerk_xy` skips jerk under 0.01, and `set_input_shaping` skips
    a frequency or damping of 0.
  - `sweep()` is inclusive, works in either direction, and allows at most 40 values. It counts the
    steps as a double first, so a tiny step over a huge range is refused instead of wrapping.
    `band_at(z, band, count)` picks the band.
  - `fits_decimals()`: the dialog refuses a from, to or step with more decimals than the setting
    shows, so the label on the model is the value sent (a 2.5 °C step would print 193 as 192).
  - `printer_problem()`: jerk and junction deviation are exclusive. Marlin 2 with
    `machine_max_junction_deviation` above 0 ignores M205 X/Y, and `set_junction_deviation` sends
    nothing on other firmware. Input shaping needs Klipper, RRF or Marlin 2, because
    `set_input_shaping` throws on the others.
  - `make_tower()`: 20×20 mm blocks with a groove between bands, and the value as raised
    seven-segment digits on the −Y face. `make_tile()`: a 22×22×4 mm tile with the value on top.
  - `object_overrides()`: per-object options for flow, speed and acceleration.
  - `prepare_profiles()`: per-setting preset changes. For example, PA turns pressure advance on and
    adaptive PA off. Speed raises the volumetric and machine speed limits to the largest value.
    Acceleration raises the machine acceleration limits. Jerk and input shaping use the
    ringing-tower profile (vase mode, one wall).
  - `Setup` round-trips through JSON for the dialog's presets.
- Towers use the new `CalibMode::Calib_Builder_Tower`. `Calib_Params` gains `builder_setting`,
  `builder_band_height`, `builder_values` and `builder_object`, the tower's `ObjectID`.
  `GCode::apply_calibration_band()` runs after the second-layer block of `process_layer`:
  - It acts only on layers where one of the `LayerToPrint`s belongs to that model object
    (`Print::apply` copies objects with their IDs). On other layers it clears `m_calib_config`:
    above a short tower, after it in print-by-object order, or once the tower is deleted. Objects
    printed on the tower's layers share its settings, which is why the legend says to print it
    alone. Printer commands (temperature and so on) are not undone after the tower.
  - `new_project()` also resets the calibration params. Before, only `load_files` did, so a new
    project with a primitive or a gallery shape kept the old tower's mode.
  - Bands count from the tower's bottom: `z - slicing_parameters().object_print_z_min`, so a raft
    does not shift them.
  - Flow, speed and acceleration go into `m_calib_config` on every layer. `_extrude` applies them,
    as the old flow and VFA tests did. They are set key by key, because `m_calib_config` has no
    definitions, so `apply()` throws.
  - The other settings send one command when a band starts: `set_temperature`,
    `set_pressure_advance`, the retraction length or speed in `m_writer.config`, `set_jerk_xy`,
    `set_junction_deviation`, or `set_input_shaping`. On Klipper, the input shaping tower also
    sends `MINIMUM_CRUISE_RATIO=0` once.
  - `m_calib_builder_band` is reset with `m_calib_config`.
- Grids add one tile per value (or the user's model). Each copy is named after its value and gets
  `object_overrides()` in its object config, then the plate is arranged. No calibration mode is set.
- `GUI/CalibrationBuilderDialog.{hpp,cpp}` collects the setup:
  - setting, from/to/step (with the value count, or why the printer cannot run the test);
  - tower or grid, and the band height (4 to 30 mm; lower leaves no gap between the digits'
    strokes);
  - built-in or own model, and an optional layer height (0.01 to 2 mm) and print speed.
  - Ranges are checked as "inside", so NaN fails them.
  - Presets are JSON in the app config section `calibration_builder`.
  - It returns `wxID_OK` and `setup()`. MainFrame then calls `Plater::calib_builder()`, outside the
    dialog's event handler.
- `Plater::calib_builder()`, FFF printers only:
  - Reads the user's model first (STL, 3MF or OBJ, through `Model::read_from_file`, the first
    object of the file). A file that cannot be read shows an error and changes nothing.
    `LoadStrategy::LoadModel` is needed, because without it the 3MF reader loads no objects.
  - Builds the objects in memory, a generated mesh or a copy of the user's object, and sets their
    config there: the tower's brim, or each grid copy's `object_overrides()`.
  - Only then starts a new project and adds them with `load_model_objects()`.
  - Checks the printer again, because discarding the edited presets in New Project can change it.
  - Then edits the selected presets.
  - Shows the band legend as a notification. If the user's own model is shorter than all the
    bands, the notification says so.
  - The edited presets stay edited, as with OrcaSlicer's calibrations. The legend says so, and
    the next new project asks whether to keep or discard them.
- `new_project()` (upstream): `plates_custom_gcodes` and `calib_pa_pattern` were cleared before the
  save prompt. Cancel lost them from the open project, and Yes saved it without them. They are now
  cleared after the prompt.
- The dialog opens with the last setup generated (app config `calibration_builder_last`).
- Both Calibration menus (top bar and macOS) now have one item, "Calibration builder...". It is
  disabled for resin printers. The hint in `hints.ini` is rewritten.
- Top bar fix (Windows and Linux): Undo, Redo and Calibration are enabled by the page change
  handler, on Prepare only. With no Home tab, the app starts on Prepare and no page change
  fires, so all three stayed disabled until the user switched tabs. `MainFrame::select_tab()`
  now sets them for the page it leaves selected.
- Tests, all tagged `[CalibrationBuilder]`:
  - `tests/libslic3r/test_calibration_builder.cpp`: sweep, decimals, printer checks, bands, meshes,
    overrides, profiles, JSON.
  - `tests/fff_print/test_calibration_builder.cpp`: sliced towers for temperature, PA, retraction,
    flow, speed, acceleration, square corner velocity, junction deviation and input shaper. Also
    two grid copies with their own flow ratio, a tower on a raft, no change without a tower, and
    none once the tower is gone. The speed and acceleration checks follow the
  feedrate and `ACCEL` in force at each extrusion, because Orca often sets F on a `G1 F` line of
  its own and travels use their own acceleration.

### Verification (Windows)
- Release build (MSVC 17.14). `libslic3r_tests`: 1053 cases pass, 13 of them `[CalibrationBuilder]`.
  `fff_print_tests`: 165 cases pass, 12 of them `[CalibrationBuilder]`.
- Dev build with a throwaway `--datadir`, on 2026-10-20:
  - With the Elegoo Mars 3 selected, Calibration is disabled.
  - On the Generic Klipper Printer, the dialog lists the 11 settings. Junction deviation shows the
    printer warning.
  - Temperature tower (230 to 190, step 5, 5 mm bands): 20 × 20.6 × 45 mm, named "Nozzle
    temperature tower", band legend shown. It sliced to 561 layers. The exported G-code has 9
    markers and 9 `M104` lines, 225 at Z 5.08 and 220 at Z 10.04, with nothing in between.
  - Preset "Flow grid" saved, then loaded back with its setting, range and layout.
  - Flow ratio grid: 11 tiles named "Flow ratio 0.9" to "Flow ratio 1.1", arranged.
  - In the grid's G-code, the last tile printed at 1.0. That came from the test, not the builder:
    clicking an object's settings icon in the object list resets its settings ("Click the icon to
    reset all settings of the object").
  - Untouched, the grid prints at 0.900, 0.920, … 1.100 of the 1.0 tile's plastic, all 11 tiles.
- 2026-10-21, after the review fixes:
  - Calibration is enabled at startup.
  - The STL/3MF/OBJ picker works, and a 20 mm cube as the tower model gets "only reaches 4 of the
    9 values".
  - Cancel on the save prompt keeps the project.
  - The ITLH options show in Advanced mode.
  - The Color mixing button appeared with two filaments on a single-nozzle printer, where the
    dialog cannot mix, so it now needs two nozzles.
- Also found and fixed in those runs:
  - The red reason under the range was cut off. Wrapping it and re-fitting the dialog widened the
    dialog and lost the model radio buttons' paint, so every reason is now one short line instead.
  - The top bar bug described above.
  - The one-line reasons were not seen in the app: a Windows TextInputHost window blocked clicks.
- Release build from `9ad906c5a`, with a clean `build\OrcaSlicer`: no errors.
  - Packaged with the `%TEMP%\tsrel008` kit (from tsrel007, only the version and paths changed).
  - 9,318 files. The removed calibration models are gone; the ones the Bambu tab uses are kept.
  - Also 38 empty files that the 0.0.7 zip lacked (Python `__init__.py` files, upstream
    placeholders).
- Release assets:
  - `TigerSlicer-0.0.8-win64-Setup.exe`, 229,470,208 B, sha256 `14f9877d…f6edbd`.
  - `TigerSlicer-0.0.8-win64.zip`, 235,856,554 B, sha256 `4d9a228e…d6ac8e`.
  - Both digests were checked on GitHub after upload.
- Smoke test: the Setup.exe test install (`TIGERSLICER_INSTALL_DIR`) on a copy of a real 0.0.6 data
  folder.
  - Its log reads "Version 0.0.8 build 9ad906c5a".
  - The user printer and process presets on the unselected vendor load. Before
    `019d95d10`, they failed with "can not find parent".
  - No crash log, and no errors that the 0.0.6 runs of that folder don't also log:
    `calc_exclude_triangles` and an unsupported printer type.
  - This run was log-only, without clicking through.

### Verification (macOS)
- Built from tag `v0.0.8` (`9ad906c5a`) on macOS 15.6.1 (Apple Silicon) with Apple clang 17.0.0
  (clang-1700.3.19.1) and cargo 1.97.1: `./build_release_macos.sh -a arm64 -x -s` (dependencies
  unchanged since 0.0.7), after deleting `build/arm64/OrcaSlicer`. Bundle `com.tigerslicer.TigerSlicer`,
  version 0.0.8.
- `TigerSlicer-0.0.8-macos-arm64.dmg`: 291,977,838 bytes, sha256 `b5dc55389e214a74f5cd41c4308a71a3f3f2419580f44daee17aada72905246f`; packaged as in
  [BUILDING.md](BUILDING.md) and uploaded to the release afterwards (digest checked).
- The app copied out of the mounted dmg and started with a throwaway `--datadir` logs
  "Current OrcaSlicer Version 0.0.8 build 9ad906c5a". No crash log. `[error]` lines: the missing
  3Dconnexion driver and "Unable to create exclude triangles" (twice), both on every launch.

---

## 0.0.7 (2026-10-20)

Theme: Auto slice, automatic hollowing for resin, brick layers and fin supports for filament, and the
last OrcaSlicer branding.

Commits, oldest first (all on `main`; the release commit follows them):

| Commit | Subject |
|---|---|
| `f84322c33` | fix(filaments): list the filament library in table order |
| `b072ff5fd` | fix(resin): stop copying the resin bundle into the data folder |
| `d426fe99c` | docs: development notes for the filament order and resin log fixes |
| `d121c7555` | feat(branding): remove the remaining OrcaSlicer branding |
| `19dd9ccd0` | fix(resin): arranging with a resin printer crashed |
| `49d31da93` | fix(resin): name the support options in the Supports workspace |
| `f9beec286` | feat: Auto slice, and automatic hollowing and drain holes for resin |
| `a25cea376` | docs: changelog and development notes for Auto slice, hollowing and the branding cleanup |
| `6d29d6472` | feat(branding): remove OrcaSlicer's Orca Arena test printer and unused Orca assets |
| `f9a0f9592` | docs: note the Orca Arena removal |
| `ad085f617` | feat: brick layers for filament prints |
| `ae504d6a9` | feat(resin): real printer photos, a shorter list, and filament/resin tabs |
| `3cb2bf668` | feat: fin supports, with optional fin holes |

### Auto slice
- `libslic3r/AutoSlice.{hpp,cpp}` holds the choices, so they are tested without the GUI
  (`tests/libslic3r/test_auto_slice.cpp`, `[AutoSlice]`):
  - `process_candidates()`: the visible process presets compatible with the selected printer. Only
    system presets when the printer has any.
  - `pick_process()`: the preset whose layer height is nearest 0.3 (Quality), 0.5 (Structural) or
    0.7 (Fast) × the nozzle diameter. Structural keeps to presets named "Strength" when the printer
    has them. For the Custom printers with a 0.4 mm nozzle this gives 0.12 mm Fine, 0.20 mm Standard
    and 0.28 mm Extra Draft. Only the preset changes; walls and infill are the profile's own.
  - `resin_support_settings()` / `apply_resin_support_settings()`: Low / Medium / High. These are
    the Supports workspace's former Light / Medium / Heavy values, which now come from here: pillar,
    head front diameter, head width, point density and contact penetration.
  - `can_hollow()`: whether `sla::generate_interior()` leaves an interior with the hollowing
    settings.
  - `place_drain_holes()`: up to two holes in mesh coordinates, inward normals, starting
    `sla::HoleStickOutLength` outside the surface.
    - Candidates are downward-facing triangles (world normal z < -0.7) within max(1 mm, radius) of
      the object's lowest point.
    - Candidates closer than radius + 0.5 mm to that band's outline are dropped, unless that drops
      them all.
    - The first hole is the candidate nearest the band's area-weighted centre; the next is the one
      farthest from the first, at least 2 × radius + 2 mm away.
    - Mirrored instances flip the winding.
- GUI:
  - `MainFrame::create_side_tools()` adds `m_auto_slice_btn` left of Slice. Its `SidePopup` offers
    Structural / Fast / Quality (filament), or Low / Medium / High supports plus a
    "Hollow + drain holes: On/Off" toggle stored in the app config key `auto_slice_hollow` (resin).
  - The Slice button's handler is now `MainFrame::start_slicing(bool all_plates)`, shared with
    Auto slice; the manual Slice button first calls `Plater::cancel_auto_slice()`.
  - `fit_tab_labels()` measures against the Auto slice button, now the leftmost side tool.
- Filament (`Plater::auto_slice_fff`):
  - Select the picked preset through the print tab. A kept unsaved change stops here.
  - Set `enable_support = false` and erase `support_type` on every object of the current plate,
    then slice. With supports off, `PrintObject::generate_support_material()` runs
    `is_support_necessary()` and adds a `SlicingNeedSupportOn` warning to objects with floating
    regions or cantilevers over 6 mm.
  - `Plater::priv::on_process_completed` hands over to `on_auto_slice_finished()` by `CallAfter`.
    That function turns tree(auto) supports on, per object, for the flagged objects and slices
    again (only the support step and later re-run).
  - A plate whose result already matched the settings does not slice again. A `CallAfter` queued on
    the plater behind the slice request catches that case: no slice running and a valid result.
  - The per-object support settings replace whatever the objects had.
- Resin (`Plater::auto_slice_resin`):
  - Write the level into the edited SLA print preset, as the workspace presets do.
  - Optionally run `auto_hollow_and_drain()`.
  - `prepare_msla_supports(quiet, &failed)`: quiet means no dialog when nothing needs a support;
    a failed generation stops Auto slice.
  - Slice.
- `Plater::auto_hollow_and_drain()` (also the Supports workspace's "Hollow + drain holes" button):
  - Objects whose mesh is closed and can be hollowed get a per-object `hollowing_enable = true`.
    Those without drain holes get two, 4 mm wide (or the workspace's size) and deeper than the wall
    plus 2 mm.
  - Open or too-thin objects get `hollowing_enable = false` when it would otherwise be on, because
    hollowing them fails the slice.
  - Holes follow the first instance's orientation; they are stored per object.

### Fixed
- Arranging with a resin printer crashed. `get_instance_arrange_poly()` (`ModelArrange.cpp`) and
  `ArrangeJob` read the FDM-only `enable_support`, `support_type` and `enable_wrapping_detection`
  without null checks, and a resin configuration has none of them. Upstream never arranges with an
  SLA configuration. Regression test in `test_arrange.cpp`.
- The SLA support options shown in the Supports workspace had no labels (Orca commented them out):
  `support_points_density_relative`, `support_head_front_diameter`, `support_head_width`,
  `support_pillar_diameter`, `support_base_height` and `support_base_safety_distance`. Labels and
  tooltips follow PrusaSlicer.

### Branding
- The Custom printers' bed texture is `Custom/tigerslicer_bed_texture.svg`, made by
  `generate_tigerslicer_branding.py` (`bed_texture_svg()`) with the same canvas, corner marks and
  logo position as Orca's whale. The Custom bundle is bumped to 02.04.00.05 so installed copies
  pick it up.
- Online help removed (the settings documented on orcaslicer.com):
  - `OptionsGroup::get_url()` returns nothing, and setting labels are never links.
  - Preferences labels are plain.
  - The "Wiki Guide" links are gone from the calibration dialogs, the OBJ and STEP import dialogs,
    Troubleshoot, Unsaved changes and the AMS dialog.
  - The Calibration Guide menu items are gone.
  - The Bambu device dialogs' wiki links are hidden.
  - The five links are gone from `hints.ini`.
- Handy models:
  - The files are renamed (CalibrationCube, CalibrationPlug, ToleranceTest, StringHell), because
    object names come from them.
  - Orca's whale combo and badge are no longer in the menu. The files are still in the package.
- Strings:
  - The 3MF "old OrcaSlicer version" message is the generic "older version" one.
  - The 2.3.1-alpha warning no longer names OrcaSlicer, and its translations are cleared so they
    show in English.
  - "OrcaCloud plugins" is now "Cloud plugins".
  - The command-line config_compatibility help says TigerSlicer.
  - The plugin sign-in errors are neutral.
  - The Python plugin docstrings say TigerSlicer.
  - The About dialog drops OrcaSlicer's promotional paragraph and keeps the credit.
- Moonraker sees the user agent "TigerSlicer". Obico, SimplyPrint and 3DPrinterOS identifiers are
  unchanged, since those services check them.
- Preset export:
  - Exports use `.tiger_bundle`, `.tiger_printer` and `.tiger_filament`.
  - Import accepts these and the old `.orca_*` (`PresetBundle` import check, `MainFrame` import
    filter).
  - The 23 translations follow.
- Vendor G-code written into output files no longer mentions Orca: SeeMeCNC, Snapmaker U1, M3D, RH3D
  and Lulzbot. Their bundles are bumped.
- The Device tab's "Printer Connection" page shows text instead of a GIF of the old OrcaSlicer UI.
- Translations that showed Orca where the English does not are corrected or cleared (ru, eu, cs).
- OrcaSlicer's own "Orca Arena X1 Carbon" test printer is removed: the `OrcaArena` vendor bundle, its
  Create Printer entries and the wizard's special case. The wizard's "Other" vendor heading reads
  "Other printers" instead of "Orca colosseum". The unused `OrcaSliced.3mf`, `OrcaBadge.3mf` and
  `setup_connection.gif` are deleted.
- Not changed:
  - Internal identifiers (`ORCA_*`, `OrcaFilamentLibrary`, the `orca` Python module).
  - The 3MF `OrcaSlicer` metadata key (the reader identifies files by it).
  - D-Bus and Linux packaging names.
  - The macOS bundle identifier.

### Resin printers and the printer page
- The resin bundle (`TigerSlicerResin.ini`, 1.4.0) keeps 38 printers: 23 Elegoo, 10 Phrozen and 5
  generic. The EPAX, Peopoly, Voxelab, Uniformation, Wanhao and Creality LD machines are removed,
  with their pictures. A user preset that inherited from one of them loses its parent.
- `TigerSlicerResin/<model>_thumbnail.png` are 240x240 PNGs with a transparent background, framed
  like Orca's `<model>_cover.png`. The guide draws every cover in a 120x120 box, so the old 192x256
  drawings were squashed.
- The Elegoo and Phrozen printers show the maker's own product photo, the way Orca's and
  PrusaSlicer's printer pictures are sourced, from the maker's own store. The original Phrozen Sonic
  uses the Wayback Machine's copy of Phrozen's page. Each photo is cut out from its backdrop and
  scaled to fill the square.
- Figurines beside the printer are cropped out (Jupiter, Mars 3 Pro, Mars 4, Saturn 3, Saturn 3
  Ultra, Saturn 8K). The Saturn 2 and Saturn S keep theirs: every Elegoo photo of them has the
  figurine in front of the printer.
- The generic resin printers get a tile like the generic FDM printers' firmware logos: an MSLA
  symbol (Z rail, plate, print in the vat, LCD) over "2K/4K/6K/8K/10K MSLA" on TigerSlicer pink.
  `generate_tigerslicer_branding.py` draws them (`write_generic_resin_tiles()`).
- Printer selection (`guide/21/common.js`, used by the setup wizard and the printer dialog):
  - Two tabs, Filament printers and Resin printers, replace the page title. Each vendor block has
    `tech="FFF"` or `tech="SLA"` and the hidden tab's blocks get `display: none`, so its ticks are
    still saved. The tabs show how many printers each lists, which follows the search. A search
    whose matches are all on the other tab switches to it, and the open search box stops left of
    the tabs so they stay clickable. "Last block" spacing uses a `LastShown` class, since the last
    block in the page may be on the hidden tab.
  - Resin printers are listed by maker in blocks keyed `TigerSlicerResin/<family>`. The checkboxes
    keep the real vendor, so selection and saving are unchanged. `WebGuideDialog` sends each resin
    model's `family`.
  - The page opens on the resin tab when only resin printers are chosen.
  - Scroll snapping and the layout switch only consider visible blocks.
- Checked in a browser at the dialog's 820x660 with the real bundle (both tabs, light and dark,
  search across tabs, select-all per maker, ticks kept across tabs and searches, 24 picks saved).
  The app builds; `libslic3r_tests` pass, including a new `[resin]` case that every bundled resin
  model is an SLA model with a variant, a maker and a picture.

### Brick layers
- New region option `brick_layers` (bool, default off; Process → Strength → Walls). Every other inner
  wall (inset 1, 3, 5, ... counted from the outer wall) is printed half a layer higher than the walls
  next to it, so the walls interlock along Z. The outer wall (inset 0) never moves.
- Prior art:
  - The Z and flow rules follow the Brick Layers post-processing scripts by Roman Tenger (TenTech) and
    Everson Siqueira (GeekDetour), both GPL-3.0.
  - Upstream has one in-slicer attempt, OrcaSlicer PR #8181 ("Staggered perimeters", still an open
    alpha). It stores a Z multiplier and an extrusion multiplier on each path. It prints infill after
    the raised walls, and it only checks the object's last layer and layer 1.
  - Nothing was ported from it.
- `libslic3r/BrickLayers.{hpp,cpp}`, `apply_brick_layers()`, runs at the end of
  `PrintObject::make_perimeters()` (after `combine_perimeters_for_independent_tools()`):
  - Pass 1 (parallel): for each layer from 1 up, the centrelines of the odd inner walls (`erPerimeter`
    paths of loops and multipaths with odd `inset_idx`) of the regions that use the option, in an
    `AABBTreeLines::LinesDistancer`. Layer 0 has none, so it is never lifted.
  - Pass 2 (parallel): each wall on layer L is sampled every 0.25 mm. Consecutive liftable paths of
    one wall are measured as one line, because Arachne splits a wall into short paths wherever its
    width changes.
    - A sample has a wall below (or above) when layer L−1 (or L+1) has a candidate within
      0.25 × the wall spacing.
    - Runs shorter than 2 mm take the class of their longer neighbour. A boundary that would leave
      a sliver of less than 0.1 mm on a path joins the neighbouring piece.
    - The paths are cut into pieces. With h the layer height and h' the next layer's:

      | Wall below | Wall above | `z_offset` | Height    | Flow | Where                                   |
      |------------|------------|------------|-----------|------|-----------------------------------------|
      | yes        | yes        | h'/2       | h         | 1×   | middle of a stack                       |
      | no         | yes        | h'/2       | h + h'/2  | 1.5× | start: layer 1, above a top surface     |
      | yes        | no         | 0          | h/2       | 0.5× | end: flush with a top or a ledge        |
      | no         | no         | 0          | unchanged | 1×   | not lifted                              |

    - `mm3_per_mm` is scaled by new height / old height. `height` becomes the new height, which feeds
      `;HEIGHT:` and the preview.
  - Skipped for spiral vase, for `independent_tool_layer_heights` (walls merged across layers), and for
    regions with `zaa_enabled`: Z contouring sets wall Z and flow itself. Overhang pieces
    (`erOverhangPerimeter`) are never lifted.
  - Also skipped for a region whose walls and infill use different filaments (the six
    `*_filament_id` keys, already resolved per region). Lifted walls go last in their extruder's pass
    over an island, and a later pass of another filament would print next to them at layer height.
  - On a slope the walls drift sideways each layer. Past the tolerance (walls flatter than about 63°
    with 0.2 mm layers and 0.45 mm walls), a lifted wall ends at 0.5× and the next one starts at 1.5×,
    which adds up to a normal wall. On a 50 mm sphere 38% of the inner wall length is lifted; on a
    cube, 67%.
- `ExtrusionPath::z_offset` (mm above print Z) is copied by every constructor and assignment and by
  `ExtrusionLoop::split_at()`.
- Wall index (`inset_idx`):
  - `ExtrusionMultiPath` copies now keep `inset_idx` (the base class was not copied).
  - Arachne's `traverse_extrusions()` now sets it on loops and multipaths; before, only classic loops
    had it.
- G-code (`GCode.cpp`):
  - `_extrude()` uses `m_nominal_z + z_offset` for the travel target, the Z reset and the scarf slope.
  - `extrude_perimeters()` skips entities with a lifted piece when the region uses the option.
    `extrude_lifted_perimeters()` prints them after ironing, island by island. A lifted bead sticks up
    h/2, and the nozzle face would drag over it while printing anything next to it. Regions without
    the option are not touched (no `m_config.apply()`).
  - `m_brick_lift_z` is the highest lifted Z printed on the layer, reset in `change_layer()`.
    `travel_to()` (also through avoid-crossing-walls hops and on the way to the wipe tower) travels at
    least that high and lowers Z only at the destination. With nothing lifted on the layer it emits
    exactly what it did before.
- Config:
  - `Layer::is_perimeter_compatible()` compares `brick_layers`.
  - `PrintObject::invalidate_state_by_config_options()` re-runs perimeters when it changes.
  - The spiral vase normalisation turns it off.
- Preview: libvgcode took a layer's Z from its last extrusion, which is now a lifted wall. That moved
  the slider label and the color-change Z by h/2. `Layers::update()` now ignores inner-wall
  extrusions once anything else has been extruded on the layer, so the slider shows the print Z.
- GUI:
  - `Tab.cpp` adds the option to Strength → Walls.
  - `ConfigManipulation.cpp` greys it out with spiral vase, with Z contouring, or with fewer than 2
    walls (unless alternate extra wall is on).
- Tests: `tests/fff_print/test_brick_layers.cpp`, `[BrickLayers]`, 6 cases, run with both the classic
  and the Arachne wall generator where it matters:
  - off by default;
  - stays off when the walls and the infill use different filaments;
  - on a 20 mm cube, walls 1 and 3 of 3 inner walls are at layer Z + 0.1 and the outer wall is at
    layer Z;
  - flow is 1.5× on layer 2, 1× in the middle and 0.5× at the top;
  - lifted walls are the last extrusions of each layer;
  - on `TestMesh::step` (×3), walls end flush under the 5 mm ledge and restart at 1.5× above it.
- Known limits:
  - Slopes fall back to normal walls, as above.
  - Islands closer than the nozzle face may still touch a neighbour's lifted walls, because islands
    are printed one after another.
  - Each layer decides "wall above" and the next layer decides "wall below" on its own samples. Where
    a wall ends or a notch is under 2 mm, the two can disagree over a few millimetres: a small blob or
    gap.
  - `{layer_z}` in tool-change or custom G-code within a layer comes from the writer's Z, so after a
    lifted wall it reads print Z + h/2. Z-hop already has the same issue.

### Fin supports
- Ported from the open upstream pull request OrcaSlicer/OrcaSlicer#15839 by abehrman ("Add fin
  supports with interface material and automatic fallback", head `93e897eb2`). Its design follows
  Slant3D's designed-in support fins and Matthew Trahan's Support Fins (MIT,
  github.com/gittrahan/support-fins). Upstream had not built or run that port; TigerSlicer builds it
  and runs its tests.
- `support_type` gains `fins` (`stFins`, appended after `tree(manual)`).
  `Support/SupportFins.{hpp,cpp}` (`generate_fin_support()`) builds the fins from the object's shape,
  not from overhang detection:
  - The fin region is the area under the object that is clear of it down to the bed.
  - It is cut into parallel strips (fins), fixed in XY so each fin is one wall, running across the
    region's long axis. There is a 1 mm foot, tines every `support_fin_tine_spacing` (packed near the
    bed), optional cross fins and an optional interface deck.
  - The fins go through the common support toolpath generator as base layers, filled concentric at
    full density.
- Supplemental supports (`support_fin_fallback`, default tree): `_generate_support_material()` runs
  the tree or normal generator with the fin columns reserved as blockers, then merges both sets of
  layers.
- Conflicts with the TigerSlicer tree were only context (upstream has belt-printer code and a newer
  test file): CMakeLists, ModelArrange (`stFins` gets the normal support's 6 mm arrange margin),
  `Print.hpp` (reserved columns), the preset key list, two hunks in `PrintObject.cpp` and
  `SupportCommon.cpp`. The upstream tests are appended to `test_support_material.cpp`, with a local
  `size_flush_to_nozzles()` (upstream adds it to `test_helpers`) and without the `SupportMaterial:`
  prefix in names.
- Fixed on top of the upstream code, after review:
  - Merging fin and supplemental layers at the same height appended one layer's collection into the
    other as a single nested entity of mixed role, which the G-code's support and interface passes
    skip. With a separate interface filament, the fins (or the tree branches) of every shared height
    were not printed. The paths themselves are now moved.
  - The merge deletes nothing until it has finished, and hands every layer back on an exception.
    A stable sort keeps the supplemental layer at a shared height.
  - The reserved fin columns are cleared when fin generation throws (cancel).
  - Fins with the tree fallback run organic tree support, so the two organic-only checks in
    `Print::validate` (independent tool layer heights, variable layer height) now cover them.
  - A fin pitch is never shorter than the fin (a negative spacing in a loaded preset looped forever).
  - `keep_lean_side()` returns when there are no columns. The shaping loop checks for cancel.
- TigerSlicer addition, `support_fin_holes` (bool, default off, advanced): a diamond lattice cut
  through the main fins (not cross fins or tines).
  - The lattice lives in each fin's own plane: struts along u + z = k·8 mm and u − z = k·8 mm (u
    along the fin), 1.6 mm wide, so the holes line up from layer to layer.
  - `hole_intervals()` gives the open stretches of one 8 mm period at a height. `fin_hole_bands()`
    turns them into bands across the fins.
  - A 2.5 mm frame stays solid: the foot and 2.5 mm above it, both ends of every fin (the fin region
    eroded by 2.5 mm), and the top edge (no holes within 2.5 mm plus the top gap and any deck under
    the object).
  - Cross fins stay whole where they cross a hole. Holes narrower than two lines are not cut (no
    hairline slots near a diamond's tip or the frame). A fin cut off by `support_fin_height` keeps a
    solid top too.
  - Why off by default: a solid fin layer is one long loop. With holes it becomes several short
    pieces with a travel (and often a retraction) over each hole, so the head rarely reaches speed.
    Estimated for a 20 mm fin layer, half open: about 3× slower on a 500 mm/s² printer and 2× on a
    10,000 mm/s² one, for half the plastic.

### Verification (Windows)
- Release build (MSVC 17.14). `libslic3r_tests`: all pass, including 9 new `[AutoSlice]` cases and
  the resin arrange regression case.
- In the app with a fresh `--datadir`:
  - The Klipper bed shows the TigerSlicer logo.
  - Auto slice is greyed out on an empty plate.
  - A T-shaped part (10 mm post, 30 mm slab on top) with Quality: 0.12 mm Fine, first slice flags a
    floating cantilever, the second slice has support and support-interface lines.
  - A 20 mm cube with Fast: 0.28 mm Extra Draft, "No supports needed".
  - Elegoo Mars 3: arrange works.
  - Hollow + drain holes on, then Medium supports: the cube is hollowed (5.86 ml instead of
    8.02 ml, the layer image is a ring) and has 2 drain holes. Supports are generated
    (10,236 triangles), 484 layers.
  - Normal quit, no crash logs.
- Seen, not changed: the Supports workspace's settings column can be wider than the space beside the
  canvas. Its full-width buttons (Auto-orient, Hollow + drain holes) then show no visible label.
- Brick layers (2026-10-19), Release build (MSVC 17.14):
  - `fff_print_tests`: all 134 cases pass in random order, including the 6 `[BrickLayers]` cases.
    `libslic3r_tests`: all 1039 pass.
  - G-code read by hand:
    - 20 mm cube, Arachne, Z-hop 0.4: inner wall 2 and the outer wall at Z10, then infill, then
      inner walls 3 and 1 at Z10.1. The travel lifts for Z-hop and drops straight down to Z10.1.
      Layer 2 has lifted walls at Z0.5 with `;HEIGHT:0.3`; the top layer has them at Z20 with
      `;HEIGHT:0.1` and half the E.
    - Stepped part (`TestMesh::step` ×3, avoid crossing walls on): flush at the 5 mm ledge, a new
      stack at 1.5× above it.
    - 50 mm sphere: 127 height changes inside walls over 500 layers, all plain vertical Z moves.
  - In the app with a fresh `--datadir` (Generic Klipper printer, 0.20 mm Standard):
    - Brick layers shows under Strength → Walls in advanced mode, unticked, with its tooltip.
    - A 25 mm cube with it on slices to 125 layers.
    - The preview slider reads 11.80 mm on layer 59, while the G-code viewer shows that layer's
      inner walls at Z11.9.
    - No crash log.
- 0.0.7 release (2026-10-20):
  - Clean release build of `9a90183e0` (MSVC 17.14, `build_release_vs2022.bat slicer`, old
    `build\OrcaSlicer` deleted first) with no errors. The app logs "Version 0.0.7 build 9a90183e0a".
  - Packaged as `TigerSlicer-0.0.7-win64-Setup.exe` (230,547,456 bytes) and
    `TigerSlicer-0.0.7-win64.zip` (236,981,052 bytes).
  - The Setup.exe test install matches the build: 9,326 files, identical `TigerSlicer.dll`.
- Smoke test of that install with an empty `--datadir`:
  - Wizard with only Klipper: the printer page has the Filament printers (386) and Resin printers
    (38) tabs. Finish opens the Klipper printer with the TigerSlicer bed logo.
  - 20 mm cube, Auto slice → Fast: 0.28 mm Extra Draft @MyKlipper, "No supports needed", 72 layers,
    7 m 25 s.
  - Select/Remove printers → Resin printers, search "mars 3" → Elegoo Mars 3. Arrange puts the cube
    on its bed without a crash.
  - Hollow + drain holes on, then Auto slice → Medium supports: "Hollowed 1 object(s) and added drain
    holes at their bottom". 484 layers, 5.93 ml (the layer image is a ring), 1 h 28 min.
  - Quit without saving: normal exit, no crash log. The only log errors are PartPlate's "Unable to
    create exclude triangles" and a settings-page selection message after switching printers.

### Verification (macOS)
- Release build on macOS 15 (Apple Silicon) from `3cb2bf668`, no errors. `libslic3r_tests`: 1,040
  cases pass. `fff_print_tests`: 153 cases pass in random order, including the 25 `[Fins]` and
  `[BrickLayers]` cases.
- Background launches with throwaway data folders: a Generic Klipper printer (selects PLA), an
  Elegoo Mars 3, and a copy of an existing 0.0.6 data folder (its printer and user filament load). No
  new errors, no crash reports. The new UI (Auto slice, fin and brick settings, the printer tabs) was
  not clicked through on macOS; the Windows checks above cover the same shared code.

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
  were checked by hand.

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
- An existing data folder last used with 0.0.3: bundles re-installed (library 02.04.00.05), its user
  filaments load (parents `Generic PLA/PETG/PLA High Speed @System` and `Bambu PAHT-CF @System`
  resolve to PLA, PETG and PA6-CF), normal exit. No new kinds of log errors.
- Seen: the system filament list was not in table order, and every launch logged "Failed to install
  bundled TigerSlicer Resin profile" twice. Both are fixed after the tag (below). A user filament
  limited to `Prusa CORE One L 0.6 nozzle` was not listed for a user printer based on it (0.0.3 does
  the same); that is the data folder, not the code (below).

### Found in the Windows check (fixed after the tag, `f84322c33`, `b072ff5fd`)
- Filament list order: `PlaterPresetComboBox::update()` sorted "System presets" with a table of
  aliases ("PLA") but looked up the map key, the full preset name ("PLA @System"). Nothing matched,
  so PLA, PETG, ABS and TPU came first (Orca's `first_types`) and the rest alphabetically. The sort
  now keys on `Preset::alias`. The display label (`preset_aliases`) cannot be used, because an edited
  preset's label starts with "(modified)". The table is now `PresetBundle::filament_library_order()`.
  A `[filament_library]` test checks it against `OrcaFilamentLibrary.json`'s `filament_list` and the
  loaded aliases.
- Resin bundle log error: `PresetBundle::setup_directories()` copied `TigerSlicerResin.ini/.idx` into
  `<data>/vendor` on every launch, a step left from the PrusaSlicer-based 1.0.0. The folder is never
  created, so the copy failed. Nothing reads it, since `load_tigerslicer_resin_bundle()` reads the ini
  from `resources/profiles`. The copy is removed.
- Hidden user filament, not a code change: `PresetCollection::load_presets()` loads `base/` (detached
  "custom root" presets) before the type folder and skips a later file with the same name ("Preset
  already present, not loading"). The data folder had both `machine/base/<printer>.json` (an older,
  detached copy) and `machine/<printer>.json` (inherits `Prusa CORE One L 0.6 nozzle`). The detached
  copy wins, so `is_compatible_with_parent_printer()` has no parent to match against the filament's
  `compatible_printers`. Moving the stale detached copy out of the data folder fixes it.
- Verified on Windows: `libslic3r_tests` 1,029 cases, 88,370 assertions, all pass (`[filament_library]`:
  5 cases, 281 assertions). In the app with an empty `--datadir`, the list runs PLA, PLA-CF, PLA-GF,
  PETG ... TPU 95A, TPU 90A, TPU 85A, PVA, BVOH, HIPS. An edited, unsaved PLA stays first. The log
  has no resin bundle error, and no `vendor` folder is created.

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
- The app on an existing 0.0.4 data folder: vendor bundles re-installed from resources, its user
  filaments (two on removed parents) load, no new errors or crashes. A fresh Klipper setup picks
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
