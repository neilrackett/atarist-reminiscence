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
#        RS_MUSIC_DIR=/some/where tools/make-music.sh ...
#
# With no arguments it reads modules from tmp/music/ and writes the
# .STM files back there, which is where tools/extract-data.sh puts
# the modules it finds on the game's own disks. There is no download
# path: the game cannot run without its data files, so anyone able to
# play already has the disks the score is on - and the disks carry a
# complete set, where the copy on The Mod Archive is missing memoire.
#
# tmp/music is gitignored: the modules are other people's work and
# the streams are derived from them, so neither belongs here.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
OUT="${RS_MUSIC_DIR:-$ROOT/tmp/music}"
STDLCONV="$ROOT/stdl/tools/stdlconv/stdlconv.py"

SRC="${1:-$OUT}"
no_modules() {
    echo "no modules in $1" >&2
    echo "Extract them from your own disks with:" >&2
    echo "    tools/extract-data.sh disk1.ipf disk2.ipf disk3.ipf disk4.ipf" >&2
    exit 2
}
if [ ! -d "$SRC" ]; then
    no_modules "$SRC"
fi
if ! ls "$SRC"/*.mod >/dev/null 2>&1; then
    no_modules "$SRC"
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
    # GEMDOS 8.3, uppercase, underscore dropped. Where a name is too
    # long, keep its LAST character rather than truncating: teleporta
    # and teleport2 differ only there and would both become TELEPORT.
    # The game derives the same name from its own track table
    # (ATARIST_musicName in src/mixer.cpp) - keep the two in step.
    up = stem.upper().replace("_", "")
    n = (up[:7] + up[-1]) if len(up) > 8 else up
    if n in used:
        print("  !! %s and %s both map to %s" % (stem, used[n], n))
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
echo "Copy them into a MUSIC\\ folder beside FLASHBAK.TOS (not DATA\\, which"
echo "stays as it came off the disks) and set music=true in RS.CFG."
echo
echo "Play one on target with STDL's example:"
echo "  cp $OUT/JUNGLE.STM somewhere/DEMO.STM"
echo "  stdl/tests/hatari/run.sh ym stdl/dist/PLAYMUS.TOS 8 'sleep 20'"
