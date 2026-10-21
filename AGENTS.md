# Working on Flashback for the Atari ST

This repository is REminiscence (Gregory Montoir's Flashback engine,
0.5.6) ported to the Atari ST, STE and Mega STE through STDL
(`stdl/`, a submodule with its own `AGENTS.md`). This file is for
anyone changing the port: how to build it, the rules the machine sets,
how a change is shown to be right, and what is known about where the
time goes. For playing it, see `README.md`; for the tools, `tools/README.md`.

## Build

```
STCMD_NO_TTY=1 stcmd make        # non-interactive shells (agents, CI)
stcmd make                        # interactive terminal
```

m68k-atari-mint-gcc 4.6.4 in a container
([atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker)),
C++98. It produces `dist/FLASHBAK.TOS` and `build/flashbak.elf` (with
symbols). Check the `.TOS`'s time after a build: a filtered log has hidden
a link error before, and the old binary was tested. The container only
sees this directory, so copy anything you want `m68k-atari-mint-objdump`
to read (an ELF from elsewhere) into `tmp/` first.

The desktop SDL build shares most of the engine and CI does not build it,
so build it by hand after touching shared files: the sources in
`Makefile.sdl` with its flags minus `-DUSE_STB_VORBIS` (on a Mac, add
`-I/opt/homebrew/include` and `-L/opt/homebrew/lib` for libmodplug).

## Rules the machine sets

- **Atari-only code sits under `#ifdef ATARIST`**, and every other path
  keeps upstream's behaviour exactly; a fix to the shared engine that
  every build gets goes in `CHANGES.txt`. Options the ST adds default to
  0 on the desktop, so a check like `replay < 0` needs its own `#ifdef`.
