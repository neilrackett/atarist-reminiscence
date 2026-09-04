
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Atari ST planar layer primitives (see video_st.h).
 */

#ifdef ATARIST

extern "C" {
#include <stdl/stdl.h>
}

#include "video_st.h"

/*
 * The engine's layers are raw caller-owned blocks (pointer-swapped
 * cutscene pages, single-memcpy restores), wrapped as STDL surfaces
 * on demand so the library primitives can draw into them. The
 * priority plane doubles as the surface mask (bit set = preserved),
 * which is exactly STDL's composition convention.
 */
// One fixed-slot cache of STDL surface headers per layer. The masked
// view composes - sprites consult the priority plane - while the bare
// view presents: a whole-content copy through the masked view would
// colour-key on that plane and drop foreground pixels.
static STDL_Surface *layerView(uint8_t *layer, bool masked) {
	enum { N = 8 };
	static uint8_t *keys[2][N];
	static STDL_Surface *surfs[2][N];
	const int v = masked ? 1 : 0;
	int i = 0;
	for (; i < N && keys[v][i]; ++i) {
		if (keys[v][i] == layer) {
			return surfs[v][i];
		}
	}
	if (i == N) {
		return 0;
	}
	surfs[v][i] = STDL_CreateSurfaceFrom(layer, kSTLayerW, kSTLayerH,
	                                     kSTRowBytes,
	                                     masked ? layer + kSTPlaneBytes : 0,
	                                     masked ? kSTPrioRowBytes : 0);
	keys[v][i] = layer;
	return surfs[v][i];
}

STDL_Surface *ST_layerSurfaceBare(uint8_t *layer) {
	return layerView(layer, false);
}

static inline uint16_t *groupPtr(uint8_t *layer, int x, int y) {
	return (uint16_t *)(layer + y * kSTRowBytes + ((x >> 4) << 3));
}

static inline uint16_t *prioPtr(uint8_t *layer, int x, int y) {
	return (uint16_t *)(layer + kSTPlaneBytes + y * kSTPrioRowBytes + ((x >> 4) << 1));
}

/*
 * Amiga planar sources straight into a layer.
 *
 * Rooms used to be decoded pixel by pixel into a chunky staging
 * buffer and converted to planar once the palettes were known: some
 * 140 cycles a pixel for the conversion alone, over a second a room
 * on an 8MHz ST, on top of a decode that tested every bit of every
 * plane. The Amiga data is already four bitplanes, so with the
 * palettes set first the colour remap can be applied plane to
 * plane. A unit is eight source pixels, one byte from each plane:
 * the four bytes spread into eight nibbles of one long (two lookups
 * a plane), then each byte of that long - a pair of pixels - indexes
 * a table holding the two pixels' remapped bits for all four planes
 * at once. Eight lookups a unit, no chunky byte anywhere.
 *
 * ST_amigaPrepare turns a tile into cells that ST_amigaPlace drops
 * into a layer. A cell is eight bytes, q0 m q1 m q2 m q3 m: the
 * four remapped plane bytes (already masked) interleaved with the
 * mask, laid out so that one movep.l reads the plane bytes and
 * another, one byte along, reads the mask replicated four times.
 * The four plane bytes of a unit sit two bytes apart in the layer
 * too, so an opaque cell is one movep in and one movep out.
 * Preparing and placing are separate because the SGD scenery draws
 * three or four screens' worth of pixels per room from about a
 * hundred distinct tiles: the remap is paid once per tile, the
 * placements cost a couple of moves each.
 */

// plane k byte -> its bits in the nibble lanes, pre-shifted by k
static uint32_t g_spread[4][256];
static uint8_t g_bitrev[256];
static uint32_t g_notRep[256];          // ~b in all four bytes
static bool g_spreadReady;

static void initSpread() {
	for (int b = 0; b < 256; ++b) {
		uint32_t v = 0;
		uint8_t r = 0;
		for (int i = 0; i < 8; ++i) {
			if (b & (1 << (7 - i))) {
				v |= 1u << (i * 4);
				r |= 1 << i;
			}
		}
		for (int k = 0; k < 4; ++k) {
			g_spread[k][b] = v << k;
		}
		g_bitrev[b] = r;
		g_notRep[b] = ~((uint32_t)b * 0x01010101u);
	}
	g_spreadReady = true;
}

// Pair tables: index = left pixel | right pixel << 4 (logical colours
// within a palette slot), value = the two pixels' hardware colour
// bits in the leftmost two bits of each plane byte, planes 0..3 from
// the most significant byte down. Pair q of a unit is the same table
// shifted right 2q. One table per palette base the room decoder
// uses (0x00, 0x10, 0x80, 0x90), rebuilt when the remap moves.
struct PairMap {
	uint16_t gen;
	uint8_t pal;
	uint32_t t[256];
};
static PairMap g_pairMaps[4];

static const uint32_t *pairMap(uint8_t pal) {
	PairMap &m = g_pairMaps[((pal >> 4) & 1) | ((pal >> 6) & 2)];
	ST_getRemap();                      // (lazy) so the generation is current
	if (m.gen == ST_remapGen() && m.pal == pal && m.gen != 0) {
		return m.t;
	}
	uint8_t map16[16];
	ST_buildMap16(pal, map16);
	for (int i = 0; i < 256; ++i) {
		const uint8_t a = map16[i & 15];
		const uint8_t b = map16[i >> 4];
		uint32_t v = 0;
		for (int k = 0; k < 4; ++k) {
			const uint32_t bits = (((a >> k) & 1) << 7) | (((b >> k) & 1) << 6);
			v |= bits << (24 - 8 * k);
		}
		m.t[i] = v;
	}
	m.gen = ST_remapGen();
	m.pal = pal;
	return m.t;
}

// n units of four planes (plane k at s0 + k * planeSize, one byte
// per unit) and their mask bytes -> n cells at out (even address:
// empty cells are cleared with long stores)
extern "C" void packCellsAsm(uint8_t *out, const uint8_t *s0, long planeSize,
	const uint8_t *mask, long n, const uint32_t *spread, const uint32_t *pair);

// rows of n cells into the layer: d is the plane byte of the first
// unit, pr its priority byte, odd whether that unit is the right
// byte of its 16-pixel group (plane bytes step 1,7,1,7... along a
// row), prio is 0xFF or 0 for what the priority plane takes where
// the cell is opaque, stride the cell bytes from one row to the
// next (the rows of a clipped block are wider than n)
extern "C" void placeCellsAsm(uint8_t *d, uint8_t *pr, const uint8_t *cells,
	long n, long odd, long prio, long rows, long stride);

// placeCellsAsm for one 8-row column of cells (a room tile): rows
// unrolled, nothing to step
extern "C" void placeTile8Asm(uint8_t *d, uint8_t *pr, const uint8_t *cells, long prio);

// one 8x8 tile (plane k at tile + k * 8) into 32 contiguous bytes
// of the same shape at t plus its eight mask bytes at m: rows are
// read from tile in steps of step (-1 from tile + 7 flips it
// vertically), bytes go through bitrev when it is set (a horizontal
// flip), and the mask is the OR of the planes, or 0xFF when
// opaque is 0xFF
extern "C" void gatherTile8Asm(uint8_t *t, uint8_t *m, const uint8_t *tile,
	long step, const uint8_t *bitrev, long opaque);

#ifdef __m68k__
// packCellsAsm, ~60 instructions a unit. The spread table for plane
// k is at spread + k * 256 longs; the index is a byte times four so
// the adds are done in word arithmetic (index < 4096). The nibble
// long is consumed a byte at a time from the bottom, so the pair
// lookups shift right by 0, 2, 4, 6 in order.
__asm__(
"    .text\n"
"    .even\n"
"_packCellsAsm:\n"
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    move.l 48(%sp),%a0\n"           /* out       */
"    move.l 52(%sp),%a1\n"           /* s0        */
"    move.l 56(%sp),%d4\n"           /* planeSize */
"    move.l 60(%sp),%a2\n"           /* mask      */
"    move.l 64(%sp),%d7\n"           /* n         */
"    move.l 68(%sp),%a3\n"           /* spread    */
"    move.l 72(%sp),%a4\n"           /* pair      */
"    lea    (%a1,%d4.l),%a5\n"       /* s1        */
"    lea    (%a5,%d4.l),%a6\n"       /* s2; s3 = (%a6,%d4.l) */
"    moveq  #0,%d6\n"
"    subq.l #1,%d7\n"
"    bmi.s  pc_done\n"
"pc_loop:\n"
"    move.b (%a2)+,%d3\n"            /* m */
"    bne.s  pc_pack\n"
"    move.l %d6,(%a0)+\n"            /* empty cell: all zero */
"    move.l %d6,(%a0)+\n"
"    addq.l #1,%a1\n"
"    addq.l #1,%a5\n"
"    addq.l #1,%a6\n"
"    dbra   %d7,pc_loop\n"
"pc_done:\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
"pc_pack:\n"
"    moveq  #0,%d1\n"                /* nibbles: plane 0 */
"    move.b (%a1)+,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    move.l (%a3,%d1.w),%d0\n"
"    moveq  #0,%d1\n"                /* plane 1 */
"    move.b (%a5)+,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  #1024,%d1\n"
"    or.l   (%a3,%d1.w),%d0\n"
"    moveq  #0,%d1\n"                /* plane 3 (before s2 moves on) */
"    move.b (%a6,%d4.l),%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  #3072,%d1\n"
"    or.l   (%a3,%d1.w),%d0\n"
"    moveq  #0,%d1\n"                /* plane 2 */
"    move.b (%a6)+,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  #2048,%d1\n"
"    or.l   (%a3,%d1.w),%d0\n"
"    moveq  #0,%d1\n"                /* pair 0 */
"    move.b %d0,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    move.l (%a4,%d1.w),%d2\n"
"    lsr.w  #8,%d0\n"                /* pair 1 */
"    moveq  #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    move.l (%a4,%d1.w),%d5\n"
"    lsr.l  #2,%d5\n"
"    or.l   %d5,%d2\n"
"    swap   %d0\n"                   /* pair 2 */
"    moveq  #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    move.l (%a4,%d1.w),%d5\n"
"    lsr.l  #4,%d5\n"
"    or.l   %d5,%d2\n"
"    lsr.w  #8,%d0\n"                /* pair 3 */
"    moveq  #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    move.l (%a4,%d1.w),%d5\n"
"    lsr.l  #6,%d5\n"
"    or.l   %d5,%d2\n"
"    move.b %d3,%d5\n"               /* mask, four copies */
"    lsl.w  #8,%d5\n"
"    move.b %d3,%d5\n"
"    move.w %d5,%d1\n"
"    swap   %d5\n"
"    move.w %d1,%d5\n"
"    and.l  %d5,%d2\n"
"    movep.l %d2,0(%a0)\n"           /* q0 q1 q2 q3 at 0 2 4 6 */
"    movep.l %d5,1(%a0)\n"           /* m at 1 3 5 7 */
"    addq.l #8,%a0\n"
"    dbra   %d7,pc_loop\n"
"    bra    pc_done\n"
);

// placeCellsAsm: an opaque cell is a movep in and a movep out plus a
// byte for the priority plane; a partial one merges under the mask.
// The destination advances 1 then 7 bytes alternately, flipped with
// an eor. The row loop is in here too: a call per row cost more
// than the row's cells for the 8x8 tiles.
__asm__(
"    .text\n"
"    .even\n"
"_placeCellsAsm:\n"
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    move.l 48(%sp),%a3\n"           /* d, row start */
"    move.l 52(%sp),%a1\n"           /* pr     */
"    move.l 56(%sp),%a0\n"           /* cells  */
"    move.l 60(%sp),%d7\n"           /* n      */
"    move.l 64(%sp),%d5\n"           /* odd    */
"    move.l 68(%sp),%d3\n"           /* prio   */
"    move.l 72(%sp),%d6\n"           /* rows   */
"    move.l 76(%sp),%d0\n"           /* stride */
"    subq.l #1,%d6\n"
"    bmi.s  pl_done\n"
"    subq.l #1,%d7\n"
"    bmi.s  pl_done\n"
"    move.l %d7,%a4\n"               /* n - 1 for each row */
"    neg.w  %d5\n"                   /* odd ? 7 : 1 */
"    and.w  #6,%d5\n"
"    addq.w #1,%d5\n"
"    move.l %d5,%a6\n"
"    move.l %d7,%d1\n"               /* stride - n * 8: cells to the next row */
"    addq.l #1,%d1\n"
"    lsl.l  #3,%d1\n"
"    sub.l  %d1,%d0\n"
"    move.l %d0,%a5\n"
"    moveq  #31,%d1\n"               /* 32 - n: priority bytes to the next row, */
"    sub.w  %d7,%d1\n"               /* kept above the cell counter */
"    swap   %d7\n"
"    move.w %d1,%d7\n"
"    swap   %d7\n"
"pl_row:\n"
"    move.l %a3,%a2\n"
"pl_loop:\n"
"    move.b 1(%a0),%d1\n"            /* m */
"    beq.s  pl_next\n"
"    cmp.b  #-1,%d1\n"
"    bne.s  pl_part\n"
"    movep.l 0(%a0),%d2\n"
"    movep.l %d2,0(%a2)\n"
"    move.b %d3,(%a1)\n"
"pl_next:\n"
"    addq.l #1,%a1\n"
"    addq.l #8,%a0\n"
"    adda.w %d5,%a2\n"
"    eori.w #6,%d5\n"
"    dbra   %d7,pl_loop\n"
"    lea    128(%a3),%a3\n"          /* next row */
"    adda.l %a5,%a0\n"
"    swap   %d7\n"
"    adda.w %d7,%a1\n"
"    swap   %d7\n"
"    move.w %a6,%d5\n"
"    move.w %a4,%d7\n"
"    dbra   %d6,pl_row\n"
"pl_done:\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
"pl_part:\n"
"    movep.l 1(%a0),%d4\n"           /* ~m ~m ~m ~m */
"    not.l  %d4\n"
"    movep.l 0(%a2),%d0\n"
"    and.l  %d4,%d0\n"
"    movep.l 0(%a0),%d2\n"
"    or.l   %d2,%d0\n"
"    movep.l %d0,0(%a2)\n"
"    move.b (%a1),%d0\n"             /* priority: clear under m, then set if prio */
"    and.b  %d4,%d0\n"
"    and.b  %d3,%d1\n"
"    or.b   %d1,%d0\n"
"    move.b %d0,(%a1)\n"
"    bra.s  pl_next\n"
);

