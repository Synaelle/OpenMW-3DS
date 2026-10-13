#!/usr/bin/env bash
# Summarizes undefined references from a failed link: scripts/link-undefined.sh <build-dir> [target]
# Prints the count and the distinct missing symbols, grouped by the library that wanted them.
cd "$1" || exit 1
ninja ${2:-} > /tmp/link.log 2>&1
grep -E "undefined reference to" /tmp/link.log \
    | sed -E "s/^.*\/lib([^/]*)\.a\(([^)]*)\).*undefined reference to .(.*).$/\1\t\3/; t; s/^.*undefined reference to .(.*).$/(objects)\t\1/" \
    | sort -u > /tmp/undef.tsv
echo "distinct (library, symbol) pairs: $(wc -l < /tmp/undef.tsv)"
echo "--- by requesting library:"
cut -f1 /tmp/undef.tsv | sort | uniq -c | sort -rn
echo "--- distinct missing symbols (first 80, ICU internals filtered):"
cut -f2 /tmp/undef.tsv | sort -u | grep -v -E "icu_73::|^(u|uprv|ucol|uloc|ures|udata|ucase|utrie|umutex|uhash|ustr|uset|unorm|ucurr|uenum|ubrk|uplrules|unum|ucln|ucnv|ufmt|uiter|ulist|ucptrie|udtitvfmt|ubidi|uscript)_" | head -80
