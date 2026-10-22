# Data and music tools

The Atari ST port uses the Amiga data files. These tools pull them
out of CAPS/SPS `.ipf` disk images of the four Flashback Amiga disks
and lay them out for GEMDOS, and build the ST's chip-music set from
the Amiga modules.

## extract-data.sh

One command from disk images to a ready-to-run game:

```
tools/extract-data.sh disks/Flashback_Disk?of4.ipf
```

For each disk it decodes the IPF to an ADF (`ipf2adf`), unpacks the
AmigaDOS filesystem (`xdftool` from `pip install amitools`), then
flattens every disk's `data/` and `cine/` directories plus the root
`font8.spr` into **`dist/DATA/`** with uppercase 8.3 names
(`replicant.spm` becomes `REPLICAN.SPM`; the ST build asks for that
name). Set `RS_DATA_DIR` to write somewhere other than `dist/DATA`.

It also collects each disk's `music/` directory into **`tmp/music/`**
as the ProTracker modules `make-music.sh` expects. Those are named
from each module's own 20-byte title field rather than its filename:
the filename is the engine's primary track name, while the port looks
up the alternate one (`ModPlayer::_names` in `src/staticres.cpp`), and
the title is the alternate. Reading it out of the file means no
mapping table here to drift out of step. Set `RS_MUSIC_DIR` to write
them somewhere else.

`dist/` is the Hatari GEMDOS C: drive during development; the game
expects its files in `DATA\` next to `FLASHBAK.TOS`, and the chip
music separately in `MUSIC\` - `DATA\` holds only what came off the
disks, so it can be compared against a fresh extraction.

## check-names.sh

```
tools/check-names.sh [dir]          # report problems, default dir: dist/
tools/check-names.sh --fix [dir]    # and rename what is safe to rename
```

Reports anything that cannot live on a GEMDOS volume: a name over
eight characters, an extension over three, lower case, characters
outside `A-Z 0-9 _ -`, more than one dot, or — the one that actually
breaks a game — two files that collide once GEMDOS has truncated
them, the way `REPLICANT.SPM` and `REPLICAN.SPM` do. Exits non-zero
if it finds anything, so it can gate a release.

`--fix` renames what it can: case, an over-long name, an over-long
extension. It refuses three things on purpose, and reports them
instead. A name whose fixed form is already taken, because two files
wanting one name is the case that loses data and choosing a winner is
not a script's decision. Odd characters, because there is no right
substitute and inventing one risks colliding with a name that was
already correct. And more than one dot, because `FILE.NAME.TXT` could
be `FILENAME.TXT` or `FILE.NAM` and only its author knows which.

It is safe to run twice — the second pass renames nothing — and safe
to run on data that is already correct, which `extract-data.sh` output
always is.

`extract-data.sh` already names its output correctly. This is for
data extracted with something else, which the main README suggests
for anyone whose disks have gone.

## ipf2adf.c

Decodes an IPF's AmigaDOS MFM tracks into a plain `.adf` sector
image. Build it once, linked against
[capsimg](https://github.com/FrodeSolheim/capsimg):

```
git clone https://github.com/FrodeSolheim/capsimg
(cd capsimg && ./bootstrap && ./configure && make)
cc -O2 -o tools/ipf2adf tools/ipf2adf.c capsimg/capsimg.so
cp capsimg/capsimg.so tools/
install_name_tool -change capsimg.so @loader_path/capsimg.so tools/ipf2adf  # macOS
```

Note the CAPS structs are `#pragma pack(1)` — the converter's local
declarations mirror that (without it the library silently returns
empty tracks).

## make-unpacked.py

```
tools/make-unpacked.py                      # dist/DATA -> dist/UNPACKED
tools/make-unpacked.py DATA_DIR [OUT_DIR]   # OUT_DIR defaults to UNPACKED beside DATA_DIR
```

Writes **`dist/UNPACKED/`**: the packed files in `DATA`, unpacked
ahead of time. The game reads a file from there in place of unpacking
it, which on an 8MHz ST was most of the wait for a cutscene, the title
or a level, and falls back to `DATA` for anything missing.

