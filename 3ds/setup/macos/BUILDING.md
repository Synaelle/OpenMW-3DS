# Building the setup app on macOS

For the macOS VM (planned for 0.1.1). The code already has its macOS parts (`src/gui/system_unix.cpp`: the folder
picker through `osascript`, the dark mode through `defaults`, the system font); nothing has been built or tried on
a Mac yet.

## 1. The tools

In Terminal:

    xcode-select --install
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    brew install cmake ninja pkg-config sdl2

(The second line installs Homebrew; skip it if `brew --version` already works.)

## 2. Build

From the repository's `setup` folder:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

This makes `build/openmw3ds-setup` (the window) and `build/openmw3ds-setup-cli` (the command line).

## 3. Check it

1. The texture cooker, against the Windows pack (it must be the same bytes):

       ./build/openmw3ds-setup-cli cook --morrowind "<Morrowind folder>" --port <port files> --out /tmp/mac.o3tp
       cmp /tmp/mac.o3tp <textures.o3tp made on Windows from the same files>

2. The window without showing it, both themes:

       ./build/openmw3ds-setup --screenshot /tmp/dark.png --state ready --theme dark
       ./build/openmw3ds-setup --screenshot /tmp/light.png --state done --theme light

3. The window itself: `./build/openmw3ds-setup` (put the `port` folder next to the program first). Check that
   Browse opens the folder picker, that it follows the system's dark/light mode, and a whole Prepare.

Send me what fails (the terminal's output) and the screenshots.

## Still to do for a release

- An app bundle (`OpenMW-3DS-Setup.app`) with the icon, made by CMake (`MACOSX_BUNDLE`) and SDL2 copied into it
  (`dylibbundler` or `install_name_tool`), so players don't need Homebrew.
- Where the `port` folder goes for a bundle (next to the `.app`): `programFolder()` in `src/core/detect.cpp` must
  then step out of `OpenMW-3DS-Setup.app/Contents/MacOS`.
- Signing and notarisation, or players will see "unidentified developer" (right click > Open gets past it).
- Apple Silicon and Intel: build on each, or `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` with a universal SDL2.
