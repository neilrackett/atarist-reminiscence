#!/usr/bin/env python3
"""
make-unpacked - the data's bytekiller streams, unpacked ahead of time.
Copyright (C) 2026 Neil Rackett

Most of the Amiga data is packed with bytekiller, and unpacking it is
most of what an 8MHz ST waits for: 3.6s for Conrad's sprites, 1.2s
before the intro, 2.3s at the start of level 1. This writes each packed
file's contents out unpacked into UNPACKED/ beside DATA/, where the ST
build reads them straight into place and falls back to DATA/ for
anything missing. RExtract (web-rextract) writes the same folder; the
two must agree byte for byte.

Only what is unpacked when a scene, the title or a level is loaded:
cutscenes (*.CMP), the title picture (PRESENT.CMP), sprites (*.SPM),
level graphics (*.SGD), objects (*.OBC) and collision data (*.CT).
Room data (*.LEV) and sprite banks (*.MBK) stay packed: the game keeps
those whole in memory, which a 2.5MB machine cannot spare unpacked.

Each output file has its DATA/ name and holds:

  "RXU1"           magic and version
  BE32 count       streams: 2 for a cutscene (polygons, commands), else 1
  BE32 size        one per stream
  the streams      as the unpacker writes them

Usage:
  make-unpacked.py [DATA_DIR [OUT_DIR]]   (default dist/DATA, dist/UNPACKED)
"""
import os
import struct
import sys


def be32(b, o):
    return struct.unpack('>I', b[o:o + 4])[0]


def unpack(src):
    """One bytekiller stream, read from its end; ValueError if it does
    not check out. A transcription of src/unpack.cpp's C version."""
    pos = len(src) - 4
    size = be32(src, pos); pos -= 4
    crc = be32(src, pos); pos -= 4
    bits = be32(src, pos); pos -= 4
    crc ^= bits
    out = bytearray(size)
    o = size

    def getbit():
        nonlocal bits, crc, pos
        bit = bits & 1
        bits >>= 1
        if bits == 0:                   # the marker went: a new long
            if pos < 0:
                raise ValueError('ran off the start of the stream')
            bits = be32(src, pos); pos -= 4
            crc ^= bits
            bit = bits & 1
            bits = (bits >> 1) | 0x80000000
        return bit

    def get(n):
        v = 0
        for _ in range(n):
            v = (v << 1) | getbit()
        return v

    left = size
    while left > 0:
        code = get(2)
        if code == 0:
            length, offset = get(3) + 1, 0
        elif code == 1:
            length, offset = 2, get(8)
        else:
            code = ((code & 1) << 1) | get(1)
            if code == 3:
                length, offset = get(8) + 9, 0
            elif code == 2:
                length = get(8) + 1
                offset = (get(8) << 4) | get(4)
            else:
                length = code + 3
                offset = (get(8) << (code + 1)) | get(code + 1)
        length = min(length, left)
        left -= length
        if offset:
            if o + offset > size:
                raise ValueError('a reference past the end of the output')
            for _ in range(length):
                o -= 1
                out[o] = out[o + offset]
        else:
            for _ in range(length):
                o -= 1
                out[o] = get(8)
    if crc != 0:
        raise ValueError('bad CRC')
    return bytes(out)


def streams(name, data):
    """The unpacked streams of one DATA/ file, or None to leave it."""
    ext = name.rsplit('.', 1)[-1]
    if name == 'PRESENT.CMP':           # the title: a length, then one stream
        return [unpack(data[4:4 + be32(data, 0)])]
    if ext == 'CMP':                    # a cutscene: polygons, then commands
        out, off = [], 0
        for _ in range(2):
            packed = struct.unpack('>i', data[off:off + 4])[0]
            off += 4
            if packed < 0:              # stored as it is
                out.append(data[off:off - packed])
                off -= packed
            else:
                out.append(unpack(data[off:off + packed]))
                off += packed
        return out
    if ext == 'OBC':                    # a length, then one stream
        return [unpack(data[4:4 + be32(data, 0)])]
    if ext in ('SPM', 'SGD', 'CT'):     # the whole file is the stream
        return [unpack(data)]
    return None


def container(parts):
    head = b'RXU1' + struct.pack('>I', len(parts))
    head += b''.join(struct.pack('>I', len(p)) for p in parts)
    return head + b''.join(parts)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    data_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'dist', 'DATA')
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(data_dir)), 'UNPACKED')
    os.makedirs(out_dir, exist_ok=True)
    written = failed = 0
    for name in sorted(os.listdir(data_dir)):
        with open(os.path.join(data_dir, name), 'rb') as f:
            data = f.read()
        try:
            parts = streams(name.upper(), data)
        except (ValueError, struct.error, IndexError) as e:
            print('  %s left packed: %s' % (name, e), file=sys.stderr)
            failed += 1
            continue
        if parts is None:
            continue
        with open(os.path.join(out_dir, name.upper()), 'wb') as f:
            f.write(container(parts))
        written += 1
    print('%d files in %s%s' % (written, out_dir, ', %d left packed' % failed if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
