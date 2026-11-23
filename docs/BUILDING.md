# Building TigerSlicer

TigerSlicer builds the same way as OrcaSlicer. The first build compiles all third-party dependencies
(about an hour); later builds only recompile the app.

On every platform you also need the [Rust toolchain](https://rustup.rs) (stable): the resin slicing
engine in `src/mslicer` is written in Rust and built with `cargo`. Install it with rustup, and make
sure `cargo` is on the `PATH` or `CARGO_HOME` is set.

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
Perl. From the repository folder (any command prompt: the script finds Visual Studio itself):

```bat
build_win.bat -d
build_win.bat -s -i
```

`-d` downloads and builds the dependencies into `deps\build` (about 20 minutes on a 16-core PC). `-s`
builds the app, and `-i` installs it into `build\OrcaSlicer`, so the app is
`build\OrcaSlicer\TigerSlicer.exe`. After the first build only the second line is needed. Add
`--arch arm64` for Windows on ARM, and run `build_win.bat -h` for the other options. For more detail,
see OrcaSlicer's [How to build](https://www.orcaslicer.com/wiki/how_to_build) page, which applies
unchanged.

Dependencies built for TigerSlicer 0.0.13 or earlier (before the move to OrcaSlicer 2.5.2) do not fit:
delete or rename the old `deps\build` and `build` folders and build both again.

## Linux

There are no Linux builds of TigerSlicer. `build_linux.sh` and `build_flatpak.sh` come from OrcaSlicer
and have not been tested with TigerSlicer.

## Tests

Configure with `-DBUILD_TESTS=ON` (on Windows `build_win.bat -s --tests`), build the test targets, and
run them with `ctest --test-dir build/tests -C Release`, which runs every case in its own process. Turn
`BUILD_TESTS` off again before a release build. How the tests are organised is described in
[tests/AGENTS.md](../tests/AGENTS.md).

A few `[Regression]` tests in `fff_print` compare a hash of the G-code moves with a recorded value. The
values are recorded with MSVC on Windows; other compilers give other values, so those cases fail on
macOS.

## Scripts at the repository root

| Script | Used for |
|---|---|
| `build_win.bat` | Windows builds |
| `build_release_macos.sh` | macOS builds |
| `build_linux.sh`, `build_flatpak.sh` | Linux, from OrcaSlicer, untested |

## Version number

The version shown in the app comes from `SoftFever_VERSION` in `version.inc`. Leave `SLIC3R_VERSION`
alone: it is the internal compatibility number for Bambu Studio project files and printer profiles.
