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
#
# -print0 rather than a bare `read`, which mangles any name with a
# space in it, and shell parameter expansion rather than basename and
# dirname: this runs for every file on four disks, and a fork costs
# more than all the tests here together.
find "$WORK" -type f -print0 | while IFS= read -r -d '' f; do
    case "$f" in
        *.xdfmeta|*.blkdev|*.bootcode) continue ;;
    esac
    d="${f%/*}"; d="${d##*/}"
    name="${f##*/}"
    case "$d" in
        [Mm][Uu][Ss][Ii][Cc])
            # The score is on the disks as one ProTracker module per
            # track. Their FILENAMES are the engine's primary track
            # names, but the port looks up the ALTERNATE name (see
            # ModPlayer::_names in src/staticres.cpp) - and that is
            # exactly what each module carries in its own 20-byte
            # title field. So the title is the name make-music.sh
            # needs, and reading it out of the file beats keeping a
            # mapping table here that could drift.
            [ "$(head -c 1084 "$f" | tail -c 4)" = "M.K." ] || {
                echo "  skipped $name: not a ProTracker module" >&2
                continue
            }
            # LC_ALL=C: a module header is not UTF-8, and tr complains
            # about it otherwise. One tr, not two - deleting everything
            # outside the set already takes the NUL padding with it.
            title="$(head -c 20 "$f" | LC_ALL=C tr -cd 'A-Za-z0-9_-')"
            [ -n "$title" ] || {
                echo "  skipped $name: no title to name it from" >&2
                continue
            }
            cp "$f" "$MUSIC/$title.mod"
            continue ;;
        [Dd][Aa][Tt][Aa]|[Cc][Ii][Nn][Ee]) ;;
        *) case "$name" in
               [Ff][Oo][Nn][Tt]8.[Ss][Pp][Rr]) ;;
               *) continue ;;
           esac ;;
    esac
    base="$(printf '%s' "$name" | LC_ALL=C tr '[:lower:]' '[:upper:]')"
    # Not a truncation rule: the ST build was changed to ASK for this
    # name, at the #ifdef ATARIST in src/staticres.cpp (_monsterNames).
    # The two have to agree, so keep them named rather than derived.
    [ "$base" = "REPLICANT.SPM" ] && base="REPLICAN.SPM"
    cp "$f" "$OUT/$base"
done
echo "$(ls "$OUT" | wc -l | tr -d ' ') files in $OUT"
echo "$(find "$MUSIC" -name '*.mod' | wc -l | tr -d ' ') modules in $MUSIC (run tools/make-music.sh to convert them)"
