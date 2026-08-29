#!/usr/bin/env python3
"""
mod2smf - ProTracker module to Standard MIDI File.

The Atari ST has no sampled music: the YM2149 gives three square
wave voices and one noise generator. STDL's stdlconv can render a
Standard MIDI File to a YM register stream, so the route from the
Amiga score to ST chip music is MOD -> SMF -> STM.

A MOD is already note data - four channels of period/instrument/
effect per row - so this reads the pattern data rather than the
audio: periods become MIDI notes, the order table is followed as
the player would follow it, and tempo comes from the speed and BPM
effects. Sample-level detail (the actual instrument waveforms, the
volume envelopes) has nowhere to go on a YM and is dropped.

Percussion is the one judgement call. stdlconv sends MIDI channel
10 to the noise generator, so a channel is marked as drums when its
samples are short, unpitched and played across a narrow note range:
that is what a MOD drum track looks like, and it is better than
having a kick drum fight the melody for one of only three voices.

Usage:
  mod2smf.py IN.mod OUT.mid [--drums auto|N|none] [--verbose]

  --drums auto   detect the percussion channel (default)
  --drums N      force MOD channel N (0-3) to MIDI channel 10
  --drums none   no percussion channel
"""

import argparse
import struct
import sys

# ProTracker period table, note 0 = C-1 .. 35 = B-3, finetune 0.
PERIODS = [
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
]
# MOD note 0 (period 856) is C-2 in the usual reckoning; MIDI 36 is
# C2, which puts the three octaves in a comfortable YM range.
MIDI_BASE = 36


def period_to_note(period):
    """Nearest table entry, so finetuned or slid periods still map."""
    if period <= 0:
        return None
    best, bestd = None, None
    for i, p in enumerate(PERIODS):
        d = abs(p - period)
        if bestd is None or d < bestd:
            best, bestd = i, d
    return MIDI_BASE + best


class Sample(object):
    __slots__ = ("name", "length", "finetune", "volume",
                 "repeat_point", "repeat_len")


def read_module(data):
    if len(data) < 1084:
        raise ValueError("too short to be a module")
    tag = data[1080:1084]
    if tag not in (b"M.K.", b"M!K!", b"4CHN", b"FLT4", b"M&K!"):
        raise ValueError("not a 4-channel ProTracker module (tag %r)" % tag)

    title = data[0:20].split(b"\0")[0].decode("latin-1", "replace")
    samples = []
    off = 20
    for _ in range(31):
        s = Sample()
        s.name = data[off:off + 22].split(b"\0")[0].decode("latin-1", "replace")
        s.length = struct.unpack(">H", data[off + 22:off + 24])[0] * 2
        s.finetune = data[off + 24] & 15
        s.volume = data[off + 25]
        s.repeat_point = struct.unpack(">H", data[off + 26:off + 28])[0] * 2
        s.repeat_len = struct.unpack(">H", data[off + 28:off + 30])[0] * 2
        samples.append(s)
        off += 30

    song_len = data[off]
    order = list(data[off + 2:off + 2 + 128])[:song_len]
    npat = max(order) + 1 if order else 0
    pat_off = 1084
    patterns = []
    for p in range(npat):
        rows = []
        base = pat_off + p * 1024
        if base + 1024 > len(data):
            break
        for r in range(64):
            row = []
            for c in range(4):
                i = base + r * 16 + c * 4
                b0, b1, b2, b3 = data[i], data[i + 1], data[i + 2], data[i + 3]
                period = ((b0 & 0x0F) << 8) | b1
                sample = (b0 & 0xF0) | (b2 >> 4)
                effect = b2 & 0x0F
                param = b3
                row.append((period, sample, effect, param))
            rows.append(row)
        patterns.append(rows)
    return title, samples, order, patterns


def pick_drum_channel(patterns, order, samples):
    """
    A percussion channel plays short unpitched samples over a narrow
    range of notes. Score each channel on those two properties and
    take the clear winner, if there is one.
    """
    stats = [{"notes": 0, "pitches": set(), "shortsam": 0, "sams": set()}
             for _ in range(4)]
    for pi in order:
        if pi >= len(patterns):
            continue
        for row in patterns[pi]:
            for c in range(4):
                period, sample, _, _ = row[c]
                if period:
                    st = stats[c]
                    st["notes"] += 1
                    st["pitches"].add(period)
                    if sample:
                        st["sams"].add(sample)
                        s = samples[sample - 1]
                        # a drum hit is short and does not loop
                        if s.length and s.length < 4000 and s.repeat_len <= 4:
                            st["shortsam"] += 1
    best, bestscore = None, 0.0
    for c in range(4):
        st = stats[c]
        if st["notes"] < 16:
            continue
        short_ratio = st["shortsam"] / float(st["notes"])
        pitch_spread = len(st["pitches"])
        # heavily short samples, few distinct pitches
        score = short_ratio * (1.0 if pitch_spread <= 6 else
                               0.5 if pitch_spread <= 12 else 0.15)
        if score > bestscore:
            best, bestscore = c, score
    return best if bestscore >= 0.5 else None


