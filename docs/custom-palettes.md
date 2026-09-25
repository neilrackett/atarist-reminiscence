# Custom palettes

[← Back to the README](../README.md)

The ST shows 16 colours at a time, so for every room the game picks
16 from the 50 or more the Amiga graphics use, favouring the colours
that cover most of the level's rooms and the ones Conrad and the
enemies are drawn in. With
`palette_custom=true` you can supply your own instead: 16 colours in a
`.hex` file in a `PALETTE\` folder next to `FLASHBAK.TOS`, one
`RRGGBB` per line, as Lospec and Aseprite export palettes. Every
colour in the room then uses the nearest of the 16.

For each room the game looks for, in order:

| File        | Used for                                 |
| ----------- | ---------------------------------------- |
| `L1R45.HEX` | level 1, room 45                         |
| `L1.HEX`    | any room of level 1 without its own file |

Level 2 is stored as two parts whose room numbers overlap, so its rooms
are named by part as well: `L2_1R17.HEX` and `L2_2R17.HEX` (`L2.HEX`
still covers the whole level). With neither file, the room keeps the
colours the game chose.

## Choosing which colour goes where (optional)

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

## What the 32 colours are

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

## Making one

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
