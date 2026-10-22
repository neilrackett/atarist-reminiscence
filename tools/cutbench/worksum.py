#!/usr/bin/env python3
"""worksum.py MACHINE TAG [TAG...] - total work per scene, run against run

Sums the work of every frame from the CUT lines in
tmp/cutbench/runs/TAG_MACHINE_SCENE/RS.LOG (tools/cutbench/run.sh) and
prints one row per scene, a column per tag, with the last tag against
the first. Each CUT line averages eight frames, so a total is to the
nearest eight frames' rounding. Run with CFG="frame_skip=false" (and
music=false, or true to count the STE mixer): with frames skipped the
work depends on which were drawn. Quote the total, not one scene.
"""
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNS = os.path.join(REPO, 'tmp', 'cutbench', 'runs')
CUT = re.compile(r'CUT (\w+) f(\d+): w(\d+)/')


def totals(tag, machine):
    out = {}
    for d in glob.glob(os.path.join(RUNS, '%s_%s_*' % (tag, machine))):
        scene = d.rsplit('_', 1)[1]
        log = open(os.path.join(d, 'RS.LOG'), errors='replace').read()
        out[scene] = sum(int(m.group(3)) * 8 for m in CUT.finditer(log))
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    machine, tags = sys.argv[1], sys.argv[2:]
    runs = [totals(t, machine) for t in tags]
    scenes = sorted(set().union(*runs))
    print('scene  ' + '  '.join('%8s' % t for t in tags))
    sums = [0] * len(tags)
    for s in scenes:
        row = [r.get(s, 0) for r in runs]
        sums = [a + b for a, b in zip(sums, row)]
        change = ('   %+.1f%%' % ((row[-1] - row[0]) * 100.0 / row[0])) if row[0] and len(row) > 1 else ''
        print('%-6s ' % s + '  '.join('%8d' % v for v in row) + change)
    change = ('   %+.1f%%' % ((sums[-1] - sums[0]) * 100.0 / sums[0])) if sums[0] and len(sums) > 1 else ''
    print('total  ' + '  '.join('%8d' % v for v in sums) + change)


if __name__ == '__main__':
    main()
