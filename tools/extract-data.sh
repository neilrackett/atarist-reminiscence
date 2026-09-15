#!/bin/bash
# Extract the Flashback Amiga data and music from CAPS IPF disk
# Copyright (C) 2026 Neil Rackett
# images into dist/DATA with GEMDOS-safe uppercase 8.3 names, and the
# score's ProTracker modules into tmp/music for tools/make-music.sh.
#
# Usage: tools/extract-data.sh disk1.ipf disk2.ipf disk3.ipf disk4.ipf
#        RS_DATA_DIR=/some/where tools/extract-data.sh ...   (override output)
#        RS_MUSIC_DIR=/some/where tools/extract-data.sh ...  (override modules)
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
MUSIC="${RS_MUSIC_DIR:-$HERE/../tmp/music}"
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
mkdir -p "$OUT" "$WORK" "$ADF" "$MUSIC"

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
        music:*)
            # The score is on the disks as one ProTracker module per
            # track. Their FILENAMES are the engine's primary track
            # names, but the port looks up the ALTERNATE name (see
            # ModPlayer::_names in src/staticres.cpp) - and that is
            # exactly what each module carries in its own 20-byte
            # title field. So the title is the name make-music.sh
            # needs, and reading it out of the file beats keeping a
            # mapping table here that could drift.
            [ "$(head -c 1084 "$f" | tail -c 4)" = "M.K." ] || continue
            title="$(head -c 20 "$f" | tr -d '\0' | tr -cd 'A-Za-z0-9_-')"
            [ -n "$title" ] || continue
            cp "$f" "$MUSIC/$title.mod"
            continue ;;
        data:*|cine:*) ;;
        *:FONT8.SPR) ;;
        *) continue ;;
    esac
    # GEMDOS 8.3: the engine's ST build asks for REPLICAN.SPM
    [ "$base" = "REPLICANT.SPM" ] && base="REPLICAN.SPM"
    cp "$f" "$OUT/$base"
done
echo "$(ls "$OUT" | wc -l | tr -d ' ') files in $OUT"
echo "$(find "$MUSIC" -name '*.mod' | wc -l | tr -d ' ') modules in $MUSIC (run tools/make-music.sh to convert them)"
