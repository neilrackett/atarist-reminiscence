#!/usr/bin/env python3
"""summarise.py TAG [TAG...] - one row per scene part from cutbench runs

Reads tmp/cutbench/runs/TAG_MACHINE_SCENE/RS.LOG (tools/cutbench/run.sh)
and prints each CUTSUM line as a table row: frames, shown, the longest
run not shown, late frames, work p50/p95/max against the budget, the
push and restore per frame, and the shape sprites refused. With two
tags, the same rows side by side, the second against the first.
"""
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNS = os.path.join(REPO, 'tmp', 'cutbench', 'runs')

SUM = re.compile(
    r'CUTSUM (\w+): (\d+) frames, (\d+) shown \((\d+)%\), longest not shown (\d+), '
    r'(\d+) late, work p50 (\d+) p95 (\d+) max (\d+) ms, budget (\d+), push (\d+), '
    r'restore (\d+) ms/frame, polygons (\d+) from the cache of (\d+), '
    r'sprites (\d+) \((\d+) baked\), refused (\d+)/(\d+)/(\d+)')


def rows(tag):
    out = {}
    for log in sorted(glob.glob(os.path.join(RUNS, tag + '_*', 'RS.LOG'))):
        run = os.path.basename(os.path.dirname(log))[len(tag) + 1:]
        machine, scene = run.split('_', 1)
        with open(log, errors='replace') as f:
            for line in f:
                m = SUM.search(line)
                if m:
                    g = [m.group(1)] + [int(x) for x in m.groups()[1:]]
                    out[(scene, machine, g[0])] = g
    return out


def fmt(g):
    (part, frames, shown, pct, run, late, p50, p95, wmax, budget, push, restore,
     hit, polys, spr, baked, rf, rc, rs) = g
    return '%3d/%-3d %3d%% run%-2d late%-3d w %3d/%3d/%-4d b%-3d p%-2d r%-2d ref %d/%d/%d' % (
        shown, frames, pct, run, late, p50, p95, wmax, budget, push, restore, rf, rc, rs)


def main():
    tags = sys.argv[1:]
    if not tags:
        print(__doc__)
        sys.exit(1)
    data = [rows(t) for t in tags]
    keys = sorted(set().union(*[d.keys() for d in data]), key=lambda k: (k[1], k[0], k[2]))
    print('%-7s %-8s %-4s  %s' % ('scene', 'machine', 'part', '  '.join(tags)))
    for k in keys:
        cells = [fmt(d[k]) if k in d else '-' for d in data]
        print('%-7s %-8s %-4s  %s' % (k[0], k[1], k[2], '  |  '.join(cells)))
    # shown over all scene parts, per tag and machine
    for t, d in zip(tags, data):
        per = {}
        for (scene, machine, part), g in d.items():
            a = per.setdefault(machine, [0, 0])
            a[0] += g[2]
            a[1] += g[1]
        print(t + ': ' + ', '.join('%s %d/%d shown (%d%%)' % (m, a[0], a[1], 100 * a[0] // max(a[1], 1))
                                    for m, a in sorted(per.items())))


if __name__ == '__main__':
    main()
