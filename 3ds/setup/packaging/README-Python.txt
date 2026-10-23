OpenMW for New 3DS {VERSION}, the setup in Python
A backup for when the setup app doesn't run on your computer. It does the same thing without a window.

1. Install Python 3 (https://www.python.org), then in a terminal: pip install pillow numpy
2. In this folder, run:
   python openmw3ds_setup.py --morrowind "the folder Morrowind is installed in"
   Your game's files are added to the "port" folder here. To put them somewhere else, add --output "a folder".
3. Copy that folder's contents to the root of your SD card.
4. Install cias/openmw3ds.cia with FBI (https://github.com/Steveice10/FBI/releases/latest) or Cement
   (https://github.com/Synaelle/cement/releases/latest), then start OpenMW. The first launch takes a while.

It takes a minute or two. The textures come out very slightly different from the app's, which doesn't change how the
game looks in practice. Keep the "port" and "data" folders next to the script. English, French and German Morrowind
work for now. Your game's files are only read, never changed, and none of them are in this download.
