# REminiscence — Atari ST

<img src="./flashback.png" alt="Flashback" width="640" height="400" />

Flashback on the Atari ST, by [Neil Rackett](https://neilrackett.com/atarist).

## Introduction

Of all the games that never had an official released on the Atari ST, one
that I was most disappointed about was Flashback (Delphine Software, 1992).

So, using [STDL](https://github.com/neilrackett/atarist-stdl)
(the `stdl/` submodule) and the Amiga data files, I've ported
[REminiscence](https://github.com/cyxx/REminiscence) 0.5.6, Gregory
Montoir's re-implementation of the original game engine, to the Atari
ST and just three decades later...

**_Say hello to Flashback for Atari ST._**

## Requirements

- Sound effects and in-game music are STE-only.
- Requires 2MB RAM.

## Installing

1. Extract the data files from the Amiga disk images (see below).
2. Copy `FLASHBAK.TOS` and the `DATA\` folder to your ST's hard disk.
3. Run `FLASHBAK.TOS` from the desktop.

The game loads its files from `DATA\` and writes savestates
(`RS<level>_<slot>.SAV`) and `RS.LOG` in the program folder. Options can be
set in `RS.CFG` (same `key=value` format as the SDL build's `rs.cfg`).

## Controls

| Key             | Action                           |
| --------------- | -------------------------------- |
| Arrow keys      | move Conrad                      |
| Shift           | talk / use / run / shoot         |
| Enter           | use the current inventory object |
| Backspace / Tab | display inventory                |
| Any key         | skip the current cutscene        |
| Escape          | display options                  |
| Ctrl S / Ctrl L | save / load game state           |
| Ctrl + / Ctrl - | change game state slot           |
| Ctrl Q          | quit                             |

A joystick in port 1 maps to the arrow keys with fire as Shift.

A controller works too, when an Xpad provider is present (a SidecarTridge
running MD/Sidepad, for example). It drives the game as keypresses, so
nothing in the game knows the difference:

| Pad                     | Key           | Action                   |
| ----------------------- | ------------- | ------------------------ |
| D-pad / left stick      | Arrow keys    | move Conrad              |
| A                       | Shift         | talk / use / run / shoot |
| B, right shoulder       | Enter         | use the inventory object |
| Y, Select, left shoulder| Backspace     | display inventory        |
| X                       | Space         |                          |
| Start                   | Escape        | display options          |

Save, load, quit and the state-slot keys stay on the keyboard: they are
Ctrl combinations, and a pad button emulates a single key.

## Building

With [atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker)
installed:

```
git submodule update --init
stcmd make
```

This produces `dist/FLASHBAK.TOS`.

The desktop SDL2 build is still available via `make -f Makefile.sdl`.

## Data files

The Atari ST build uses the data files from the Amiga release,
placed in a `DATA\` folder next to `FLASHBAK.TOS` with uppercase
names (`replicant.spm` renamed to `REPLICAN.SPM` for GEMDOS 8.3).
`tools/extract-data.sh` extracts and lays them out directly from
CAPS/SPS `.ipf` disk images — see [tools/README.md](tools/README.md).

There is no music in the Amiga data set: the Amiga score ships as
separate `.mod` files, and they are sampled music no ST can play
(there is no DMA sound on a plain ST, and no software mixer here).
`tools/make-music.sh --download` converts those modules into YM2149
register streams instead, which every ST can play — a chip version
of the soundtrack rather than the sampled original. Copy the
resulting `.STM` files into `DATA\` and set `music=true` in
`RS.CFG`. Without them the game plays as before: each missing track
is noted once in `RS.LOG` (with `logging=true`) and the scene runs silent. Unlike the
sampled sound effects this needs no STE — the YM2149 is in every
ST, and the replay costs nothing measurable (35.20 vs 35.11
ms/frame).

If you can't find your original Amiga disks, try the
[TOSEC Commodore Amiga collection](https://ia600803.us.archive.org/view_archive.php?archive=/21/items/Commodore_Amiga_TOSEC_2012_04_10/Commodore_Amiga_TOSEC_2012_04_10.zip).

Other platforms' data files (DOS floppy/CD, Macintosh `FLASHBACK.BIN`
/ `FLASHBACK.RSRC`, PC98, SegaCD `VOICE.VCE` for speech, `.mod`
music sets [4]) are supported by the SDL build — see the upstream
[README](https://github.com/cyxx/REminiscence) for details.

## Configuration

Options go in `RS.CFG`, a plain text file next to `FLASHBAK.TOS`,
one `name=value` per line (`true`/`1` to enable). Lines starting
with `#` are ignored, and the file is optional — every option
defaults to off.

| Option                     | Effect                                                                 |
| -------------------------- | ---------------------------------------------------------------------- |
| `skip_intro`               | Go straight to the title screen, skipping the intro sequence           |
| `crop_screen`              | Crop 12 lines off the top and bottom instead of squashing 224 into 200 |
| `overscan`                 | Open the top border: all 224 lines shown natively, nothing dropped     |
| `music`                    | YM chip music, if the tracks have been built (see below)               |
| `logging`                  | Write progress and warnings to `RS.LOG` (errors are always written)    |
| `log_fps`                  | Log the frame rate to `RS.LOG`, averaged over 64 frames                |
| `bench`                    | Benchmark: time 512 gameplay frames, log the result, then **quit**     |
| `bypass_protection`        | Skip the copy-protection screen                                        |
| `enable_password_menu`     | Show the level password menu                                           |
| `fade_out_palette`         | Fade the palette out between screens                                   |
| `use_text_cutscenes`       | Replace missing cutscenes with their text                              |
| `use_white_tshirt`         | Conrad's t-shirt is white in the intro                                 |
| `play_asc_cutscene`        | Play the ASC cutscene (level 2 fuse)                                   |
| `play_caillou_cutscene`    | Play the CAILLOU cutscene (save checkpoints)                           |
| `play_metro_cutscene`      | Play the METRO cutscene                                                |
| `play_serrure_cutscene`    | Play the SERRURE cutscene                                              |
| `play_carte_cutscene`      | Play the CARTE cutscene (keys)                                         |
| `restore_memo_cutscene`    | Draw the extra shapes in the MEMO cutscene                             |
| `order_inventory_original` | Order inventory items as the original did                              |

The ST-specific ones are `overscan`, `crop_screen`, `skip_intro`,
`logging`, `log_fps` and `bench`. `logging` is off by default (every
line is a file append); turn it on to see what the game is doing
(rooms, cutscenes, missing files),
and `log_fps` and `bench` switch it on for themselves. `bench` exists because frame rates measured against live play
are not comparable between runs — the random seed and input timing
change what is on screen — so it fixes the seed, takes no input and
runs a set number of frames. It ends by quitting to the desktop,
which is indistinguishable from a crash if you have forgotten it is
on, so it announces itself in `RS.LOG` at startup. Turn it off
before playing.

`overscan` and `crop_screen` choose how the game's 224 lines reach
the ST's 200: by default every ninth line is dropped, which keeps
the whole playfield visible but slices through sprites and text.
Cropping instead hides the top and bottom 12 lines — every
displayed line stays intact, at the cost of hiding the edges of
each room. Overscan removes the top border so all 224 lines display
natively with nothing dropped or hidden — the picture reaches the
top edge of the tube and sits ~27 lines higher than a stock screen
on a CRT. A 60Hz/NTSC machine is switched to 50Hz while the game
runs (the border trick needs a PAL frame), and it costs about 2ms
a frame in extra blitting. The Amiga and DOS releases never had
to choose: DOS programmed a ~256x224 VGA mode, and NTSC Amigas
opened a display taller than 200 lines.

## To-do

- Non-STE sound effects (the YM can carry them; the samples cannot)
- Make the main menu look more like the original game
- Make it faster. Measured on level 1 with `bench`: ~16.5 fps on a
  stock 8MHz ST, ~29.5 fps on a 16MHz Mega STE. The shifted sprite
  blit is hand-written 68000 now, so the remaining wins are
  structural: drawing sprites straight to the screen instead of
  through the front layer, or baking sprites pre-shifted.
- Cutscenes still run behind their scripted frame delays: the intro
  wants ~57s of work against a 27s budget on an 8MHz machine, so
  they play slow rather than at the pace the scripts ask for.
  Closing that needs fewer full-page copies and less fill volume,
  not faster instructions.
- SDL 1.2 version for TT and Falcon

## Credits

- A massive thank you to [Gregory Montoir for REminiscence](https://github.com/cyxx/REminiscence) and agreeing to let me use it.
- Delphine Software, obviously, for making another great game.
- Yaz0r, Pixel and gawd for sharing information they gathered on the game.

## More info

If you'd like more information about Flashback:

- [1] http://www.mobygames.com/game/flashback-the-quest-for-identity
- [2] http://en.wikipedia.org/wiki/Flashback:_The_Quest_for_Identity
- [3] http://ramal.free.fr/fb_en.htm
- [4] https://www.exotica.org.uk/wiki/Flashback

## License

**This repository is not open source.**

The REminiscence engine is copyright
[Gregory Montoir](https://github.com/cyxx) with no OSS licence — parts of
it are a direct translation of the game's disassembly, so none fitted.
This port exists by his express permission, given to me for this port
and not as a licence to you: to reuse the engine, ask him. Core-engine
changes made here are listed in [CHANGES.txt](CHANGES.txt), as requested.

Atari ST port related code is copyright (C) 2026 Neil Rackett

[STDL](https://github.com/neilrackett/atarist-stdl), the display and audio
library underneath, is a separate project of mine under the
LGPL-2.1-or-later.

Flashback and its data files are copyright Delphine Software; no game data
is distributed here.
