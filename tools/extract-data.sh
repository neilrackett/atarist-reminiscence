#!/bin/bash
# Extract the Flashback Amiga data files from CAPS IPF disk images
# into dist/DATA with GEMDOS-safe uppercase 8.3 names.
#
# Usage: tools/extract-data.sh disk1.ipf disk2.ipf disk3.ipf disk4.ipf
#        RS_DATA_DIR=/some/where tools/extract-data.sh ...   (override output)
#
# Requires:
#  - tools/ipf2adf built against capsimg
#    (https://github.com/FrodeSolheim/capsimg):
#      git clone https://github.com/FrodeSolheim/capsimg
#      (cd capsimg && ./bootstrap && ./configure && make)
#      cc -O2 -o tools/ipf2adf tools/ipf2adf.c capsimg/capsimg.so
#  - xdftool from amitools (pip install amitools) for the AmigaDOS
#    filesystem
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${RS_DATA_DIR:-$HERE/../dist/DATA}"
WORK="$HERE/extracted"
ADF="$HERE/adf"

if [ $# -lt 1 ]; then
    echo "usage: $0 disk1.ipf [disk2.ipf ...]" >&2
    exit 2
fi
if [ ! -x "$HERE/ipf2adf" ]; then
    echo "error: $HERE/ipf2adf not built - see the header of this script" >&2
    exit 1
fi
if ! command -v xdftool >/dev/null 2>&1; then
    # pip --user installs often aren't on PATH
    USERBIN="$(python3 -m site --user-base 2>/dev/null)/bin"
    if [ -x "$USERBIN/xdftool" ]; then
        PATH="$PATH:$USERBIN"
    else
        echo "error: xdftool not found - pip install amitools" >&2
        exit 1
    fi
fi

rm -rf "$WORK" "$ADF"
mkdir -p "$OUT" "$WORK" "$ADF"

n=1
for ipf in "$@"; do
    "$HERE/ipf2adf" "$ipf" "$ADF/disk$n.adf"
    xdftool "$ADF/disk$n.adf" unpack "$WORK"
    n=$((n+1))
done

# Flatten each disk's data/ and cine/ directories (plus the root
# font8.spr) into OUT with uppercase names.
find "$WORK" -type f | while read -r f; do
    case "$f" in
        *.xdfmeta|*.blkdev|*.bootcode) continue ;;
    esac
    dir="$(basename "$(dirname "$f")" | tr '[:upper:]' '[:lower:]')"
    base="$(basename "$f" | tr '[:lower:]' '[:upper:]')"
    case "$dir:$base" in
        data:*|cine:*) ;;
        *:FONT8.SPR) ;;
        *) continue ;;
    esac
    # GEMDOS 8.3: the engine's ST build asks for REPLICAN.SPM
    [ "$base" = "REPLICANT.SPM" ] && base="REPLICAN.SPM"
    cp "$f" "$OUT/$base"
done
echo "$(ls "$OUT" | wc -l | tr -d ' ') files in $OUT"
