# Changelog

Downloads for every version are on the [Releases page](https://github.com/tiggy0808/TigerSlicer/releases).
Technical notes for each release are in [docs/DEVELOPMENT_NOTES.md](docs/DEVELOPMENT_NOTES.md).

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

- Resin (MSLA) printing in the same app, with native printer file export and a raster inspector.
- Independent tool layer heights for multi-tool printers.
- Full Spectrum colour mixing.
- A Physical Printer tab for printer connections.
- Pink-and-black TigerSlicer theme and logo.

Presets from 1.0.0 are not imported, because the PrusaSlicer and OrcaSlicer settings formats differ.
They are left untouched in the settings folder.
