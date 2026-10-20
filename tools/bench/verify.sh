#!/bin/bash
# verify.sh TAG [PREV] - does a build still draw what it should, and how fast is it
#
# Plays every scenario (or those in SCENS) twice with dist/FLASHBAK.TOS:
#  - hashing every frame (replay_hash=true), compared window by window
#    with the reference run REF: the change must draw exactly what the
#    reference does, and end each replay where its recording did;
#  - untimed, for its frame times, compared with run PREV if given, then
#    the time each room took to enter ("Room 45 (2055ms)" in RS.LOG).
# A copy of the binary is kept as tmp/bench/bin/TAG.TOS for later runs.
#
#   REF      the reference tag (default ref)
#   MACHINE  Hatari machine (default st)
#   SCENS    scenario names, space-separated (default: every one in SCEN)
#   SCEN     where the scenarios live (default tmp/bench/scenarios)
#   MEM, UNP as replay.sh takes them
#
# The reference is a replay_hash run of the build that draws every
# sprite from its source and redraws whole frames, made again after a
# scenario is added or re-recorded:
#   CFG="ab=24;replay_hash=true" tools/bench/replay.sh ref st <scenarios>
# Exits non-zero when a scenario's pictures or ending differ.
set -u
TAG=$1; PREV=${2:-}
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
REF=${REF:-ref}
M=${MACHINE:-st}
SCEN=${SCEN:-tmp/bench/scenarios}
SCENS=${SCENS:-$(ls "$SCEN")}
mkdir -p tmp/bench/bin
BIN=tmp/bench/bin/$TAG.TOS
cp dist/FLASHBAK.TOS "$BIN"

echo "== $TAG: pictures against $REF"
PROG=$BIN CFG="replay_hash=true" tools/bench/replay.sh "h$TAG" "$M" $SCENS | awk '{print $1, $NF}'
tools/bench/compare.py --frames "$REF" "h$TAG" "$M" $SCENS
bad=$?
for S in $SCENS; do
	if ! grep -q "REPLAY done.* match" "tmp/bench/runs/h${TAG}_${M}_$S/RS.LOG" 2>/dev/null; then
		echo "$S: did not end where its recording did"
		bad=1
	fi
done

echo "== $TAG: frame times${PREV:+ against $PREV}"
PROG=$BIN tools/bench/replay.sh "$TAG" "$M" $SCENS >/dev/null
if [ -n "$PREV" ]; then
	tools/bench/compare.py "$PREV" "$TAG" "$M" $SCENS
else
	for S in $SCENS; do
		printf "%-10s %s\n" "$S" "$(grep 'REPLAY done' tmp/bench/runs/${TAG}_${M}_$S/RS.LOG | sed 's/.*steady/steady/')"
	done
fi

echo "== $TAG: entering rooms (room:ms)"
for S in $SCENS; do
	printf "%-10s %s\n" "$S" "$(grep -o 'Room [0-9]* ([0-9]*ms)' tmp/bench/runs/${TAG}_${M}_$S/RS.LOG | sed 's/Room \([0-9]*\) (\([0-9]*\)ms)/\1:\2/' | tr '\n' ' ')"
done
exit $bad
