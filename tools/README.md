# Data extraction tools

The Atari ST port uses the Amiga data files. These tools pull them
out of CAPS/SPS `.ipf` disk images of the four Flashback Amiga disks
and lay them out for GEMDOS.

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
