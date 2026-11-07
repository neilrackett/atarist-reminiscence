# Working on Flashback for the Atari ST

This repository is REminiscence (Gregory Montoir's Flashback engine,
0.5.6) ported to the Atari ST, STE and Mega STE through STDL
(`stdl/`, a submodule with its own `AGENTS.md`). This file is for
anyone changing the port: how to build it, the rules the machine sets,
how a change is shown to be right, and what is known about where the
time goes. For playing it, see `README.md`; for the tools, `tools/README.md`.

## Finding your way

Most of `src/` is upstream's engine, changed in place under
`#ifdef ATARIST` where the ST needs something else, and under
`#ifdef ST_PLANAR` where that something is the ST build's planar picture
(see "The 030 build" at the end). The port's own files:

| File | What it is |
|---|---|
| `entry_atari.cpp` | where the program starts, ahead of mintlib: clears the BSS, and on a TT or a Falcon the ST build hands over to `FBACK030.TOS` |
| `main_atari.cpp` | `main`: reads `RS.CFG` into `g_options`, finds the data |
| `systemstub_atari.cpp`, `.h` | the platform layer both Atari builds share, on STDL: keys, joystick and pads, the clock, the machine set-up and its log lines |
| `systemstub_stdl.cpp` | the ST build's screen on top of it: screen modes and borders, the colour remap and its quantiser (256 logical colours to 16), the copy of the layer to the screen (`copyRectPlanar`) |
| `systemstub_030.cpp` | the 030 build's screen on top of it: STDL's SDL2 add-on on its 256-colour responsive screen, a texture over each of the engine's buffers and a push a copy of its box |
| `video_st.cpp`, `video_st.h` | the planar layer the engine draws into (4 planes plus a priority plane, 256x224), its drawing primitives, room tiles and SGD backgrounds, perf counters |
| `pagetrack.cpp`, `.h` | cutscene page tracking for both builds: the band of rows a scene draws in, and a box per page around what differs from the background, so copies and pushes move only that; the 030 build's page copies |
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
STCMD_NO_TTY=1 stcmd make 030    # the 030 build, dist/FBACK030.TOS
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
and the port moves to it in a commit of its own ("STDL v1.15.3: ..."),
measured like any other change. A release is an annotated `v*` tag on
STDL's `main`: its workflow rebuilds the rolling `latest` release's
files, and the port pins the submodule to the tag and builds the
library itself. For now it is pinned to a commit on STDL's `sdl2`
branch instead, untagged (November 2026): v1.20.0 and STDL's SDL2
add-on (`stdl/addons/sdl2`, `stdl/docs/sdl2.md`), which the 030 build
draws through, so that what the move finds can still be fixed there
before a release. A fix goes onto `sdl2` and the pointer moves to it.
`git -C stdl fetch --tags` exits
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
  every build gets goes in `CHANGES.txt`. What only the ST build's planar
  picture needs sits under `#ifdef ST_PLANAR` instead, which the ST
  build defines with `ATARIST`, so its objects do not change when a
  block moves between the two: compare their `objdump -d -r` before and
  after. Options the ST adds default to
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
  (`entry_atari.cpp`, named by `-Wl,-e` in the Makefile), which zeroes
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
display list, `ab=24`), checks each replay's ending, checks after every
frame that the screen shows the layer ("REPLAY screen" in the log: a
hash of the layer cannot see a box the push left out), times the build
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
  r30's own: a cutscene change must match frame for frame. After every
  push the run also compares the screen with the page it came from, and
  its `CUTSCREEN` lines must say they agreed. The `cutscene=` option
  sets the text's colours as the game has them, before every scene but
  the logos and the intro (November 2026): until then the credits and
  captions were black, and the ST locked each scene's colours without
  the text's, so the seven others' references were made again then,
  from a build that matched r30 on the old start frame for frame (the
  old ones are `cref30`).
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
copy) and see the check fail before relying on it. The screen checks
were tried that way: a push one row short, in cutscenes and in play,
left every frame hash the same and failed both (301 of the intro's 353
pushes, 600 of l1r26's frames).

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
- **A TT and Falcon build: a prototype** on branch `tt`,
  `FBACK030.TOS`; its own section at the end.
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

## The 030 build: a TT or a Falcon in 256 colours

A prototype on branch `tt` (October 2026). `make 030` builds
`dist/FBACK030.TOS` for a 68020 or better, which sits beside
`FLASHBAK.TOS` and reads the same Amiga data. What it is for is colour:
TT Low and the Falcon's 8-bit modes hold the Amiga's palette exactly, 12
bits a colour, so every room and scene shows in its own colours where
the ST quantises them to 16.

**How it is put together:**
- **Two macros.** `ATARIST` is the platform both Atari builds share:
  STDL, `RS.CFG`, the title's menus, the sound, record and replay, the
  pacing, `UNPACKED\`. `ST_PLANAR` (defined in `intern.h` for an
  `ATARIST` build without `ATARIST_030`) is the ST build's planar picture:
  `video_st.cpp`, the bakes, the display list, the cutscene caches and
  frame skipper, the quantiser. The 030 build leaves the planar files out
  and draws with upstream's chunky code, as the desktop build does.
- **Two stubs on one base.** `systemstub_atari.cpp` is the shared
  platform layer. It has no data members, so the ST stub kept its
  layout when it was split out of it.
- **No FPU needed.** The toolchain's `m68020-60` libraries assume an
  FPU, and their mintlib will not start without one ("requires a 68881",
  on a stock Falcon). So the objects build for the 68020 with
  `-msoft-float` and link against the 68000 libraries.
- **Anything but a TT or a Falcon gets a message.** `__st_entry` reads
  `_MCH` in 68000 instructions before anything else runs, and on any
  other machine prints where the ST version is, waits for a key and
  quits. Before that, an ST got as far as `main`'s first `printf` and
  bombed. `_MCH` rather than `_CPU`, because an ST with a 68030 card
  would run the code but has neither machine's modes. Checked on an ST
  (TOS 1.04, with no cookie jar, and 2.06) and a Mega STE.
- **The ST build hands a TT or a Falcon to this one**, so a player
  launches `FLASHBAK.TOS` on any machine. On a TT or a Falcon with
  `FBACK030.TOS` beside it, `__st_entry` gives back all of the program
  but its basepage and its own code, starts `FBACK030.TOS` and ends
  with its exit code; without the file it goes on as the ST build, as
  an STE. TOS cannot replace a running program, so what it keeps stays
  under the 030 build: 1,090 bytes, as `entry_atari.cpp` is first in
  the Makefile's list and sits straight after crt0, and Mshrink keeps
  the start of a program's memory. From `main` it would have kept
  about 800K of a 4MB Falcon (its code, BSS and 64K stack). Nothing is
  copied, so no cache can hold a stale copy. In Hatari the 030 build
  had 2,748K free started so on a Falcon against 2,750K started itself
  (2,657K against 2,658K on a TT), and a replay ran frame for frame
  the same either way. A broken `FBACK030.TOS` gets "would not start"
  and a key, as the ST build is gone by then; a player who wants the ST
  build on a Falcon removes the file.
- **Sound Quality is always High** (12517Hz, where the ST build mixes
  at 6258 by default). Options has no line for it, and
  `sound_quality=` in `RS.CFG` is overridden. With the modules
  playing, High adds 2.6-3.6% to a TT's cutscene work and 4.8% to a
  Falcon's. A Falcon is said to have no 6258Hz DMA sound at all (a
  Falcon FAQ: that rate is its pause), which is not checked.
- **The screen is STDL's SDL2 add-on's** (`stdl/addons/sdl2`,
  `stdl/docs/sdl2.md`), since November 2026, on STDL's responsive
  screen (`STDL_UseResponsive(COLOUR | HEIGHT)`, `stdl_responsive.h`):
  320x240 in 8 interleaved planes, the display's own layout. Only this
  build links it (`STDL_SDL2_LIB` in the Makefile, its headers on the
  030 objects' path alone); the stub is one of its programs, with
  STDL's keyboard, pads, voices and clock beside it as the ST build has
  them. STDL does what the stub had done itself through the XBIOS, and
  more:
  - the mode: TT Low, 480 lines, on a TT; on a Falcon 8 bits on VGA
    (320x240, the VIDEL doubling the lines) or on RGB and a TV (240
    lines in overscan, a 384-pixel line with the picture centred, PAL
    or NTSC as the desktop had it). A mono monitor, TT High on a TT,
    is refused with a message;
  - two or three pages in ST-RAM (`STDL_SDL2_HINT_PAGES`), the 256 colours, the VBL rate
    measured after the mode set, and the keyboard and pads with the
    mode (`STDL_OpenInput`, which STDL v1.18.0 added for this build, is
    no longer called);
  - the desktop back at every exit: `error()`, `exit()` and a crash,
    through GEMDOS's terminate vector, where the stub's `atexit` missed
    the last. Its colours are copied from the registers, as the stub
    did: `VgetRGB` read all black under TOS 4.04's desktop.

  The add-on is the conversion and the page keeping this stub had of
  its own on STDL's screen, moved into STDL and made general for any
  8-bit chunky program: the pages hold byte for byte what the stub's
  own conversion put there, and what the XBIOS stub's screen held.
  Before that, SDL 1.2 set the mode and the colours in the first version, and
  cost more than that was worth: 240K of code (699K against 442K), a
  75-150K chunky surface it never used, start-up traps (its `Super(0)`
  toggle, its input driver, its clock), updates wrong on two machines
  (a TT's doubled 320x240 put a box not at the top on line y rather
  than 2y; a Falcon on RGB drew 32 pixels left of centre), and a chunky
  mode on a Falcon with a SuperVidel, where planes would be garbage.
- **Each page owes what was pushed since it was last drawn on.** The
  engine pushes boxes as if to one screen that keeps them all, and the
  add-on's screen keeps them: a texture lies over each of the engine's
  buffers (`STDL_SDL2_CreateTextureFrom`, no copy), and `copyRect` is an
  `SDL_RenderCopy` of its box rounded out to 16-pixel groups, the whole
  groups the stub's own conversion converted. The add-on keeps a mask
  of groups a row for each page and ORs every pushed box into all of
  them. Of what the page drawn on owes, this frame's boxes are
  converted from the textures and the rest is copied from the page last
  put up, which holds the screen as it stands. So the stub is one
  screen to the engine whatever the engine does with a picture between
  pushes: a frame's boxes from one picture are converted before a box
  from another, before that picture can change. A frame that pushes
  nothing does not flip.
  - Rows needing the same blocks go up as one box (`Video::updateScreen`,
    this build's branch), as the ST build sends them: a copy through
    the add-on costs a Falcon about 1,060 cycles (STDL `6274603`;
    2,033 before it), mostly its code fetched from ST-RAM, where the
    stub's own push had been an OR a row. Play's copies went from 16 a
    frame to 9.5.
  - The stub's first version converted everything a page owed from this
    frame's picture, which is right only if the engine's buffer holds
    what the screen shows outside this frame's boxes. The title breaks
    that: it pushes its buffer black, decodes the picture into it and
    reveals it two rows a step, and the pages that owed the black push
    showed the whole picture, every other frame of the wipe (Neil, in
    Hatari). The replays and the cutscene bench never show the title,
    so `tmp/tt/title030.sh` runs it with the screen check on and quits
    from the menu: 37 of 65 flips unlike their pushes then, none now.
  - `kBlockDirty` = 2 is no help here: it pushes a block on the frame
    after it was drawn on too, for the erase after a sprite moves, and
    with two pages a sprite drawn once would come back every other
    frame (traced, not built).
  - On a TT the conversion writes both of a row's lines itself, half
    the stride apart, and reports nothing, so Flip has nothing to copy:
    STDL's present copies one box a page round everything reported, and
    gameplay's scattered boxes would have made that most of the screen.
- **The colours are staged** (`STDL_StagePalette`): the stub's
  palette is the renderer's (`STDL_SDL2_SetRenderPalette`), and what
  `SDL_SetPaletteColors` changes in it is set just before Flip, written
  in the blank that shows that page, so a new room's picture never goes
  up in the old colours or the old in the new. STDL's 8-plane screen
  wrote them at once until this build asked for that (STDL `1fc5843`),
  and its voices now write them before they mix. A fade is colours
  alone: each step is staged at once (`STDL_SDL2_StagePalette`) and
  lands in the next blank. The stub hands the renderer the engine's
  changes once, before the present: a cutscene sets 32 entries a
  frame, and a call of the renderer's for each cost a Falcon's level 1
  opening 1.8ms a frame.
- **Register 0 is black everywhere.** It is also the colour of the
  border round the screen memory, which nothing drawn can reach (a
  Falcon's on RGB), and the game's colour 0 is a room's background, so
  the jungle had a teal border. Every pixel is XORed with `0x70` as it
  is converted (the add-on's border index, `STDL_SDL2_SetBorderIndex`),
  and the palette goes to the registers permuted to match:
  colour 0 lands on register `0x70`, and register 0 gets slot 7, which
  the Amiga data never uses, held black.
- **The conversion** is the add-on's (`stdl/addons/sdl2/src/c2p*.c`):
  ten merges a 16-pixel group, the XOR done as the inversion of planes
  4-6 at the end, in loops built for that one border index
  (`c2px.c`), which cost what the stub's own routine did. Wide boxes
  go through assembler that spreads its writes to ST-RAM among the next
  group's arithmetic; boxes narrower than 4 groups go through its C
  twin, where the assembler's set-up costs more than it gains. The
  loops must fit the 68030's 256-byte instruction cache, and the build
  fails if one outgrows it: a first C version did not fit, and took
  twice the time SDL's own conversion took. STDL tests it: the C on
  the host against a pixel-by-pixel reference (`test_c2p`), and the
  assembler against the C on the machine (`SDL2CHK.TOS`), which the
  stub had done at start-up ("Planes:" in `RS.LOG`, gone with it).
- **Only what was drawn on goes back each frame.** Upstream copies the
  whole background layer over the front one before every gameplay frame,
  57K each way; this build puts back only the blocks with a dirty count
  (`Video::restoreFront`). The whole layer still comes back after a full
  refresh, or after anything but the game loop has updated the screen (a
  cutscene, a menu, the inventory). The replays' frame hashes are the
  front layer's, and all ten matched before and after. Its rows are
  copied a few longs at a time in the loop: a `memcpy` a row was 160
  calls a frame, and where mintlib's `memmove` fell in the 68030's
  instruction cache against the loop moved a Falcon's frame 0.4-0.8ms
  from one build to the next, as the add-on's size moved the library.
- **Polygon spans are filled in the loop** (`Graphics::fillArea`, this
  build's branch), not through `memset`: a call a span was 560 a frame
  in level 1's opening, and where `memset` fell against the loop in the
  instruction cache moved a Falcon's frame 1.8ms between builds that
  differed only in the add-on.
- **Cutscenes move only what changed, tracked by the ST's code.** A
  scripted scene runs in the ST build's banded loop (`stBandedLoop`) on
  its page tracking, which both builds compile (`pagetrack.cpp`): the
  band of rows the scene draws in, and for each page a box around where
  it differs from the background. A restore, a page copy and the push to
  the screen take that box, not the band. The 030 build's pages, a byte
  a pixel, have their own `ST_copyPage`, `ST_clearLayer` and
  `ST_fillRect` there, and its chunky drawing touches the pages as the
  ST's planar drawing does: a polygon and an ellipse their box, a line
  its box once, a point and a glyph their own.
  - The drawing's touches are gathered on the side (`pageTouchLater`, a
    few compares a primitive) and handed over when something reads the
    tracking. A Falcon runs its code from ST-RAM: `pageTouch` inlined at
    every polygon cost its level 1 opening (0x00) 7-8ms a frame, more
    than the smaller restore saved, and a touch a point, through
    `drawPoint`, put a TT's at 85ms a frame against 60.
  - Against the band it replaced (rows 50-178 of every page, every
    frame), the cutscene bench's work fell 21.1% on a Falcon and 21.8% on
    a TT (music off, every frame drawn). The band itself had taken 29%
    off a Falcon's. `ab=0x400000` turns the tracking off: every copy and
    push then takes the whole page, in both builds.
- **The frame skipper is the ST build's** (`frame_skip`, on by
  default). That covers:
  - the scripted clock;
  - frames not drawn where a part does not build on the last, and
    hidden where it does;
  - the prescan that finds those parts;
  - the per-frame `CUT` and `CUTSUM` log lines.

  Only the planar parts stayed behind (the palette lock, idle bakes). In
  the prescan, `op_drawShape` returns at once, as the ST's does. Voyage's
  text loop no longer sleeps there, in either build: it waited 1.1s
  before the scene began.
- **Sines come from the ST's table.** The 030 build's soft-float
  `sin`/`cos` differed from the table in one frame of 0x2F. The table is
  the formula in double precision, which the ST and desktop builds use,
  so that frame is now drawn as they draw it.

**Measured** in Hatari (`--mmu off`, 4MB, no TT-RAM; a TT at 32MHz with
TOS 3.06, a Falcon at 16MHz with TOS 4.04 and VGA), cutscenes as their
work against their budget (music off, every frame drawn):

| | TT, this build | Falcon, this build | Falcon, ST build |
|---|---|---|---|
| Replay l1r26, ms a frame | 16.1 | 31.5 | 11.7 |
| Intro | 26% | 54% | 16% |
| Level 1's opening | 44% | 87% | 24% |

With the player's settings (music on, frame skipping on), the frames
shown:

| | TT | Falcon |
|---|---|---|
| Intro | 98% | 93% |
| Level 1's opening | 100% | 92% |
| Voyage (0x34) | 98% | 86% |
| Level 4, part 2 (0x39) | 97% | 66% |

The longest run of frames not shown was one on a TT and three on a
Falcon.

- **Correctness:**
  - Every replay ends where its recording did, its frames hash the same
    as with SDL, and the pictures are right on a TT and on a Falcon on
    VGA and RGB. Dropping SDL changed no timing.
  - The band drew exactly what the whole page drew: every frame of the
    cutscene bench's nine scenes, the credits to their end, on a TT and
    on a Falcon on VGA, and three scenes on RGB, 12,362 frames, each
    hashed from the screen. The tracking draws exactly what the band
    drew, on a TT and on a Falcon on VGA (5,825 frames each; RGB was not
    run again).
- **Where the time went:** in Hatari these machines are bound by
  memory, not the CPU. A Falcon at 32MHz took exactly as long as at 16
  (its start-up speed loop did speed up), and everything sits in
  ST-RAM. So the gains came from moving fewer bytes:
  - the layer restore: -25% of a gameplay frame on both;
  - converting straight from the layers: -15% of a cutscene frame;
  - the band in cutscenes: another -29%;
  - only what changed, inside the band: another -21%.
- **The TT has room to spare, and the Falcon keeps up.**
  - In play, 31.5ms a frame is just inside a 30Hz step.
  - Its cutscenes are within their budget on average, and the frame
    skipper keeps them at their scripted pace. Before this they ran up
    to 48% slow (level 1's opening, music on).
  - The push is a quarter of a Falcon's intro frame now (26%; 46% with
    the band). Where everything moves, the box is the band: level 4's
    second part (0x39) spends 28ms of a frame on the push and 8 on the
    restore, and 54 drawing, against a budget of 64.
  - The ST build's planar caches still do far less work than upstream's
    chunky drawing.
- **Set aside on the way (TT):** rows written twice into a 480-line
  surface (27.4ms a frame, 121% in the intro); SDL's doubled mode
  updated from the top (42.4ms, 84%); SDL's routine fed the layers
  directly, which cannot do the XOR (16.4ms, 66%).

**On STDL's screen** (November 2026, three pages; against the XBIOS
stub on the same STDL, in Hatari as above):
- **Correctness:** every replay put up what the XBIOS stub put up, frame
  for frame, the page and its colours (`SHASH.BIN`), on a TT and on a
  Falcon on VGA and RGB, with two pages and with three; the screen
  check agreed at every flip. So did every scene of the cutscene bench,
  the credits to their end (`CUTHASH.BIN`, `CUTPAL.BIN`): 5,825 frames
  on each machine, with two pages and with three.
- **In play** (the ten replays, which are not paced; Flip's wait taken
  out): a frame's work went from 23.03 to 23.84ms on a TT (+3.5%) and
  from 44.18 to 46.03 on a Falcon on VGA (+4.2%); l1r26 16.4 to 17.3
  and 31.7 to 33.6. A page drawn three frames ago must catch up on
  three frames' boxes besides this one's, where the one screen caught
  up on two. Flip waited 1.1ms a frame on a TT and 0.2 on a Falcon.
  With two pages it waits for the VBL that shows its page: 31.85 and
  53.87ms a frame with the wait, which in play would hold a Falcon's
  frame to three VBLs at 60Hz, past the 30Hz step.
  - The first version, which converted every owed box from the
    engine's picture (the title's flicker), took 23.64 and 45.61. The
    copy from the page last shown read both of a TT row's lines (24.54)
    and ORed each box into every page's mask (47.21 on a Falcon) until
    it read a row's first line once and folded the frame's boxes into
    the other pages once a frame.
- **Cutscenes** (eight of the bench's scenes, the credits left out):
  - work, music off and every frame drawn, up 8.8% on a TT and 7.4% on
    a Falcon on VGA (two pages: 40% and 22%);
  - frames shown with the player's settings, TT 99.1% to 98.9%, Falcon
    on VGA 90.7% to 89.0% (two pages: 98.2% and 84.8%).
- **Memory:** the largest free block at a level's load went from 1,633K
  to 1,304K on a TT (4MB, no TT-RAM) and from 1,800K to 1,622K on a
  Falcon on VGA, the pages' ST-RAM; the bank arena kept its 384K and no
  room took longer to enter. `FBACK030.TOS` went from 452,136 bytes to
  480,901, STDL's 256-colour code about 25K of it.
- **What it bought:** no tearing (the stub converted into the screen
  being shown), colours that change with their picture, the desktop
  back after a crash, the display's rate measured, and the mode set,
  desktop and registers no longer the port's code.

**On the SDL2 add-on** (November 2026, three pages; against the stub
with its own conversion on STDL's screen, in Hatari as above):
- **Correctness:** every replay put up what that stub put up, frame for
  frame, the page and its colours, on a TT and on a Falcon on VGA and
  RGB, with three pages and with two, on a TT with TT-RAM, and
  `FLASHBAK.TOS` handing over; the screen check agreed at every flip.
  So did every scene of the cutscene bench, the credits to their end,
  on all three, and the title (65 flips). The ST build, relinked on the
  new STDL (its `event.c`), drew what `ref` draws in every replay, in
  the same time (320,185ms against 320,200).
- **In play** (the ten replays): 24.44ms a frame against 24.79 on a TT
  (-1.4%), 44.44 against 46.23 on a Falcon on VGA (-3.9%).
- **Cutscenes** (eight scenes, the credits left out): work, music off
  and every frame drawn, 30,055ms against 32,195 on a TT (-6.6%) and
  60,040 against 63,575 on a Falcon (-5.6%); frames shown with the
  player's settings, TT 98.9% to 99.2%, Falcon 89.0% to 90.4%.
- **Memory:** 10K less free at a level's load (1,262K against 1,272K
  on a TT, 1,579K against 1,589K on a Falcon); `FBACK030.TOS` 491,149
  bytes against 481,510.
- **How it got there.** On the add-on as it first stood, every picture
  was right but a Falcon's frames were 8.3% slower in play and its
  cutscene work 5.1% more. Profiled (symbols at TEXT `0x1efe4` on a
  Falcon with TOS 4.04; `tmp/tt/p030.sh` and `pc030.sh` run one replay
  or scene under the profiler, `tmp/tt/profdiff.py` compares two):
  - a copy took 269 instructions and 2,033 cycles, and the catch-up
    read the renderer's fields again for every long: fixed in the
    add-on (STDL `6274603`), and play's boxes merged (above);
  - a palette entry a call to the renderer: sent once a frame (above);
  - mintlib's `memmove` and `memset`, which move with the add-on's
    size, against `restoreFront`'s and `fillArea`'s loops in the
    instruction cache: those loops copy and fill inline now (above).
- **Tried and dropped:** the assembler for boxes narrower than 4 groups
  too (the conversion 5.67ms a frame to 6.55 on a Falcon); the C loop
  with post-increments (gcc 4.6 built the same code); the stub padded
  so the layers sat where they had (no change: not their alignment).
  The add-on's C conversion of narrow boxes is about 0.3ms a frame
  slower than the stub's had been on a Falcon, with the same
  instructions and more cache misses, and is left.

**Testing it:**
- Run Hatari's TT with `--mmu off`. With the MMU and the data cache
  both emulated, the emulator falls to half real time once the game
  runs (52.8s of host time against 26.5s), which looks like the game in
  slow motion. The MMU core's timing also looks no truer: the start-up
  speed loop reads 506MHz with it and 102MHz without.
- **TT-RAM needs `--addr24 off` too**: without it, TOS 3.06 with
  `--ttram` never starts the program. The runners take it through
  `LATE`, which `run.sh` puts after its own options:
  `LATE="--memsize 2 --ttram 4 --addr24 off"` is Neil's TT, 2MB of
  ST-RAM and 4MB of TT-RAM.
  - The binary loads into alt-RAM and allocates from it (header flags
    7), so only the pages and the sound ring are in ST-RAM. The game
    counts both RAMs (2048K and 4096K) and takes the 4MB extras.
  - Run that way (November 2026), every replay, the title and eight
    cutscenes drew what the 4MB ST-RAM TT drew, frame for frame, and
    `FLASHBAK.TOS` handed over as it does there.
  - Play took 21.37ms a frame against 23.84, and cutscene work was 22%
    less (25,160ms against 32,195). The largest free block at a level's
    load was 1,731K against 1,304K.
- `tmp/tos/` has TOS 3.06 for the TT (`TOS 3.06 - UK.rom`, which
  every TT run above used) and TOS 4.04 for the Falcon; its EmuTOS
  image is the ST one, which Hatari will not run as a TT or Falcon.
  `tools/bench/` boots TOS 2.06 on an ST: a TT or Falcon run needs its
  TOS, `MACHINE=tt` or `falcon`, and the 030 binary.
- **Pictures in play:** with `replay_hash=true` a replay writes the
  page each frame put up, hashed with the colours as the registers got
  them, into `SHASH.BIN` (a pair of longs a frame), beside the layer's
  `FHASH.BIN`. The layer's hash cannot see a box the push left out, nor
  a page a frame behind. With it the stub also turns on the add-on's
  check (`STDL_SDL2_HINT_CHECK`): a chunky model of the one screen the
  engine pushes to, compared, converted, with every page before it goes
  up, and the stub logs what it found ("SCREEN check" in `RS.LOG` at
  exit, cutscenes and menus included).
  - `tmp/tt/r030.sh TAG tt|fv|fr PROG SCENARIO...` runs the replays (a
    Falcon on VGA or on RGB with a PAL desktop, through its NVRAM in a
    `HOME` of its own), and `tmp/tt/shcmp.py TAG REF MACH SCENARIO...`
    compares two runs frame by frame. They run at most six emulators at
    once (`MAXH`), every script's counted: dozens side by side made
    Neil's Mac unusable, and emulated time does not move with the
    host's load, so queueing costs only the wait.
  - The runners pass `--confirm-quit false`. A `HOME` of its own gives
    Hatari its defaults, which confirm a quit, so every finished
    emulator waited on that dialog and kept its slot: two batches of 24
    slots spent five hours on the six the finished ones left them
    (November 2026).
  - Tested by breaking it: every page's owed boxes dropped after each
    conversion left 560 of l1r26's 638 frames unlike the reference, and
    the screen check flagged the same 560 flips; the copy from the page
    last shown left out, 586 of l1r26's frames and 62 of the title's 65
    flips.
  - `SDL_VIDEODRIVER=dummy` runs Hatari with no window: replays end and
    hash as they do with one, and no window can take a key typed on the
    desktop. One windowed hash run of the credits on a TT ended a third
    of the way through, normally, its pictures right to there, most
    likely a key typed on the desktop reaching it; run again, all 4,686
    frames matched.
- **Cutscene pictures:** with `replay_hash=true` the 030 build hashes
  the screen memory after each frame into `CUTHASH.BIN`, and the
  colours into `CUTPAL.BIN`. `tmp/tt/c030.sh` runs scenes as `r030.sh`
  runs replays, and `tmp/tt/cutsum.py MACH TAG...` tabulates their work
  and frames shown.
  - The reference for a change is the same binary with the change
    switched off (`ab=0x400000` for the tracking), or the binary before
    it, as the stub has no planar page to compare.
  - The scripts are `tmp/tt/cut030.sh`, and `tmp/tt/skip/cmp030.py`,
    which compares two runs.
  - A Falcon's credits with hashing take longer than `run.sh`'s two
    minutes of `waitfile`: chain several.
  - Tested by breaking it: one row left out of the push, or the
    band's out-of-band versions never moved, and frames in every scene tried
    differed; a glyph that did not touch its page, and voyage's text
    frame differed.
- Hatari's 68030 timing is approximate: the figures above are
  estimates until a TT and a Falcon run it. Neil is setting up a TT
  (November 2026) and has no Falcon, so a Falcon's figures stay
  Hatari's.

**Not done, or not known yet:**
- The Falcon's heaviest cutscenes still drop frames: level 4's second
  part (0x39) shows 66% of them, voyage 86%. There everything moves,
  so the box is the band, and upstream's chunky polygons are the rest
  of the frame.
- The music's pace on the machines. STDL measures the VBL rate after
  the mode set and holds the voices' tick, the music and the effects to
  50 a second by it (v1.19.0): Hatari gives 60 on a TT and a Falcon on
  VGA, and RGB at the desktop's PAL or NTSC. A real TT is to say what
  its TT Low runs at.
- Left out with the planar picture: `palette_custom`, and the screen
  modes and refresh rate (Options offers neither). The splash is the same picture,
  decoded to chunky pixels and shown through the stub. The `log_fps` and
  replay lines carry only the room and bank counters.

**Considered and set aside:**
- A TT Low driver in STDL, a chunky surface it converts to 8 planes:
  STDL stays planar. It got responsive screens instead (v1.20.0), 8
  interleaved planes on a TT or a Falcon, which this build now draws
  into with its own conversion.
- Widening the port's planar path to 8 planes: every kernel hard-codes
  4 planes and the 128-byte stride, and a 68030 needs none of what that
  path was built for.
- Writing fewer of the screen's planes in cutscene pushes (November
  2026). A scene's colours, 0xC0-0xDF with text in 0xEF, leave planes 6
  and 7 the same on every pixel after the XOR, and plane 5 too where
  there is no text, so a push could write six planes or five of the
  eight, with a guard at every primitive, glyph and fill falling back to
  more when a colour broke that. The pictures matched frame for frame,
  but in one binary it took only 0.9% off a Falcon's cutscene work and
  1.8% off a TT's (the intro's push 3% and 13%): the push is not bound
  by its writes to ST-RAM. The guards and the layout cost more than
  that, and the binary came out 2% slower than without it.
- The engine built with `ATARIST` off, with STDL only for input and
  sound: it would have lost the menus, the port's sound, record and
  replay, and the pacing.
