#!/usr/bin/env bash
# Helper to create a dependency patch from an edited source tree.
#   deps/mkpatch.sh <component> <patch-name> <relative/file> [more files...]
# Compares .deps/src/<component>/<file> against the pristine copy from the downloaded archive
# (re-extracted to a temp dir) and writes deps/patches/<component>/<patch-name>.patch.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
comp="$1" name="$2"; shift 2
src="$ROOT/.deps/src/$comp"
pristine="$ROOT/.deps/pristine/$comp"
[ -d "$pristine" ] || { echo "no pristine copy at $pristine (build-deps.sh keeps one)" >&2; exit 1; }
mkdir -p "$ROOT/deps/patches/$comp"
out="$ROOT/deps/patches/$comp/$name.patch"
: > "$out"
for f in "$@"; do
    (cd "$ROOT/.deps" && git diff --no-index --no-prefix -- "pristine/$comp/$f" "src/$comp/$f" || true) \
        | sed -e "s#^--- pristine/$comp/#--- a/#" -e "s#^+++ src/$comp/#+++ b/#" \
              -e "s#^diff --git pristine/$comp/\([^ ]*\) src/$comp/#diff --git a/\1 b/#" >> "$out"
done
echo "wrote $out ($(grep -c '^@@' "$out") hunks)"
