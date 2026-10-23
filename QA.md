# Q/A

<details>
<summary><b>Which consoles does it run on?</b></summary>

The New 3DS, New 3DS XL and New 2DS XL, with custom firmware. A port for the original 3DS and 2DS is in the works.

</details>

<details>
<summary><b>Which Morrowind do I need?</b></summary>

Morrowind for PC, in English, French or German, from Steam, GOG or the disc. Other languages will get support later.
The setup app checks your copy before it starts.

</details>

<details>
<summary><b>Do Tribunal and Bloodmoon work?</b></summary>

Not yet. The game runs Morrowind alone for now. They're on the [roadmap](ROADMAP.md).

</details>

<details>
<summary><b>Why a CIA and not a .3dsx?</b></summary>

The game needs all the memory the New 3DS gives an installed title, and a .3dsx gets less.

</details>

<details>
<summary><b>Copying to the SD card over Wi-Fi is slow. Is that normal?</b></summary>

Yes. The port is made of many small files, and FTP moves them one by one. A faster way is planned: the setup app will
talk to the game and send everything as a few big archives, unpacked on the console. If you can, putting the SD card
in your computer is much quicker.

</details>

<details>
<summary><b>The first launch is long. Is it stuck?</b></summary>

Probably not. The first time, the game goes through every place in Morrowind once to count what's in it, and keeps
the result in `/openmw/cache` on your SD card. The next launches skip that and are quicker.

</details>

<details>
<summary><b>How do I update?</b></summary>

Download the new release, run its setup app again, and copy the folder it makes to your SD card like the first time.
Your saves stay where they are. The next release will make this much smoother.

</details>

<details>
<summary><b>Where are my saves?</b></summary>

In `/openmw/saves` on your SD card. Back that folder up before you change anything big.

</details>

<details>
<summary><b>Can I use mods?</b></summary>

Not for now.

</details>

<details>
<summary><b>Does it run on an emulator?</b></summary>

Yes, in Azahar with the New 3DS mode turned on.

</details>

<details>
<summary><b>What are the controls?</b></summary>

| Button | In the game |
| --- | --- |
| Circle pad | Move (push it past halfway to run) |
| C stick | Look around |
| ZR | Attack or cast |
| ZL | Jump |
| A | Activate (talk, open, pick up) |
| B | Inventory, and close menus |
| X | Ready your weapon |
| Y | Ready your magic |
| L (hold) | Sneak |
| R | First or third person view |
| D pad up | Journal |
| D pad down | Rest |
| Start | Game menu |
| Select | Console, with the system keyboard |

Hold X or Y and press ZL or ZR to switch weapons or spells. Menus are on the touch screen. In class creation, which is
on the top screen, the circle pad moves a pointer.

</details>

<details>
<summary><b>Something went wrong. What do I do?</b></summary>

See [Bug report](BUG_REPORT.md).

</details>
