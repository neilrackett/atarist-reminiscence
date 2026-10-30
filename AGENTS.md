# Working on Flashback for the Atari ST

This repository is REminiscence (Gregory Montoir's Flashback engine,
0.5.6) ported to the Atari ST, STE and Mega STE through STDL
(`stdl/`, a submodule with its own `AGENTS.md`). This file is for
anyone changing the port: how to build it, the rules the machine sets,
how a change is shown to be right, and what is known about where the
time goes. For playing it, see `README.md`; for the tools, `tools/README.md`.

## Finding your way

Most of `src/` is upstream's engine, changed in place under
`#ifdef ATARIST` where the ST needs something else. The port's own files:

| File | What it is |
|---|---|
| `main_atari.cpp` | entry point: reads `RS.CFG` into `g_options`, finds the data |
| `systemstub_stdl.cpp` | the platform layer on STDL: screen modes and borders, the colour remap and its quantiser (256 logical colours to 16), input, the copy of the layer to the screen (`copyRectPlanar`), the start-up log lines |
| `video_st.cpp`, `video_st.h` | the planar layer the engine draws into (4 planes plus a priority plane, 256x224), its drawing primitives, room tiles and SGD backgrounds, perf counters |
| `spritecache_st.cpp` | gameplay sprites baked to planar once and kept in a RAM-sized ring |
| `displaylist_st.cpp`, `.h` | a frame's draws written down, compared with the last frame's, only what changed redrawn |
| `palette_st.cpp` | custom palettes (`PALETTE\`, `palette_custom=true`) |
| `ym_sfx_st.cpp` | sound effects on the YM for an ST with no DMA sound |
| `st_stubs.cpp` | empty stand-ins for the engine's other platforms, so their code is not linked |

Also changed for the ST, among others: `game.cpp` (the title menu with
Start and Options, the 30Hz deadline pacing in `updateTiming`, record and
replay, the save format), `cutscene.cpp` (the frame skipper, shapes as
sprites and their bakes, the per-frame stats and the `cutscene=` bench;
its polygons and ellipses come from the cache in `video_st.cpp`, kept
as each row's span and drawn by asm span cores, `segWalk` walking a
shape's primitives), `graphics.cpp` (shadow fills and the ellipse
spans the cache keeps), `resource.cpp` (`UNPACKED\`, the bank arena, resident monster
sprites), `video.cpp` (the room decode, the room cache, the block restore
and the frame begin and end), `mixer.cpp` and `mod_player.cpp` (ProTracker
modules on an STE's DMA sound through STDL's voices, YM streams from
`MUSIC\*.STM` on a plain ST). `docs/configuration.md` lists the `RS.CFG`
options a player sees; the hidden ones (`record=`, `replay=`, `ab=`...)
are in `src/intern.h`.

## Setting up

- **Game data** comes from the player's own Amiga disks and never goes in
  git. `tools/extract-data.sh disks/*.ipf` (see `tools/README.md`) or
  RExtract (`labs.neilrackett.com/web-rextract`) fill `dist/DATA\`;
  `tools/make-music.sh` makes `dist/MUSIC\` and
  `tools/make-unpacked.py` `dist/UNPACKED\`. Neil's ADFs are in
  `tools/adf/`.
- **Hatari** with cycle-exact emulation, and TOS images in `tmp/tos/`
  (not in git): the bench boots `TOS 2.06 - UK - Original.IMG`.
- **The bench scenarios** (`tmp/bench/scenarios/`) are recorded from
  Neil's data, so a fresh clone has none: record some
  (`tools/README.md`, "Recording"), then make the reference run before
  using `verify.sh`.

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

**STDL** is the `stdl/` submodule, a library with its own repository,
releases and `AGENTS.md`; the Makefile builds `stdl/libstdl.a` from it.
Never change it from here: a library change is made there and released,
and the port moves to it in a commit of its own ("STDL v1.15.3: ...",
currently v1.15.3), measured like any other change. A release is an
annotated `v*` tag on STDL's `main`: its workflow rebuilds the rolling
`latest` release's files, and the port pins the submodule to the tag
and builds the library itself. `git -C stdl fetch --tags` exits
non-zero once `latest` has moved ("would clobber"); add `--force`. To
measure a library change before its release, copy the changed files
into `stdl/` for one build and put them back with `git -C stdl
checkout --`. STDL's code, docs, commits and tags never name this game:
"one port".

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
- **The game clears its own BSS.** It starts in `__st_entry`
  (`main_atari.cpp`, named by `-Wl,-e` in the Makefile), which zeroes
  the BSS and then runs mintlib's crt0. ACSI2STM's GemDrive loader stops
  clearing after 130,560 bytes (one 255-sector DMA run) of the 158K, so
  a second run from a GemDrive drive started on the first run's
  leftovers and bombed. `tools/gemdrive/run.sh` re-creates that in Hatari.
- **Low memory is supervisor-only.** `phystop` and friends are read once
  at start-up (`ATARIST_ramK()`); the `Resource` constructor runs before
  supervisor mode, so it asks GEMDOS (`ST_freeK()`).
- **RAM.** 2.5MB is the minimum; 4MB machines get more: a 384K bank arena
  (where 2MB is free at start-up), room data from `UNPACKED\` and a bigger
  room tile cache (`ATARIST_ramK() >= kSTResidentRamK`, 3.5MB). Big caches
  come from GEMDOS and go back at a level load (`ST_releaseRamCaches`), so
  nothing long-lived sits between one level's data and the next. Real
  hardware has less free than Hatari by whatever is resident: about 70K
  for a hard disk driver alone, but 550K on a 4MB Mega STE with a network
  stack, a pad driver and accessories loaded (October 2026), which cost
  the room cache its slots and a room revisit 245ms instead of 60.
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

**Cutscenes have their own bench, `tools/cutbench/`.** The hidden
`cutscene=N` option (hex as `0x34`) plays one scene and quits, and with
`logging=true` the log carries a `CUT` line per eight frames and a
`CUTSUM` line per scene part (`Cutscene::stStatsEnd`), giving:
- frames shown, and the longest run not shown;
- work p50/p95/max against the budget;
- the push and the restore;
- shapes drawn as sprites, and why the rest were refused.

The tools:
- `tools/cutbench/run.sh TAG MACHINE SCENE...` plays scenes side by side,
  with the player's defaults (music on, frame skipping on) unless `CFG`
  says otherwise. Use `CFG="frame_skip=false;music=false"` for the work a
  scene takes.
- `tools/cutbench/summarise.py TAG [TAG...]` tabulates the runs, and
  `worksum.py MACHINE TAG...` totals their work per scene: quote the
  total work for a code change. Frames shown is what a player sees, but
  it moves with timing alone, and idle bakes follow spare time.
- `tools/cutbench/verify.sh TAG [REF]` hashes every drawn frame
  (`CUTHASH.BIN`), and the colours it was shown in (`CUTPAL.BIN`), over
  the intro, every level opening, a mid-level scene and the credits. It
  compares them with run `REF`, by default `cref`, whose pixels are
  r30's own: a cutscene change must match frame for frame.
- `tools/cutbench/prof.sh OUT MACHINE SCENE` profiles one scene, and
  `prof.py` reads the profile.

The `kAbCut*` bits switch each cutscene cache and change off inside one
binary. A mixer change is measured on `ste` with
`CFG="frame_skip=false;music=true"`: the gameplay replays hardly play
music.

**Test a check before trusting it.** The cutscene frame hash used to
rotate five bits a long and XOR: a row is 32 longs, so every row folded
in alike, and a shape drawn a row too high over a plain background hashed
the same. It is h * 33 + each long now, and `cref` was made again from
r30 itself. Break the code on purpose (draw a row low, flip a bit of a
copy) and see the check fail before relying on it.

**Profiling.** Load symbols made from `build/flashbak.elf`
(`m68k-atari-mint-nm -n -C`, `::` turned into `__`, only `T`/`t`/`W`
lines kept) at TEXT `0x149f2` (TOS 2.06, as run.sh boots it), turn
`profile on` from the fifo, and print `profile cycles N` at a breakpoint
(`b pc = Game__ST_replayDone :once :file cmds.ini`). Bracket a short event
with fast-forward off (`FF=off`), or the emulator is past it before the
fifo is read. In the profile's listing, `== $address` is worked out from
the registers when it was printed, not when the instruction ran. Code
reached by a `jmp` counts towards the function that called, so the span
cores' rows show as `sp_go`; asm labels are symbols of their own, so a
variant table's time spreads over many small entries. A C function's
exclusive time is not what rewriting it saves: the C around a new
polygon's record came to 4.3K cycles by the profile, and doing that work
in one pass instead gained 0.1%.

**UNPACKED\\** is written by `tools/make-unpacked.py` and by RExtract
(`web-rextract`, `src/core/unpacked.ts`); the two must agree byte for
byte. RExtract's tests compare them over the real disks with
`REXTRACT_FIXTURES` (see its README). Do not run RExtract's
`pnpm run build`, or touch a dev server on port 5173: those are Neil's.

**Traps that have each cost time:**

- In zsh an unquoted `$LIST` stays one word: run scenario lists through
  `bash -c`, or the scripts get one long scenario name.
- Hatari's `--memsize 2.5` means 2MB (whole MiB, or KiB): 2.5MB is
  `2560`. 2MB is not supported, so a failure there is not a bug.
- Never boot TOS 1.02 through run.sh.
- Two builds in the same minute show the same time on the `.TOS`: compare
  the files, not the listing. A `make` straight after an edit in the same
  second can skip the rebuild (STDL's host tests did): delete the
  target.
- `git checkout -- FILE` to undo an experiment also drops every other
  uncommitted change in that file: revert experiments by hand, or commit
  first.
- An ST rounds each instruction's cycles up to a multiple of four on the
  bus (a 14-cycle `move.b` costs 16): count asm that way.
- The data is too big for a floppy: the game always runs from a hard
  disk, so floppy timing never matters.

## Shipping

- Neil reviews before anything is committed or pushed; commits carry no
  `Co-Authored-By` trailer, and their messages say what changed and why,
  with the measurement.
- **Anything that lands on `main` is a release**: `release.yml` builds
  it, tags the next `v0.5.6-atarist.N`, moves the `latest` tag and
  replaces the files on the `latest` release (pushes touching only
  Markdown or `CHANGES.txt` are skipped). So work happens on a branch,
  and reaches `main` only after Neil's hardware test. `pr.yml` only
  builds.
- `CHANGES.txt` lists the changes to the shared engine that every build
  gets; Atari-only work is not listed there.
- Some things are left out of the player's docs on purpose: the hidden
  `RS.CFG` options, and the overscan modes' RAM gate (`kOverscanMinK`,
  2.5MB).
- `gh` against these repositories needs `-R owner/repo`.

## Settled, parked and closed

Investigations that ended, so nobody starts them again without a reason:

- **The STE's audio click: closed.** Two STDL bugs fixed (v1.8.2); the
  rare click left is the hardware's own baseline. Do not hunt it again.
- **Overscan on hardware: parked.** The default screen is Fit (no border
  open); the border modes stay as options.
- **Double buffering: parked**, to be looked at with an STDL bump; measure
  the copy pass before starting.
- **Pacing**: the 30Hz deadline pacing shipped (about 30 game steps a
  second on every machine); locking a Mega STE to 60Hz is deferred.
- **A mono (SM124) build: parked**; neither prototype made a seamless
  mono picture at colour speed on every machine.
- **A TT and Falcon build: not started**; the plan, and what was
  measured on an emulated TT, are in their own section at the end.
- **2MB machines: dropped** (2.5MB minimum since September 2026). Real
  hardware has less free than Hatari, by 70K or a lot more depending on
  what is resident (see "RAM" above).
- **Pre-rendered cutscenes: not wanted** (Neil, October 2026). Cutscenes
  get faster through the code, with 4MB-only caches and precomputed
  `UNPACKED\` data as boosters where code is not enough.
- **A retained draw list for cutscenes** (redraw only what changed): not
  built. 36-61% of a talky scene's shapes repeat, but in frames already
  on time; the frames that drop are rotating or zooming shapes and
  polygons seen for the first time.
- **An FPU (SFP-004) for cutscenes: no use.** The polygon path is
  integer, and the coprocessor's register protocol costs more than the
  `muls.w` it would replace.

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
   10K (`kDlMax` 160), the glyph cache 16K (pixels and row masks), the palette merge memo 9K,
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

**Cutscenes**, after the 8MHz round (October 2026, branch
`cutscene-speed`; Hatari, the player's settings): frames shown over the
bench set ST 95%, STE 93%, Mega STE 96% (83/81/86% before it). The intro
shows 96% on an ST, 87% on an STE (97% with music off: the mixer is the
STE's whole gap), 97% on a Mega STE at 16MHz and 87% at 8MHz. Level 4's
second part (0x39) is the worst: ST 59%, STE 54%, Mega STE 71%. In the
intro on a plain ST the work is about a third the rows of cached polygons
(`sp_go`), 8% their per-polygon set-up (`sh_core`), 14% the restore and
the push (25% in level 1's opening; both are `movem` copies near the
bus's limit), and each polygon seen for the first time about 10.7K
cycles before its rows. On an STE with music the voice mixer is about
25% of the machine and the module tick 4%.

Each of these was measured and left for later:

1. **Rotating and zooming shapes** (`op_drawShapeScaleRotate`) are what
   drops frames in 0x39, 0x41 and 0x00: about 12K cycles a primitive
   before its rows - the transform about 5K, already 16-bit `muls.w`,
   `ST_drawPolygonFast`'s C set-up about 3K, the walk about 3.4K. The
   set-up and walk in asm might take 10-15% off those frames.
2. **A table-driven row header** in the span cores: about 25-30 of a
   row's ~364 cycles, about 2% of the intro.
3. **The module tick**, upstream's sequencer, about 4% of an STE.
4. **Booster:** every polygon's spans precomputed in `UNPACKED\`, for
   the first frames of each part (4MB machines only;
   `tools/make-unpacked.py` and RExtract must agree byte for byte).
5. **STDL's mixer** (written down in `stdl/src/voice.c`): the wrap
   inside its asm loop (about 250 cycles a wrap rather than 800),
   `voice_mix` in asm (about 1K of a voice's 1.9K cycles a block, ~2.4%
   of an STE), eight frames a pass (-2.3% of the mixer).
6. **Smaller:** shape sprites blitted with `movem` for long runs (1-2%
   in still scenes); the colour lock at each part's start (~65ms); the
   palette prescan before a scene (~1.1s before the intro on an ST - a
   wait, not choppiness).

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
- Cutscenes: baking a shape at its third sighting, or only in a frame's
  spare time: 3.9% slower, as still scenes need the early bakes; in a
  pan a bake (~29ms) only breaks even.
- Cutscenes: shape sprites pre-shifted from another phase (no gain: few
  were made, and clipped ones were wasted); a restore and push box per
  shape (shapes overlap, so the boxes add up to 103-170% of the one box);
  `drawShapeScaleRotate`'s state kept in locals (-0.3%); a new polygon's
  parse and record in one pass (-0.1%).
- STDL's mixer: two voices a pass (~1%).

## A possible TT and Falcon build

Not started (October 2026); written down so the weighing is not done
twice. What it would be for is colour: TT Low (320x480, 256 of 4096)
and the Falcon's 8-bit modes hold the Amiga's palette exactly, 12 bits
a colour, so the quantiser and the palette merge have nothing to do.

**Today's binary already runs on a TT**, in ST Low with 16 colours. In
Hatari (`--machine tt`, TOS 3.06, 4MB ST-RAM) replay l1r26 matches the
reference frame for frame at 5.4ms a frame, against 34.9 on a plain
ST. The start-up line calls it an STE at "about 506MHz": the 68030's
cache runs the timing loop in a fraction of a 68000's cycles, and the
figure is only logged. On testing it:
- **Run Hatari's TT with `--mmu off`.** With the MMU and the data cache
  both emulated, the emulator falls to half real time once the game
  runs (52.8s of host time for a stretch that takes 26.5s with either
  off), which looks like the game in slow motion, picture and sound.
  Neither TOS nor the game needs the MMU, and its core's timing looks
  no truer: the speed loop reads 506MHz with it and 102MHz without.
- `tmp/tos/` has no TOS 3.06 or 4.04 image; EmuTOS 1.4 runs both
  machines.
- Hatari's 68030 timing is less exact than its 68000's, so a TT frame
  time from it is an estimate. Hardware testing would need a TT and a
  Falcon.

**The plan, if it is wanted:** one SDL 1.2 binary for both machines
(Patrice Mandin's Atari port, `libSDL.a` in the toolkit image, linked
with `-lgem`; it needs no MiNT), on upstream's chunky engine, with
STDL for what SDL does less well:
- **The engine builds as the desktop build does** (`ATARIST` off). The
  planar layer, the bakes, the display list and the span cores are for
  a 68000: they hard-code 4 planes and the 128-byte stride, and a 68030
  with TT-RAM and multiply in hardware needs none of them.
- **SDL owns the screen**: an 8-bit 320x240 mode with a hardware
  palette (on a TT it doubles the lines itself), `SDL_UpdateRects` with
  upstream's dirty blocks only, the palette set only when it changes,
  and no scalers, libmodplug or SDL_mixer. Compile for the 68030, after
  checking which CPU the toolkit's `libSDL.a` was built for. SDL
  converts each update to planes in ST-RAM: a full line-doubled frame
  is about 114K of writes, four times an ST's 28K, into RAM the TT's
  CPU reaches slowly. Time that first.
- **STDL owns the keyboard, all of it.** Its `ikbdsys` handler carries
  the keys, mouse, joysticks, Xpad pads and rumble, and SDL's default
  Atari input driver takes the keyboard interrupt itself, so the two
  cannot split the devices: SDL is switched to a driver that only polls
  TOS (`SDL_ATARI_EVENTSDRIVER`) and never pumped, and the input code in
  `systemstub_stdl.cpp` carries across. Both points about SDL are from
  memory, not checked against its source.
- **The sound is the port's own**, on STDL's voices: MODs on DMA, mixed
  in the VBL, so a level load does not interrupt it. It needs a define
  of its own for `mixer.cpp`, `mod_player.cpp` and `sfx_player.cpp`
  and the two options they read (`music`, `ste_sound`) - a small split,
  where splitting all of `ATARIST` (nearly 300 lines in the shared
  engine) is not worth doing. Not `STDL_Audio` with upstream's mixer as
  its callback: it refills only from the pump, so a load longer than
  about 150ms repeats its buffer. The TT's DMA sound is the STE's; the
  Falcon's should play STE-style sound, untested.
- **Record and replay** are under `ATARIST` too, and the bench needs
  them to time the build.
- **The two libraries in one program.** `libstdl.a` defines its own
  `SDL_*` and `Mix_*` functions (`compat.c`, `mixer.c`), so the port's
  Makefile builds STDL's objects for this target without them, STDL
  itself unchanged. STDL runs the program in supervisor mode and
  `Super()` is a toggle, so check SDL never calls it. Tear down in the
  reverse order of start-up.
- **To check on the way:** STDL's voices are paced by the VBL and it
  assumes 50Hz on a TT, so measure the rate TT Low really runs at. The
  TT has no STE enhanced joystick ports; the Falcon's use the STE's
  registers.

**The first step** is upstream's engine on a minimal SDL 1.2 stub,
timed in Hatari as a TT. If it holds 30Hz, STDL's input and sound go
on top; if it does not, the plane conversion is the first place to
look.

**Considered and set aside:**
- A TT Low driver in STDL, a chunky surface it converts to 8 planes:
  one library owning the machine, and cleaner on a TT alone, but it
  covers only the TT (the Falcon's VIDEL needs a mode set for each
  monitor type, which SDL already has), and it widens STDL beyond its
  stated target of the ST, STE and Mega STE.
- Widening the port's planar path to 8 planes: see the engine point
  above.
