#!/usr/bin/env python3
"""compare.py TAG_A TAG_B MACHINE SCENARIO... - two sets of replays side by side

Reads tmp/bench/runs/<tag>_<machine>_<scenario>/RS.LOG as written by
tools/bench/replay.sh. Prints each scenario's steady ms/frame (every
window but the first, which holds the save's load), the change, and
whether each replay ended where its recording did, then the total over
all scenarios: quote the total, never one scenario, since code layout
alone moves a single scenario by a few percent.

--frames compares the replay_hash window hashes instead (run both with
CFG=replay_hash=true): identical windows mean identical pictures.
"""
import re
import sys

RUNS = "tmp/bench/runs"


def done_line(tag, machine, scen):
    path = f"{RUNS}/{tag}_{machine}_{scen}/RS.LOG"
    try:
        for line in open(path, errors="replace"):
            if "REPLAY done" in line:
                return line
    except OSError:
        pass
    return None


def window_hashes(tag, machine, scen):
    path = f"{RUNS}/{tag}_{machine}_{scen}/RS.LOG"
    out = []
    try:
        for line in open(path, errors="replace"):
            m = re.search(r"REPLAY window \d+ .* hash ([0-9a-f]+)", line)
            if m:
                out.append(m.group(1))
    except OSError:
        pass
    return out


def main():
    args = sys.argv[1:]
    frames = "--frames" in args
    args = [a for a in args if a != "--frames"]
    if len(args) < 4:
        print(__doc__)
        sys.exit(2)
    a, b, machine, scens = args[0], args[1], args[2], args[3:]
    if frames:
        bad = 0
        for s in scens:
            ha, hb = window_hashes(a, machine, s), window_hashes(b, machine, s)
            same = ha == hb and ha
            bad += not same
            print(f"{s:8} {'identical' if same else 'DIFFER'} ({len(ha)}/{len(hb)} windows)")
        sys.exit(1 if bad else 0)
    ta = tb = 0
    for s in scens:
        la, lb = done_line(a, machine, s), done_line(b, machine, s)
        if not la or not lb:
            print(f"{s:8} missing")
            continue
        ma = int(re.search(r"frames (\d+) ms", la).group(1))
        mb = int(re.search(r"frames (\d+) ms", lb).group(1))
        sa = float(re.search(r"steady ([\d.]+)", la).group(1))
        sb = float(re.search(r"steady ([\d.]+)", lb).group(1))
        ta += ma
        tb += mb
        print(f"{s:8} {machine:8} {sa:7.2f} -> {sb:7.2f} ms/frame {100 * (sb - sa) / sa:+6.1f}%  "
              f"{la.split()[-1]}/{lb.split()[-1]}")
    if ta:
        print(f"TOTAL    {machine:8} {ta} -> {tb} ms {100 * (tb - ta) / ta:+.1f}%")


if __name__ == "__main__":
    main()
