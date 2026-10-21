#!/bin/bash
# prof.sh OUT MACHINE SCENE - a Hatari profile of one cutscene
#
# Plays SCENE as tools/cutbench/run.sh does (every frame drawn, no
# music unless CFG says otherwise), with the profiler on from the
# scene's start line in the log to its first CUTSUM line, and saves
# the profile to OUT. tools/cutbench/prof.py OUT reads it. Symbols come
# from build/flashbak.elf, so build first and keep the binary that
# matches it in dist/.
#
#   CFG   extra RS.CFG lines, ;-separated
set -u
OUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); M=$2; S=$3
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
D=tmp/cutbench/runs/prof_${M}_$S
SYM=$REPO/tmp/cutbench/SYM.txt
python3 tools/cutbench/syms.py build/flashbak.elf "$SYM"
rm -rf "$D"; mkdir -p "$D"
cp -R dist/DATA "$D"/
[ -d dist/MUSIC ] && cp -R dist/MUSIC "$D"/
[ -d dist/UNPACKED ] && cp -R dist/UNPACKED "$D"/
cp dist/FLASHBAK.TOS "$D"/FLASHBAK.TOS
printf 'skip_intro=true\nlogging=true\nframe_skip=false\nmusic=false\ncutscene=%s\n' "$S" > "$D"/RS.CFG
[ -n "${CFG:-}" ] && echo "$CFG" | tr ';' '\n' >> "$D"/RS.CFG
# Symbols load at the program's first log line (they take a while);
# the profile runs from the scene's start line to the first per-frame
# line, which is written once the scene is over (the log writing after
# it stays out). At real speed: under fast-forward a short scene is
# over before the fifo has been read.
W="waitfile CUT.[0-9A-F]*.f0: $D/RS.LOG"
FF=off TOS="tmp/tos/TOS 2.06 - UK - Original.IMG" \
EXTRA="--cpu-exact on --compatible on --memsize 4" MACHINE=$M \
	stdl/tests/hatari/run.sh "cbprof_${M}_$S" "$D"/FLASHBAK.TOS 8 \
	"waitfile REminiscence $D/RS.LOG; fifo hatari-debug symbols $SYM 0x149f2 0x149f2 0x149f2; waitfile Cutscene $D/RS.LOG; fifo hatari-debug profile on; $W; $W; $W; fifo hatari-debug profile save $OUT; sleep 3" >/dev/null 2>&1
grep -h "CUTSUM" "$D"/RS.LOG | head -1