// placeTile8Asm: the cell at K, its plane bytes at D, its priority
// byte at P, all constant offsets
#define TILE_ROW(K, D, P) \
"    move.b " K "+1(%a0),%d1\n" \
"    beq.s  1f\n" \
"    cmp.b  #-1,%d1\n" \
"    bne.s  2f\n" \
"    movep.l " K "(%a0),%d2\n" \
"    movep.l %d2," D "(%a1)\n" \
"    move.b %d3," P "(%a2)\n" \
"    bra.s  1f\n" \
"2:\n" \
"    movep.l " K "+1(%a0),%d4\n" \
"    not.l  %d4\n" \
"    movep.l " D "(%a1),%d0\n" \
"    and.l  %d4,%d0\n" \
"    movep.l " K "(%a0),%d2\n" \
"    or.l   %d2,%d0\n" \
"    movep.l %d0," D "(%a1)\n" \
"    move.b " P "(%a2),%d0\n" \
"    and.b  %d4,%d0\n" \
"    and.b  %d3,%d1\n" \
"    or.b   %d1,%d0\n" \
"    move.b %d0," P "(%a2)\n" \
"1:\n"

__asm__(
"    .text\n"
"    .even\n"
"_placeTile8Asm:\n"
"    movem.l %d2-%d4/%a2,-(%sp)\n"
"    move.l 20(%sp),%a1\n"           /* d      */
"    move.l 24(%sp),%a2\n"           /* pr     */
"    move.l 28(%sp),%a0\n"           /* cells  */
"    move.l 32(%sp),%d3\n"           /* prio   */
TILE_ROW("0","0","0")
TILE_ROW("8","128","32")
TILE_ROW("16","256","64")
TILE_ROW("24","384","96")
TILE_ROW("32","512","128")
TILE_ROW("40","640","160")
TILE_ROW("48","768","192")
TILE_ROW("56","896","224")
"    movem.l (%sp)+,%d2-%d4/%a2\n"
"    rts\n"
);
#undef TILE_ROW

// gatherTile8Asm: a copy loop with the flips folded in; the C loop
// it replaces cost more than the packing of the tile
__asm__(
"    .text\n"
"    .even\n"
"_gatherTile8Asm:\n"
"    movem.l %d2-%d5/%a2-%a3,-(%sp)\n"
"    move.l 28(%sp),%a0\n"           /* t      */
"    move.l 32(%sp),%a1\n"           /* m      */
"    move.l 36(%sp),%a2\n"           /* tile   */
"    move.l 40(%sp),%d4\n"           /* step   */
"    move.l 44(%sp),%a3\n"           /* bitrev */
"    move.l 48(%sp),%d5\n"           /* opaque */
"    moveq  #7,%d3\n"
"    move.l %a3,%d0\n"
"    bne.s  gt_flip\n"
"gt_loop:\n"
"    move.b (%a2),%d0\n"
"    move.b 8(%a2),%d1\n"
"    move.b 16(%a2),%d2\n"
"    move.b %d0,(%a0)\n"
"    move.b %d1,8(%a0)\n"
"    move.b %d2,16(%a0)\n"
"    or.b   %d1,%d0\n"
"    or.b   %d2,%d0\n"
"    move.b 24(%a2),%d1\n"
"    move.b %d1,24(%a0)\n"
"    or.b   %d1,%d0\n"
"    or.b   %d5,%d0\n"
"    move.b %d0,(%a1)+\n"
"    addq.l #1,%a0\n"
"    adda.l %d4,%a2\n"
"    dbra   %d3,gt_loop\n"
"    movem.l (%sp)+,%d2-%d5/%a2-%a3\n"
"    rts\n"
"gt_flip:\n"
"    moveq  #0,%d0\n"
"    moveq  #0,%d1\n"
"    moveq  #0,%d2\n"
"    move.b (%a2),%d0\n"
"    move.b (%a3,%d0.w),%d0\n"
"    move.b 8(%a2),%d1\n"
"    move.b (%a3,%d1.w),%d1\n"
"    move.b 16(%a2),%d2\n"
"    move.b (%a3,%d2.w),%d2\n"
"    move.b %d0,(%a0)\n"
"    move.b %d1,8(%a0)\n"
"    move.b %d2,16(%a0)\n"
"    or.b   %d1,%d0\n"
"    or.b   %d2,%d0\n"
"    moveq  #0,%d1\n"
"    move.b 24(%a2),%d1\n"
"    move.b (%a3,%d1.w),%d1\n"
"    move.b %d1,24(%a0)\n"
"    or.b   %d1,%d0\n"
"    or.b   %d5,%d0\n"
"    move.b %d0,(%a1)+\n"
"    addq.l #1,%a0\n"
"    adda.l %d4,%a2\n"
"    dbra   %d3,gt_flip\n"
"    movem.l (%sp)+,%d2-%d5/%a2-%a3\n"
"    rts\n"
);
#else
// C twins of the kernels, byte-identical semantics (the reference)
extern "C" void packCellsAsm(uint8_t *out, const uint8_t *s0, long planeSize,
		const uint8_t *mask, long n, const uint32_t *spread, const uint32_t *pair) {
	const uint8_t *s1 = s0 + planeSize;
	const uint8_t *s2 = s1 + planeSize;
	const uint8_t *s3 = s2 + planeSize;
	for (; --n >= 0; out += 8) {
		const uint8_t m = *mask++;
		if (m == 0) {
			memset(out, 0, 8);
			++s0; ++s1; ++s2; ++s3;
			continue;
		}
		const uint32_t nib = spread[*s0++] | spread[256 + *s1++] | spread[512 + *s2++] | spread[768 + *s3++];
		const uint32_t q = pair[nib & 0xFF]
			| (pair[(nib >> 8) & 0xFF] >> 2)
			| (pair[(nib >> 16) & 0xFF] >> 4)
			| (pair[nib >> 24] >> 6);
		out[0] = (uint8_t)(q >> 24) & m;
		out[2] = (uint8_t)(q >> 16) & m;
		out[4] = (uint8_t)(q >> 8) & m;
		out[6] = (uint8_t)q & m;
		out[1] = out[3] = out[5] = out[7] = m;
	}
}

extern "C" void placeCellsAsm(uint8_t *d, uint8_t *pr, const uint8_t *cells,
		long n, long odd, long prio, long rows, long stride) {
	for (; --rows >= 0; cells += stride, pr += kSTPrioRowBytes, d += kSTRowBytes) {
		const uint8_t *c = cells;
		uint8_t *p = pr, *q = d;
		int step = odd ? 7 : 1;
		for (long i = n; --i >= 0; c += 8, ++p, q += step, step ^= 6) {
			const uint8_t m = c[1];
			if (m == 0) {
				continue;
			}
			if (m == 0xFF) {
				q[0] = c[0];
				q[2] = c[2];
				q[4] = c[4];
				q[6] = c[6];
				*p = (uint8_t)prio;
				continue;
			}
			const uint8_t nm = (uint8_t)~m;
			q[0] = (q[0] & nm) | c[0];
			q[2] = (q[2] & nm) | c[2];
			q[4] = (q[4] & nm) | c[4];
			q[6] = (q[6] & nm) | c[6];
			*p = (*p & nm) | (m & (uint8_t)prio);
		}
	}
}

extern "C" void placeTile8Asm(uint8_t *d, uint8_t *pr, const uint8_t *cells, long prio) {
	placeCellsAsm(d, pr, cells, 1, 0, prio, 8, 8);
}

extern "C" void gatherTile8Asm(uint8_t *t, uint8_t *m, const uint8_t *tile,
		long step, const uint8_t *bitrev, long opaque) {
	for (int y = 0; y < 8; ++y, tile += step) {
		uint8_t p0 = tile[0], p1 = tile[8], p2 = tile[16], p3 = tile[24];
		if (bitrev) {
			p0 = bitrev[p0];
			p1 = bitrev[p1];
			p2 = bitrev[p2];
			p3 = bitrev[p3];
		}
		t[y] = p0;
		t[8 + y] = p1;
		t[16 + y] = p2;
		t[24 + y] = p3;
		m[y] = (uint8_t)(p0 | p1 | p2 | p3 | opaque);
	}
}
#endif

void ST_amigaPrepare(uint8_t *out, const uint8_t *planes, int planeSize, int units, int rows, const uint8_t *mask, uint8_t pal) {
	if (!g_spreadReady) {
		initSpread();
	}
	packCellsAsm(out, planes, planeSize, mask, (long)units * rows, g_spread[0], pairMap(pal));
}

void ST_amigaTile8(uint8_t *out, const uint8_t *tile, uint8_t pal, bool xflip, bool yflip, bool keyZero) {
	if (!g_spreadReady) {
		initSpread();
	}
	// the flips are applied on the way into a 32-byte copy, which
	// the kernel then reads like any other four-plane source
	uint8_t t[32], m[8];
	gatherTile8Asm(t, m, yflip ? tile + 7 : tile, yflip ? -1 : 1,
		xflip ? g_bitrev : 0, keyZero ? 0 : 0xFF);
	packCellsAsm(out, t, 8, m, 8, g_spread[0], pairMap(pal));
}

void ST_amigaPlaceTile8(uint8_t *layer, int x, int y, const uint8_t *prep, bool setPrio) {
	const int bx = x >> 3;
	placeTile8Asm(layer + y * kSTRowBytes + ((bx >> 1) << 3) + (bx & 1),
		layer + kSTPlaneBytes + y * kSTPrioRowBytes + bx,
		prep, setPrio ? 0xFF : 0);
}

void ST_amigaPlace(uint8_t *layer, int x, int y, const uint8_t *prep, int units, int rows, bool setPrio) {
	int r0 = 0, r1 = rows;
	if (y < 0) {
		r0 = -y;
	}
	if (y + rows > kSTLayerH) {
		r1 = kSTLayerH - y;
	}
	if (r0 >= r1) {
		return;
	}
	const int stride = units * 8;
	const long prio = setPrio ? 0xFF : 0;
	if ((x & 7) == 0) {
		// byte-aligned: every unit lands in one plane byte per plane
		int u0 = 0, u1 = units;
		if (x < 0) {
			u0 = -x >> 3;
		}
		if (x + units * 8 > kSTLayerW) {
			u1 = (kSTLayerW - x) >> 3;
		}
		if (u0 >= u1) {
			return;
		}
		const int bx0 = (x >> 3) + u0;
		const uint8_t *c = prep + r0 * stride + u0 * 8;
		uint8_t *d = layer + (y + r0) * kSTRowBytes + ((bx0 >> 1) << 3) + (bx0 & 1);
		uint8_t *pr = layer + kSTPlaneBytes + (y + r0) * kSTPrioRowBytes + bx0;
		placeCellsAsm(d, pr, c, u1 - u0, bx0 & 1, prio, r1 - r0, stride);
		return;
	}
	// Unaligned (never seen in the shipped rooms, kept for safety):
	// each unit straddles two plane bytes, so shift its cell into a
	// 16-bit window and place the two halves separately.
	const int s = x & 7;
	const int bxBase = x >> 3;              // arithmetic: x may be negative
	for (int r = r0; r < r1; ++r) {
		const uint8_t *c = prep + r * stride;
		uint8_t *row = layer + (y + r) * kSTRowBytes;
		uint8_t *prRow = layer + kSTPlaneBytes + (y + r) * kSTPrioRowBytes;
		for (int u = 0; u < units; ++u, c += 8) {
			if (c[1] == 0) {
				continue;
			}
			uint8_t half[8];
			for (int side = 0; side < 2; ++side) {
				const int bx = bxBase + u + side;
				if (bx < 0 || bx >= kSTLayerW / 8) {
					continue;
				}
				for (int k = 0; k < 8; ++k) {
					half[k] = side == 0 ? (uint8_t)(c[k] >> s) : (uint8_t)(c[k] << (8 - s));
				}
				if (half[1]) {
					placeCellsAsm(row + ((bx >> 1) << 3) + (bx & 1), prRow + bx, half, 1, bx & 1, prio, 1, 8);
				}
			}
		}
	}
}

