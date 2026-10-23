# OpenMW for New 3DS: setup app

Prepares the folder players copy to their New 3DS's SD card, from their own Morrowind installation: the
configuration (with Morrowind.ini's values), the sound archive, the game's files and the textures cooked for the
3DS's GPU, next to the port's own files and its CIA. The game's files are only read, never modified or shared.

Preparing a folder that already has an install updates it: the sounds, the game's files and the textures are kept
when nothing they're made from has changed (`openmw/setup-state.txt` remembers what they were made from), so an
update mostly copies the port's new files. The app remembers the last folder it prepared and offers it next time.

Rust, with a Tauri window. Licence: GPL-3.0, as the port.

## What's here

- `core/`: the preparation (no interface): checking the Morrowind folder and telling its edition, finding it (Steam,
  GOG, the original installer), `openmw.cfg` and its fallback values, `sound3ds.bsa`, the texture cooker, the space
  an install takes, checking a copy on the SD card, the log (`openmw/logs/setup.log`). The cooker gives the same
  bytes as the port's `tools/cook_textures.py` and as the C++ setup app it replaced (checked on the whole pack, for
  the English, French and German games). Its ETC1 encoder is rg_etc1, ported (`core/src/cooker/etc1.rs`).
- `app/` and `ui/`: the window. `app/` is the Rust side (the commands the page calls, the preparation's thread),
  `ui/` the page (plain HTML, CSS and JavaScript, no build step). Light or dark as the system is.
- `cli/`: the command-line tool, and developer commands (`cook`, `dump`, `space`, `check`).
- `python/openmw3ds_setup.py`: a backup in Python (Pillow and numpy), without a window, for when the app doesn't run.
  The same folder, except the ETC1 blocks, which its own encoder packs a little differently.
- `data/`: lists taken from OpenMW's source (`tools/update_*.py` refresh them).
- `packaging/`: the READMEs that go in the release's downloads.

## Building

With [Rust](https://rustup.rs) and Node.js (for Tauri's command-line tool, fetched by `npx`). On Windows, the MSVC
toolchain (`rustup toolchain install stable-x86_64-pc-windows-msvc`, with Visual Studio's Build Tools): with it the
program is one file. On Linux, Tauri's libraries first (`libwebkit2gtk-4.1-dev` and the rest of
[its list](https://v2.tauri.app/start/prerequisites/)).

    cd app
    npx @tauri-apps/cli@2.12.4 build --no-bundle

The program is `target/release/openmw3ds-setup(.exe)`. It looks for the port's files (laid out as on the SD card:
`openmw/...`, `cias/openmw3ds.cia`) in a `port` folder next to it, or inside the app for the macOS and Linux
releases (`tauri.release.conf.json` puts `app/port` there).

The command-line tool alone: `cargo build --release -p openmw3ds-setup-cli`. The tests: `cargo test`.

The releases: `3ds/scripts/make-release.ps1` builds the Windows app. The macOS app (one for Intel and Apple Silicon)
and the Linux AppImage are built on GitHub by `.github/workflows/setup-app.yml`, with the release's port files.

## Using it

The window: `openmw3ds-setup`. The command line:

    openmw3ds-setup-cli --morrowind "<Morrowind folder>" --output <folder> --port <the release's port files>

The backup:

    python python/openmw3ds_setup.py --morrowind "<Morrowind folder>" --output <folder> --port <port files>

Checking the texture cooker against a reference pack: `openmw3ds-setup-cli cook ... --out new.o3tp --keys new.keys`,
then `python tools/compare_packs.py reference.o3tp new.o3tp --keys new.keys`.
