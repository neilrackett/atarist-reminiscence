#!/usr/bin/env python3
# Measure the Amiga sound effects, to write their YM versions by.
# Copyright (C) 2026 Neil Rackett
#
# A plain ST has no DMA sound, so src/ym_sfx_st.cpp plays a YM2149
# approximation of each effect worth hearing. Those recipes are
# written by hand; this prints what to write them from, for each
# effect in dist/DATA's sample banks:
#
#   - length, and a volume curve in 40ms steps on the YM's scale
#     (15 = the loudest frame of any effect, one step per 3dB)
#   - how noise-like each step is (spectral flatness: near 1 is noise,
#     near 0 a tone) and its centroid, and for tonal steps the pitch
#     as a YM tone period
#
# It reads the player's own data and writes nothing: the numbers are
# for choosing a recipe's shape, not for copying into the source.
#
# Usage: tools/ym-sfx-report.py [-s] [id ...]   (default: every effect)
#        -s  one line each: length, volume at the quarters, mean
#            flatness and centroid, pitch at the start and the end
import os, struct, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, '..', 'dist', 'DATA')
RATE = 3546897 // 650            # Resource::load_SPL's playback rate
YM_CLOCK = 2000000               # tone Hz = clock / (16 * period)
STEP = RATE * 40 // 1000

NAMES = """pneuma05 bip00105 bip00205 laser205 tir2 explo mort0105 mort0310
bouclier asc_debut asc_milieu asc_fin verre_casse chalu110 saut trappe
impact_shield stby0105 teletower desint recharge mitrail touche coup
chenille robot tombe porte_ferme canon_down elec mater210 mater07
mur_bouge taxi souris et et_touche et_transform alien_move jungle2
jungle1 piaf2 goute_eau piaf1 pas1 pas2 croa alien1 alien2 alien3
ventilo poussiere bip electri machine alarme2 cerveau reflet
roule_boule hehe3 recept bestiole lampe mine effort frappe""".split()


def banks():
    """{id: samples} from the first bank that carries each effect"""
    out = {}
    for name in ('LEVEL1.SPL', 'LEVEL3.SPL', 'LEVEL4.SPL'):
        path = os.path.join(DATA, name)
        if not os.path.exists(path):
            continue
        d = open(path, 'rb').read()
        o = 0
        for i in range(66):
            size = struct.unpack('>H', d[o:o + 2])[0]
            o += 2
            if size & 0x8000:
                continue
            if i not in out:
                out[i] = np.frombuffer(d[o:o + size], dtype=np.int8).astype(float)
            o += size
    return out


def report(i, x, top):
    print('%2d %-14s %4dms' % (i, NAMES[i], len(x) * 1000 // RATE))
    for s in range(0, len(x), STEP):
        seg = x[s:s + STEP]
        if len(seg) < 16:
            break
        rms = np.sqrt((seg ** 2).mean())
        vol = 15 + int(round(20 * np.log10(max(rms, 1e-9) / top) / 3))
        spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2 + 1e-12
        f = np.fft.rfftfreq(len(seg), 1 / RATE)
        flat = np.exp(np.log(spec).mean()) / spec.mean()
        cen = (spec * f).sum() / spec.sum()
        pk = f[1 + np.argmax(spec[1:])]
        period = int(YM_CLOCK / (16 * pk)) if pk > 0 else 0
        print('   %4dms vol %3d  flat %.2f  centroid %5dHz  peak %5dHz (period %4d)'
              % (s * 1000 // RATE, max(vol, 0), flat, cen, pk, period))


def summary(i, x, top):
    rows = []
    for s in range(0, len(x) - 16, STEP):
        seg = x[s:s + STEP]
        rms = np.sqrt((seg ** 2).mean())
        spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2 + 1e-12
        f = np.fft.rfftfreq(len(seg), 1 / RATE)
        rows.append((15 + 20 * np.log10(max(rms, 1e-9) / top) / 3,
                     np.exp(np.log(spec).mean()) / spec.mean(),
                     (spec * f).sum() / spec.sum(),
                     f[1 + np.argmax(spec[1:])]))
    n = len(rows)
    vols = [max(0, int(round(rows[min(n - 1, k * n // 4)][0]))) for k in range(4)]
    vols.append(max(0, int(round(rows[-1][0]))))
    period = lambda hz: int(YM_CLOCK / (16 * hz)) if hz > 0 else 0
    print('%2d %-14s %4dms vol %-16s flat %.2f centroid %5dHz  period %4d -> %4d'
          % (i, NAMES[i], len(x) * 1000 // RATE, '/'.join(map(str, vols)),
             np.mean([r[1] for r in rows]), np.mean([r[2] for r in rows]),
             period(rows[0][3]), period(rows[-1][3])))


def main():
    brief = '-s' in sys.argv[1:]
    if brief:
        sys.argv.remove('-s')
    fx = banks()
    if not fx:
        sys.exit('no sample banks in %s' % DATA)
    top = max(np.sqrt((x[s:s + STEP] ** 2).mean())
              for x in fx.values() for s in range(0, len(x) - STEP, STEP))
    ids = [int(a) for a in sys.argv[1:]] or sorted(fx)
    for i in ids:
        if i in fx:
            (summary if brief else report)(i, fx[i], top)
        else:
            print('%2d %-14s not in any bank' % (i, NAMES[i]))


if __name__ == '__main__':
    main()