/*
 * Front-to-back placement. The SGD scenery paints a room three or
 * four times over: the list is back to front, and most of what an
 * early tile draws is under a later one. Walking the list the other
 * way with a coverage byte per unit (the layer's priority plane,
 * zero at the time and cleared again afterwards) lets each cell
 * write only the pixels nothing has claimed yet, and skip
 * altogether once its unit is full. The layer starts black, so a
 * unit no cell has touched takes its first cell with one movep,
 * and later cells OR in under the inverse of the coverage. The
 * chunky room's colour 0 was a real colour, so a closing pass
 * (ST_amigaPlaceCovEnd) paints the background into whatever no
 * cell reached, and gives the priority plane back.
 */

// as placeCellsAsm with cov the coverage byte of the first unit and
// notRep the ~byte-times-four table
extern "C" void placeCellsCovAsm(uint8_t *d, uint8_t *cov, const uint8_t *cells,
	long n, long odd, const uint32_t *notRep, long rows, long stride);

#ifdef __m68k__
// One cell: D is the plane byte offset from a2, C the coverage byte
// offset from a1, K the cell offset from a0. Four cells at a time
// in the main loop, so the destination step is a constant offset
// rather than the alternating add of placeCellsAsm, and a long
// compare of their coverage bytes skips the lot once they are full
// - which, drawing front to back, most of the cells of most of the
// tiles are.
#define COV_CELL(D, C, K) \
"    move.b " K "+1(%a0),%d1\n" \
"    beq.s  1f\n" \
"    moveq  #0,%d0\n" \
"    move.b " C "(%a1),%d0\n" \
"    bne.s  2f\n" \
"    movep.l " K "(%a0),%d2\n" \
"    movep.l %d2," D "(%a2)\n" \
"    move.b %d1," C "(%a1)\n" \
"    bra.s  1f\n" \
"2:  move.b %d0,%d3\n" \
"    not.b  %d3\n" \
"    and.b  %d1,%d3\n" \
"    beq.s  1f\n" \
"    or.b   %d1," C "(%a1)\n" \
"    add.w  %d0,%d0\n" \
"    add.w  %d0,%d0\n" \
"    move.l (%a3,%d0.w),%d4\n" \
"    movep.l " K "(%a0),%d2\n" \
"    and.l  %d4,%d2\n" \
"    movep.l " D "(%a2),%d0\n" \
"    or.l   %d2,%d0\n" \
"    movep.l %d0," D "(%a2)\n" \
"1:\n"

__asm__(
"    .text\n"
"    .even\n"
"_placeCellsCovAsm:\n"
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    move.l 48(%sp),%a4\n"           /* d, row start */
"    move.l 52(%sp),%a1\n"           /* cov    */
"    move.l 56(%sp),%a0\n"           /* cells  */
"    move.l 60(%sp),%d6\n"           /* n      */
"    move.l 64(%sp),%d5\n"           /* odd    */
"    move.l 68(%sp),%a3\n"           /* notRep */
"    tst.l  %d6\n"
"    ble    pcv_done\n"
"    tst.l  72(%sp)\n"               /* rows, counted down in place */
"    ble    pcv_done\n"
"    moveq  #32,%d0\n"               /* 32 - n: coverage bytes to the next row */
"    sub.l  %d6,%d0\n"
"    move.l %d0,%a6\n"
"    move.l 76(%sp),%d0\n"           /* stride - n * 8: cells to the next row */
"    move.l %d6,%d1\n"
"    lsl.l  #3,%d1\n"
"    sub.l  %d1,%d0\n"
"    move.l %d0,%a5\n"
"pcv_row:\n"
"    move.l %a4,%a2\n"
"    move.l 60(%sp),%d6\n"
"    tst.l  %d5\n"
"    beq    pcv_even\n"
COV_CELL("0", "0", "0")              /* lone right-hand unit first (d already at its byte) */
"    addq.l #7,%a2\n"
"    addq.l #1,%a1\n"
"    addq.l #8,%a0\n"
"    subq.l #1,%d6\n"
"    beq    pcv_rowend\n"
"pcv_even:\n"
"    move.w %d6,%d7\n"               /* the pair and cell left over */
"    lsr.l  #2,%d6\n"                /* quads */
"    beq    pcv_rest\n"
"    subq.l #1,%d6\n"
"pcv_quad:\n"
"    cmp.l  #-1,(%a1)\n"             /* four full units: nothing left to paint */
"    beq    3f\n"
COV_CELL("0", "0", "0")
COV_CELL("1", "1", "8")
COV_CELL("8", "2", "16")
COV_CELL("9", "3", "24")
"3:  lea    16(%a2),%a2\n"
"    addq.l #4,%a1\n"
"    lea    32(%a0),%a0\n"
"    dbra   %d6,pcv_quad\n"
"pcv_rest:\n"
"    btst   #1,%d7\n"
"    beq    pcv_tail\n"
"    cmp.w  #-1,(%a1)\n"
"    beq    4f\n"
COV_CELL("0", "0", "0")
COV_CELL("1", "1", "8")
"4:  addq.l #8,%a2\n"
"    addq.l #2,%a1\n"
"    lea    16(%a0),%a0\n"
"pcv_tail:\n"
"    btst   #0,%d7\n"
"    beq.s  pcv_rowend\n"
COV_CELL("0", "0", "0")
"    addq.l #1,%a1\n"
"    addq.l #8,%a0\n"
"pcv_rowend:\n"
"    lea    128(%a4),%a4\n"          /* next row */
"    adda.l %a5,%a0\n"
"    adda.l %a6,%a1\n"
"    subq.l #1,72(%sp)\n"
"    bne    pcv_row\n"
"pcv_done:\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
);
#undef COV_CELL
#else
extern "C" void placeCellsCovAsm(uint8_t *d, uint8_t *cov, const uint8_t *cells,
		long n, long odd, const uint32_t *notRep, long rows, long stride) {
	(void)notRep;
	for (; --rows >= 0; cells += stride, cov += kSTPrioRowBytes, d += kSTRowBytes) {
		const uint8_t *c = cells;
		uint8_t *p = cov, *q = d;
		int step = odd ? 7 : 1;
		for (long i = n; --i >= 0; c += 8, ++p, q += step, step ^= 6) {
			const uint8_t m = c[1];
			if (m == 0) {
				continue;
			}
			const uint8_t have = *p;
			if (have == 0) {
				q[0] = c[0];
				q[2] = c[2];
				q[4] = c[4];
				q[6] = c[6];
				*p = m;
				continue;
			}
			if ((m & ~have) == 0) {
				continue;
			}
			*p |= m;
			const uint8_t nc = (uint8_t)~have;
			q[0] |= c[0] & nc;
			q[2] |= c[2] & nc;
			q[4] |= c[4] & nc;
			q[6] |= c[6] & nc;
		}
	}
}
#endif

void ST_amigaPlaceCov(uint8_t *layer, int x, int y, const uint8_t *prep, int units, int rows) {
	if (!g_spreadReady) {
		initSpread();
	}
	int r0 = 0, r1 = rows;
	if (y < 0) {
		r0 = -y;
	}
	if (y + rows > kSTLayerH) {
		r1 = kSTLayerH - y;
	}
	if (r0 >= r1) {
		return;
	}
	const int stride = units * 8;
	if ((x & 7) == 0) {
		int u0 = 0, u1 = units;
		if (x < 0) {
			u0 = -x >> 3;
		}
		if (x + units * 8 > kSTLayerW) {
			u1 = (kSTLayerW - x) >> 3;
		}
		if (u0 >= u1) {
			return;
		}
		const int bx0 = (x >> 3) + u0;
		const uint8_t *c = prep + r0 * stride + u0 * 8;
		uint8_t *d = layer + (y + r0) * kSTRowBytes + ((bx0 >> 1) << 3) + (bx0 & 1);
		uint8_t *cov = layer + kSTPlaneBytes + (y + r0) * kSTPrioRowBytes + bx0;
		placeCellsCovAsm(d, cov, c, u1 - u0, bx0 & 1, g_notRep, r1 - r0, stride);
		return;
	}
	// unaligned, as in ST_amigaPlace
	const int s = x & 7;
	const int bxBase = x >> 3;
	for (int r = r0; r < r1; ++r) {
		const uint8_t *c = prep + r * stride;
		uint8_t *row = layer + (y + r) * kSTRowBytes;
		uint8_t *covRow = layer + kSTPlaneBytes + (y + r) * kSTPrioRowBytes;
		for (int u = 0; u < units; ++u, c += 8) {
			if (c[1] == 0) {
				continue;
			}
			uint8_t half[8];
			for (int side = 0; side < 2; ++side) {
				const int bx = bxBase + u + side;
				if (bx < 0 || bx >= kSTLayerW / 8) {
					continue;
				}
				for (int k = 0; k < 8; ++k) {
					half[k] = side == 0 ? (uint8_t)(c[k] >> s) : (uint8_t)(c[k] << (8 - s));
				}
				if (half[1]) {
					placeCellsCovAsm(row + ((bx >> 1) << 3) + (bx & 1), covRow + bx, half, 1, bx & 1, g_notRep, 1, 8);
				}
			}
		}
	}
}

// the background into every pixel the coverage plane has clear: bg
// is the four plane bytes of the colour as a movep long (plane 0
// first), notRep as for placeCellsCovAsm
extern "C" void fillUncoveredAsm(uint8_t *layer, const uint32_t *notRep, uint32_t bg);

#ifdef __m68k__
// an untouched unit takes the pattern with one movep; a full one is
// a byte compare; a partial one ORs the pattern in under ~coverage
#define UNC_UNIT(D) \
"    moveq  #0,%d0\n" \
"    move.b (%a1)+,%d0\n" \
"    beq.s  2f\n" \
"    cmp.b  #-1,%d0\n" \
"    beq.s  1f\n" \
"    add.w  %d0,%d0\n" \
"    add.w  %d0,%d0\n" \
"    move.l (%a3,%d0.w),%d1\n" \
"    and.l  %d5,%d1\n" \
"    movep.l " D "(%a0),%d2\n" \
"    or.l   %d1,%d2\n" \
"    movep.l %d2," D "(%a0)\n" \
"    bra.s  1f\n" \
"2:  movep.l %d5," D "(%a0)\n" \
"1:\n"

__asm__(
"    .text\n"
"    .even\n"
"_fillUncoveredAsm:\n"
"    movem.l %d2-%d5/%a2-%a3,-(%sp)\n"
"    move.l 28(%sp),%a0\n"           /* layer  */
"    move.l 32(%sp),%a3\n"           /* notRep */
"    move.l 36(%sp),%d5\n"           /* bg     */
"    lea    28672(%a0),%a1\n"        /* coverage plane (kSTPlaneBytes) */
"    move.w #223,%d4\n"              /* kSTLayerH - 1 */
"unc_row:\n"
"    moveq  #15,%d3\n"               /* 16 groups of two units */
"unc_group:\n"
UNC_UNIT("0")
UNC_UNIT("1")
"    addq.l #8,%a0\n"
"    dbra   %d3,unc_group\n"
"    dbra   %d4,unc_row\n"
"    movem.l (%sp)+,%d2-%d5/%a2-%a3\n"
"    rts\n"
);
#undef UNC_UNIT
#else
extern "C" void fillUncoveredAsm(uint8_t *layer, const uint32_t *notRep, uint32_t bg) {
	uint8_t *cov = layer + kSTPlaneBytes;
	for (int y = 0; y < kSTLayerH; ++y, layer += kSTRowBytes) {
		for (int g = 0; g < 16; ++g) {
			for (int side = 0; side < 2; ++side) {
				const uint8_t c = *cov++;
				if (c == 0xFF) {
					continue;
				}
				uint8_t *d = layer + g * 8 + side;
				const uint8_t nc = (uint8_t)~c;
				d[0] |= (uint8_t)(bg >> 24) & nc;
				d[2] |= (uint8_t)(bg >> 16) & nc;
				d[4] |= (uint8_t)(bg >> 8) & nc;
				d[6] |= (uint8_t)bg & nc;
			}
		}
		(void)notRep;
	}
}
#endif

void ST_amigaPlaceCovEnd(uint8_t *layer, uint8_t colour8) {
	if (!g_spreadReady) {
		initSpread();
	}
	const uint8_t v = ST_getRemap()[colour8];
	if (v != 0) {
		uint32_t bg = 0;
		for (int k = 0; k < 4; ++k) {
			if (v & (1 << k)) {
				bg |= 0xFFu << (24 - 8 * k);
			}
		}
		fillUncoveredAsm(layer, g_notRep, bg);
	}
	memset(layer + kSTPlaneBytes, 0, kSTPrioRowBytes * kSTLayerH);
}