- **The ST reads the Amiga data only** (`DATA\`, plus the optional
  `MUSIC\`, `UNPACKED\` and `PALETTE\` beside it); `detectVersion` in
  `main_atari.cpp` accepts nothing else.
- **gcc 4.6 on a 68000**:
  - a 32-bit multiply, divide or `%` is a library call (`__mulsi3`): keep
    them out of anything run per sprite, per tile or per object. Two
    `uint8_t` fields multiplied as ints are one; `STDL_MulU16` from
    `stdl/stdl_math.h` is the 16-bit instruction;
  - a pointer difference over a struct whose size is not a power of two
    divides; the hot tables are parallel arrays for that reason;
  - shifts by more than 8 take a count in a register, twice the cycles
    (16 is a free `swap`);
  - `(d8,An,Xn)` has an 8-bit displacement, a.out alignment is 2 at
    most, and gcc does not unswitch loops or always keep a value whose
    address is taken in a register - measure what you restructure;
  - `-fno-strict-aliasing` is on.
  Check `m68k-atari-mint-objdump -dr build/<file>.o` for `jsr` to
  `__mulsi3`/`__divsi3` in hot code.
- **Assembly kernels have C twins** under `#ifdef __m68k__`
  (`video_st.cpp`, `spritecache_st.cpp`); a source-only change to one
  (a macro, a reordering) is checked by comparing the object's
  disassembly before and after.
- **Low memory is supervisor-only.** `phystop` and friends are read once
  at start-up (`ATARIST_ramK()`); the `Resource` constructor runs before
  supervisor mode, so it asks GEMDOS (`ST_freeK()`).
- **RAM.** 2.5MB is the minimum; 4MB machines get more: a 384K bank arena
  (where 2MB is free at start-up), room data from `UNPACKED\` and a bigger
  room tile cache (`ATARIST_ramK() >= kSTResidentRamK`, 3.5MB). Big caches
  come from GEMDOS and go back at a level load (`ST_releaseRamCaches`), so
  nothing long-lived sits between one level's data and the next. Real
  hardware has about 70K less free than Hatari.
- **Comments say why**, with the number that decided it.

## Testing

Everything runs in Hatari, always cycle-exact
(`EXTRA="--cpu-exact on --compatible on"`), through
`stdl/tests/hatari/run.sh` (its header documents the commands). A 2.5MB
machine is `--memsize 2560`: `2.5` there means 2MB. Neil tests on
hardware (a 4MB Mega STE) before anything is released.

**A change must draw exactly what the reference draws, and the game must
end every replay where its recording did.** The bench replays
(`tools/bench/`, documented in `tools/README.md`) are ten recorded
sessions in `tmp/bench/scenarios/` - fights, the inventory, every level's
start - made from Neil's game data, so not in git. One command checks a
build:

```
tools/bench/verify.sh TAG [PREV]
```

It hashes every frame against the reference run `ref` (no bakes and no
display list, `ab=24`), checks each replay's ending, times the build
against run `PREV`, and lists the time each room took to enter. Quote the
total over all scenarios: code layout alone moves one scenario 1-3%. A
Mega STE's times move with layout through its cache, so compare features
inside one binary with `ab=` (the `kAb*` bits in `src/intern.h` switch
each optimisation off). Run a change that touches memory sizing on 2.5MB
as well (`MEM=2560`), and one that touches the restore or the display
list with `CFG="ab=16;replay_hash=true"` too.

`tmp/cutbench/run.sh` times the intro and level 1's opening cutscene; it
writes `dist/RS.CFG`, which goes to the hardware with the binary, so put
that file back afterwards.

**Profiling.** Load symbols made from `build/flashbak.elf`
(`m68k-atari-mint-nm -n -C`, `::` turned into `__`, only `T`/`t`/`W`
lines kept) at TEXT `0x149f2` (TOS 2.06, as run.sh boots it), turn
`profile on` from the fifo, and print `profile cycles N` at a breakpoint
(`b pc = Game__ST_replayDone :once :file cmds.ini`). Bracket a short event
with fast-forward off (`FF=off`), or the emulator is past it before the
fifo is read. In the profile's listing, `== $address` is worked out from
the registers when it was printed, not when the instruction ran.

**UNPACKED\\** is written by `tools/make-unpacked.py` and by RExtract
(`web-rextract`, `src/core/unpacked.ts`); the two must agree byte for
byte. RExtract's tests compare them over the real disks with
`REXTRACT_FIXTURES` (see its README). Do not run RExtract's
`pnpm run build`, or touch a dev server on port 5173: those are Neil's.

## Shipping

Neil reviews before anything is committed or pushed; commits carry no
`Co-Authored-By` trailer. Releases are tags he makes after a hardware
test. `gh` against these repositories needs `-R`.

## Where the time goes, and what is left

Measured over the bench replays in October 2026 (r29 plus the
busy-rooms round): they run at 23-55ms a frame on a plain ST and 15-38ms
on a Mega STE, over 40% faster than r29. Death Tower, by share of a
frame on a plain ST: game logic about 27%, the sprite path (bakes, the
display list, STDL's blits) about 32%, the copy to the screen about 5%,
restoring blocks about 5.5%.

Each of these was measured and left for later:

1. **Game logic, about 17% of a Death Tower frame in two functions**:
   `pge_process` 12% (with `pge_execute`, `pge_setupAnim` and
   `pge_setupOtherPieges` inlined into it, its only caller: -3.5% of a
   frame) and `col_preparePiegeState` 5% (with `col_getGridPos` inlined:
   -1%). What is left there is upstream's interpreter itself: each object
   rule's opcodes are called through member-function pointers, an
   `ObjectOpcodeArgs` on the stack each, and the opcode handlers cannot be
   inlined through that table. Turning the table into plain functions, or
   the commonest opcodes into a switch, is the next step and a bigger one.
   Single-caller helpers in hot loops are worth inlining first
   (`prepareAnimsHelper` and `drawAnimBuffer` have several callers, so
   inlining them copies them); the replays' end states catch any change in
   behaviour.
2. **A level's first room** takes 1.1-2.1s on a plain ST; a level's later
   rooms 0.4-0.95s, a room seen before about 65ms. In the first: the
   palette merge (`SystemStub_STDL::quantiseClusters`) about 600ms
   (it remembers its last four merges, so only new palettes pay), and on
   level 1 placing the SGD background (`ST_amigaPlaceCov` and its
   packing) about 300ms.
3. **Bake set-up**: the scans that trim a decoded frame (an OR of its
   planes per row and per column) before `bakeUnitsAsm`, 1-2% of a fight
   frame; most of what is left in the sprite path is bakes, about one
   draw in five.
4. **The pre-shifted copy's lookup**: a sprite off a 16-pixel boundary
   looks its shifted copy up again on every draw. `findRec` is inline and
   cheap since October 2026 (the sprite lookup as a whole went 0.9%
   faster), so this is small now; a pointer from the base bake, checked
   against a per-segment flush count, would skip it.
5. **BSS every machine pays** (2.5MB machines feel it): the display list
   10K (`kDlMax` 160), the glyph cache 14K, the palette merge memo 9K,
   the sprite pair tables 8K.
6. **Monster bakes keyed on the load, not the monster**
   (`_stMonsterEpoch`): on a 2.5MB machine, where monsters share one
   buffer, two kinds alternating lose each other's bakes at every load.
7. **Several keys for one fact.** Palette tokens (bakes), per-base pair
   table versions (room tiles, SGD cells) and the room cache's remap
   bytes all say what a colour bank maps to; a bank's address plus the
   arena epoch says which bank bytes these are, where a bank number and
   offset would survive the 28K arena's clears and remove the tile
   cache's copy table (`g_stTileOrig`) and its "stable" fallback.
8. **Level 2's half change** re-reads its packed room data and the whole
   unpacked copy (about 140K), and 4MB machines keep the packed room data
   and banks although only their tables are read once `UNPACKED\`
   covers them.
9. **A room change copies the layer three times** (about 10-15ms on a
   plain ST); skipping the last may not be safe.
10. **Mega STE**: locking the game to 60Hz, and double buffering, are
    parked (the copy pass would have to be measured first).

Measured and rejected, so not worth trying again as they were:

- `READ_BE_UINT16`/`32` as word loads everywhere: no gain in play or
  cutscenes, 4.7K more code (only `Resource::readUint16` does it: -1.6%).
- No pre-shifted copies: 1% slower. UNDER blits split into bands: they
  are only 2-5% of a frame. The BLiTTER on or off in play: no difference.
- The two live-object walks (`stepLogic`, `pge_prepare`) as one shared
  iterator: 1.3% slower.
- Turning away objects with no collision data inline, before
  `col_preparePiegeState`'s eleven-register save: 0.1%, not worth a
  second definition in game.h.
- Unpacked banks in a block of their own rather than in the bank arena:
  the room cache lost its slots, and level 5's revisits went from 65ms to
  800ms.
