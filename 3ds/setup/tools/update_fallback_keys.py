#!/usr/bin/env python3
"""Refreshes data/fallback_keys.txt from OpenMW's mwiniimporter (the Morrowind.ini keys it imports as fallback
values), when the port moves to a newer OpenMW:

    python tools/update_fallback_keys.py <OpenMW source>/apps/mwiniimporter/importer.cpp
"""

import os
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
block = re.search(r"const char\* fallback\[\] = \{(.*?)\n\s*0\s*\};", src, re.S)
if not block:
    sys.exit("no fallback[] list in " + sys.argv[1])
keys = re.findall(r'"([^"]+:[^"]*)"', block.group(1))
out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "fallback_keys.txt")
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(keys) + "\n")
print(f"{len(keys)} keys written to {out}")