void ST_drawSprite(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, int w, int h, const uint8_t *map16, unsigned flags, bool setPrio) {
	STDL_Surface *s = layerView(layer, true);
	if (!s) {
		return;
	}
	unsigned f = 0;
	if (flags & kSTSpriteXflip) {
		f |= STDL_I8_XFLIP;
	}
	if (flags & kSTSpriteColMajor) {
		f |= STDL_I8_COLMAJOR;
	}
	if (flags & kSTSpriteRespectPrio) {
		f |= STDL_I8_UNDER;
	}
	if (setPrio) {
		f |= STDL_I8_MARK;
	}
	STDL_BlitIndexed8(s, src, pitch, x, y, w, h, map16, f);
}

// --- planar sprite cache -------------------------------------------
//
// Drawing from chunky costs a per-pixel gather every frame: the
// source byte is mapped, then scattered a bit at a time into four
// plane words. The same frame, colour bank, flags and palette always
// produce the same plane words, so bake them once and afterwards
// just move words into the layer.
//
// The bake goes through STDL_BlitIndexed8 into a scratch surface,
// which already owns the flip, column-major and palette-map rules -
// there is no second copy of that logic here. STDL_I8_MARK gives the
// scratch mask a set bit under every drawn pixel, which is exactly
// the "opaque" mask the blit below wants.

enum { kSprSlots = 64, kSprMaxW = 128, kSprMaxH = 160 };   // power of two: direct-mapped

struct SprEntry {
	const uint8_t *src;
	uint16_t gen;
	uint8_t colMask;
	uint8_t flags;
	uint8_t w, h;
	int groups;
	uint16_t *block;        // allocation base (guard words first)
	uint16_t *planes;       // groups*4 words per row
	uint16_t *mask;         // groups words per row
	int cap;                // usable words at planes
};

static SprEntry g_spr[kSprSlots];

// Bake on the second sighting (see below): remembers sources seen
// once, so a bake is only spent on frames that repeat.
enum { kSeen = 48 };
static const uint8_t *g_seenSrc[kSeen];
static uint8_t g_seenMask[kSeen];
static int g_seenPos;

// [g_bakeLo, g_bakeHi]: bounds of every source address the bake and
// seen tables have referenced. Most recycled slabs never fed a bake,
// so the common invalidation is two compares instead of a 112-entry
// scan. Bounds only grow; the full flush resets them.
static const uint8_t *g_bakeLo = (const uint8_t *)~(uintptr_t)0;
static const uint8_t *g_bakeHi;

static inline void bakeBoundsAdd(const uint8_t *p) {
	if (p < g_bakeLo) g_bakeLo = p;
	if (p > g_bakeHi) g_bakeHi = p;
}

void ST_flushSpriteCache() {
	for (int i = 0; i < kSprSlots; ++i) {
		g_spr[i].src = 0;
	}
	for (int i = 0; i < kSeen; ++i) {
		g_seenSrc[i] = 0;
	}
	g_bakeLo = (const uint8_t *)~(uintptr_t)0;
	g_bakeHi = 0;
}

// The planar cache keys on source pointers, and everything those
// pointers reach through gets recycled under it: the decode caches
// reuse their slabs, and the resource bank arena resets wholesale
// when it fills mid-game (Resource::clearBankData). The same
// address then holds a different sprite, so each recycler reports
// its range here and the bakes keyed inside it die with the old
// content - without this, a stale bake keeps being drawn (seen as
// confetti fireflies after a room entry, when the bank arena
// wrapped between a fly's two sightings). Ranges, not single
// pointers: draw calls pass clip- and mirror-adjusted pointers
// into the middle of these buffers.
void ST_invalidateBakedRange(const uint8_t *p, uint32_t len) {
	if (p > g_bakeHi || p + len <= g_bakeLo) {
		return;
	}
	for (int i = 0; i < kSprSlots; ++i) {
		if ((uint32_t)(g_spr[i].src - p) < len) {
			g_spr[i].src = 0;
		}
	}
	for (int i = 0; i < kSeen; ++i) {
		if ((uint32_t)(g_seenSrc[i] - p) < len) {
			g_seenSrc[i] = 0;
		}
	}
}

static bool bakeSprite(SprEntry *e, const uint8_t *src, int pitch, int w, int h, const uint8_t *map16, unsigned f) {
	const int groups = (w + 15) >> 4;
	const int planeWords = groups * 4 * h;
	const int words = planeWords + groups * h;
	// The unaligned blit path reads one group either side of the
	// rows, so the block carries guard words at both ends: borrowed
	// blocks owe the same slack library surfaces have (see
	// STDL_CreateSurfaceFrom). A leading read is masked off by the
	// edge mask, but the contract asks for the room regardless.
	enum { kGuard = 4 };
	if (e->cap < words) {
		// grow only: a slot large enough for one frame is large
		// enough for the next one that lands in it, so after warm-up
		// a miss costs no allocation at all
		free(e->block);
		e->block = (uint16_t *)malloc((words + 2 * kGuard) * sizeof(uint16_t));
		e->cap = e->block ? words : 0;
	}
	if (!e->block) {
		e->planes = 0;
		return false;
	}
	e->planes = e->block + kGuard;
	e->mask = e->planes + planeWords;
	e->groups = groups;
	// STDL's source-mask convention is bit set = destination
	// preserved, so the mask starts opaque-everywhere and
	// BlitIndexed8's default maintenance clears a bit under each
	// pixel it draws.
	memset(e->mask, 0xFF, groups * h * sizeof(uint16_t));

	// One scratch header, reused: STDL_CreateSurfaceFrom per bake was
	// a malloc/free pair on the miss path.
	static STDL_Surface *scratch;
	if (!scratch) {
		scratch = STDL_CreateSurfaceFrom((uint8_t *)e->planes,
			groups * 16, h, groups * 8, (uint8_t *)e->mask, groups * 2);
		if (!scratch) {
			return false;
		}
	}
	scratch->pixels = (uint8_t *)e->planes;
	scratch->w = (int16_t)(groups * 16);
	scratch->h = (int16_t)h;
	scratch->stride = (uint16_t)(groups * 8);
	scratch->mask = (uint8_t *)e->mask;
	scratch->maskstride = (uint16_t)(groups * 2);
	scratch->clip.x = 0;
	scratch->clip.y = 0;
	scratch->clip.w = (uint16_t)(groups * 16);
	scratch->clip.h = (uint16_t)h;
	STDL_BlitIndexed8(scratch, src, pitch, 0, 0, w, h, map16, f);
	memset(e->block, 0, kGuard * sizeof(uint16_t));
	memset(e->planes + words, 0, kGuard * sizeof(uint16_t));
	return true;
}

// Present one baked frame through STDL. The layer's mask is the
// game's priority plane, so UNDER passes the sprite behind marked
// foreground and MARK claims it - the same semantics the chunky path
// gets from STDL_BlitIndexed8.
static void blitBaked(uint8_t *layer, const SprEntry *e, int x, int y, bool respectPrio, bool setPrio) {
	STDL_Surface *dst = layerView(layer, true);
	if (!dst) {
		return;
	}
	static STDL_Surface *view;
	if (!view) {
		view = STDL_CreateSurfaceFrom((uint8_t *)e->planes,
			e->groups * 16, e->h, e->groups * 8,
			(uint8_t *)e->mask, e->groups * 2);
		if (!view) {
			return;
		}
		// STDL_TRANSPARENT: use the bake's mask as-is. A numeric
		// key would rebuild the mask by scanning the plane words,
		// and a bake's fully-masked words are uninitialised - the
		// first entry through here had its mask clobbered from
		// garbage, and that sprite drew as confetti ever after.
		STDL_SetColourKey(view, 1, STDL_TRANSPARENT);
	}
	view->pixels = (uint8_t *)e->planes;
	view->w = (int16_t)(e->groups * 16);
	view->h = (int16_t)e->h;
	view->stride = (uint16_t)(e->groups * 8);
	view->mask = (uint8_t *)e->mask;
	view->maskstride = (uint16_t)(e->groups * 2);
	view->clip.x = 0;
	view->clip.y = 0;
	view->clip.w = (uint16_t)(e->groups * 16);
	view->clip.h = (uint16_t)e->h;

	STDL_Rect sr, dr;
	sr.x = 0;
	sr.y = 0;
	sr.w = (uint16_t)e->w;
	sr.h = (uint16_t)e->h;
	dr.x = (int16_t)x;
	dr.y = (int16_t)y;
	dr.w = (uint16_t)e->w;
	dr.h = (uint16_t)e->h;
	unsigned f = 0;
	if (respectPrio) {
		f |= STDL_BLIT_UNDER;
	}
	if (setPrio) {
		f |= STDL_BLIT_MARK;
	}
	STDL_BlitSurfaceEx(view, &sr, dst, &dr, f);
}

void ST_drawSpriteCached(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, int w, int h, uint8_t colMask, unsigned flags, bool setPrio) {

	if (w <= 0 || h <= 0 || w > kSprMaxW || h > kSprMaxH
	    || x >= kSTLayerW || y >= kSTLayerH || x + w <= 0 || y + h <= 0) {
		if (w > kSprMaxW || h > kSprMaxH) {
			uint8_t map16[16];
			ST_buildMap16(colMask, map16);
			ST_drawSprite(layer, src, pitch, x, y, w, h, map16, flags, setPrio);
		}
		return;
	}
	const unsigned bakeFlags = flags & (kSTSpriteXflip | kSTSpriteColMajor);
	const uint16_t gen = ST_remapGen();
	// direct-mapped: scanning every slot cost more than the blit it
	// was there to save
	const int slot = (int)((((unsigned long)src >> 4) ^ colMask) & (kSprSlots - 1));
	SprEntry *e = &g_spr[slot];
	if (e->src == src && e->gen == gen && e->colMask == colMask
	    && e->flags == (uint8_t)bakeFlags && e->w == (uint8_t)w
	    && e->h == (uint8_t)h) {
	} else {
		e = 0;
	}
	if (!e) {
		// Baking costs about what one chunky draw costs, so a frame
		// drawn once would pay for a bake it never reuses. Bake on the
		// second sighting: one-off frames cost exactly what they did
		// before, repeated ones (every animation cycle) go fast.
		{
			int slot = -1;
			for (int i = 0; i < kSeen; ++i) {
				if (g_seenSrc[i] == src && g_seenMask[i] == colMask) {
					slot = i;
					break;
				}
			}
			if (slot < 0) {
				g_seenSrc[g_seenPos] = src;
				g_seenMask[g_seenPos] = colMask;
				bakeBoundsAdd(src);
				if (++g_seenPos == kSeen) {   // 48 is no power of two:
					g_seenPos = 0;            // % would be a __modsi3
				}
				uint8_t map16[16];
				ST_buildMap16(colMask, map16);
				ST_drawSprite(layer, src, pitch, x, y, w, h, map16, flags, setPrio);
				return;
			}
		}
		e = &g_spr[slot];
		uint8_t map16[16];
		ST_buildMap16(colMask, map16);
		unsigned f = 0;
		if (bakeFlags & kSTSpriteXflip) {
			f |= STDL_I8_XFLIP;
		}
		if (bakeFlags & kSTSpriteColMajor) {
			f |= STDL_I8_COLMAJOR;
		}
		if (!bakeSprite(e, src, pitch, w, h, map16, f)) {
			e->src = 0;
			return;
		}
		e->src = src;
		bakeBoundsAdd(src);
		e->gen = gen;
		e->colMask = colMask;
		e->flags = (uint8_t)bakeFlags;
		e->w = (uint8_t)w;
		e->h = (uint8_t)h;
	}
	blitBaked(layer, e, x, y, (flags & kSTSpriteRespectPrio) != 0, setPrio);
}

void ST_drawGlyph(uint8_t *layer, const uint8_t *src, int x, int y, uint8_t colour8) {
	const uint8_t v = ST_getRemap()[colour8];
	uint8_t map16[16];
	memset(map16, v, sizeof(map16));
	ST_drawSprite(layer, src, 16, x, y, 8, 8, map16, 0, (colour8 & 0x80) != 0);
}

// which pixels of 16-pixel group g lie inside [lo, hi]
static inline uint16_t groupMask(int g, int lo, int hi) {
	uint16_t m = 0xFFFF;
	if ((g << 4) < lo) {
		m >>= (lo & 15);
	}
	const int gend = (g << 4) + 15;
	if (gend > hi) {
		m &= 0xFFFF << (gend - hi);
	}
	return m;
}

// fill one group of a row through a mask (the edge case)
static inline void fillGroupMasked(uint16_t *dst, uint16_t *prio,
		uint16_t m, uint16_t f0, uint16_t f1, uint16_t f2, uint16_t f3,
		bool setPrio) {
	const uint16_t keep = (uint16_t)~m;
	dst[0] = (uint16_t)((dst[0] & keep) | (f0 & m));
	dst[1] = (uint16_t)((dst[1] & keep) | (f1 & m));
	dst[2] = (uint16_t)((dst[2] & keep) | (f2 & m));
	dst[3] = (uint16_t)((dst[3] & keep) | (f3 & m));
	if (setPrio) {
		*prio |= m;
	} else {
		*prio &= keep;
	}
}