What is unpacked as something loads: cutscenes (`.CMP`), the title
picture (`PRESENT.CMP`), sprites (`.SPM`), level graphics (`.SGD`),
objects (`.OBC`) and collision data (`.CT`). And what is unpacked on
entering a room: the room data (`.LEV`) and the object and tile banks
(`.MBK`). The game keeps those two whole for a level, so it reads them
from here only where there is room: the banks go into the bank arena,
which on a 4MB machine holds a whole level's, and the room data (71-143K
a level) needs 3.5MB of RAM. A 2.5MB machine unpacks them room by room
from `DATA` as before.

Each file keeps its `DATA` name and holds `RXU1`, a BE32 stream count,
a BE32 size per stream, then the streams - two for a cutscene
(polygons, then commands), 64 for room data (a room with no data is an
empty stream), one per bank entry for banks (an empty entry likewise),
one for everything else. The game checks that the sizes add up before
trusting a file, and that every bank is the size `DATA`'s table says.

RExtract writes the same folder. The decoder here is a transcription
of `src/unpack.cpp`'s C version, and its output was checked byte for
byte against that decoder and against RExtract's on the real disks.
A file that will not unpack is reported and left out, so the game
unpacks that one itself. No dependencies beyond Python 3.

## make-music.sh

The Amiga score is sampled music the ST cannot play: there is no DMA
sound on a plain ST, and this port has no software mixer. Every ST
does have the YM2149, so the modules are converted offline into YM
register streams:

```
tools/make-music.sh                 # convert what extract-data.sh found
RS_MUSIC_DIR=/some/where tools/make-music.sh
```

`extract-data.sh` takes the modules off the game's own disks into
`tmp/music/`, which is where this reads from by default, so there is
nothing to fetch and the music is the player's own data. The streams
go to **`dist/MUSIC/`**, beside `dist/DATA/` - the layout the ST
wants, so there is nothing to move by hand afterwards. `RS_MUSIC_DIR`
and `RS_MUSIC_OUT` override the two ends.

There is deliberately no download path. The game cannot run without
its data files, so anyone able to play already has the disks the
score is on - and the disks carry all 21 tracks, where the copy on
The Mod Archive is missing `memoire` and leaves that cue silent.
That gap is how the whole thing was found.

The chain is `MOD -> SMF -> STM`: `mod2smf.py` reads the module's
pattern data and writes a Standard MIDI File, then STDL's
`stdlconv midi` renders that to an STM register stream for
`STDL_Music`. Modules and streams both live in `tmp/music/`, which is
gitignored - the modules are somebody else's work and the streams are
derived from them, so neither belongs in this repository.

All 21 Flashback modules (by Raphael Gesqua) convert to about 107KB
of STM in total, from a 6-second lift cue to the 198-second options
theme. They carry the same track names the engine uses internally -
`jungle`, `holocube`, `introb`, `options1` - so they map onto its
music numbers directly. Output names are uppercase 8.3 for GEMDOS,
keeping the last character where a name is too long rather than
truncating: `teleport2` and `teleporta` differ only there and would
both become `TELEPORT`, so they become `TELEPOR2` and `TELEPORA`.
`ATARIST_musicName` in `src/mixer.cpp` derives the same name on the
ST - the two have to agree.

To hear one before wiring anything up, STDL's example player takes an
STM as `DEMO.STM`:

```
cp tmp/music/JUNGLE.STM /some/gemdos/dir/DEMO.STM
stdl/tests/hatari/run.sh ym stdl/dist/PLAYMUS.TOS 8 "sleep 20"
```

Verified on an emulated plain STF: the streams play about eight times
louder than the STE sample effects, which suits a machine where they
are the only sound there is.

## mod2smf.py

ProTracker module to Standard MIDI File, used by `make-music.sh` and
useful on its own:

```
tools/mod2smf.py IN.mod OUT.mid [--drums auto|N|none] [--verbose]
```

Periods become MIDI notes, the order table is followed as the player
would follow it, and speed/tempo effects set the MIDI tempo. Sample
detail - the instrument waveforms, the volume envelopes - has nowhere
to go on a YM and is dropped, so expect a chip cover rather than a
reproduction.

Two details worth knowing if it ever needs changing:

- **Percussion.** `stdlconv` sends MIDI channel 10 to the noise
  generator, so one MOD channel is marked as drums when its samples
  are short, unlooped and played over a narrow pitch range. With only
  three voices, a kick drum competing with the melody is a poor
  trade.
