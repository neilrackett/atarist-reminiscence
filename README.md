# REminiscence — Atari ST

<img src="./flashback.png" alt="Flashback" width="640" height="400" />

Flashback on the Atari ST, by [Neil Rackett](https://neilrackett.com/atarist).

## Introduction

Of all the games that never had an official released on the Atari ST, one
that I was most disappointed about was Flashback (Delphine Software, 1992).

So, using [STDL](https://github.com/neilrackett/atarist-stdl)
(the `stdl/` submodule) and the Amiga data files, I've ported
[REminiscence](https://github.com/cyxx/REminiscence), an open source
re-implementation of the original game engine, to the Atari ST and
just three decades later...

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

| Key             | Action                            |
| --------------- | --------------------------------- |
| Arrow keys      | move Conrad                       |
| Shift           | talk / use / run / shoot          |
| Enter           | use the current inventory object  |
| Backspace / Tab | display inventory                 |
| Any key         | skip the current cutscene         |
| Escape          | display options                   |
| Ctrl S / Ctrl L | save / load game state            |
| Ctrl + / Ctrl - | change game state slot            |
| Ctrl Q          | quit                              |

A joystick in port 1 maps to the arrow keys with fire as Shift.

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
| `log_fps`                  | Log the frame rate to `RS.LOG`, averaged over 64 frames                |
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

The ST-specific ones are `crop_screen`, `skip_intro` and `log_fps`.

`crop_screen` chooses how the game's 224 lines reach the ST's 200:
by default every ninth line is dropped, which keeps the whole
playfield visible but slices through sprites and text. Cropping
instead hides the top and bottom 12 lines — every displayed line
stays intact and the screen copy is one contiguous run, at the cost
of hiding the edges of each room. The Amiga and DOS releases never
had to choose: DOS programmed a ~256x224 VGA mode, and NTSC Amigas
opened a display taller than 200 lines. The ST's 200-line low
resolution is fixed in the Shifter.

## To-do

- Non-STE sound effects
- Non-STE music
- Make it faster (measured ~10.8 fps on a stock 8MHz ST, ~20.4 fps on a
  16MHz Mega STE, level 1).
  Cutscenes already run at the frame delays their scripts ask for.

## Credits

- A massive thank you to [Gregory Montoir for REminiscence](https://github.com/cyxx/REminiscence).
- Delphine Software, obviously, for making another great game.
- Yaz0r, Pixel and gawd for sharing information they gathered on the game.

## More info

If you'd like more information about Flashback:

- [1] http://www.mobygames.com/game/flashback-the-quest-for-identity
- [2] http://en.wikipedia.org/wiki/Flashback:_The_Quest_for_Identity
- [3] http://ramal.free.fr/fb_en.htm
- [4] https://www.exotica.org.uk/wiki/Flashback