// fill helper: writes the constant colour to [x, x+w) of one row.
// The full-group middle run is the hot part of every cutscene
// polygon span; two pattern longs per group beat gcc's word-by-word
// masked stores about 2.5x (this path fills ~2M pixels in the
// intro alone).
static void fillRowPtr(uint16_t *row, uint16_t *prow, int x, int x1,
		uint16_t f0, uint16_t f1, uint16_t f2, uint16_t f3, bool setPrio);

static void fillRow(uint8_t *layer, int x, int w, int y, uint8_t v, bool setPrio) {
	int x1 = x + w - 1;
	uint16_t *dst = groupPtr(layer, x, y);
	uint16_t *prio = prioPtr(layer, x, y);
	const uint16_t f0 = (v & 1) ? 0xFFFF : 0;
	const uint16_t f1 = (v & 2) ? 0xFFFF : 0;
	const uint16_t f2 = (v & 4) ? 0xFFFF : 0;
	const uint16_t f3 = (v & 8) ? 0xFFFF : 0;
	fillRowPtr(dst - (x >> 4) * 4, prio - (x >> 4), x, x1,
		f0, f1, f2, f3, setPrio);
}

#ifdef __m68k__
// The fill core, hand-written: gcc 4.6 compiled the C twin below to
// 203 instructions with 22 stack references, and at ~3100 cycles per
// 15-pixel span it cost more than the whole scan-converter feeding
// it. Full groups are two pattern longs and a priority word; partial
// groups do read-modify-write in memory so nothing spills. Args are
// longs so every stack slot is 4 bytes.
__asm__(
"    .text\n"
"    .even\n"
"_fillRowAsm:\n"
"    movem.l %d2-%d7,-(%sp)\n"
"    move.l 28(%sp),%a0\n"          /* row  */
"    move.l 32(%sp),%a1\n"          /* prow */
"    move.l 36(%sp),%d2\n"          /* x    */
"    move.l 40(%sp),%d3\n"          /* x1   */
"    move.l 44(%sp),%d0\n"          /* f0:f1  */
"    move.l 48(%sp),%d1\n"          /* f2:f3  */
"    move.l 52(%sp),%d5\n"          /* prio word (0 or 0xFFFF) */
"    bsr.s  fra_core\n"
"    movem.l (%sp)+,%d2-%d7\n"
"    rts\n"
"\n"
/* register-level entry: a0=row a1=prow d0=f0:f1 d1=f2:f3 d2=x d3=x1
 * d5=prio word; clobbers d0-d4,d6,d7,a0,a1 */
"fra_core:\n"
"    move.l %d2,%d4\n"
"    asr.l  #4,%d4\n"               /* g0 */
"    move.l %d3,%d6\n"
"    asr.l  #4,%d6\n"               /* g1 */
"    sub.l  %d4,%d6\n"
"    addq.l #1,%d6\n"               /* d6 = group count */
"    move.l %d4,%d7\n"
"    lsl.l  #3,%d7\n"
"    add.l  %d7,%a0\n"              /* dst = row + g0*4 words */
"    add.l  %d4,%d4\n"
"    add.l  %d4,%a1\n"              /* prio = prow + g0 */
"    moveq  #15,%d4\n"
"    and.l  %d2,%d4\n"
"    move.w #-1,%d2\n"
"    lsr.w  %d4,%d2\n"              /* d2 = left mask */
"    moveq  #15,%d4\n"
"    and.l  %d3,%d4\n"
"    moveq  #15,%d3\n"
"    sub.l  %d4,%d3\n"
"    move.w #-1,%d4\n"
"    lsl.w  %d3,%d4\n"              /* d4 = right mask */
"    subq.l #1,%d6\n"
"    bne.s  fra_multi\n"
"    and.w  %d4,%d2\n"              /* one group: m = lm & rm */
"    bra.s  fra_masked\n"           /* its rts returns for us */
"fra_multi:\n"
"    addq.l #1,%d6\n"               /* back to group count */
"    cmp.w  #-1,%d2\n"
"    beq.s  fra_noleft\n"
"    bsr.s  fra_masked\n"
"    subq.l #1,%d6\n"
"fra_noleft:\n"
"    cmp.w  #-1,%d4\n"
"    beq.s  fra_nora\n"
"    subq.l #1,%d6\n"
"fra_nora:\n"
"    tst.l  %d6\n"
"    ble.s  fra_right\n"
"    move.l %d6,%d7\n"
"    subq.l #1,%d7\n"
"fra_fill:\n"
"    move.l %d0,(%a0)+\n"
"    move.l %d1,(%a0)+\n"
"    move.w %d5,(%a1)+\n"
"    dbra   %d7,fra_fill\n"
"fra_right:\n"
"    cmp.w  #-1,%d4\n"
"    beq.s  fra_done\n"
"    move.w %d4,%d2\n"
"    bra.s  fra_masked\n"           /* tail call */
"fra_done:\n"
"    rts\n"
"\n"
/* one group through mask d2: memory RMW, no temps to spill */
"fra_masked:\n"
"    move.w %d2,%d7\n"
"    not.w  %d7\n"
"    move.l %d0,%d3\n"
"    swap   %d3\n"
"    and.w  %d2,%d3\n"
"    and.w  %d7,(%a0)\n"
"    or.w   %d3,(%a0)+\n"
"    move.w %d0,%d3\n"
"    and.w  %d2,%d3\n"
"    and.w  %d7,(%a0)\n"
"    or.w   %d3,(%a0)+\n"
"    move.l %d1,%d3\n"
"    swap   %d3\n"
"    and.w  %d2,%d3\n"
"    and.w  %d7,(%a0)\n"
"    or.w   %d3,(%a0)+\n"
"    move.w %d1,%d3\n"
"    and.w  %d2,%d3\n"
"    and.w  %d7,(%a0)\n"
"    or.w   %d3,(%a0)+\n"
"    tst.w  %d5\n"
"    beq.s  fra_pclr\n"
"    or.w   %d2,(%a1)+\n"
"    rts\n"
"fra_pclr:\n"
"    and.w  %d7,(%a1)+\n"
"    rts\n"
"\n"
/* The polygon rasteriser. rs_run fills count rows between two
 * linear edges; rasterSeg wraps it for the C line path, polyWalk
 * drives it round a whole polygon, stepping the two vertex chains
 * itself - the C walker's per-segment call, state shuffle and
 * multiply cost about as much as the rows it asked for (a cutscene
 * row is under three groups wide on average, so the row overhead
 * is the whole cost and the fill is nearly free). All stepping
 * state lives in the caller's struct, advanced in place: fa/fb, row
 * and prio pointers walk on across segments. Inside, everything is
 * in registers. The edges carry the clip origin (crx) already, so
 * the clip is a packed constant; the end masks come from a table as
 * m:m longs so a partial group is one long op per plane pair. The
 * row body is generated per colour and priority mode (48 variants
 * from one macro): the shared header sorts and clips the span and
 * jumps into the variant, which does the left end group, an
 * unrolled run of full groups and the right end group straight-line
 * - no calls, no tests for a full end mask (an all-ones partial is
 * the same write, and rarer than the tests cost). Priority modes: 0
 * clears, 1 sets, 2 leaves the plane alone (cutscene pages, whose
 * plane nobody reads), 3 is 2 for a polygon that cannot need x
 * clipping. Modes 2 and 3 run on their own core (rs2/rs3) with the
 * freed priority registers holding what mode 0 keeps on the stack.
 * RasterState offsets: fa 0, sa 4, fb 8, sb 12, row 16, prow 20,
 * f01 24, f23 28, pv 32, lim 36 (xlo:xhi, clip-inclusive, crx
 * applied), fn 40 (colour nibble | prio mode << 4, 64 variants).
 * PolyState adds: pts 44, n 48, imax 52, ia 56, ib 60, ya 64, yb 68,
 * y 72, ylast 76, crx 80, recip 84 (n..crx as words in the low half).
 * regs: d0 f01, d1 f23, d5 pv, d6 lim, a2 variant entry,
 * a3 fa, a4 fb, a5 row, a6 prow; rs_run stack: ret, count, sa, sb
 * per row: d2 left mask (m:m), d3 ~m, d4 right mask, d7 full groups,
 * a0 group pointer, a1 priority pointer.
 * Page cores (modes 2, 3): a0 state, a1 group pointer, a6 sb,
 * d5 dbra count, sa read from the state; no stack arguments. */
"    .even\n"
/* load the fill registers from the state in a0 */
".macro RS_LOAD\n"
"    move.l (%a0),%a3\n"             /* fa */
"    move.l 8(%a0),%a4\n"            /* fb */
"    move.l 16(%a0),%a5\n"           /* row  */
"    move.l 20(%a0),%a6\n"           /* prow */
"    move.l 24(%a0),%d0\n"           /* f01  */
"    move.l 28(%a0),%d1\n"           /* f23  */
"    move.l 32(%a0),%d5\n"           /* pv   */
"    move.l 36(%a0),%d6\n"           /* lim  */
"    move.l 40(%a0),%d2\n"           /* fn   */
"    add.w  %d2,%d2\n"
"    lea    rs_mtab(%pc),%a1\n"
"    move.w (%a1,%d2.w),%d2\n"
"    lea    (%a1,%d2.w),%a2\n"
".endm\n"
/* write the walk back to the state in a0 */
/* write the walk back to the state in a0 (prow is not kept: nothing
 * reads it after the last row) */
".macro RS_STORE\n"
"    move.l %a3,(%a0)\n"
"    move.l %a4,8(%a0)\n"
"    move.l %a5,16(%a0)\n"
".endm\n"
"_rasterSeg:\n"                      /* (state, count) */
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    move.l 48(%sp),%a0\n"
"    RS_LOAD\n"
"    move.b 43(%a0),%d2\n"           /* prio mode 2/3: the page core */
"    btst   #5,%d2\n"
"    bne.s  rseg_page\n"
"    move.l 12(%a0),-(%sp)\n"        /* sb */
"    move.l 4(%a0),-(%sp)\n"         /* sa */
"    move.l 60(%sp),-(%sp)\n"        /* count */
"    bsr    rs_run\n"
"    lea    12(%sp),%sp\n"
"    move.l 48(%sp),%a0\n"
"    bra.s  rseg_out\n"
"rseg_page:\n"
"    move.l 12(%a0),%a6\n"           /* sb */
"    move.l 52(%sp),%d5\n"           /* count */
"    subq.w #1,%d5\n"
"    btst   #4,%d2\n"
"    bne.s  1f\n"
"    bsr    rs2_run\n"
"    bra.s  rseg_out\n"
"1:  bsr    rs3_run\n"
"rseg_out:\n"
"    RS_STORE\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
/* edge step: dx in d3, dy in d7 (1..), 16.16 result in d2; a0 = state.
 * dy of 1 and 2 are shifts, up to 255 a 16x16 multiply by the
 * reciprocal table (exact to under 0.005 pixel per row, and every
 * edge change reloads the exact vertex), taller than that a divide
 * at 8.8 (rare - taller than the clip). Clobbers a1. */
".macro RS_STEP\n"
"    cmp.w  #256,%d7\n"
"    bcc.s  7f\n"
"    move.w %d3,%d2\n"
"    swap   %d2\n"
"    clr.w  %d2\n"                   /* dx << 16 */
"    cmp.w  #2,%d7\n"
"    bhi.s  6f\n"
"    bcs.s  8f\n"                    /* dy 1 */
"    asr.l  #1,%d2\n"                /* dy 2 */
"    bra.s  8f\n"
"6:  move.l 84(%a0),%a1\n"
"    add.w  %d7,%d7\n"
"    move.w %d3,%d2\n"
"    muls.w (%a1,%d7.w),%d2\n"
"    bra.s  8f\n"
"7:  move.w %d3,%d2\n"
"    ext.l  %d2\n"
"    lsl.l  #8,%d2\n"
"    divs.w %d7,%d2\n"
"    ext.l  %d2\n"
"    lsl.l  #8,%d2\n"
"8:\n"
".endm\n"
/* advance chain a (backwards through the vertices) or b (forwards)
 * while its edge has ended at or above y (d4): reload the exact
 * vertex and the step for the next edge. a1 = pts. */
".macro RS_CHAIN idx,yend,step,dir\n"
"1:  cmp.w  \\yend(%a0),%d4\n"
"    blt    9f\n"                    /* yend > y: edge still running */
"    move.w \\idx(%a0),%d2\n"
"    cmp.w  54(%a0),%d2\n"           /* imax: chain done */
"    beq.s  9f\n"
"    move.w %d2,%d3\n"
"    .if \\dir < 0\n"
"    subq.w #1,%d3\n"
"    bpl.s  2f\n"
"    move.w 50(%a0),%d3\n"           /* n */
"    subq.w #1,%d3\n"
"    .else\n"
"    addq.w #1,%d3\n"
"    cmp.w  50(%a0),%d3\n"
"    bne.s  2f\n"
"    moveq  #0,%d3\n"
"    .endif\n"
"2:  move.w %d3,\\idx(%a0)\n"
"    move.l 44(%a0),%a1\n"
"    lsl.w  #2,%d2\n"
"    lsl.w  #2,%d3\n"
"    move.w 2(%a1,%d3.w),%d7\n"      /* pts[j].y */
"    move.w %d7,\\yend(%a0)\n"
"    sub.w  2(%a1,%d2.w),%d7\n"      /* dy */
"    move.w (%a1,%d2.w),%d2\n"       /* pts[i].x */
"    move.w (%a1,%d3.w),%d3\n"
"    sub.w  %d2,%d3\n"               /* dx */
"    add.w  82(%a0),%d2\n"           /* + crx */
"    swap   %d2\n"
"    clr.w  %d2\n"
"    .if \\dir < 0\n"
"    move.l %d2,%a3\n"
"    .else\n"
"    move.l %d2,%a4\n"
"    .endif\n"
"    moveq  #0,%d2\n"
"    tst.w  %d7\n"
"    ble.s  3f\n"
"    RS_STEP\n"
"3:  move.l %d2,\\step(%a0)\n"
"    bra    1b\n"
"9:\n"
".endm\n"
/* The segment loop: step both chains to the row y, fill up to the
 * nearer edge end (or the clip bottom), repeat. Mode 0 is the
 * priority-plane core with its stack arguments and a0 free-running
 * (the state pointer is reloaded from the frame); modes 2 and 3 are
 * the page cores, which keep a0. d4 = y throughout. */
".macro PW_LOOP mode\n"
"pw\\mode\\()_seg:\n"
"    .if \\mode==0\n"
"    move.l 48(%sp),%a0\n"
"    .endif\n"
"    move.w 74(%a0),%d4\n"           /* y */
"    RS_CHAIN 58,66,4,-1\n"
"    RS_CHAIN 62,70,12,1\n"
"    .if \\mode!=0\n"
"    move.l 12(%a0),%a6\n"           /* sb (chain b may have moved) */
"    .endif\n"
"    move.w 66(%a0),%d2\n"           /* stop = min(ya, yb) */
"    cmp.w  70(%a0),%d2\n"
"    ble.s  1f\n"
"    move.w 70(%a0),%d2\n"
"1:  move.w 78(%a0),%d3\n"           /* ylast */
"    cmp.w  %d3,%d2\n"
"    ble.s  2f\n"
"    move.w %d3,%d2\n"
"    addq.w #1,%d2\n"
"2:  cmp.w  %d4,%d2\n"
"    bgt.s  3f\n"
"    move.w %d4,%d2\n"               /* stop <= y: one row */
"    addq.w #1,%d2\n"
"3:  sub.w  %d4,%d2\n"               /* count */
"    tst.w  %d4\n"
"    bpl.s  5f\n"
"    move.w %d4,%d3\n"               /* above the clip: step without */
"    neg.w  %d3\n"                   /* drawing, min(count, -y) rows */
"    cmp.w  %d2,%d3\n"
"    ble.s  4f\n"
"    move.w %d2,%d3\n"
"4:  sub.w  %d3,%d2\n"
"    add.w  %d3,%d4\n"
"    subq.w #1,%d3\n"
"6:  add.l  4(%a0),%a3\n"
"    .if \\mode==0\n"
"    add.l  12(%a0),%a4\n"
"    lea    32(%a6),%a6\n"
"    .else\n"
"    add.l  %a6,%a4\n"
"    .endif\n"
"    lea    128(%a5),%a5\n"
"    dbra   %d3,6b\n"
"5:  tst.w  %d2\n"
"    ble.s  7f\n"
"    add.w  %d2,%d4\n"
"    move.w %d4,74(%a0)\n"           /* y += count */
"    .if \\mode==0\n"
"    move.l 12(%a0),-(%sp)\n"        /* sb */
"    move.l 4(%a0),-(%sp)\n"         /* sa */
"    ext.l  %d2\n"
"    move.l %d2,-(%sp)\n"            /* count */
"    bsr    rs_run\n"
"    lea    12(%sp),%sp\n"
"    move.l 48(%sp),%a0\n"
"    .else\n"
"    move.w %d2,%d5\n"
"    subq.w #1,%d5\n"
"    bsr    rs\\mode\\()_run\n"
"    .endif\n"
"    move.w 74(%a0),%d4\n"
"7:  move.w %d4,74(%a0)\n"
"    cmp.w  78(%a0),%d4\n"
"    ble    pw\\mode\\()_seg\n"      /* y <= ylast */
"    bra    pw_out\n"
".endm\n"
"_polyWalk:\n"                       /* (state) */
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    move.l 48(%sp),%a0\n"
"    RS_LOAD\n"
"    move.b 43(%a0),%d2\n"
"    btst   #5,%d2\n"
"    beq    pw0_seg\n"
"    btst   #4,%d2\n"
"    bne    pw3_seg\n"
"    PW_LOOP 2\n"
"    PW_LOOP 3\n"
"    PW_LOOP 0\n"
"pw_out:\n"
"    RS_STORE\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
/* count rows from the registers; stack: ret, count, sa, sb */
"rs_run:\n"
"rs_row:\n"
"    move.l %a3,%d2\n"
"    move.l %a4,%d3\n"
"    swap   %d2\n"
"    swap   %d3\n"
"    cmp.w  %d3,%d2\n"
"    ble.s  1f\n"
"    exg    %d2,%d3\n"
"1:  cmp.w  %d6,%d3\n"               /* xhi */
"    ble.s  2f\n"
"    move.w %d6,%d3\n"
"2:  swap   %d6\n"
"    cmp.w  %d6,%d2\n"               /* xlo */
"    bge.s  3f\n"
"    move.w %d6,%d2\n"
"3:  swap   %d6\n"
"    cmp.w  %d3,%d2\n"
"    bgt.s  rs_skip\n"
"    move.w %d2,%d4\n"
"    asr.w  #4,%d4\n"                /* g0 */
"    move.w %d3,%d7\n"
"    asr.w  #4,%d7\n"
"    sub.w  %d4,%d7\n"               /* d7 = g1 - g0 */
"    lea    rs_lmask(%pc),%a1\n"
"    and.w  #15,%d2\n"
"    add.w  %d2,%d2\n"
"    add.w  %d2,%d2\n"
"    move.l (%a1,%d2.w),%d2\n"       /* left mask m:m */
"    and.w  #15,%d3\n"
"    add.w  %d3,%d3\n"
"    add.w  %d3,%d3\n"
"    move.l 64(%a1,%d3.w),%d3\n"     /* right mask (rs_rmask) */
"    add.w  %d4,%d4\n"
"    lea    (%a6,%d4.w),%a1\n"       /* prio = prow + g0 */
"    lsl.w  #2,%d4\n"
"    lea    (%a5,%d4.w),%a0\n"       /* dst = row + g0*8 */
"    move.l %d3,%d4\n"
"    tst.w  %d7\n"
"    beq.s  rs_single\n"
"    subq.w #1,%d7\n"                /* full groups between the ends */
"    move.l %d2,%d3\n"
"    not.l  %d3\n"
"    jmp    (%a2)\n"                 /* left end, run, right end */
"rs_single:\n"
"    and.l  %d2,%d4\n"               /* one group: m = lm & rm */
"    jmp    -20(%a2)\n"              /* the variant's right end alone */
"rs_skip:\n"
"    add.l  8(%sp),%a3\n"            /* fa += sa */
"    add.l  12(%sp),%a4\n"           /* fb += sb */
"    lea    128(%a5),%a5\n"          /* row += one line */
"    lea    32(%a6),%a6\n"           /* prow += one line */
"    subq.l #1,4(%sp)\n"
"    bne    rs_row\n"
"    rts\n"
"\n"
/* The page cores (modes 2 and 3): no priority plane, so a1 is the
 * group pointer, a6 the b step, d5 the dbra count and a0 stays the
 * state - nothing on the stack, and the row tail is a dbra. Mode 3
 * is for a polygon whose vertices all lie inside the clip: its rows
 * cannot need clamping, so the header skips the four compares. */
".macro RS_PAGE_BODY\n"
"    move.w %d2,%d4\n"
"    asr.w  #4,%d4\n"                /* g0 */
"    move.w %d3,%d7\n"
"    asr.w  #4,%d7\n"
"    sub.w  %d4,%d7\n"               /* d7 = g1 - g0 */
"    lea    rs_lmask(%pc),%a1\n"
"    and.w  #15,%d2\n"
"    add.w  %d2,%d2\n"
"    add.w  %d2,%d2\n"
"    move.l (%a1,%d2.w),%d2\n"       /* left mask m:m */
"    and.w  #15,%d3\n"
"    add.w  %d3,%d3\n"
"    add.w  %d3,%d3\n"
"    move.l 64(%a1,%d3.w),%d3\n"     /* right mask (rs_rmask) */
"    lsl.w  #3,%d4\n"
"    lea    (%a5,%d4.w),%a1\n"       /* dst = row + g0*8 */
"    move.l %d3,%d4\n"
"    tst.w  %d7\n"
"    beq.s  8f\n"
"    subq.w #1,%d7\n"                /* full groups between the ends */
"    move.l %d2,%d3\n"
"    not.l  %d3\n"
"    jmp    (%a2)\n"                 /* left end, run, right end */
"8:  and.l  %d2,%d4\n"               /* one group: m = lm & rm */
"    jmp    -20(%a2)\n"              /* the variant's right end alone */
".endm\n"
"rs2_run:\n"
"rs2_row:\n"
"    move.l %a3,%d2\n"
"    move.l %a4,%d3\n"
"    swap   %d2\n"
"    swap   %d3\n"
"    cmp.w  %d3,%d2\n"
"    ble.s  1f\n"
"    exg    %d2,%d3\n"
"1:  cmp.w  %d6,%d3\n"               /* xhi */
"    ble.s  2f\n"
"    move.w %d6,%d3\n"
"2:  swap   %d6\n"
"    cmp.w  %d6,%d2\n"               /* xlo */
"    bge.s  3f\n"
"    move.w %d6,%d2\n"
"3:  swap   %d6\n"
"    cmp.w  %d3,%d2\n"
"    bgt.s  rs2_skip\n"
"    RS_PAGE_BODY\n"
"rs2_skip:\n"
"    add.l  4(%a0),%a3\n"            /* fa += sa */
"    add.l  %a6,%a4\n"               /* fb += sb */
"    lea    128(%a5),%a5\n"          /* row += one line */
"    dbra   %d5,rs2_row\n"
"    rts\n"
"rs3_run:\n"
"rs3_row:\n"
"    move.l %a3,%d2\n"
"    move.l %a4,%d3\n"
"    swap   %d2\n"
"    swap   %d3\n"
"    cmp.w  %d3,%d2\n"
"    ble.s  1f\n"
"    exg    %d2,%d3\n"
"1:  RS_PAGE_BODY\n"
"rs3_skip:\n"
"    add.l  4(%a0),%a3\n"
"    add.l  %a6,%a4\n"
"    lea    128(%a5),%a5\n"
"    dbra   %d5,rs3_row\n"
"    rts\n"
"\n"
/* One end group, planes p0..p3 and priority mode pm: d2 = m:m,
 * d3 = ~m:~m; the group pointer (a0, or a1 for the page cores) and
 * the priority pointer advance past the group. */
".macro RS_END p0,p1,p2,p3,pm,dst\n"
"    .if \\p0+\\p1==2\n"
"    or.l   %d2,(\\dst)+\n"
"    .elseif \\p0+\\p1==0\n"
"    and.l  %d3,(\\dst)+\n"
"    .elseif \\p0==1\n"
"    or.w   %d2,(\\dst)+\n"
"    and.w  %d3,(\\dst)+\n"
"    .else\n"
"    and.w  %d3,(\\dst)+\n"
"    or.w   %d2,(\\dst)+\n"
"    .endif\n"
"    .if \\p2+\\p3==2\n"
"    or.l   %d2,(\\dst)+\n"
"    .elseif \\p2+\\p3==0\n"
"    and.l  %d3,(\\dst)+\n"
"    .elseif \\p2==1\n"
"    or.w   %d2,(\\dst)+\n"
"    and.w  %d3,(\\dst)+\n"
"    .else\n"
"    and.w  %d3,(\\dst)+\n"
"    or.w   %d2,(\\dst)+\n"
"    .endif\n"
"    .if \\pm==1\n"
"    or.w   %d2,(%a1)+\n"
"    .elseif \\pm==0\n"
"    and.w  %d3,(%a1)+\n"
"    .endif\n"
".endm\n"
/* A row variant: the right-end-only entry sits 20 bytes before the
 * main one (padded), so the header reaches both from one pointer.
 * The full-group run is entered part way through, d7 groups from
 * its end: 6 bytes of code per group with a priority word, 4
 * without. Each mode's tail returns to its own core. */
".macro RS_ROW p0,p1,p2,p3,pm,dst,skip\n"
"rs_t\\p0\\p1\\p2\\p3\\pm:\n"
"    move.l %d4,%d2\n"
"    move.l %d4,%d3\n"
"    not.l  %d3\n"
"    RS_END \\p0,\\p1,\\p2,\\p3,\\pm,\\dst\n"
"    bra    \\skip\n"
"    .space 20-(.-rs_t\\p0\\p1\\p2\\p3\\pm)\n"
"rs_v\\p0\\p1\\p2\\p3\\pm:\n"
"    RS_END \\p0,\\p1,\\p2,\\p3,\\pm,\\dst\n"
"    add.w  %d7,%d7\n"
"    .if \\pm>=2\n"
"    add.w  %d7,%d7\n"
"    .else\n"
"    move.w %d7,%d3\n"
"    add.w  %d7,%d7\n"
"    add.w  %d3,%d7\n"
"    .endif\n"
"    neg.w  %d7\n"
"    jmp    rs_e\\@(%pc,%d7.w)\n"
"    .rept 16\n"
"    move.l %d0,(\\dst)+\n"
"    move.l %d1,(\\dst)+\n"
"    .if \\pm<2\n"
"    move.w %d5,(%a1)+\n"
"    .endif\n"
"    .endr\n"
"rs_e\\@:\n"
"    bra    rs_t\\p0\\p1\\p2\\p3\\pm\n"
".endm\n"
".macro RS_TAB p0,p1,p2,p3,pm\n"
"    .word rs_v\\p0\\p1\\p2\\p3\\pm-rs_mtab\n"
".endm\n"
".irp p3,0,1\n"
".irp p2,0,1\n"
".irp p1,0,1\n"
".irp p0,0,1\n"
"    RS_ROW \\p0,\\p1,\\p2,\\p3,0,%a0,rs_skip\n"
"    RS_ROW \\p0,\\p1,\\p2,\\p3,1,%a0,rs_skip\n"
"    RS_ROW \\p0,\\p1,\\p2,\\p3,2,%a1,rs2_skip\n"
"    RS_ROW \\p0,\\p1,\\p2,\\p3,3,%a1,rs3_skip\n"
".endr\n"
".endr\n"
".endr\n"
".endr\n"
/* entry offsets by fn, and the end masks as m:m longs; both are
 * offsets from their own label, nothing to relocate */
"    .even\n"
"rs_mtab:\n"
".irp pm,0,1,2,3\n"
".irp p3,0,1\n"
".irp p2,0,1\n"
".irp p1,0,1\n"
".irp p0,0,1\n"
"    RS_TAB \\p0,\\p1,\\p2,\\p3,\\pm\n"
".endr\n"
".endr\n"
".endr\n"
".endr\n"
".endr\n"
"rs_lmask:\n"
"    .long 0xFFFFFFFF\n"
"    .long 0x7FFF7FFF\n"
"    .long 0x3FFF3FFF\n"
"    .long 0x1FFF1FFF\n"
"    .long 0x0FFF0FFF\n"
"    .long 0x07FF07FF\n"
"    .long 0x03FF03FF\n"
"    .long 0x01FF01FF\n"
"    .long 0x00FF00FF\n"
"    .long 0x007F007F\n"
"    .long 0x003F003F\n"
"    .long 0x001F001F\n"
"    .long 0x000F000F\n"
"    .long 0x00070007\n"
"    .long 0x00030003\n"
"    .long 0x00010001\n"
"rs_rmask:\n"
"    .long 0x80008000\n"
"    .long 0xC000C000\n"
"    .long 0xE000E000\n"
"    .long 0xF000F000\n"
"    .long 0xF800F800\n"
"    .long 0xFC00FC00\n"
"    .long 0xFE00FE00\n"
"    .long 0xFF00FF00\n"
"    .long 0xFF80FF80\n"
"    .long 0xFFC0FFC0\n"
"    .long 0xFFE0FFE0\n"
"    .long 0xFFF0FFF0\n"
"    .long 0xFFF8FFF8\n"
"    .long 0xFFFCFFFC\n"
"    .long 0xFFFEFFFE\n"
"    .long 0xFFFFFFFF\n"
);

