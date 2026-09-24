# Flashback for Atari ST

<img src="./flashback.png" alt="Flashback" width="640" height="400" />

Flashback ported to the Atari ST via [REminiscence](https://github.com/cyxx/REminiscence),
by [Neil Rackett](https://neilrackett.com/atarist).

## Introduction

Of all the games that never had an official release on the Atari ST, one of
the ones I was most disappointed about was Flashback (Delphine Software, 1992).

Using [STDL](https://github.com/neilrackett/atarist-stdl) and the Amiga
data files, I've ported [REminiscence](https://github.com/cyxx/REminiscence),
Gregory Montoir's re-implementation of the original game engine, to the Atari ST.

So, just three decades after the original was released... **_Say hello to Flashback for the Atari ST._**

## Requirements

- Works on any ST with at least 2MB RAM (4MB recommended) and 3MB of hard disk space.
- Sound effects are STE-only.
- Optional music works on any ST (see below).

On a 2MB machine it's a tight fit, especially with music and overscan on,
so keep desk accessories and resident programs to a minimum.

## Installing

1. Copy `FLASHBAK.TOS` (and optionally `RS.CFG`) to your ST's hard disk.
2. Extract all of the files from inside the `cine` and `data` folders, plus `font8.spr`
   from the root of disk 1, on your original Amiga installation disks into a `DATA`
   folder next to `FLASHBAK.TOS` (see below).
3. Run `FLASHBAK.TOS`.

Saved games (`RS<level>_<slot>.SAV`) and `RS.LOG` live in the program folder.
The default `RS.CFG` includes all of the available options (see below).

## Controls

You can control Conrad with the keyboard, a joystick in port 1, or a gamepad:

| Key             | Joystick   | Gamepad                  | Action                           |
| --------------- | ---------- | ------------------------ | -------------------------------- |
| Arrow keys      | Directions | D-pad / left stick       | move Conrad                      |
| Shift           | Fire       | A                        | talk / use / run / shoot         |
| Enter           |            | B, right shoulder        | use the current inventory object |
| Backspace / Tab |            | Y, Select, left shoulder | display inventory                |
| Space           |            | X                        | toggle the gun on / off          |
| Escape          |            | Start                    | display options                  |
| Any key         | Fire       | Any button               | skip the current cutscene        |
| Ctrl S / Ctrl L |            |                          | save / load game state           |
| Ctrl + / Ctrl - |            |                          | change game state slot           |
| Ctrl Q          |            |                          | quit                             |

Gamepads are supported via [Xpad](https://downloads.neilrackett.com/atarist-xpad)
providers, including [MD/Sidepad](https://downloads.neilrackett.com/md-sidepad).

Save, load, quit and the state-slot keys stay on the keyboard: they are
Ctrl combinations, and a pad button emulates a single key.

## Data files

**Flashback is still copyright Delphine Software, so you'll need to extract the
files needed from your legally owned original Amiga installation disks.**

The easiest way to extract the data files from your Amiga installation disks
in the correct format is to use the [RExtract online tool](https://labs.neilrackett.com/web-rextract/).

### Extracting the files manually

Everything you need is on one or more of the four disks (some may span multiple disks):

| On the disk | What it is                                        | Where it goes               |
| ----------- | ------------------------------------------------- | --------------------------- |
| `data/`     | the game itself — levels, sprites, palettes, text | Copy all files into `DATA\` |
| `cine/`     | the cutscenes                                     | Copy all files into `DATA\` |
| `font8.spr` | the font, in the root of disk 1                   | Copy the file into `DATA\`  |
| `music/`    | the score, as ProTracker modules (optional)       | (see below)                 |

For CAPS/SPS `.IPF` disk images, `tools/extract-data.sh` extracts and renames the
files for you; see [tools/README.md](tools/README.md) for the dependencies
you'll need to install first.

Alternatively, if you have `.ADF` disk images, you could try
[HxC Floppy Emulator software](https://hxc2001.com/download/floppy_drive_emulator/#sdhxc)
or one of the other tools available via a
[quick Google search](https://www.google.com/search?q=tools+for+extracting+data+from+amiga+disk+images).

The extracted files all go into a `DATA\` folder next to `FLASHBAK.TOS`,
and must be renamed to have uppercase 8.3 ST-friendly names, e.g. `replicant.spm`
becomes `REPLICAN.SPM`.

You can run `tools/check-names.sh` after extracting the files and it will tell
you about any name too long, not uppercase, or quietly colliding with another.
Add `--fix` and it renames the ones it can do safely, listing anything that
needs you to decide.

_All of the `.sh` tools run on macOS, Linux, or Windows via WSL._

For the SDL build, see the upstream [README](https://github.com/cyxx/REminiscence)
for more information.

### Music

Music is still experimental and so remains optional.

The score is on the disks, in `music/` — one ProTracker module per
track. The ST cannot play them as they are, so `tools/make-music.sh`
converts them offline into YM2149 register streams, which every ST
can play.

`extract-data.sh` puts the modules in `tmp/music`, and
`tools/make-music.sh` with no arguments converts whatever it finds
there into `dist/MUSIC`. Copy that `MUSIC\` folder to your ST, next to
`FLASHBAK.TOS` and alongside `DATA\`, and set `music=true` in
`RS.CFG`.

Without the `.STM` files the game plays as it always did: each
missing track is noted once in `RS.LOG` (with `logging=true`) and the
scene runs silent.

## Custom palettes

The ST shows 16 colours at a time, so for every room the game picks
16 from the 50 or more the Amiga graphics use, favouring the colours
that cover most of the level's rooms and the ones Conrad and the
enemies are drawn in. With
`palette_custom=true` you can supply your own instead: 16 colours in a
`.hex` file in a `PALETTE\` folder next to `FLASHBAK.TOS`, one
`RRGGBB` per line, as Lospec and Aseprite export palettes. Every
colour in the room then uses the nearest of the 16.

For each room the game looks for, in order:

| File              | Used for                                    |
| ----------------- | ------------------------------------------- |
| `L1R45.HEX`       | level 1, room 45                            |
| `L1.HEX`          | any room of level 1 without its own file    |

Level 2 is stored as two parts whose room numbers overlap, so its rooms
are named by part as well: `L2_1R17.HEX` and `L2_2R17.HEX` (`L2.HEX`
still covers the whole level). With neither file, the room keeps the
colours the game chose.

### Choosing which colour goes where (optional)

On the Amiga, a room is drawn in 32 colours: 0-15 for the background
and 16-31 for Conrad, objects and items. After any of your 16 colours
you can list which of those 32 it replaces:

```
000000 = 1,7,16
00aa00 = 2,3,4,13
00aaaa = 0,14
```

The lists are optional. An Amiga colour that isn't listed uses
whichever of your 16 is nearest, and so do enemies and the few colours
outside the 32. In a level file (`L1.HEX`) a number means that position
in whichever room is showing, which on most levels is the same colour
throughout.

### What the 32 colours are

The colours themselves change from level to level, but their roles
mostly don't:

```
0-15  = the room's background scenery, back layer and foreground
16    = black
17    = Conrad's highlights
18    = objects
19    = Conrad's trousers, darker shade
20    = Conrad's skin
21    = Conrad's grey
22    = Conrad's trousers
23    = Conrad's jacket
24-31 = items and effects
```

Enemies are drawn with half of 16-31: 16-23 or 24-31, depending on the
enemy, so on some levels they share Conrad's colours. The background's
roles are whatever the room's artist chose; press Ctrl+P to see a
room's actual colours.

### Making one

To start from the game's own choice, press **Ctrl+P** in play: the
room's palette is written to the program folder as that room's file
(e.g. `L1R45.HEX`), and the name is shown on screen. The file lists the
room's 32 Amiga colours by number in comments, and each of the 16 lines
already names the ones it's used for, so you can change colours and
move numbers from line to line. Then move it into `PALETTE\`, as it is
for that room or renamed to `L1.HEX` for the whole level. Cutscenes
keep their own colours.

Press **Ctrl+Shift+P** to reload: the game looks in `PALETTE\` again
and redraws the room with whatever file now applies to it, so you can
edit a palette and see the result without leaving the room. The file
in use is shown on screen, or "automatic" when there isn't one.

Without lists, each colour takes the nearest of your 16. The jungle's
greens are muted olives, and with a palette whose only greens are
bright - EGA's, say - nearest means grey. Listing them against a green
fixes that exactly; `palette_hue=true` is the quick alternative, which
matches by hue instead, though greys and pale colours tend to come out
noisier.

## Configuration

Options go in `RS.CFG`, a plain text file next to `FLASHBAK.TOS`,
one `name=value` per line (`true`/`1` to enable). Lines starting
with `#` or `;` are ignored, and the file is optional.

The `RS.CFG` that ships with the build includes every option,
with defaults named, so switching one is a matter of deleting a `#`.

| Option                     | ST-specific | Effect                                                                                                                                                                                                                         |
| -------------------------- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `skip_intro`               | ✓           | Go straight to the title screen, skipping the intro sequence                                                                                                                                                                   |
| `screen`                   | ✓           | How 224 lines meet the 200-line screen: `fit` drops a row in eleven (default), `fill` crops 18 off the top and 6 off the bottom in play (12 and 12 elsewhere), `top` opens the top border, `full` both. Also on the title menu |
| `palette_custom`           | ✓           | Room palettes from a `PALETTE\` folder, and Ctrl+P to write the current one out (see [Custom palettes](#custom-palettes))                                                                                                        |
| `palette_hue`              | ✓           | Match colours to a custom palette by hue rather than nearest colour (see [Custom palettes](#custom-palettes))                                                                                                                  |
| `music`                    | ✓           | YM chip music, if the tracks have been built (see above)                                                                                                                                                                       |
| `music_volume`             | ✓           | Chip music level, 0-100 (default 70, which sits it level with the sampled effects)                                                                                                                                             |
| `cheats`                   | ✓           | `true` for all of them, or bits added together: 1 monsters die in one hit, 2 Conrad is never hit, 4 his life never drops                                                                                                       |
| `ste_sound`                | ✓           | Open the STE sample device (**on** by default; `false` disables sampled sound entirely, chip music unaffected)                                                                                                                 |
| `megaste_speedup`          | ✓           | Switch a Mega STE to 16MHz with its cache (**on** by default; `false` keeps the speed set before the game ran)                                                                                                                 |
| `frame_skip`               | ✓           | Drop cutscene frames to hold the scripted pace (**on** by default; `frame_skip=false` draws every frame, slower)                                                                                                               |
| `bypass_protection`        |             | Skip the copy-protection screen                                                                                                                                                                                                |
| `enable_password_menu`     |             | Show the level password menu                                                                                                                                                                                                   |
| `fade_out_palette`         |             | Fade the palette out between screens                                                                                                                                                                                           |
| `use_text_cutscenes`       |             | Replace missing cutscenes with their text                                                                                                                                                                                      |
| `use_white_tshirt`         |             | Conrad's t-shirt is white in the intro                                                                                                                                                                                         |
| `play_asc_cutscene`        |             | Play the ASC cutscene (level 2 fuse)                                                                                                                                                                                           |
| `play_caillou_cutscene`    |             | Play the CAILLOU cutscene (save checkpoints)                                                                                                                                                                                   |
| `play_metro_cutscene`      |             | Play the METRO cutscene                                                                                                                                                                                                        |
| `play_serrure_cutscene`    |             | Play the SERRURE cutscene                                                                                                                                                                                                      |
| `play_carte_cutscene`      |             | Play the CARTE cutscene (keys)                                                                                                                                                                                                 |
| `restore_memo_cutscene`    |             | Draw the extra shapes in the MEMO cutscene                                                                                                                                                                                     |
| `order_inventory_original` |             | Order inventory items as the original did                                                                                                                                                                                      |
| &nbsp;                     |             |                                                                                                                                                                                                                                |
| **Diagnostics**            |             |                                                                                                                                                                                                                                |
| `blitter`                  | ✓           | Use the BLiTTER where the machine has one (**on** by default; `blitter=false` forces the CPU paths, for diagnosis)                                                                                                             |
| `logging`                  | ✓           | Write progress and warnings to `RS.LOG` (errors are always written)                                                                                                                                                            |
| `log_fps`                  | ✓           | Log the frame rate to `RS.LOG`, averaged over 64 frames                                                                                                                                                                        |
| `bench`                    | ✓           | Benchmark: time 512 gameplay frames, log the result, then **quit**                                                                                                                                                             |

## Work in progress

While the game is now pretty much feature complete, it is still a work in progress.

### Feedback

Please [submit an issue](https://github.com/neilrackett/atarist-reminiscence/issues)
or [open a pull request](https://github.com/neilrackett/atarist-reminiscence/pulls)
if you find anything that doesn't work or if you'd like to contribute to make it better.

Please ensure that you include as much information as possible in your submission,
including:

- The Atari ST model and TOS version (ST, STE, Mega ST, Mega STE)
- Whether you are using an emulator (e.g. [Hatari](https://www.hatari-emu.org/)) or real hardware
- The amount of RAM you have installed
- Any devices you have connected
- The exact steps to reproduce the issue
- Any error messages or logs you see

### To-do

- Simplified, non-STE sound effects using YM
- We're using the Amiga menu (level selector), let's see if we can implement features from the DOS menu, including difficulty selection and save/load functionality
- Play holds ~28fps on an 8MHz ST, even in a busy room, the same as a Mega STE: the rest of the way to 30 (the game's maximum) is lost to the 200Hz timer rounding each frame's pause. Can we pace it more finely?
- Can we achieve smooth cutscenes playback without dropping frames?

## Building

With [atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker)
installed:

```
git submodule update --init
stcmd make
```

This produces `dist/FLASHBAK.TOS`.

The desktop SDL2 build is still available via `make -f Makefile.sdl`.

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
library underneath, is a separate project under LGPL-2.1-or-later.

Flashback and its data files are copyright Delphine Software; no game data
is distributed here.
