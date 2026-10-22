"""Generate OpenMW `fallback=` lines from a Morrowind.ini, like OpenMW's mwiniimporter.

The list of imported keys is read from openmw/apps/mwiniimporter/importer.cpp so it always matches
upstream. Conversion rule (MwIniImporter::mergeFallback): "Section:Key" -> "Section_Key" with spaces
replaced by '_', then ",<value>".

    python tools/mwini_fallbacks.py <Morrowind.ini> [--encoding cp1252] > fallbacks.cfg
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IMPORTER = os.path.join(HERE, "..", "..", "apps", "mwiniimporter", "importer.cpp")


def fallback_keys():
    src = open(IMPORTER, encoding="utf-8").read()
    block = re.search(r"const char\* fallback\[\] = \{(.*?)\n\s*0\s*\};", src, re.S)
    if not block:
        sys.exit("could not find the fallback[] list in importer.cpp")
    return re.findall(r'"([^"]+:[^"]*)"', block.group(1))


def parse_ini(path, encoding):
    """Returns {"Section:Key": [values...]} the way MwIniImporter::loadIniFile does."""
    result = {}
    section = ""
    with open(path, encoding=encoding, errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith(";"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            key, value = key.strip(), value.strip()
            if not key:
                continue
            result.setdefault(f"{section}:{key}", []).append(value)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ini")
    ap.add_argument("--encoding", default="cp1252", help="cp1252 for English/Western releases")
    args = ap.parse_args()

    ini = parse_ini(args.ini, args.encoding)
    keys = fallback_keys()
    out = sys.stdout
    out.write(f"# fallback values imported from {os.path.basename(args.ini)} by tools/mwini_fallbacks.py\n")
    count = 0
    for key in keys:
        for value in ini.get(key, []):
            name = key.replace(" ", "_").replace(":", "_")
            out.write(f"fallback={name},{value}\n")
            count += 1
    print(f"{count} fallback values from {len(keys)} known keys", file=sys.stderr)


if __name__ == "__main__":
    main()
