# Changelog

Downloads for every version are on the [Releases page](https://github.com/tiggy0808/TigerSlicer/releases).
Technical notes for each release are in [docs/DEVELOPMENT_NOTES.md](docs/DEVELOPMENT_NOTES.md).

## Unreleased

**Changed**
- **One bed temperature.** Filament settings have a single bed temperature (first layer and other
  layers), like the nozzle, instead of one per build plate. Every plate prints at it. You still pick
  the build plate, for printers whose start G-code needs to know which one is installed.
- Experimental features can be turned on and off where you'd look for them: the flask button next
  to the Simple/Advanced switch at the top of the process settings (pink when they are on), and
  **View > Show Experimental Features**. The setting in Preferences > General still works too.

## 0.0.10 (2026-10-23)

**New (experimental)**

Small feature accuracy: settings for features narrower than the walls, such as the tips of gear and
belt pulley teeth, and for gaps narrower than two lines. They are all experimental.
- **Show experimental features** (Preferences > General) is off by default. While it is off, the
  experimental settings are hidden and slicing ignores them, even when a preset or project sets them,
  so nothing changes. When it is on, each one is marked "(Experimental)". A print that uses one says
  so in a slicing notice and in the G-code header.
- **Deviation** is a new preview view. For the top visible layer, it shows model area left unprinted
  in red and extrusion outside the model in blue. The further off, the darker.
- Slicing warns how many features are **below the printable width** for the nozzle. The width is set
  by **Small feature minimum width**, 60% of the nozzle by default.
- **Small feature policy** (Strength > Small features) prints what the walls leave out of a thin
  feature. Each one gets a single line, run from the wall out to the feature's tip and back:
  - **Drop** leaves the feature out, as before.
  - **Inside the model** never makes the part bigger, but the feature may come out short.
  - **Balanced** leaves the least area wrong.
  - **Outside the model** never leaves the feature short: the line reaches 0.05 mm past the tip.

  The lines print at their own speed (15 mm/s) and flow, and don't retract.
- **Narrow gaps: Keep open** prints the walls beside a slot, vent or clearance narrower than two
  lines thinner, so they don't spill into it and close it.
- All of these can be set per object and per modifier. For example, use Outside on a gear's teeth
  and Inside around its bore.

## 0.0.9 (2026-10-22)

**New**
- **Fins (manual)**, next to the existing fins, now called **Fins (auto)**. Like Normal and Tree
  (manual), it puts fins only where you ask for support: under areas painted with the support
  painting tool, or under support enforcer volumes. Its supplemental tree or normal supports are
  manual too.

**Changed**
- **More settings per model.** Select a model and switch the process settings from Global to Objects to
  change these for that model only:
  - First layer speed, first layer infill speed and first layer line width.
  - Number of slow layers.
  - Avoid crossing walls and its maximum detour length.
  - Reduce infill retraction.

  Almost every other process setting could already be set per model. Settings that only make sense for
  the whole plate stay global: print sequence, first layer height, skirt, prime tower, spiral vase,
  travel speed and G-code output.
- **Flow ratio per model** is on the first page of the Objects view. It multiplies the filament's flow
  ratio for that model's walls, infill and surfaces, so one model can print with more or less filament
  without changing the filament profile. Its supports, brim and skirt keep the global flow.
- Fins run the way the part leans. A long part tilted along its length now gets one fin down its
  length, instead of a row of short fins standing across it. The fins are also centered under the
  part, so a narrow part gets one fin down its middle.
- **Fin spacing** and **Tine spacing** take mm or %, like infill density.
  - Fin spacing as a percentage is the share of the area under the part that the fins fill.
  - Tine spacing as a percentage is the share of each fin's height that is tines.
  - The new defaults, 15% and 4%, are about the old 8 mm and 5 mm.
  - Presets and projects saved with mm values keep them.
- The Auto slice button is a whole rounded button. Its left edge was squared off like the Slice
  plate button's, which joins a dropdown.

**Fixed**
- With fins and supplemental tree or normal supports, painted support enforcers grew those
  supports inside the fins.
- On tilted parts, fins were cut into pieces to leave room for tree or normal supports on slopes
  those supports then did not support, because they are steeper than the threshold angle.

## 0.0.8 (2026-10-21)

**New**
- **Calibration builder** (Calibration menu). One tool replaces the separate calibration tests.
  Pick what to test: temperature, pressure advance, retraction length or speed, flow ratio, print
  speed, acceleration, jerk (square corner velocity or junction deviation), or input shaping
  frequency or damping. Then pick the range and step.
  - **Tower:** one print in bands, each band at the next value. The built-in tower has the value
    raised on every band, and you can use your own model instead.
  - **Grid:** a labelled copy for each value (flow ratio, print speed and acceleration), all
    printed in one go.
  - Optionally set the layer height and print speed used for the test. Save a setup as a preset to
    run it again later.
- **Color mixing** button in the sidebar's Filament header, on printers with two or more extruders
  (toolchangers, IDEX). It opens Full Spectrum colour mixing, which was only in the View menu
  before.

**Changed**
- **Independent tool layer heights** can now be found and changed: Process → Quality → Layer height,
  Printer → Basic information → Advanced, and an "Independent layer height" group on each
  extruder's page. These settings show in Advanced mode, not just Expert.
- The old calibration dialogs are removed: the pressure advance line, pattern and tower tests,
  the flow rate passes, the temperature tower, max volumetric speed, retraction, VFA, input shaping
  and cornering. The calibration builder covers each of these.
  The Calibration tab for Bambu Lab printers is unchanged.

**Fixed**
- Undo, Redo and the Calibration menu in the title bar now work as soon as TigerSlicer starts.
  Before, they stayed greyed out until you switched tabs once.
- Cancelling "New project" no longer removes the current project's colour changes and pauses.
- Upgrading no longer hides your own printers and process presets that are based on a printer you
  don't have selected in the printer list.

## 0.0.7 (2026-10-20)

**New**
- **Auto slice.** A button left of "Slice plate" with a menu of what the print is for.
  - Filament printers: **Structural**, **Fast** or **Quality**. Each picks the process profile with
    the matching layer height for your printer and nozzle (Structural uses a Strength profile when
    the printer has one).
  - Auto slice then slices once to find objects that would print in mid-air or with a long
    overhang, turns tree supports on for just those objects, and slices again.
  - Resin printers: **Low**, **Medium** or **High supports**. Auto slice generates the supports and
    slices. Turn on "Hollow + drain holes" in the same menu to hollow the models as well.
- **Hollow + drain holes** for resin, also as a button in the Supports workspace. It hollows every
  model thick enough for the wall and drills two drain holes at its bottom, nearest the build plate,
  so resin drains and no suction cup forms. Models that already have drain holes keep theirs.
- **Brick layers** for filament prints, in the process settings under Strength → Walls (off by
  default). Every other inner wall is printed half a layer higher, like bricks in a wall, so the
  walls lock into each other and parts are stronger between layers. The outer wall stays where it
  is, so the surface looks the same. Works best with 3 or more walls.
- **Fin supports**, a third support type next to Normal and Tree. For parts printed tilted (on an
  edge or a corner, which makes them stronger), thin walls stand on the bed under the part and
  hold it up, with small "tines" every few millimetres that fuse into the part and snap off
  afterwards. Tree supports fill in automatically for any overhang the fins can't reach. Settings
  cover fin spacing, thickness, tines, height, cross bracing and a top deck. **Fin holes** (off by
  default) cuts a diamond lattice through the middle of each fin to save filament; it usually
  prints a little slower.

**Changed**
- The last OrcaSlicer branding is gone:
  - The bed of the Klipper, Marlin, RRF, Repetier and ToolChanger printers shows the TigerSlicer
    logo.
  - Settings and calibration dialogs no longer link to OrcaSlicer's online wiki.
  - The OrcaSlicer whale and badge models are no longer in the handy models menu, and OrcaSlicer's
    own "Orca Arena X1 Carbon" test printer is no longer in the printer list.
  - Messages and some printers' start G-code no longer mention OrcaSlicer.
- Exported presets use `.tiger_bundle`, `.tiger_printer` and `.tiger_filament`. Files exported
  earlier (`.orca_*`) still import.
- The support presets in the Supports workspace are named Low, Medium and High.
- The filament list shows the materials in the library's order: PLA, PLA-CF, PLA-GF, PETG and so on.
- Printer selection has two tabs, **Filament printers** and **Resin printers**, each with its own
  list of makers. Resin printers are listed by maker (Elegoo, Phrozen) like filament printers.
  Printers you tick stay ticked when you switch tabs.
- The resin printers in the printer list show photos of the real printers, from the makers' own
  websites, instead of drawings. The pictures are no longer squashed. The generic resin printers
  have their own symbol, like the generic filament printers' firmware logos.
- The resin printer list keeps Elegoo, Phrozen and the generic printers. The EPAX, Peopoly,
  Voxelab, Uniformation, Wanhao and Creality LD printers are removed. If you used one, pick the
  generic printer with the same screen size.

**Fixed**
- Arranging the plate with a resin printer selected crashed.
- Some resin support settings in the Supports workspace (point density, pinhead size, pillar
  diameter, support base) had no names.
- Every launch logged an error about installing the resin profile.

## 0.0.6 (2026-10-13)

**Changed**
- The filament list shows the materials directly instead of inside a "Generic" submenu.
- There is nothing to install for filaments any more: the setup wizard goes from printers straight
  to the end, and "Add/Remove filament" and the filament settings button beside "Project Filaments"
  are gone.

## 0.0.5 (2026-10-13)

One filament profile per material, for every printer.

**Changed**
- **Filaments are listed by material, not brand.** The thousands of brand profiles (Bambu PLA Basic,
  Elegoo PLA, Generic PLA and so on) are replaced by one profile per material: PLA, PETG, PCTG, ABS,
  ASA, PC, PA6, PA12, PA612, PPA, PPS, PEI, PEKK, PEEK and PP, each also as a carbon fibre (CF) and
  glass fibre (GF) version, plus TPU 95A, TPU 90A, TPU 85A, PVA, BVOH and HIPS. All of them work with
  every printer and are always in the filament list.
- Filament profiles you create, with "Save as" or "Create filament", work with every printer.
- Your own filament profiles and older projects that used a removed brand profile now use the profile
  for its material instead.

## 0.0.4 (2026-10-12)

The first macOS build with the 0.0.3 resin fixes, and no more Home tab.

**Changed**
- The Home tab (New project, Open project, recent projects) is gone. TigerSlicer opens straight to
  Prepare. Save and open projects as `.3mf` from the File menu, which still lists recent files.
  The "Default page" preference is removed with it.

**Fixed**
- Adding a resin printer with "Select/Remove printers" could still crash on macOS.
- Switching from a filament printer to a resin printer in the printer list crashed when a model was
  on the plate.

## 0.0.3 (2026-10-08)

Fixes for resin printing, found in the first Windows build of 0.0.2.

**Fixed**
- TigerSlicer crashed right after you added a resin printer, in the setup wizard or with
  "Select/Remove printers" in the printer list.
- Switching between a resin printer and a filament printer in the printer list crashed.
- Picking a resin printer in the printer list left the Process box empty.
- "Export resin file" crashed.

## 0.0.2 (2026-10-08)

Resin printing now works from start to finish.

**New**
- **Resin printers in the setup wizard.** All 67 bundled resin printers (Elegoo, Phrozen, Creality,
  EPAX, Peopoly, Voxelab, Uniformation, Wanhao and generic screens) are listed under "Resin printers
  (MSLA)". Picking only resin printers skips the filament step.
- **Resin settings you can edit.** With a resin printer selected, the sidebar shows a Resin selector
  in place of the filament list, and the settings pages cover the screen and output format (printer),
  exposure, light power, wait times and lift/retract motion (resin), and layers, anti-aliasing and
  hollowing (print).
- **Hollowing and drain holes.** The Supports workspace (View menu) can hollow a model and place drain
  holes by clicking on it, with the hole size and depth set beside the button. Support points and
  drain holes are now drawn on the model.
- **Layer slider in the resin preview.** Step through every layer image instead of only the middle one.
- **Send to Elegoo and other ChiTu-board printers.** In the printer connection dialog, choose
  "ChiTu SDCP (local LAN)" and enter the printer's address; "Send to printer" then uploads the file.
  The print is not started automatically.
- The export button reads "Export resin file" for resin printers.

**Fixed**
- TigerSlicer crashed with a resin printer selected: on startup, when opening a model, and when
  slicing.
- Resin printers used OrcaSlicer's default 200 mm bed instead of their own build area and height.
- Resin printers showed an internal ID such as "ELEGOO_MARS_3" instead of their name.
- A resin printer started on placeholder "Default Setting" profiles, which refused to slice.
- Support presets and "Save print settings profile" in the Supports workspace did nothing.

**Known issues**
- In Preview, resin plates show only the layer image panel; the 3D view stays empty.
- Resin printers are not found automatically on the network; enter the printer's address instead.

## 0.0.1 (2026-10-06)

This release makes TigerSlicer fully its own app instead of a renamed OrcaSlicer.

**New**
- First macOS release (Apple Silicon), with a rounded-square app icon that matches other Mac apps.
- New black-and-pink startup screen.
- The last of OrcaSlicer's teal is gone: the 3D view, tools, dialogs and web pages are pink, and new parts
  start pink. If your saved default part colour was OrcaSlicer's teal, it is switched to pink once.

**Changed**
- All cloud features are removed: no sign-in, no cloud preset sync, and no update checks against
  OrcaSlicer's servers. Printing to Bambu Lab printers over your local network is kept, but hasn't been
  tested with this release yet.
- The app says "TigerSlicer" everywhere, including all 23 translations, which now load correctly.
- On Windows the program is `TigerSlicer.exe`, and it no longer takes over OrcaSlicer's file
  associations or `orcaslicer://` links.
- On macOS, TigerSlicer has its own app identity, so the system no longer confuses it with an installed
  OrcaSlicer.

**Fixed**
- TigerSlicer could silently sign in to your OrcaSlicer account because both apps shared one saved login.
  TigerSlicer now keeps its own. Presets you saved while that happened are in
  `user/<account number>/` inside the settings folder; copy them to `user/default/` to see them again.
- Selected parts no longer turn white when their colour is light.
- Filament colour swatches show the real filament colour again.

## 0.0.0 (2026-10-02)

The first TigerSlicer release built on OrcaSlicer, replacing the PrusaSlicer-based 1.0.0. Windows x64.

- Resin (MSLA) printing in the same app: slicing, generated supports, native printer file export and
  a preview of the exposed layer image.
- Independent tool layer heights for multi-tool printers.
- Full Spectrum colour mixing.
- A Physical Printer tab for printer connections.
- Pink-and-black TigerSlicer theme and logo.

Presets from 1.0.0 are not imported, because the PrusaSlicer and OrcaSlicer settings formats differ.
They are left untouched in the settings folder.
