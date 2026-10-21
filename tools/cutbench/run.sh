#!/bin/bash
# run.sh TAG MACHINE SCENE... - play cutscenes on their own in Hatari
#
# Each scene (an id: 0x00, 0x34, 13...) runs through the hidden
# cutscene=N option, which plays that one scene from where the intro
# would and quits, in its own copy of the game as Hatari's C: drive,
# so scenes run side by side. The log lands in
# tmp/cutbench/runs/TAG_MACHINE_SCENE/RS.LOG, with a CUT line per eight
# frames and a CUTSUM line per scene part (Cutscene::stStatsEnd);
# tools/cutbench/summarise.py makes a table of them.
#
#   CFG   extra RS.CFG lines, ;-separated: "frame_skip=false;music=false"
#         for the work a scene takes, replay_hash=true for CUTHASH.BIN
#   MEM   Hatari --memsize: 4 (default) or 2560 for a 2.5MB machine
#   PROG  the binary (default dist/FLASHBAK.TOS)
#
# Defaults are the player's: music on, frame skipping on, screen Fit.
# Always cycle-exact (see stdl/tests/hatari/run.sh).
set -u
TAG=$1; M=$2; shift 2
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
PROG=${PROG:-dist/FLASHBAK.TOS}
MEM=${MEM:-4}
RUNS=tmp/cutbench/runs
mkdir -p "$RUNS"
for S in "$@"; do
	D="$RUNS/${TAG}_${M}_$S"
	rm -rf "$D"; mkdir -p "$D"
	cp -R dist/DATA "$D"/
	[ -d dist/MUSIC ] && cp -R dist/MUSIC "$D"/
	[ -d dist/UNPACKED ] && cp -R dist/UNPACKED "$D"/
	cp "$PROG" "$D"/FLASHBAK.TOS
	printf 'skip_intro=true\nlogging=true\ncutscene=%s\n' "$S" > "$D"/RS.CFG
	[ -n "${CFG:-}" ] && echo "$CFG" | tr ';' '\n' >> "$D"/RS.CFG
	# waitfile gives up after two minutes of host time: three of them
	# cover the credits
	W="waitfile bench.done $D/RS.LOG"
	TOS="tmp/tos/TOS 2.06 - UK - Original.IMG" \
	EXTRA="--cpu-exact on --compatible on --memsize $MEM" MACHINE=$M \
		stdl/tests/hatari/run.sh "cb_${TAG}_${M}_$S" "$D"/FLASHBAK.TOS 8 \
		"$W; $W; $W; sleep 1" >/dev/null 2>&1 &
done
wait
for S in "$@"; do
	D="$RUNS/${TAG}_${M}_$S"
	grep -h "CUTSUM" "$D"/RS.LOG 2>/dev/null | sed "s/^/$M  /" || echo "$M  $S: no CUTSUM line"
done
