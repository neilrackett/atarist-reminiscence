#!/bin/bash
# verify.sh TAG [REF] - do the cutscenes draw exactly what REF drew?
#
# Plays the scene set (SCENES, default the intro, every level opening,
# a mid-level scene and the credits) on a plain ST with every frame
# drawn and no music, hashing each drawn frame into CUTHASH.BIN
# (replay_hash=true, see Cutscene::stStatsEnd), and compares the hashes
# with run REF's (default cref), and the colours each was shown in
# (CUTPAL.BIN) where both runs have them. The frame count and every hash must
# match; the first frame that differs is printed. A run with no REF yet
# becomes one.
#
#   PROG    the binary (default dist/FLASHBAK.TOS)
#   CFG     extra RS.CFG lines, ;-separated, e.g. "ab=0x200"
#   SCENES  scene ids to play
set -u
TAG=$1; REF=${2:-cref}
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
SCENES=${SCENES:-"0x40 0x0D 0x00 0x2F 0x34 0x39 0x35 0x41 0x3D"}
EXTRA_CFG=${CFG:-}
CFG="frame_skip=false;music=false;replay_hash=true${EXTRA_CFG:+;$EXTRA_CFG}" \
	tools/cutbench/run.sh "$TAG" st $SCENES >/dev/null
python3 - "$TAG" "$REF" $SCENES <<'EOF'
import os, struct, sys
tag, ref, scenes = sys.argv[1], sys.argv[2], sys.argv[3:]
runs = 'tmp/cutbench/runs'
def load(t, s, name='CUTHASH.BIN'):
    p = os.path.join(runs, '%s_st_%s' % (t, s), name)
    if not os.path.exists(p):
        return None
    d = open(p, 'rb').read()
    parts, i = [], 0
    while i + 8 <= len(d):
        assert d[i:i+4] == b'CUTH', p
        sid, n = struct.unpack('>HH', d[i+4:i+8])
        parts.append((sid, list(struct.unpack('>%dI' % n, d[i+8:i+8+4*n]))))
        i += 8 + 4 * n
    return parts
bad = 0
for s in scenes:
    a, b = load(tag, s), load(ref, s)
    if a is None:
        print('%-5s no CUTHASH.BIN from %s' % (s, tag)); bad += 1; continue
    if b is None:
        print('%-5s %d parts, %d frames: no reference yet' % (s, len(a), sum(len(h) for _, h in a))); continue
    msg = []
    for (sa, ha), (sb, hb) in zip(a, b):
        if sa != sb or len(ha) != len(hb):
            msg.append('%02X: %d frames vs %d' % (sa, len(ha), len(hb)))
            continue
        diff = [i for i in range(len(ha)) if ha[i] != hb[i]]
        if diff:
            msg.append('%02X: %d of %d frames differ, first %d' % (sa, len(diff), len(ha), diff[0]))
    if len(a) != len(b):
        msg.append('%d parts vs %d' % (len(a), len(b)))
    # the colours shown, where both runs recorded them
    pa, pb = load(tag, s, 'CUTPAL.BIN'), load(ref, s, 'CUTPAL.BIN')
    if pa is not None and pb is not None:
        for (sa, ha), (sb, hb) in zip(pa, pb):
            diff = [i for i in range(min(len(ha), len(hb))) if ha[i] != hb[i]]
            if diff or len(ha) != len(hb):
                msg.append('%02X colours: %d of %d frames differ, first %d' % (sa, len(diff), len(ha), diff[0] if diff else -1))
    if msg:
        bad += 1
        print('%-5s DIFFERS: %s' % (s, '; '.join(msg)))
    else:
        print('%-5s identical (%d frames)' % (s, sum(len(h) for _, h in a)))
sys.exit(1 if bad else 0)
EOF
