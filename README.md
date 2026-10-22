<h1 align="center">OpenMW port for the (New) 3DS</h1>

<p align="center">
  <a href="QA.md">Q/A</a> ·
  <a href="BUG_REPORT.md">Bug report</a> ·
  <a href="ROADMAP.md">Roadmap</a> ·
  <a href="CONTRIBUTING.md">Contributing</a>
</p>

[OpenMW](https://openmw.org) (Morrowind's open-source engine) for the New 3DS.

> [!CAUTION]
> No assets from the game are provided here. You need your own legally owned copy of Morrowind for PC (Steam, GOG or
> the disc), and the setup app turns its files into what the 3DS needs. This is an unofficial fan project, not
> affiliated with or endorsed by Bethesda Softworks, ZeniMax Media or Nintendo.

> [!WARNING]
> The port is experimental, but the main quest should be completable from start to finish. Some side quests may have
> issues. If you run into one, please [report it](BUG_REPORT.md).

## What you need

* A New 3DS, New 3DS XL or New 2DS XL with custom firmware. A port for the original 3DS and 2DS is in the works.
* Morrowind for PC, the English version (other languages are coming soon).
* A computer running Windows or Linux for the setup app.

## Getting started

1. Download the [latest release](https://github.com/Synaelle/OpenMW-3DS/releases/latest) and run the setup app. It
   finds your Morrowind and prepares everything.
2. Copy the folder it makes to your SD card.
3. Install the CIA with [FBI](https://github.com/Steveice10/FBI/releases/latest) or
   [Cement](https://github.com/Synaelle/cement/releases/latest) and start OpenMW. The first launch takes a while.

## What to expect

Tribunal and Bloodmoon aren't supported yet, and there are no shadows, water reflections or distant land, which the
3DS can't afford. Loading a new place takes a few seconds. Everything we know about is in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Building

The port builds on Windows, in PowerShell, from the repository's root. Everything specific to the 3DS is in the `3ds`
folder, OpenMW's own code is at the root.

1. Install the tools (MSYS2, devkitPro's 3DS toolchain and libraries, makerom and bannertool) and the Python
   packages the tools use. It's safe to run again.
   ```powershell
   3ds\scripts\bootstrap.ps1
   ```
2. Build OpenMW's dependencies for the 3DS (OpenSceneGraph, Bullet, Boost, ICU, SQLite...). This takes a while the
   first time.
   ```powershell
   3ds\scripts\build-deps.ps1
   ```
3. Build the game. The CIA ends up in `3ds\build\openmw\apps\openmw\openmw.cia`.
   ```powershell
   3ds\scripts\build-openmw.ps1
   ```
   This is a test build, with debug tools. Add `-NoDebug` for a build like the releases.

To build a full release (the CIA, the setup app for Windows and Linux, and the packages), run
`3ds\scripts\make-release.ps1 -Version x.y.z`. The Linux setup app is built in WSL with Docker. The setup app's own
instructions are in [3ds/setup](3ds/setup/README.md).

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

This port stands on the work of many people. See [CREDITS.md](CREDITS.md).

## License

GNU General Public License v3.0, like OpenMW. The Elder Scrolls III: Morrowind is © Bethesda Softworks LLC, a ZeniMax
Media company.
