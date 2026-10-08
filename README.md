<div align="center">

<img src="resources/icons/branding/TigerSlicer_192px.png" alt="TigerSlicer" width="128">

# TigerSlicer

A 3D printing slicer for FDM and resin printers, in one app.

[Download](https://github.com/tiggy0808/TigerSlicer/releases/latest) · [What's new](CHANGELOG.md) · [Build from source](docs/BUILDING.md)

</div>

TigerSlicer is my personal slicer. It is built on [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer), so
it has everything OrcaSlicer does (calibration tools, tree supports, scarf seams, a large printer
library), plus the features below.

## Features

- **Resin (MSLA) printing in the same app.** Switch a plate to resin and slice it with the
  [mslicer](https://github.com/connorslade/mslicer) engine. Exports native printer files (Chitu `.ctb`,
  Elegoo `.goo`, NanoDLP) and can send them straight to a networked printer. The preview includes a
  layer-by-layer raster inspector.
- **Independent tool layer heights.** On a multi-tool printer, each tool can print at its own layer
  height, so a fine nozzle and a fast nozzle work on the same print at their own pace.
- **Full Spectrum colour mixing.** Mix new colours from the filaments you have loaded and paint them onto
  a model. TigerSlicer builds them from the real filaments layer by layer.
- **Printer connections in one tab.** Your physical printers and their network connections have their own
  tab next to Prepare and Preview.
- **No account, no cloud.** No sign-in, no cloud preset sync, and TigerSlicer itself doesn't contact
  any server when it starts. Your presets stay on your computer.
- **Its own look.** A pink-and-black theme throughout, a new startup screen, and a native-looking Mac icon.

## Download

Get the latest version from the [Releases page](https://github.com/tiggy0808/TigerSlicer/releases/latest).

| Platform | File | Notes |
|---|---|---|
| Windows 10/11 (x64) | `TigerSlicer-<version>-win64-Setup.exe` | Installer, no admin rights needed. A portable `.zip` is also available. |
| macOS (Apple Silicon) | `TigerSlicer-<version>-macos-arm64.dmg` | Open it and drag TigerSlicer into Applications. |

Neither build is code-signed yet, so the first launch shows a warning:

- **Windows:** SmartScreen says "Windows protected your PC". Click **More info → Run anyway**.
- **macOS:** right-click TigerSlicer in Applications and choose **Open**, then **Open** again.

Settings are stored in `%APPDATA%\TigerSlicer` on Windows and `~/Library/Application Support/TigerSlicer`
on macOS, and are kept when you upgrade.

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md) for Windows and macOS build steps.

## Credits and license

TigerSlicer is free software under the [GNU Affero General Public License v3](LICENSE.txt).

It stands on the work of others:

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) by SoftFever and contributors, which builds on
  [Bambu Studio](https://github.com/bambulab/BambuStudio) by Bambu Lab and
  [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which in turn grew out of
  [Slic3r](https://github.com/slic3r/Slic3r) by Alessandro Ranellucci.
- [mslicer](https://github.com/connorslade/mslicer) by Connor Slade (GPL-3.0), the resin slicing engine.

TigerSlicer is an independent project and is not affiliated with or endorsed by any of them.
