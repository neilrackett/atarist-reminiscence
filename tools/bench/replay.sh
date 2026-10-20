#!/bin/bash
# replay.sh TAG MACHINE SCENARIO... - play recorded sessions back in Hatari
#
# A scenario is a directory holding a save (RS<level>_<slot>.SAV) and
# the recording made from it (REC01.DEM, written by record=1 in RS.CFG
# after a title Load). replay=1 plays it back with no pacing, so the
# log says what every frame cost on the emulated machine, and checks
# that the game ended where the recording did. Each run gets its own
# copy of the game as Hatari's C: drive, so scenarios run side by side;
# the log lands in tmp/bench/runs/TAG_MACHINE_SCENARIO/RS.LOG.
#
#   PROG  the binary to run (default dist/FLASHBAK.TOS)
#   CFG   extra RS.CFG lines, ;-separated, e.g. "ab=16;replay_hash=true"
#   MEM   Hatari --memsize: 4 (default) or 2560 for a 2.5MB machine
#   SCEN  where the scenarios live (default tmp/bench/scenarios)
#   UNP   the UNPACKED folder to give the game (default dist/UNPACKED),
#         or none
#
# Always cycle-exact (see stdl/tests/hatari/run.sh). Compare two runs
# with tools/bench/compare.py. A Mega STE moves with code layout, so
# compare it inside one binary (ab= switches the optimisations off).
set -u
TAG=$1; M=$2; shift 2
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
PROG=${PROG:-dist/FLASHBAK.TOS}
SCEN=${SCEN:-tmp/bench/scenarios}
MEM=${MEM:-4}
UNP=${UNP:-dist/UNPACKED}
RUNS=tmp/bench/runs
mkdir -p "$RUNS"
for S in "$@"; do
	D="$RUNS/${TAG}_${M}_$S"
	rm -rf "$D"; mkdir -p "$D"
	cp -R dist/DATA dist/MUSIC "$D"/
	[ "$UNP" != none ] && [ -d "$UNP" ] && cp -R "$UNP" "$D"/UNPACKED
	[ -d dist/PALETTE ] && cp -R dist/PALETTE "$D"/
	cp "$SCEN/$S"/*.SAV "$SCEN/$S"/REC01.DEM "$D"/
	cp "$PROG" "$D"/FLASHBAK.TOS
	printf 'skip_intro=true\nlogging=true\nreplay=1\nmusic=false\n' > "$D"/RS.CFG
	[ -n "${CFG:-}" ] && echo "$CFG" | tr ';' '\n' >> "$D"/RS.CFG
	TOS="tmp/tos/TOS 2.06 - UK - Original.IMG" \
	EXTRA="--cpu-exact on --compatible on --memsize $MEM" MACHINE=$M \
		stdl/tests/hatari/run.sh "rp_${TAG}_${M}_$S" "$D"/FLASHBAK.TOS 8 \
		"waitfile REPLAY.done $D/RS.LOG; sleep 1" >/dev/null 2>&1 &
done
wait
for S in "$@"; do
	printf "%-8s %-8s %s\n" "$S" "$M" "$(grep 'REPLAY done' "$RUNS/${TAG}_${M}_$S/RS.LOG" | sed 's/REPLAY done: //')"
done
