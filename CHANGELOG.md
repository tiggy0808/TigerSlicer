# Changelog

Downloads for every version are on the [Releases page](https://github.com/tiggy0808/TigerSlicer/releases).
Technical notes for each release are in [docs/DEVELOPMENT_NOTES.md](docs/DEVELOPMENT_NOTES.md).

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
