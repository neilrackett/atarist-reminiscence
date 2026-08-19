#!/bin/bash
# Extract the Flashback Amiga data files from CAPS IPF disk images
# into dist/DATA with GEMDOS-safe uppercase 8.3 names.
#
# Usage: tools/extract-data.sh disk1.ipf disk2.ipf disk3.ipf disk4.ipf
#
# Requires:
#  - capsimg (https://github.com/FrodeSolheim/capsimg) built as
#    capsimg.so next to the ipf2adf binary
#  - amitools (pip install amitools) for xdftool
#
# Build the converter first:
#   cc -O2 -o tools/ipf2adf tools/ipf2adf.c path/to/capsimg.so
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../dist/DATA"
WORK="$HERE/extracted"
ADF="$HERE/adf"
mkdir -p "$OUT" "$WORK" "$ADF"

n=1
for ipf in "$@"; do
    "$HERE/ipf2adf" "$ipf" "$ADF/disk$n.adf"
    xdftool "$ADF/disk$n.adf" unpack "$WORK"
    n=$((n+1))
done

# Flatten data/ and cine/ (plus the root font) with uppercase names.
find "$WORK" -type f | while read -r f; do
    case "$f" in
        *.xdfmeta|*.blkdev|*.bootcode) continue ;;
    esac
    rel="${f#"$WORK"/}"
    dir="$(basename "$(dirname "$f")")"
    base="$(basename "$f" | tr '[:lower:]' '[:upper:]')"
    case "$dir/$base" in
        */FONT8.SPR|DATA/*|CINE/*) ;;
        *) continue ;;
    esac
    # GEMDOS 8.3: the engine's ST build asks for REPLICAN.SPM
    [ "$base" = "REPLICANT.SPM" ] && base="REPLICAN.SPM"
    cp "$f" "$OUT/$base"
done
echo "$(ls "$OUT" | wc -l) files in $OUT"