// (dx << 16) / dy without __divsi3: dy is 1..255 on these shapes,
// so dx * (65536/dy) as a hardware 16x16 multiply is exact to under
// 0.005 pixel per row - and every edge change reloads the exact
// vertex, so error cannot accumulate across edges.
static uint16_t g_recip16[256];

static void initRecip16() {
	if (g_recip16[3] == 0) {
		for (int dy = 3; dy < 256; ++dy) {
			g_recip16[dy] = (uint16_t)(65536 / dy);
		}
	}
}

// dy of 1 and 2 are shifts, so the reciprocals stay under 0x8000 and
// both factors are signed 16-bit: gcc then emits one muls.w instead
// of calling __mulsi3, which was most of an edge's cost.
static inline int32_t edgeStep(int dx, int dy) {
	if (dy >= 256) {                        // taller than any shape row
		return ((int32_t)dx << 16) / dy;    // rare: keep it exact
	}
	if (dy <= 2) {
		return (int32_t)dx << (17 - dy);
	}
	return (int32_t)(int16_t)dx * (int32_t)(int16_t)g_recip16[dy];
}

struct RasterState {
	int32_t fa, sa, fb, sb;      // edge x in 16.16, clip origin included
	uint16_t *row;
	uint16_t *prow;
	unsigned long f01, f23, pv;
	unsigned long lim;           // xlo << 16 | xhi (inclusive, in layer x)
	unsigned long fn;            // colour nibble | prio mode << 4
};

