# Extracting the files manually

[← Back to the README](../README.md)

Everything you need is on one or more of the four disks (some may span multiple disks):

| On the disk | What it is                                        | Where it goes                     |
| ----------- | ------------------------------------------------- | --------------------------------- |
| `data/`     | the game itself — levels, sprites, palettes, text | Copy all files into `DATA\`       |
| `cine/`     | the cutscenes                                     | Copy all files into `DATA\`       |
| `font8.spr` | the font, in the root of disk 1                   | Copy the file into `DATA\`        |
| `music/`    | the score, as ProTracker modules (optional)       | (see [Music](../README.md#music)) |

For CAPS/SPS `.IPF` disk images, `tools/extract-data.sh` extracts and renames the
files for you; see [tools/README.md](../tools/README.md) for the dependencies
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
