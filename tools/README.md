# Data extraction tools

The Atari ST port uses the Amiga data files. These tools pull them
out of CAPS/SPS `.ipf` disk images of the four Flashback Amiga disks
and lay them out for GEMDOS.

## ipf2adf.c

Decodes an IPF's AmigaDOS MFM tracks into a plain `.adf` sector
image. Links against [capsimg](https://github.com/FrodeSolheim/capsimg):

```
git clone https://github.com/FrodeSolheim/capsimg
(cd capsimg && ./bootstrap && ./configure && make)
cc -O2 -o tools/ipf2adf tools/ipf2adf.c capsimg/capsimg.so
```

Note the CAPS structs are `#pragma pack(1)` — the converter's local
declarations mirror that.

## extract-data.sh

Runs `ipf2adf` on each disk, unpacks the AmigaDOS filesystems with
`xdftool` (from `pip install amitools`), and flattens `data/`,
`cine/` and `font8.spr` into `dist/DATA/` with uppercase 8.3 names
(`replicant.spm` becomes `REPLICAN.SPM`; the ST build asks for that
name).

```
tools/extract-data.sh disks/Flashback_Disk?of4.ipf
```

`dist/` is the Hatari GEMDOS C: drive during development; the game
expects its files in `DATA\` next to `FLASHBAK.TOS`.