// Priority mode for rasterSeg's fn: 0 clears the plane, 1 sets it,
// 2 skips it, 3 skips it for a shape whose x range lies inside the
// clip (the row header then skips the clamp). Cutscene pages skip:
// nothing reads their plane and the room rebake after the scene
// rewrites it, so the word per group was a fifth of the fill for
// nothing.
static inline unsigned long prioMode(uint8_t colour8, bool xInside) {
	if (ST_cutscenePalMode()) {
		return xInside ? 3 : 2;
	}
	return (colour8 & 0x80) ? 1 : 0;
}

// rasterSeg's state plus the vertex walk for polyWalk (offsets are
// what the asm expects; it reads the low word of each long)
struct PolyState : RasterState {
	const Point *pts;
	long n, imax;
	long ia, ib;                 // chain positions
	long ya, yb;                 // y where each chain's edge ends
	long y, ylast;
	long crx;
	const uint16_t *recip;
};

extern "C" void rasterSeg(RasterState *st, long count);
extern "C" void polyWalk(PolyState *st);

extern "C" void fillRowAsm(uint16_t *row, uint16_t *prow, long x, long x1,
	unsigned long f01, unsigned long f23, unsigned long pv);

static inline void fillRowPtr(uint16_t *row, uint16_t *prow, int x, int x1,
		uint16_t f0, uint16_t f1, uint16_t f2, uint16_t f3, bool setPrio) {
	fillRowAsm(row, prow, x, x1,
		((unsigned long)f0 << 16) | f1,
		((unsigned long)f2 << 16) | f3,
		setPrio ? 0xFFFFul : 0ul);
}
#else
// C twin of fillRowAsm, byte-identical semantics (the reference)
static void fillRowPtr(uint16_t *row, uint16_t *prow, int x, int x1,
		uint16_t f0, uint16_t f1, uint16_t f2, uint16_t f3, bool setPrio) {
	uint16_t *dst = row + (x >> 4) * 4;
	uint16_t *prio = prow + (x >> 4);
	const int g0 = x >> 4;
	const int g1 = x1 >> 4;

	if (g0 == g1) {
		fillGroupMasked(dst, prio, groupMask(g0, x, x1),
			f0, f1, f2, f3, setPrio);
		return;
	}
	int nfull = g1 - g0 + 1;
	const uint16_t lm = (uint16_t)(0xFFFFu >> (x & 15));
	const uint16_t rm = (uint16_t)(0xFFFFu << (15 - (x1 & 15)));
	if (lm != 0xFFFF) {
		fillGroupMasked(dst, prio, lm, f0, f1, f2, f3, setPrio);
		dst += 4;
		++prio;
		--nfull;
	}
	const bool rpart = (rm != 0xFFFF);
	if (rpart) {
		--nfull;
	}
	if (nfull > 0) {
#ifdef __m68k__
		{
			uint32_t l01 = ((uint32_t)f0 << 16) | f1;
			uint32_t l23 = ((uint32_t)f2 << 16) | f3;
			uint16_t *d = dst;
			int n = nfull - 1;
			__asm__ volatile(
				"1:\n\t"
				"move.l %2,(%0)+\n\t"
				"move.l %3,(%0)+\n\t"
				"dbra %1,1b"
				: "+a"(d), "+d"(n)
				: "d"(l01), "d"(l23)
				: "memory", "cc");
			const uint16_t pv = setPrio ? 0xFFFF : 0;
			uint16_t *pp = prio;
			n = nfull - 1;
			__asm__ volatile(
				"2:\n\t"
				"move.w %2,(%0)+\n\t"
				"dbra %1,2b"
				: "+a"(pp), "+d"(n)
				: "d"(pv)
				: "memory", "cc");
		}
#else
		for (int n = 0; n < nfull; ++n) {
			dst[n * 4 + 0] = f0;
			dst[n * 4 + 1] = f1;
			dst[n * 4 + 2] = f2;
			dst[n * 4 + 3] = f3;
			prio[n] = setPrio ? 0xFFFF : 0;
		}
#endif
		dst += nfull * 4;
		prio += nfull;
	}
	if (rpart) {
		fillGroupMasked(dst, prio, rm, f0, f1, f2, f3, setPrio);
	}
}
#endif

void ST_fillRect(uint8_t *layer, int x, int y, int w, int h, uint8_t colour8) {
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > kSTLayerW) w = kSTLayerW - x;
	if (y + h > kSTLayerH) h = kSTLayerH - y;
	if (w <= 0 || h <= 0) {
		return;
	}
	const uint8_t v = ST_getRemap()[colour8];
	const bool setPrio = (colour8 & 0x80) != 0;
	for (int j = 0; j < h; ++j) {
		fillRow(layer, x, w, y + j, v, setPrio);
	}
}

void ST_hspan(uint8_t *layer, int x1, int x2, int y, uint8_t colour8) {
	if (y < 0 || y >= kSTLayerH) {
		return;
	}
	if (x1 < 0) x1 = 0;
	if (x2 >= kSTLayerW) x2 = kSTLayerW - 1;
	if (x1 > x2) {
		return;
	}
	fillRow(layer, x1, x2 - x1 + 1, y, ST_getRemap()[colour8], (colour8 & 0x80) != 0);
}

// The whole polygon in one call: walks drawPolygon's run list
// (y, then x1,x2 pairs until x1 < 0), stepping the row pointers
// incrementally. Per-row address recomputation and the call chain
// were costing more than the fills for the average 15-pixel span.
void ST_fillArea(uint8_t *layer, const int16_t *pts, int crx, int cry,
		int crw, uint8_t v, bool setPrio) {
	int y = cry + *pts++;
	int x1 = *pts++;
	if (x1 < 0) {
		return;
	}
	const uint16_t f0 = (v & 1) ? 0xFFFF : 0;
	const uint16_t f1 = (v & 2) ? 0xFFFF : 0;
	const uint16_t f2 = (v & 4) ? 0xFFFF : 0;
	const uint16_t f3 = (v & 8) ? 0xFFFF : 0;
	uint16_t *row = (uint16_t *)(layer + y * kSTRowBytes);
	uint16_t *prow = (uint16_t *)(layer + kSTPlaneBytes + y * kSTPrioRowBytes);
	do {
		int x2 = *pts++;
		if (x2 > crw - 1) {
			x2 = crw - 1;
		}
		int a = crx + x1;
		int b = crx + x2;
		if ((unsigned)y < (unsigned)kSTLayerH && a <= b) {
			if (a < 0) {
				a = 0;
			}
			if (b >= kSTLayerW) {
				b = kSTLayerW - 1;
			}
			if (a <= b) {
				fillRowPtr(row, prow, a, b, f0, f1, f2, f3, setPrio);
			}
		}
		++y;
		row += kSTRowBytes / 2;
		prow += kSTPrioRowBytes / 2;
		x1 = *pts++;
	} while (x1 >= 0);
}

