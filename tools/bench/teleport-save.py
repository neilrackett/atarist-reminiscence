#!/usr/bin/env python3
"""teleport-save.py SRC.SAV LEVEL ROOM X Y OUT_DIR - a save that starts in another room

Copies a saved game (best one made at the level's start, so every enemy
is where the level puts it) with Conrad moved to ROOM at X,Y, and names
it RS<level>_<room>.SAV in OUT_DIR, so the title's Load line says where
it goes. Y should be one of Conrad's floors - 70, 142 or 214 - with X
clear of walls and enemies; check the result in Hatari before relying
on it. For reaching busy rooms to measure or test, not for playing on:
the level's progress (doors, lifts, keys) is still its start's.

The save layout is Game::saveState's: a 38-byte header (FBSV, version,
a 32-byte description), skill, score and two collision indices, then
30 bytes for each live object, Conrad first - type, x, y, animation
step, room...
"""
import struct
import sys

HEADER = 4 + 2 + 32
PGE_TABLE = HEADER + 1 + 4 + 4 + 4


def main():
    if len(sys.argv) != 7:
        print(__doc__)
        sys.exit(2)
    src, level, room, x, y, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
    d = bytearray(open(src, "rb").read())
    if d[:4] != b"FBSV":
        sys.exit(f"{src}: not a saved game")
    desc = f"level={level} room={room}".encode()
    d[6:HEADER] = desc + bytes(32 - len(desc))
    struct.pack_into(">hh", d, PGE_TABLE + 2, x, y)
    d[PGE_TABLE + 7] = room
    name = f"{out}/RS{level}_{room:02d}.SAV"
    open(name, "wb").write(d)
    print(name)


if __name__ == "__main__":
    main()
