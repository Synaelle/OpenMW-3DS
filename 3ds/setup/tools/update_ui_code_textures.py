#!/usr/bin/env python3
"""Refreshes data/ui_code_textures.txt: the texture names OpenMW's interface code names ("textures/..." literals in
apps/openmw/mwgui/*.cpp and .hpp), which go in the UI atlas with the ones the MyGUI skins and layouts reference
(those are read from the port's files at setup time). Run when the port's interface code changes:

    python tools/update_ui_code_textures.py <OpenMW source>/apps/openmw/mwgui
"""

import os
import re
import sys

pattern = re.compile(r'"(textures[/\\][A-Za-z0-9_/\\]+?)(?:\.(?:dds|tga|png))?"')
names = set()
folder = sys.argv[1]
for file in os.listdir(folder):
    if file.endswith((".cpp", ".hpp")):
        with open(os.path.join(folder, file), encoding="utf-8", errors="replace") as f:
            for match in pattern.finditer(f.read()):
                names.add(os.path.splitext(match.group(1).replace("\\", "/").lower())[0])
out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "ui_code_textures.txt")
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(sorted(names)) + "\n")
print(f"{len(names)} names written to {out}")