// Fast scan conversion for the cutscene polygons. The upstream
// converter is exact but general: its state machine, per-edge
// divides and per-scanline clamps measured ~19k cycles for an
// average 8-scanline polygon - more than the fills it feeds. This
// walker handles the common case (opaque, 3+ points, not flat):
// two edge chains stepped in 16.16 fixed point from the top vertex,
// one divide per edge, rows filled directly through the pointer
// core with no intermediate run list. Alpha/shadow polygons, lines
// and flat polygons stay on the reference path (they are rare and
// their semantics are fiddly). Edges can differ from the reference
// by a pixel - invisible in motion, and the trade is the reason
// scenes hold their scripted pace.
// fill words per colour nibble: a plane pair as 0xFFFF/0 halves
#define FILL2(v, a, b) ((((v) & (a)) ? 0xFFFF0000ul : 0ul) | (((v) & (b)) ? 0xFFFFul : 0ul))
#define FILL01(v) FILL2(v, 1, 2)
#define FILL23(v) FILL2(v, 4, 8)
static const unsigned long kFill01[16] = {
	FILL01(0), FILL01(1), FILL01(2), FILL01(3), FILL01(4), FILL01(5), FILL01(6), FILL01(7),
	FILL01(8), FILL01(9), FILL01(10), FILL01(11), FILL01(12), FILL01(13), FILL01(14), FILL01(15)
};
static const unsigned long kFill23[16] = {
	FILL23(0), FILL23(1), FILL23(2), FILL23(3), FILL23(4), FILL23(5), FILL23(6), FILL23(7),
	FILL23(8), FILL23(9), FILL23(10), FILL23(11), FILL23(12), FILL23(13), FILL23(14), FILL23(15)
};
#undef FILL2
#undef FILL01
#undef FILL23

// One pixel of colour nibble v at (x, y): the far end of a line.
// fillRow's group machinery cost a thousand cycles for it.
static inline void plotV(uint8_t *layer, int x, int y, uint8_t v, unsigned long pm) {
	uint16_t *p = (uint16_t *)(layer + y * kSTRowBytes + ((x >> 4) << 3));
	const uint16_t bit = 0x8000 >> (x & 15);
	if (v & 1) p[0] |= bit; else p[0] &= ~bit;
	if (v & 2) p[1] |= bit; else p[1] &= ~bit;
	if (v & 4) p[2] |= bit; else p[2] &= ~bit;
	if (v & 8) p[3] |= bit; else p[3] &= ~bit;
	if (pm < 2) {
		uint16_t *q = (uint16_t *)(layer + kSTPlaneBytes + y * kSTPrioRowBytes + ((x >> 4) << 1));
		if (pm) *q |= bit; else *q &= ~bit;
	}
}

bool ST_drawPolygonFast(uint8_t *layer, const void *ptsv, int n,
		uint8_t colour8, int crx, int cry, int crw, int crh) {
	const Point *pts = (const Point *)ptsv;
	if (n < 2) {
		return false;
	}
	initRecip16();
	// vertex scan: the extremes in registers, not re-read through
	// pts[imin] each turn (gcc 4.6 made a meal of that)
	int imin = 0, imax = 0;
	int16_t xlo = pts[0].x, xhi = xlo;
	int16_t ytop = pts[0].y, ybot = ytop;
	{
		const Point *p = pts + 1;
		for (int i = 1; i < n; ++i, ++p) {
			const int16_t x = p->x, y = p->y;
			if (y < ytop) { ytop = y; imin = i; }
			if (y > ybot) { ybot = y; imax = i; }
			if (x < xlo) xlo = x;
			if (x > xhi) xhi = x;
		}
	}
	const int xmaxv = (crw < kSTLayerW - crx ? crw : kSTLayerW - crx) - 1;
	if (ytop == ybot) {
		// flat: one row across the x extent
		const int sy = cry + ytop;
		if ((unsigned)sy >= (unsigned)kSTLayerH
		    || (unsigned)ytop >= (unsigned)crh) {
			return true;
		}
		if (xlo < 0) xlo = 0;
		if (xhi > xmaxv) xhi = xmaxv;
		if (xlo > xhi) {
			return true;
		}
		ST_hspanV(layer, crx + xlo, crx + xhi, sy, ST_getRemap()[colour8],
			(colour8 & 0x80) != 0);
		return true;
	}

	int ylast = crh - 1;
	if (ylast > kSTLayerH - 1 - cry) {
		ylast = kSTLayerH - 1 - cry;
	}
	const uint8_t v = ST_getRemap()[colour8] & 15;
	const unsigned long pm = prioMode(colour8, xlo >= 0 && xhi <= xmaxv);
	PolyState st;
	st.f01 = kFill01[v];
	st.f23 = kFill23[v];
	st.pv = (colour8 & 0x80) ? 0xFFFFul : 0ul;
	st.lim = ((unsigned long)crx << 16) | (unsigned)(crx + xmaxv);
	st.fn = v | (pm << 4);
	st.fa = (int32_t)(pts[imin].x + crx) << 16;

	if (n == 2) {
		// A line, drawn as a one-row-step-wide trapezoid: each row
		// fills the horizontal run the line crosses, like a solid
		// Bresenham. Mid-line rows span [f, f+step]; the final row
		// lands exactly on the far endpoint.
		const int dy = ybot - ytop;
		st.sa = st.sb = edgeStep(pts[imax].x - pts[imin].x, dy);
		st.fb = st.fa + st.sa;
		int y = ytop;
		int count = dy;                    // rows ytop..ybot-1
		if (y < 0) {
			const int skip = (-y < count) ? -y : count;
			st.fa += st.sa * skip;
			st.fb += st.sb * skip;
			y += skip;
			count -= skip;
		}
		if (y + count - 1 > ylast) {
			count = ylast - y + 1;
		}
		if (count > 0) {
			st.row = (uint16_t *)(layer + (cry + y) * kSTRowBytes);
			st.prow = (uint16_t *)(layer + kSTPlaneBytes + (cry + y) * kSTPrioRowBytes);
			rasterSeg(&st, count);
		}
		if (ybot >= 0 && ybot <= ylast) {
			const int xe = pts[imax].x;
			if (xe >= 0 && xe <= xmaxv) {
				plotV(layer, crx + xe, cry + ybot, v, pm);
			}
		}
		return true;
	}

	if (ylast > ybot) {
		ylast = ybot;
	}
	if (ytop > ylast) {
		return true;                  // fully below the clip
	}

	// chain a walks backwards through the vertex list, chain b
	// forwards; both start at the top vertex. polyWalk steps the
	// edges and fills the rows between them.
	st.fb = st.fa;
	st.sa = st.sb = 0;
	st.row = (uint16_t *)(layer + (cry + ytop) * kSTRowBytes);
	st.prow = (uint16_t *)(layer + kSTPlaneBytes + (cry + ytop) * kSTPrioRowBytes);
	st.pts = pts;
	st.n = n;
	st.imax = imax;
	st.ia = st.ib = imin;
	st.ya = st.yb = ytop;
	st.y = ytop;
	st.ylast = ylast;
	st.crx = crx;
	st.recip = g_recip16;
	polyWalk(&st);
	return true;
}

// Row fill with the colour already remapped: fillArea resolves the
// polygon colour once instead of per span (a cutscene frame emits
// hundreds of spans, and the remap fetch was pure overhead on each)
void ST_hspanV(uint8_t *layer, int x1, int x2, int y, uint8_t v, bool setPrio) {
	if (y < 0 || y >= kSTLayerH) {
		return;
	}
	if (x1 < 0) x1 = 0;
	if (x2 >= kSTLayerW) x2 = kSTLayerW - 1;
	if (x1 > x2) {
		return;
	}
	fillRow(layer, x1, x2 - x1 + 1, y, v, setPrio);
}

// The chunky code's cutscene "shadow" effect ORs the colour's slot
// bits into the underlying pixel (*dst |= colour & 0xF8), selecting
// the artist's shadow variant of whatever is underneath. In hardware
// colour space that becomes a per-slot lookup (ST_getOrMap): read
// each covered pixel, translate, write back. Outside cutscene mode
// (no representative table) it falls back to an opaque fill.
void ST_hspanOr(uint8_t *layer, int x1, int x2, int y, uint8_t colour8) {
	if (y < 0 || y >= kSTLayerH) {
		return;
	}
	if (x1 < 0) x1 = 0;
	if (x2 >= kSTLayerW) x2 = kSTLayerW - 1;
	if (x1 > x2) {
		return;
	}
	if (!ST_cutscenePalMode()) {
		fillRow(layer, x1, x2 - x1 + 1, y, ST_getRemap()[colour8], (colour8 & 0x80) != 0);
		return;
	}
	uint8_t orMap[16];
	ST_getOrMap(colour8, orMap);
	uint16_t *dst = groupPtr(layer, x1, y);
	uint16_t *prio = prioPtr(layer, x1, y);
	for (int g = x1 >> 4; g <= (x2 >> 4); ++g) {
		const uint16_t m = groupMask(g, x1, x2);
		// translate the covered pixels through the shadow map
		uint16_t n0 = 0, n1 = 0, n2 = 0, n3 = 0;
		uint16_t bit = 0x8000;
		for (int i = 0; i < 16; ++i, bit >>= 1) {
			if (!(m & bit)) {
				continue;
			}
			int v = ((dst[0] & bit) ? 1 : 0) | ((dst[1] & bit) ? 2 : 0)
			      | ((dst[2] & bit) ? 4 : 0) | ((dst[3] & bit) ? 8 : 0);
			const uint8_t nv = orMap[v];
			if (nv & 1) n0 |= bit;
			if (nv & 2) n1 |= bit;
			if (nv & 4) n2 |= bit;
			if (nv & 8) n3 |= bit;
		}
		const uint16_t keep = ~m;
		dst[0] = (dst[0] & keep) | n0;
		dst[1] = (dst[1] & keep) | n1;
		dst[2] = (dst[2] & keep) | n2;
		dst[3] = (dst[3] & keep) | n3;
		*prio |= m;
		dst += 4;
		++prio;
	}
}

void ST_drawPoint(uint8_t *layer, int x, int y, uint8_t colour8) {
	if (x < 0 || x >= kSTLayerW || y < 0 || y >= kSTLayerH) {
		return;
	}
	const uint8_t v = ST_getRemap()[colour8];
	const uint16_t bit = 0x8000 >> (x & 15);
	const uint16_t keep = ~bit;
	uint16_t *dst = groupPtr(layer, x, y);
	uint16_t *prio = prioPtr(layer, x, y);
	dst[0] = (dst[0] & keep) | ((v & 1) ? bit : 0);
	dst[1] = (dst[1] & keep) | ((v & 2) ? bit : 0);
	dst[2] = (dst[2] & keep) | ((v & 4) ? bit : 0);
	dst[3] = (dst[3] & keep) | ((v & 8) ? bit : 0);
	if (colour8 & 0x80) {
		*prio |= bit;
	} else {
		*prio &= keep;
	}
}

// Whole-layer copy. The bus allows ~2MB/s at 8MHz whatever does the
// moving; a 12-register movem burst gets closest (mintlib's memcpy
// managed 1.6). Copies planes and the priority plane in one run -
// the layer is one contiguous block.
template <int kBytes>
static inline void copyBlock(uint8_t *dst, const uint8_t *src) {
#ifdef __m68k__
	__asm__ volatile(
		"move.w %2,%%d0\n"
		"1:\n\t"
		"movem.l (%1)+,%%d1-%%d7/%%a2-%%a6\n\t"
		"movem.l %%d1-%%d7/%%a2-%%a6,(%0)\n\t"
		"lea 48(%0),%0\n\t"
		"dbra %%d0,1b"
		: "+a"(dst), "+a"(src)
		: "i"(kBytes / 48 - 1)
		: "d0","d1","d2","d3","d4","d5","d6","d7",
		  "a2","a3","a4","a5","a6","memory","cc");
	// kBytes % 48 tail
	{
		const uint32_t *s32 = (const uint32_t *)src;
		uint32_t *d32 = (uint32_t *)dst;
		for (int n = (kBytes % 48) / 4; --n >= 0; ) {
			*d32++ = *s32++;
		}
	}
#else
	memcpy(dst, src, kBytes);
#endif
}

void ST_copyLayer(uint8_t *dst, const uint8_t *src) {
	copyBlock<kSTPlaneBytes + kSTPrioRowBytes * kSTLayerH>(dst, src);
}

// Cutscene page copy: the planes only. The scene's pages carry no
// live priority plane (see prioMode) and the copy is a fifth shorter
// without it - 51 of them in the first scene.
void ST_copyPage(uint8_t *dst, const uint8_t *src) {
	copyBlock<kSTPlaneBytes>(dst, src);
}

void ST_clearLayer(uint8_t *layer, uint8_t colour8) {
	const uint8_t v = ST_getRemap()[colour8];
	uint16_t row[4];
	row[0] = (v & 1) ? 0xFFFF : 0;
	row[1] = (v & 2) ? 0xFFFF : 0;
	row[2] = (v & 4) ? 0xFFFF : 0;
	row[3] = (v & 8) ? 0xFFFF : 0;
	uint32_t *dst = (uint32_t *)layer;
	const uint32_t a = ((uint32_t)row[0] << 16) | row[1];
	const uint32_t b = ((uint32_t)row[2] << 16) | row[3];
	for (int n = 0; n < kSTPlaneBytes / 8; ++n) {
		*dst++ = a;
		*dst++ = b;
	}
	if (!ST_cutscenePalMode()) {         // cutscene pages: no live plane
		memset(layer + kSTPlaneBytes, (colour8 & 0x80) ? 0xFF : 0, kSTPrioRowBytes * kSTLayerH);
	}
}

#endif // ATARIST
