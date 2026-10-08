# Building TigerSlicer

TigerSlicer builds the same way as OrcaSlicer. The first build compiles all third-party dependencies
(about an hour); later builds only recompile the app.

## macOS (Apple Silicon)

Needs the Xcode Command Line Tools (`xcode-select --install`) and [Homebrew](https://brew.sh). Full
Xcode is not required.

```bash
brew install cmake ninja gettext nasm autoconf automake libtool texinfo pkg-config
export PATH="$(brew --prefix libtool)/libexec/gnubin:$(brew --prefix texinfo)/bin:$PATH"
export CMAKE_POLICY_VERSION_MINIMUM=3.5   # CMake 4 rejects some dependencies' old minimum versions
./build_release_macos.sh -a arm64 -x      # -x builds with Ninja; add -s to rebuild only the app
```

The app is written to `build/arm64/OrcaSlicer/TigerSlicer.app`.

To package a disk image like the ones on the Releases page:

```bash
mkdir -p dmg && ditto build/arm64/OrcaSlicer/TigerSlicer.app dmg/TigerSlicer.app
codesign --force --deep --sign - dmg/TigerSlicer.app
ln -s /Applications dmg/Applications
hdiutil create -volname TigerSlicer -srcfolder dmg -format UDZO TigerSlicer-<version>-macos-arm64.dmg
```

Keep the volume name `TigerSlicer`: the app checks for it to tell when it is being run from the disk
image.

## Windows (x64)

Needs Visual Studio 2022 with the "Desktop development with C++" workload, CMake, Git and Strawberry
Perl. From a Visual Studio 2022 developer command prompt in the repository folder:

```bat
build_release_vs2022.bat
```

Add `arm64` to build for Windows on ARM. For more detail, see OrcaSlicer's
[How to build](https://www.orcaslicer.com/wiki/how_to_build) page, which applies unchanged.

## Version number

The version shown in the app comes from `SoftFever_VERSION` in `version.inc`. Leave `SLIC3R_VERSION`
alone: it is the internal compatibility number for Bambu Studio project files and printer profiles.
