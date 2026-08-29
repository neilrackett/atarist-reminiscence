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

`dist/` is the Hatari GEMDOS C: drive during development; the game
expects its files in `DATA\` next to `FLASHBAK.TOS`.

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

## make-music.sh

The Amiga score is sampled music the ST cannot play: there is no DMA
sound on a plain ST, and this port has no software mixer. Every ST
does have the YM2149, so the modules are converted offline into YM
register streams:

```
tools/make-music.sh --download      # fetch modules, then convert
tools/make-music.sh                 # convert what is already there
RS_MUSIC_DIR=/some/where tools/make-music.sh
```

The chain is `MOD -> SMF -> STM`: `mod2smf.py` reads the module's
pattern data and writes a Standard MIDI File, then STDL's
`stdlconv midi` renders that to an STM register stream for
`STDL_Music`. Modules and streams both live in `tmp/music/`, which is
gitignored - the modules are somebody else's work and the streams are
derived from them, so neither belongs in this repository.

All 21 Flashback modules (by Raphael Gesqua, from
[The Mod Archive](https://modarchive.org)) convert to about 106KB of
STM in total, from a 6-second lift cue to the 198-second options
theme. They carry the same track names the engine uses internally -
`jungle`, `holocube`, `introb`, `options1` - so they map onto its
music numbers directly. Output names are uppercase 8.3 for GEMDOS and
uniquified where they would collide (`teleport2` and `teleporta` both
truncate to `TELEPORT`, so the second becomes `TELEPOR1`).

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