- **Looping.** A module that jumps backwards through its order table
  (`Bxx`) is saying "loop"; the converter stops at the first revisited
  position and lets the player repeat the track. Following the jump
  instead rendered three cues as 22-minute files, which overflowed the
  STM header's 16-bit frame count.

## gen-palette-tables.py

Writes `src/palette_tables.h`, the data behind the ST's automatic room
palette:

- **`kSTLab`**: CIELAB for each of the STE's 4096 colours, so the
  quantiser can compare colours the way the eye does without floating
  point.
- **`kSTColourUse`**: for each level, palette number and colour index,
  the average share of the room pixels drawn in that colour, measured
  over every room of the level. The quantiser weighs colours by it, so
  a colour covering a quarter of the screen is not traded away for one
  that covers a handful of pixels.

(Which half of the object palette a level's enemies are drawn with is
worked out in the game from its monster list, so those colours count
like Conrad's.)

The header is committed and only needs regenerating if the weighting
is changed. The pixel counts come from rooms rendered by the desktop
(SDL) build with upstream's disabled room dump turned on: the `if (0)`
block in `Game::loadLevelData` writes `DUMP/level<L>_room<NN>.bmp` for
every room of a level. In a copy of the sources, make that block run
when an environment variable is set, exit once it has, name the files
`level<L>_part<P>_room<NN>.bmp` (`<P>` is `_res._levNum`, 0 outside
level 2), and for level 2 dump each of its two level files in turn.
Then:

```sh
tools/gen-palette-tables.py path/to/DUMP dist/DATA > src/palette_tables.h
```

Needs Python 3 with Pillow.

## bench/

Gameplay benchmarks that play. `bench=true` times an idle Conrad in a
room whose sprites are all cached already, which is the one case that
never shows a slow frame. These replay real sessions instead.

**Recording.** Put `record=1` in `RS.CFG`, choose Load on the title and
play: running, fighting, rooms changing. Escape, a death, a cutscene or
a new level ends the recording, and `REC01.DEM` is written beside the
game. It holds the save it started from (level and slot), the seed and
one byte of input per game step. Play with fast-forward off. Ctrl keys
and menus are not recorded, so leave them alone while recording.

A scenario is a directory holding the save and its `REC01.DEM`. Keep
them in `tmp/bench/scenarios/` (they come from your own game data, so
they stay out of git).

**Replaying.**

```
tools/bench/replay.sh TAG MACHINE SCENARIO...
```

This plays each scenario in Hatari (cycle-exact, one emulator per
scenario, side by side) with `replay=1`. The game runs with no pacing,
logs what every 64 frames cost and what the sprite caches did, and ends
with `REPLAY done: ... ms/frame, steady ..., worst ..., hash ... match`.
`match` means the game ended in exactly the state the recording did, so
the logic ran the same. `PROG=` picks another binary, `MEM=2560` a 2.5MB
machine, and `CFG=` adds `RS.CFG` lines, for example `ab=16` to switch
one optimisation off (the `kAb*` bits in `src/intern.h`) or
`replay_hash=true` to hash every frame's picture.

**Comparing.**

```
tools/bench/compare.py TAG_A TAG_B MACHINE SCENARIO...
tools/bench/compare.py --frames TAG_A TAG_B MACHINE SCENARIO...
```

The first prints steady ms/frame per scenario and the total. Quote the
total: code layout alone moves a single scenario by a few percent. The
second compares two `replay_hash=true` runs: every frame's picture is
hashed, the hashes are combined per 64-frame window and logged, and the
windows must all match. That is how an optimisation is shown to draw
exactly what it replaced. Hashing costs about 30ms a frame on a plain ST,
inside the timed frame, so a hash run's times say nothing; time with a
run of its own. A Mega STE's frame times move with code layout through
its cache, so compare it inside one binary, using `ab=` to switch
features.

When two runs part, the run directories say where: `FHASH.BIN` holds
each frame's own hash (`cmp` two of them for the first frame that
differs), and `replay_dump=N` in `CFG` writes frame N's layer and
colours to `DMP.BIN` to look at.

**Checking a change.**

```
tools/bench/verify.sh TAG [PREV]
```

