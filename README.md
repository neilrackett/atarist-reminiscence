# Flashback for Atari ST

<img src="./flashback.png" alt="Flashback" width="640" height="400" />

Flashback ported to the Atari ST via [REminiscence](https://github.com/cyxx/REminiscence),
by [Neil Rackett](https://neilrackett.com/atarist).

## Introduction

Of all the games that never had an official release on the Atari ST, one of
the ones I was most disappointed about was Flashback (Delphine Software, 1992).

So, using [STDL](https://github.com/neilrackett/atarist-stdl) and the Amiga
data files, I've ported [REminiscence](https://github.com/cyxx/REminiscence),
Gregory Montoir's re-implementation of the original game engine, to the Atari ST.

So, just three decades after the original was released...

**_Say hello to Flashback for the Atari ST._**

## Requirements

- Works on any ST with at least 2MB RAM (4MB recommended) and 3MB of hard disk space.
- Sound effects are STE-only.
- Optional music works on any ST (see below).

## Installing

1. Extract the data files from your legally owned original Amiga installation disks (see below).
2. Copy `FLASHBAK.TOS` (and optionally `RS.CFG`) to your ST's hard disk.
3. Copy the extracted files into a `DATA\` folder next to `FLASHBAK.TOS`.
4. Run `FLASHBAK.TOS`.

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

The Atari ST version uses the Amiga data files. If you can't find
your original Amiga disks, try the
[TOSEC Commodore Amiga collection](https://ia600803.us.archive.org/view_archive.php?archive=/21/items/Commodore_Amiga_TOSEC_2012_04_10/Commodore_Amiga_TOSEC_2012_04_10.zip).

You can use `tools/extract-data.sh`, which runs on macOS, Linux or via WSL on Windows,
to extract the files directly from CAPS/SPS `.ipf` disk images; see [tools/README.md](tools/README.md).

Extracted files should be placed in a `DATA\` folder next to `FLASHBAK.TOS` with uppercase 8.3
names, e.g. `replicant.spm` becomes `REPLICAN.SPM`.

Alternative tools for extracting files from Amiga disk images are available via a
[quick Google search](https://www.google.com/search?q=tools+for+extracting+data+from+amiga+disk+images).

Other platforms' data files (DOS floppy/CD, Macintosh `FLASHBACK.BIN`
/ `FLASHBACK.RSRC`, PC98, SegaCD `VOICE.VCE` for speech, `.mod`
music sets) are supported by the SDL build. See the upstream
[README](https://github.com/cyxx/REminiscence) for details.

### Music

There is no music in the Amiga data set: the Amiga score ships as
separate `.mod` files. `tools/make-music.sh --download` converts
those modules into YM2149 register streams instead, which every
ST can play — a chip version of the soundtrack rather than the
sampled original. Copy the resulting `.STM` files into `DATA\` and
set `music=true` in `RS.CFG`. Without them the game plays as before:
each missing track is noted once in `RS.LOG` (with `logging=true`)
and the scene runs silent.

## Configuration

Options go in `RS.CFG`, a plain text file next to `FLASHBAK.TOS`,
one `name=value` per line (`true`/`1` to enable). Lines starting
with `#` or `;` are ignored, and the file is optional.

The `RS.CFG` that ships with the build includes every option,
with defaults named, so switching one is a matter of deleting a `#`.

| Option                     | ST-specific | Effect                                                                                                             |
| -------------------------- | ----------- | ------------------------------------------------------------------------------------------------------------------ |
| `skip_intro`               | ✓           | Go straight to the title screen, skipping the intro sequence                                                       |
| `crop_screen`              | ✓           | With `overscan=false`: crop 12 lines off the top and bottom instead of squashing 224 into 200                      |
| `overscan`                 | ✓           | Open the top border: all 224 lines shown natively, nothing dropped (**on** by default)                             |
| `overscan_bottom`          | ✓           | Open the bottom border too, centring the picture in 273 lines (off by default: can be unstable on Mega STE)        |
| `music`                    | ✓           | YM chip music, if the tracks have been built (see above)                                                           |
| `frame_skip`               | ✓           | Drop cutscene frames to hold the scripted pace (**on** by default; `frame_skip=false` draws every frame, slower)   |
| `bypass_protection`        |             | Skip the copy-protection screen                                                                                    |
| `enable_password_menu`     |             | Show the level password menu                                                                                       |
| `fade_out_palette`         |             | Fade the palette out between screens                                                                               |
| `use_text_cutscenes`       |             | Replace missing cutscenes with their text                                                                          |
| `use_white_tshirt`         |             | Conrad's t-shirt is white in the intro                                                                             |
| `play_asc_cutscene`        |             | Play the ASC cutscene (level 2 fuse)                                                                               |
| `play_caillou_cutscene`    |             | Play the CAILLOU cutscene (save checkpoints)                                                                       |
| `play_metro_cutscene`      |             | Play the METRO cutscene                                                                                            |
| `play_serrure_cutscene`    |             | Play the SERRURE cutscene                                                                                          |
| `play_carte_cutscene`      |             | Play the CARTE cutscene (keys)                                                                                     |
| `restore_memo_cutscene`    |             | Draw the extra shapes in the MEMO cutscene                                                                         |
| `order_inventory_original` |             | Order inventory items as the original did                                                                          |
| &nbsp;                     |             |                                                                                                                    |
| **Diagnostics**            |             |                                                                                                                    |
| `blitter`                  | ✓           | Use the BLiTTER where the machine has one (**on** by default; `blitter=false` forces the CPU paths, for diagnosis) |
| `logging`                  | ✓           | Write progress and warnings to `RS.LOG` (errors are always written)                                                |
| `log_fps`                  | ✓           | Log the frame rate to `RS.LOG`, averaged over 64 frames                                                            |
| `bench`                    | ✓           | Benchmark: time 512 gameplay frames, log the result, then **quit**                                                 |

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
- We've got ~25fps on an 8MHz machine, can we hit 30 (the maximum the game supports)?
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
