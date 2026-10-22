# OpenMW for New 3DS: setup app

Prepares the folder players copy to their New 3DS's SD card, from their own Morrowind installation: the
configuration (with Morrowind.ini's values), the sound archive, the game's files and the textures cooked for the
3DS's GPU, next to the port's own files and its CIA. The game's files are only read, never modified or shared.

Part of M21 (the first release). It lives in the port's repository for now and will get its own once it's polished.
Licence: GPL-3.0, as the port.

## What's here

- `src/core/`: the preparation (no interface): checking the Morrowind folder, finding it (Steam, GOG, the original
  installer), `openmw.cfg` and its fallback values, `sound3ds.bsa`, the texture cooker. The cooker gives the same
  bytes as the port's `tools/cook_textures.py` (checked on the whole pack).
- `src/gui/`: the window (Dear ImGui over SDL2), light or dark as the system is.
- `src/cli/`: the command-line tool, and the developer commands `cook` and `dump`.
- `python/openmw3ds_setup.py`: a backup in Python (Pillow and numpy), without a window, for when the app doesn't run.
  The same folder, except the ETC1 blocks, which its own encoder packs a little differently.
- `data/`: lists taken from OpenMW's source (`tools/update_*.py` refresh them).
- `third_party/`: Dear ImGui, stb_image, rg_etc1 (from devkitPro's tex3ds).

## Building (Windows, MSYS2 UCRT64 with SDL2)

    cmake -S . -B build -G Ninja
    cmake --build build

The release puts the port's files (laid out as on the SD card: `openmw/...`, `cias/openmw3ds.cia`) in a `port`
folder next to the program.

## Using it

The window: `openmw3ds-setup`. The command line:

    openmw3ds-setup-cli --morrowind "<Morrowind folder>" --output <folder> --port <the release's port files>

The backup:

    python python/openmw3ds_setup.py --morrowind "<Morrowind folder>" --output <folder> --port <port files>

Testing the window without showing it: `openmw3ds-setup --screenshot shot.png --state ready|working|done|failed|run
[--theme dark|light] [--output folder]` (`run` really prepares, then pictures the result).