Does the lot for `dist/FLASHBAK.TOS`, keeping a copy as
`tmp/bench/bin/TAG.TOS`: every scenario hashed and compared with the
reference, its ending checked, then timed and compared with run `PREV`,
then the time each room took to enter. It exits non-zero when a picture
or an ending differs. `SCENS=` narrows the scenarios, `MACHINE=`,
`MEM=` and `UNP=` pass through.

The reference is one `replay_hash=true` run, tag `ref`, of the build
with every sprite drawn from its source and whole frames redrawn
(`ab=24`, no bakes and no display list - the drawing nearest upstream's).
Make it again whenever a scenario is added or recorded again:

```
CFG="ab=24;replay_hash=true" tools/bench/replay.sh ref st SCENARIO...
```

**Saves in busy rooms.** `tools/bench/teleport-save.py SRC.SAV LEVEL ROOM
X Y OUT_DIR` copies a save made at a level's start with Conrad moved to
another room, named `RS<level>_<room>.SAV`, so a room full of enemies is
one title Load away for testing on hardware. Y is one of Conrad's
floors (70, 142, 214); try the save in Hatari first, as a spot inside a
wall or over a drop kills him.

## cutbench/

Cutscenes, one at a time. The hidden `cutscene=N` option (hex as `0x34`)
plays scene N from where the intro would and quits, and with
`logging=true` the log has a `CUT` line for every eight frames and a
`CUTSUM` line per scene part:
- **`CUT` lines:** work, budget, push and restore ms; frames late,
  skipped and unshown; shapes, sprites and why the rest were refused;
  polygons from the cache; ellipses and shadows.
- **`CUTSUM` lines:** frames shown, the longest run not shown, and work
  p50/p95/max.

```
tools/cutbench/run.sh TAG MACHINE SCENE...     # e.g. b1 st 0x0D 0x00 0x39
tools/cutbench/summarise.py TAG [TAG...]       # one row per scene part
tools/cutbench/worksum.py MACHINE TAG [TAG...] # total work per scene, run against run
tools/cutbench/verify.sh TAG [REF]             # frame-exact against REF
tools/cutbench/prof.sh OUT MACHINE SCENE       # a Hatari profile; prof.py OUT reads it
```

- **`run.sh`** uses the player's settings (music on, frame skipping on,
  Fit) unless `CFG` adds lines. `CFG="frame_skip=false;music=false"`
  measures the work a scene takes. The runs land in
  `tmp/cutbench/runs/TAG_MACHINE_SCENE/`.
- **`worksum.py`** adds up those runs' work, scene by scene, and the
  total, the last tag against the first: the number to quote for a
  code change. Frames shown (`summarise.py`, the player's settings) is
  what a player sees, but it moves with timing alone - a 21-frame scene
  by three frames - and idle bakes follow spare time, so a faster
  build can bake less. `CFG="...;ab=0x200"` (no shape sprites) times
  the polygon path on its own.
- **`verify.sh`** plays the intro, every level opening, a mid-level
  scene and the credits with `replay_hash=true`. Every drawn frame's
  pixels go into `CUTHASH.BIN` and the colours it was shown in into
  `CUTPAL.BIN`, and it compares both with run `REF`, by default `cref`.
  A change has to match every frame. `cref`'s pixels are r30's own:
  r30 built with nothing added but `cutscene=` and this hash. Its
  colours are those of the first harness build's old-palette run
  (`ab=0x20000`). A run with no `REF` yet becomes one.

## gemdrive/

A second run from an ACSI2STM GemDrive drive. GemDrive's loader clears
only the first 130,560 bytes of a program's BSS (one 255-sector DMA
run), so the rest of the game's 158K started with what the last program
left there, and the second run bombed. The game now clears its own BSS
before mintlib starts (`__st_entry` in `main_atari.cpp`).

```
tools/gemdrive/run.sh       # PASS or FAIL; PROG= another binary, MACHINE= (default st)
```

`gdemu.c` runs the game twice in Hatari from the same address (the
logos, `cutscene=0x40`, then quit). Before the second run it puts the
first run's leftovers back into the BSS past that point, and scribbles
the rest of free memory as the desktop would. r31 fails it with a bus
error in mintlib's `localsub`; the build with the stub passes.