def var_len(n):
    out = bytearray()
    out.append(n & 0x7F)
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def build_smf(title, samples, order, patterns, drum_ch, ppq=96, verbose=False):
    """
    One MIDI track, all four MOD channels on their own MIDI channel
    (drums on 10). Rows are walked in play order so pattern jumps
    and breaks land where the player would put them.
    """
    events = []          # (tick, priority, bytes)
    tick = 0
    speed = 6            # ticks (rows) per beat divisor, ProTracker default
    bpm = 125
    ticks_per_row = ppq // 4          # a row is a 16th at speed 6
    playing = {}         # midi channel -> note

    def note_off(ch, note, at):
        events.append((at, 0, bytes([0x80 | ch, note, 0])))

    def note_on(ch, note, vel, at):
        events.append((at, 1, bytes([0x90 | ch, note, vel])))

    # initial tempo
    events.append((0, 0, b"\xff\x51\x03" + struct.pack(">I", 60000000 // bpm)[1:]))
    if title:
        t = title.encode("latin-1", "replace")[:127]
        events.append((0, 0, b"\xff\x03" + bytes([len(t)]) + t))

    # A module ends either by running off the order list or by
    # jumping back into it (Bxx), which is how a MOD says "loop".
    # Stop at the first revisit: the game loops the track itself,
    # and following the jump would render the same music forever
    # (three cues came out 22 minutes long before this).
    order_pos = 0
    visited = set()
    while order_pos < len(order):
        if order_pos in visited:
            break
        visited.add(order_pos)
        pat = order[order_pos]
        if pat >= len(patterns):
            order_pos += 1
            continue
        rows = patterns[pat]
        r = 0
        jumped = False
        while r < 64:
            row = rows[r]
            for c in range(4):
                period, sample, effect, param = row[c]
                midi_ch = 9 if c == drum_ch else (c if c < 9 else c + 1)
                # effects that change timing
                if effect == 0x0F:
                    if param < 0x20:
                        speed = max(1, param)
                        ticks_per_row = max(1, (ppq // 4) * speed // 6)
                    else:
                        bpm = param
                        events.append((tick, 0, b"\xff\x51\x03" +
                                       struct.pack(">I", 60000000 // bpm)[1:]))
                if period:
                    note = period_to_note(period)
                    if note is not None:
                        old = playing.get(midi_ch)
                        if old is not None:
                            note_off(midi_ch, old, tick)
                        vel = 100
                        if sample and sample <= len(samples):
                            v = samples[sample - 1].volume
                            vel = max(16, min(127, int(v * 127 / 64)))
                        if midi_ch == 9:
                            # one drum voice: keep it on a fixed key so
                            # stdlconv's noise mapping stays predictable
                            note = 38
                        note_on(midi_ch, note, vel, tick)
                        playing[midi_ch] = note
            tick += ticks_per_row
            # pattern break / position jump
            brk = None
            for c in range(4):
                _, _, effect, param = row[c]
                if effect == 0x0D:
                    brk = ((param >> 4) * 10 + (param & 15))
                elif effect == 0x0B:
                    order_pos = param
                    jumped = True
            r += 1
            if brk is not None:
                r = 64
                if not jumped:
                    order_pos += 1
                    jumped = True
                    # continue at row brk of the next pattern
                    if order_pos < len(order) and order[order_pos] < len(patterns):
                        rows = patterns[order[order_pos]]
                        r = min(63, brk)
                        jumped = False
                        continue
        if not jumped:
            order_pos += 1

    for ch, note in playing.items():
        note_off(ch, note, tick)
    events.append((tick, 2, b"\xff\x2f\x00"))

    events.sort(key=lambda e: (e[0], e[1]))
    track = bytearray()
    last = 0
    for at, _, payload in events:
        track += var_len(at - last)
        track += payload
        last = at

    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppq)
    chunk = b"MTrk" + struct.pack(">I", len(track)) + bytes(track)
    if verbose:
        print("  %d events, %d ticks, drums=%s" %
              (len(events), tick, "ch%d" % drum_ch if drum_ch is not None else "none"))
    return header + chunk


def main():
    ap = argparse.ArgumentParser(description="ProTracker MOD to Standard MIDI File")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--drums", default="auto",
                    help="auto (default), a channel number 0-3, or none")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    title, samples, order, patterns = read_module(data)
    if args.drums == "auto":
        drum_ch = pick_drum_channel(patterns, order, samples)
    elif args.drums == "none":
        drum_ch = None
    else:
        drum_ch = int(args.drums)

    smf = build_smf(title, samples, order, patterns, drum_ch,
                    verbose=args.verbose)
    open(args.output, "wb").write(smf)
    if args.verbose:
        print("%s -> %s (%d patterns, %d orders, %d bytes)" %
              (args.input, args.output, len(patterns), len(order), len(smf)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
