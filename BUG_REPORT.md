# Bug report

Found something broken? Thanks for telling us!

Before you start, check the [known issues](KNOWN_ISSUES.md). Your problem might already be there.

## Report it on GitHub

[Open a bug report](https://github.com/Synaelle/OpenMW-N3DS/issues/new?template=bug_report.yml). The form asks for:

1. **What happened**, and what you expected instead.
2. **Where it happened**, and the quest if it's about one.
3. **How to make it happen again**, step by step if you can.
4. **Your console and the port's version.** The version is on the game's start menu.
5. **The logs** (see below).
6. **A screenshot or a photo**, if you can.
7. **A save from just before it happens**, if you can.

Don't want to use GitHub? A Google Form is coming for that.

## Where to find the logs

Put your SD card in your computer, or open it over FTP, and go to `/openmw/logs`.

* Send `openmw3ds.log` and `openmw.log`.
* If the game crashed, send `openmw3ds-crash.log` too.
* Started the game again after the problem? Then send `openmw3ds-1.log` and `openmw-1.log` instead. Those are from the
  run before.

## Where to find your saves

In `/openmw/saves` on your SD card, one folder per character. Pick the save from just before the problem and put it in
a .zip file, GitHub won't take it otherwise.
