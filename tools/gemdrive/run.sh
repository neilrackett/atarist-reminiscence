#!/bin/bash
# run.sh - does the game survive a second run from an ACSI2STM GemDrive drive?
#
# GemDrive's loader clears only the first 130,560 bytes of a program's
# BSS (one 255-sector DMA run), so the rest starts with what the last
# program left there. gdemu.c re-creates that in Hatari: it runs
# dist/FLASHBAK.TOS twice from the same address (cutscene=0x40, the
# logos, then quit), puts the first run's leftovers back into the
# second run's BSS past that point, and scribbles the rest of free
# memory in between. It passes when both runs return 0; r31, before
# the game cleared its own BSS, bus-errors in mintlib's localsub.
#
#   MACHINE  Hatari machine (default st)
#   PROG     the binary (default dist/FLASHBAK.TOS)
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
D=tmp/gemdrive/run
rm -rf "$D"; mkdir -p "$D"
cp -R dist/DATA "$D"/
[ -d dist/MUSIC ] && cp -R dist/MUSIC "$D"/
[ -d dist/UNPACKED ] && cp -R dist/UNPACKED "$D"/
cp "${PROG:-dist/FLASHBAK.TOS}" "$D"/FLASHBAK.TOS
printf 'logging=true\ncutscene=0x40\n' > "$D"/RS.CFG
STCMD_NO_TTY=1 stcmd m68k-atari-mint-gcc -O2 -Wall -o "$D"/GDEMU.TOS tools/gemdrive/gdemu.c 2>&1 | grep -v ST_WORKING
TOS="tmp/tos/TOS 2.06 - UK - Original.IMG" \
EXTRA="--cpu-exact on --compatible on --memsize 4" MACHINE=${MACHINE:-st} \
	stdl/tests/hatari/run.sh gemdrive "$D"/GDEMU.TOS 6 "waitfor run 1 returned; waitfor GDEMU done" >/dev/null 2>&1
LOG=stdl/tests/hatari/out/gemdrive.log
grep '^GDEMU' "$LOG"
if grep -q 'run 1 returned 0' "$LOG" && grep -q 'run 2 returned 0' "$LOG"; then
	echo PASS
else
	echo FAIL
	exit 1
fi
