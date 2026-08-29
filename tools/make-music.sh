#!/bin/bash
# Build the Atari ST chip-music set from the Amiga Flashback modules.
# Copyright (C) 2026 Neil Rackett
#
# The Amiga score is sampled music the ST cannot play: no DMA on a
# plain ST, and no software mixer in this port. The YM2149 is on
# every ST though, so the modules are converted to YM register
# streams offline: MOD -> SMF (tools/mod2smf.py) -> STM (stdlconv).
#
# Usage: tools/make-music.sh [module-dir]
#        tools/make-music.sh --download        (fetch the modules first)
#        RS_MUSIC_DIR=/some/where tools/make-music.sh ...
#
# With no arguments it reads modules from tmp/music/ and writes the
# .STM files back there. Both directories are gitignored: the
# modules are other people's work and the streams are derived from
# them, so neither belongs in the repository.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
OUT="${RS_MUSIC_DIR:-$ROOT/tmp/music}"
STDLCONV="$ROOT/stdl/tools/stdlconv/stdlconv.py"

# The Mod Archive ids for the 21 Flashback modules by Raphael
# Gesqua. They carry the same track names the engine uses, which is
# what makes the mapping in the port possible at all.
MODULES="
82409:options2 82410:reunion 82411:taxi 82412:teleport2
82413:teleporta 82414:voyage 84980:game_over 84981:holocube
84982:introb 84983:jungle 84984:logo 84985:missionca
84986:missions2 86732:ascenseur 87379:options1 87915:ceinturea
87916:chute 87917:desintegr 87918:donneobjt 87919:fin 87956:fin2
"

download() {
    mkdir -p "$OUT"
    for entry in $MODULES; do
        id="${entry%%:*}"
        name="flashback-${entry##*:}.mod"
        if [ -f "$OUT/$name" ]; then
            continue
        fi
        echo "  fetching $name"
        curl -sfL --max-time 60 -A "Mozilla/5.0" \
             -o "$OUT/$name" \
             "https://api.modarchive.org/downloads.php?moduleid=$id"
        sleep 1                      # be polite to the archive
    done
}

if [ "${1:-}" = "--download" ]; then
    echo "Downloading modules from The Mod Archive:"
    download
    shift || true
fi

SRC="${1:-$OUT}"
if [ ! -d "$SRC" ]; then
    echo "no module directory: $SRC (try --download)" >&2
    exit 2
fi
if ! ls "$SRC"/*.mod >/dev/null 2>&1; then
    echo "no .mod files in $SRC (try --download)" >&2
    exit 2
fi
mkdir -p "$OUT"

echo "Converting modules to YM streams:"
python3 - "$SRC" "$OUT" "$HERE/mod2smf.py" "$STDLCONV" <<'PY'
import glob, os, subprocess, sys

src, out, mod2smf, stdlconv = sys.argv[1:5]
used, total, failed = {}, 0, 0
for path in sorted(glob.glob(os.path.join(src, "*.mod"))):
    base = os.path.basename(path)
    stem = base[:-4]
    if stem.startswith("flashback-"):
        stem = stem[len("flashback-"):]
    # GEMDOS 8.3, uppercase, uniquified: teleport2/teleporta would
    # otherwise both truncate to TELEPORT
    name = stem.upper().replace("_", "")[:8]
    n, i = name, 1
    while n in used:
        n = name[:7] + str(i)
        i += 1
    used[n] = stem

    mid = os.path.join(out, stem + ".mid")
    stm = os.path.join(out, n + ".STM")
    r1 = subprocess.run([sys.executable, mod2smf, path, mid],
                        capture_output=True, text=True)
    r2 = subprocess.run([sys.executable, stdlconv, "midi", mid, stm],
                        capture_output=True, text=True)
    if os.path.exists(stm):
        size = os.path.getsize(stm)
        secs = ""
        if "frames" in r2.stdout:
            secs = r2.stdout.split("=")[1].split("at")[0].strip()
        print("  %-14s -> %-13s %6d bytes  %s" % (stem, n + ".STM", size, secs))
        total += size
    else:
        print("  %-14s FAILED: %s" % (stem, (r1.stderr or r2.stderr).strip()[:60]))
        failed += 1
    if os.path.exists(mid):
        os.remove(mid)                # the SMF is just an intermediate

print("\n%d tracks, %d bytes total%s"
      % (len(used) - failed, total,
         "" if not failed else ", %d FAILED" % failed))
PY

echo
echo "Streams are in $OUT"
echo "Play one on target with STDL's example:"
echo "  cp $OUT/JUNGLE.STM somewhere/DEMO.STM"
echo "  stdl/tests/hatari/run.sh ym stdl/dist/PLAYMUS.TOS 8 'sleep 20'"
