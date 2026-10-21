
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Atari ST planar layer primitives (see video_st.h).
 */

#ifdef ATARIST

extern "C" {
#include <stdl/stdl.h>
#include <stdl/stdl_math.h>
}

#include "video_st.h"

// group-rows the drawing on a tracked page touched, box by box (the
// per-frame stats: what one box per page costs against these)
uint32_t g_stCutTouchGR;

// see ST_pageBandStart; the whole page when no band is in force
static int16_t g_bandY0 = 0, g_bandY1 = kSTLayerH;
static int16_t g_bandFill = -1;   // hardware value outside the band

static inline void bandTouch(int y0, int y1) {
	if (y0 < g_bandY0) {
		g_bandY0 = (int16_t)(y0 < 0 ? 0 : y0);
	}
	if (y1 > g_bandY1) {
		g_bandY1 = (int16_t)(y1 > kSTLayerH ? kSTLayerH : y1);
	}
}

// Cutscene page tracking (see ST_pageBandStart): for the two pages
// that take turns on screen, and for the screen itself, a box around
// everything that differs from the aux page - the background every
// frame is restored from. A restore then copies only what the last
// frame on that page drew over, and a push only what changed since
// the last one, instead of the whole band every time. One box, not
// a mask of blocks: marking every block of every shape, and copying
// a region in pieces, cost more on a 68000 than the few clean blocks
// a box takes along. Groups [g0, g1) of 16 pixels, rows [y0, y1).
struct TrkBox {
	int16_t g0, g1, y0, y1;
};
static bool g_trk;
static uint8_t *g_trkPage[2];
static uint8_t *g_trkAux;
static TrkBox g_trkBox[2];
static TrkBox g_scrBox;

static inline void boxClear(TrkBox *b) {
	b->g0 = 16;
	b->g1 = 0;
	b->y0 = kSTLayerH;
	b->y1 = 0;
}

static inline bool boxEmpty(const TrkBox *b) {
	return b->y0 >= b->y1;
}

static inline void boxAdd(TrkBox *b, int g0, int g1, int y0, int y1) {
	if (g0 < b->g0) b->g0 = (int16_t)g0;
	if (g1 > b->g1) b->g1 = (int16_t)g1;
	if (y0 < b->y0) b->y0 = (int16_t)y0;
	if (y1 > b->y1) b->y1 = (int16_t)y1;
}

static inline void boxUnion(TrkBox *b, const TrkBox *o) {
	if (!boxEmpty(o)) {
		boxAdd(b, o->g0, o->g1, o->y0, o->y1);
	}
}

static inline TrkBox *trkBoxOf(const uint8_t *layer) {
	if (layer == g_trkPage[0]) {
		return &g_trkBox[0];
	}
	if (layer == g_trkPage[1]) {
		return &g_trkBox[1];
	}
	return 0;
}

// pixels [x0, x1) of rows [y0, y1) of layer are about to change;
// inlined, as it runs once per shape and the call cost more than
// the work
static inline __attribute__((always_inline)) void pageTouch(const uint8_t *layer, int x0, int x1, int y0, int y1) {
	// no scene in progress: the band is the whole page, nothing to do
	if (!g_trk) {
		return;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (y1 > kSTLayerH) {
		y1 = kSTLayerH;
	}
	if (y0 >= y1) {
		return;
	}
	bandTouch(y0, y1);
	if (x0 < 0) {
		x0 = 0;
	}
	if (x1 > kSTLayerW) {
		x1 = kSTLayerW;
	}
	if (x0 >= x1) {
		return;
	}
	const int g0 = x0 >> 4, g1 = ((x1 - 1) >> 4) + 1;
	if (layer == g_trkAux) {
		// the background itself: everything else now differs there
		boxAdd(&g_trkBox[0], g0, g1, y0, y1);
		boxAdd(&g_trkBox[1], g0, g1, y0, y1);
		boxAdd(&g_scrBox, g0, g1, y0, y1);
		return;
	}
	if (TrkBox *b = trkBoxOf(layer)) {
		boxAdd(b, g0, g1, y0, y1);
		g_stCutTouchGR += STDL_MulU16((uint16_t)(g1 - g0), (uint16_t)(y1 - y0));
	}
}

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
// What has written a priority plane since the last frame's restore,
// as one bounding box. The plane changes only where something marks
// or clears it - the fills, and the few sprites drawn with priority,
// which in an ordinary frame is none, or the inventory icon when a
// sprite crossed it - so the per-block restore of it (measured at 2.1ms a frame on an
// 8MHz ST) is skipped everywhere the box does not reach. Writers
// report their own rectangle; the ones that cover a layer say so
// with ST_prioTouched.
static int16_t g_prioX0, g_prioY0, g_prioX1 = -1, g_prioY1 = -1;

// The same touches as blocks, kept until taken (ST_takePrioBlocks): the
// display list restores the priority plane only where it may differ from
// the room's, and a frame's MARKs can stay on the layer for many frames.
static uint16_t g_prioBlk[kSTBlockRows];
// the whole layer is marked already, for each of the two takers: a
// room build marks it a tile at a time, and the blocks' loop made that
// a sixth of the build
static bool g_prioAllRect, g_prioAllBlk;

const uint16_t kSTColsFrom[16] = {
	0xFFFF, 0x7FFF, 0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF,
	0x00FF, 0x007F, 0x003F, 0x001F, 0x000F, 0x0007, 0x0003, 0x0001
};
const uint16_t kSTColsTo[16] = {
	0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
	0xFF80, 0xFFC0, 0xFFE0, 0xFFF0, 0xFFF8, 0xFFFC, 0xFFFE, 0xFFFF
};

void ST_takePrioBlocks(uint16_t *acc) {
	// two rows a long (see Video::ST_frameEnd)
	uint32_t *a = (uint32_t *)acc, *b = (uint32_t *)g_prioBlk;
	for (int n = kSTBlockRows / 2; --n >= 0; ) {
		*a++ |= *b;
		*b++ = 0;
	}
	g_prioAllBlk = false;
}

void ST_prioDropBlocks() {
	memset(g_prioBlk, 0, sizeof(g_prioBlk));
	g_prioAllBlk = false;
}

void ST_prioTouchedRect(int x, int y, int w, int h) {
	if (w <= 0 || h <= 0) {
		return;
	}
	const int x1 = x + w - 1;
	const int y1 = y + h - 1;
	{
		const int bx0 = (x < 0) ? 0 : (x >> 4);
		const int bx1 = (x1 >= kSTLayerW) ? 15 : (x1 >> 4);
		const int by0 = (y < 0) ? 0 : (y >> 3);
		const int by1 = (y1 >= kSTLayerH) ? kSTBlockRows - 1 : (y1 >> 3);
		if (bx0 <= bx1) {
			const uint16_t m = (uint16_t)(kSTColsFrom[bx0] & kSTColsTo[bx1]);
			for (int j = by0; j <= by1; ++j) {
				g_prioBlk[j] |= m;
			}
		}
	}
	if (g_prioX1 < g_prioX0) {
		g_prioX0 = (int16_t)x;
		g_prioY0 = (int16_t)y;
		g_prioX1 = (int16_t)x1;
		g_prioY1 = (int16_t)y1;
		return;
	}
	if (x < g_prioX0) {
		g_prioX0 = (int16_t)x;
	}
	if (y < g_prioY0) {
		g_prioY0 = (int16_t)y;
	}
	if (x1 > g_prioX1) {
		g_prioX1 = (int16_t)x1;
	}
	if (y1 > g_prioY1) {
		g_prioY1 = (int16_t)y1;
	}
}

void ST_prioTouched() {
	if (g_prioAllRect && g_prioAllBlk) {
		return;
	}
	ST_prioTouchedRect(0, 0, kSTLayerW, kSTLayerH);
	g_prioAllRect = g_prioAllBlk = true;
}

bool ST_takePrioRect(int *x0, int *y0, int *x1, int *y1) {
	if (g_prioX1 < g_prioX0) {
		return false;
	}
	*x0 = g_prioX0;
	*y0 = g_prioY0;
	*x1 = g_prioX1;
	*y1 = g_prioY1;
	g_prioX0 = g_prioY0 = 0;
	g_prioX1 = g_prioY1 = -1;
	g_prioAllRect = false;
	return true;
}

void ST_prioDropRect() {
	g_prioX0 = g_prioY0 = 0;
	g_prioX1 = g_prioY1 = -1;
	g_prioAllRect = false;
}

STDL_Surface *ST_layerView(uint8_t *layer, bool masked) {
	enum { N = 8 };
	static uint8_t *keys[2][N];
	static STDL_Surface *surfs[2][N];
	// the caller asks for the same layer over and over - once per
	// sprite blit - so answer that from one slot before the scan
	static uint8_t *lastKey[2];
	static STDL_Surface *lastSurf[2];
	const int v = masked ? 1 : 0;
	if (lastKey[v] == layer) {
		return lastSurf[v];
	}
	int i = 0;
	for (; i < N && keys[v][i]; ++i) {
		if (keys[v][i] == layer) {
			lastKey[v] = layer;
			lastSurf[v] = surfs[v][i];
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
	lastKey[v] = layer;
	lastSurf[v] = surfs[v][i];
	return surfs[v][i];
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
static uint32_t g_rep[256];             // b in all four bytes
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
		g_rep[b] = (uint32_t)b * 0x01010101u;
		g_notRep[b] = ~g_rep[b];
	}
	g_spreadReady = true;
}

const uint32_t *ST_spreadTable() {
	if (!g_spreadReady) {
		initSpread();
	}
	return g_spread[0];
}

const uint32_t *ST_repTable() {
	if (!g_spreadReady) {
		initSpread();
	}
	return g_rep;
}

const uint8_t *ST_bitrevTable() {
	if (!g_spreadReady) {
		initSpread();
	}
	return g_bitrev;
}

// Pair tables: index = left pixel | right pixel << 4 (logical colours
// within a palette slot), value = the two pixels' hardware colour
// bits in the leftmost two bits of each plane byte, planes 0..3 from
// the most significant byte down. Pair q of a unit is the same table
// shifted right 2q. One table per palette base the room decoder
// uses (0x00, 0x10, 0x80, 0x90), rebuilt when the remap moves its
// colours; ver changes with it, and only then.
struct PairMap {
	uint16_t gen;
	uint8_t pal;
	uint16_t ver;
	uint8_t map16[16];
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
	if (m.gen != 0 && m.pal == pal && memcmp(map16, m.map16, sizeof(map16)) == 0) {
		m.gen = ST_remapGen();          // the remap moved, but not these colours
		return m.t;
	}
	memcpy(m.map16, map16, sizeof(map16));
	if (++m.ver == 0) {
		m.ver = 1;
	}
	ST_buildPairTable(map16, m.t);
	m.gen = ST_remapGen();
	m.pal = pal;
	return m.t;
}

// A pair entry is its left pixel's hardware colour bits, one at the top
// of each plane byte, ORed with its right pixel's a bit lower: two
// sixteen-entry tables and an OR. Built a bit at a time, through
// variable shifts, it cost a sixth of a room build on a plain ST.
void ST_buildPairTable(const uint8_t *map16, uint32_t *t) {
	uint32_t left[16], right[16];
	for (int c = 0; c < 16; ++c) {
		const uint8_t a = map16[c];
		const uint32_t v = ((uint32_t)(a & 1) << 31) | ((uint32_t)((a >> 1) & 1) << 23)
			| ((uint32_t)((a >> 2) & 1) << 15) | ((uint32_t)((a >> 3) & 1) << 7);
		left[c] = v;
		right[c] = v >> 1;
	}
	for (int i = 0; i < 256; ++i) {
		t[i] = left[i & 15] | right[i >> 4];
	}
}

uint16_t ST_tilePalette(uint8_t pal, const uint32_t **pair) {
	*pair = pairMap(pal);
	return g_pairMaps[((pal >> 4) & 1) | ((pal >> 6) & 2)].ver;
}

// packCellsAsm (declared in video_st.h): n units of four planes (plane
// k at s0 + k * planeSize, one byte per unit) and their mask bytes ->
// n cells at out (even address: empty cells are cleared with long
// stores)

// rows of n cells into the layer: d is the plane byte of the first
// unit, pr its priority byte, odd whether that unit is the right
// byte of its 16-pixel group (plane bytes step 1,7,1,7... along a
// row), prio is 0xFF or 0 for what the priority plane takes where
// the cell is opaque, stride the cell bytes from one row to the
// next (the rows of a clipped block are wider than n)
extern "C" void placeCellsAsm(uint8_t *d, uint8_t *pr, const uint8_t *cells,
	long n, long odd, long prio, long rows, long stride);


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
"    .globl _packCellsAsm\n"            /* the sprite bakes use it too (spritecache_st.cpp) */
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
"    .globl _placeTile8Asm\n"
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

// placeTile8OpaqueAsm: every row's mask is 0xFF, so nothing to test
#define TILE_ROW_OPAQUE(K, D, P) \
"    movep.l " K "(%a0),%d2\n" \
"    movep.l %d2," D "(%a1)\n" \
"    move.b %d1," P "(%a2)\n"

__asm__(
"    .text\n"
"    .even\n"
"    .globl _placeTile8OpaqueAsm\n"
"_placeTile8OpaqueAsm:\n"
"    movem.l %d2/%a2,-(%sp)\n"
"    move.l 12(%sp),%a1\n"           /* d      */
"    move.l 16(%sp),%a2\n"           /* pr     */
"    move.l 20(%sp),%a0\n"           /* cells  */
"    move.l 24(%sp),%d1\n"           /* prio   */
TILE_ROW_OPAQUE("0","0","0")
TILE_ROW_OPAQUE("8","128","32")
TILE_ROW_OPAQUE("16","256","64")
TILE_ROW_OPAQUE("24","384","96")
TILE_ROW_OPAQUE("32","512","128")
TILE_ROW_OPAQUE("40","640","160")
TILE_ROW_OPAQUE("48","768","192")
TILE_ROW_OPAQUE("56","896","224")
"    movem.l (%sp)+,%d2/%a2\n"
"    rts\n"
);
#undef TILE_ROW_OPAQUE

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

extern "C" void placeTile8OpaqueAsm(uint8_t *d, uint8_t *pr, const uint8_t *cells, long prio) {
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

void ST_amigaTile8(uint8_t *out, const uint8_t *tile, const uint32_t *pair, bool xflip, bool yflip, bool keyZero) {
	if (!g_spreadReady) {
		initSpread();
	}
	// the flips are applied on the way into a 32-byte copy, which
	// the kernel then reads like any other four-plane source
	uint8_t t[32], m[8];
	gatherTile8Asm(t, m, yflip ? tile + 7 : tile, yflip ? -1 : 1,
		xflip ? g_bitrev : 0, keyZero ? 0 : 0xFF);
	packCellsAsm(out, t, 8, m, 8, g_spread[0], pair);
}

void ST_amigaPlace(uint8_t *layer, int x, int y, const uint8_t *prep, int units, int rows, bool setPrio) {
	ST_prioTouched();
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
	ST_prioTouched();
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
	ST_prioTouched();
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

unsigned ST_i8Flags(unsigned flags) {
	unsigned f = 0;
	if (flags & kSTSpriteXflip) {
		f |= STDL_I8_XFLIP;
	}
	if (flags & kSTSpriteColMajor) {
		f |= STDL_I8_COLMAJOR;
	}
	return f;
}

void ST_drawSprite(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, int w, int h, const uint8_t *map16, unsigned flags, bool setPrio) {
	// maskless view unless the priority plane is read or set: see
	// blitRec (spritecache_st.cpp)
	STDL_Surface *s = ST_layerView(layer, (flags & kSTSpriteRespectPrio) != 0 || setPrio);
	if (!s) {
		return;
	}
	pageTouch(layer, x, x + w, y, y + h);
	unsigned f = ST_i8Flags(flags);
	if (flags & kSTSpriteRespectPrio) {
		f |= STDL_I8_UNDER;
	}
	if (setPrio) {
		f |= STDL_I8_MARK;
		ST_prioTouchedRect(x, y, w, h);
	}
	STDL_BlitIndexed8(s, src, pitch, x, y, w, h, map16, f);
}

// What the gameplay sprite path did since the last log line (see
// STPerf); the bakes themselves live in spritecache_st.cpp.
STPerf g_stPerf;

void ST_perfFormat(const STPerf &p, char *buf, int len) {
	snprintf(buf, len, "room %u/%u/%u/%u dl %u/%u blit u%u/%u p%u/%u bake %u/%u(%u)/%u sh %u/%u ring %u/%u/%u p8 %luK mon %u bank %u/%u/%luK",
		(unsigned)p.roomHits, (unsigned)p.roomRemap, (unsigned)p.roomDecodes, (unsigned)p.tilePreps,
		(unsigned)p.dlDrawn, (unsigned)p.dlKept,
		(unsigned)p.blitUnderAl, (unsigned)p.blitUnderSh, (unsigned)p.blitPlainAl, (unsigned)p.blitPlainSh,
		(unsigned)p.bakeHits, (unsigned)p.bakeMakes, (unsigned)p.bakePlanar, (unsigned)p.chunky,
		(unsigned)p.shBuilds, (unsigned)p.shRefused,
		(unsigned)p.promotes, (unsigned)p.segFlush, (unsigned)p.unused,
		(unsigned long)(p.planar8Px >> 10), (unsigned)p.monsterLoads,
		(unsigned)p.bankLoads, (unsigned)p.bankClears, (unsigned long)(p.bankBytes >> 10));
}

// out of line: inlined with x + 8 and y + 8, gcc folds pageTouch's
// empty-box tests away and warns that it assumed no overflow
static __attribute__((noinline)) void glyphTouch(const uint8_t *layer, int x, int y) {
	pageTouch(layer, x, x + 8, y, y + 8);
}

void ST_drawGlyph(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, uint8_t colour8) {
	const uint8_t v = ST_getRemap()[colour8];
	const bool setPrio = (colour8 & 0x80) != 0;
	if (ST_dlRecording(layer) && ST_dlAddGlyph(src, pitch, x, y, v, setPrio)) {
		return;
	}
	if (x < 0 || y < 0 || x + 8 > kSTLayerW || y + 8 > kSTLayerH) {
		uint8_t map16[16];
		memset(map16, v, sizeof(map16));
		ST_drawSprite(layer, src, pitch, x, y, 8, 8, map16, 0, setPrio);
		return;
	}
	// A glyph is one colour through a mask, so it goes straight into
	// the planes: what STDL_BlitIndexed8 does with it (0 transparent,
	// anything else the colour, the priority bits marked under it), a
	// row's eight pixels as one shifted mask over the one or two
	// groups they fall in. The general chunky blit cost ~2ms a glyph
	// on an 8MHz ST, and a cutscene caption is 30 of them, twice.
	glyphTouch(layer, x, y);
	if (setPrio) {
		ST_prioTouchedRect(x, y, 8, 8);
	}
	const uint16_t f0 = (v & 1) ? 0xFFFF : 0, f1 = (v & 2) ? 0xFFFF : 0;
	const uint16_t f2 = (v & 4) ? 0xFFFF : 0, f3 = (v & 8) ? 0xFFFF : 0;
	const int shift = 24 - (x & 15);
	uint16_t *g = groupPtr(layer, x, y);
	uint16_t *pr = prioPtr(layer, x, y);
	for (int r = 0; r < 8; ++r, src += pitch, g += kSTRowBytes / 2, pr += kSTPrioRowBytes / 2) {
		unsigned mb = 0;
		for (int i = 0; i < 8; ++i) {
			mb = (mb << 1) | (src[i] != 0);
		}
		if (mb == 0) {
			continue;
		}
		const uint32_t m32 = (uint32_t)mb << shift;
		uint16_t m = (uint16_t)(m32 >> 16);
		if (m) {
			const uint16_t keep = (uint16_t)~m;
			g[0] = (uint16_t)((g[0] & keep) | (f0 & m));
			g[1] = (uint16_t)((g[1] & keep) | (f1 & m));
			g[2] = (uint16_t)((g[2] & keep) | (f2 & m));
			g[3] = (uint16_t)((g[3] & keep) | (f3 & m));
			if (setPrio) {
				pr[0] |= m;
			}
		}
		m = (uint16_t)m32;
		if (m) {
			const uint16_t keep = (uint16_t)~m;
			g[4] = (uint16_t)((g[4] & keep) | (f0 & m));
			g[5] = (uint16_t)((g[5] & keep) | (f1 & m));
			g[6] = (uint16_t)((g[6] & keep) | (f2 & m));
			g[7] = (uint16_t)((g[7] & keep) | (f3 & m));
			if (setPrio) {
				pr[1] |= m;
			}
		}
	}
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
 * y 72, ylast 76, crx 80, recip 84 (n..crx as words in the low half),
 * rec 88 (non-zero: record each row's span there instead of filling -
 * mode 4, rs4_run).
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
"    tst.l  88(%a0)\n"                 /* rec: record, don't fill */
"    bne    pw4_seg\n"
"    move.b 43(%a0),%d2\n"
"    btst   #5,%d2\n"
"    beq    pw0_seg\n"
"    btst   #4,%d2\n"
"    bne    pw3_seg\n"
"    PW_LOOP 2\n"
"    PW_LOOP 3\n"
"    PW_LOOP 0\n"
"    PW_LOOP 4\n"
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
".macro RS_PAGE_BODY tofs=20\n"
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
"    jmp    -\\tofs(%a2)\n"         /* the variant's right end alone */
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
/* The record core (mode 4, see ST_segRecord): a page core's calling
 * convention, but instead of filling its rows it appends each row's
 * span - the two edges' x as the row header sorts them - at the
 * state's rec pointer (offset 88), stepping the edges as a fill
 * would. */
"rs4_run:\n"
"    move.l 88(%a0),%a1\n"
"1:  move.l %a3,%d2\n"
"    move.l %a4,%d3\n"
"    swap   %d2\n"
"    swap   %d3\n"
"    cmp.w  %d3,%d2\n"
"    ble.s  2f\n"
"    exg    %d2,%d3\n"
"2:  move.w %d2,(%a1)+\n"
"    move.w %d3,(%a1)+\n"
"    add.l  4(%a0),%a3\n"
"    add.l  %a6,%a4\n"
"    dbra   %d5,1b\n"
"    move.l %a1,88(%a0)\n"
"    rts\n"
/* Replay a recorded polygon's spans (ST_segDraw). ReplayState past
 * RasterState: span 44 (the first row to fill), rows 48, xo 52 (the
 * origin's layer x, clip origin included), in the low words. */
"_segReplay:\n"                      /* (state) */
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    move.l 48(%sp),%a0\n"
"    move.l 16(%a0),%a5\n"            /* row */
"    move.l 20(%a0),%a6\n"            /* prow */
"    move.l 24(%a0),%d0\n"            /* f01 */
"    move.l 28(%a0),%d1\n"            /* f23 */
"    move.l 36(%a0),%d6\n"            /* lim */
"    move.l 44(%a0),%a3\n"            /* span */
"    move.w 50(%a0),%d7\n"            /* rows */
"    move.w 54(%a0),%a4\n"            /* xo */
"    move.w 42(%a0),%d2\n"            /* fn */
"    bsr    sp_go\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
/* The span cores: a recorded polygon is its rows' spans (rs4_run),
 * so a replay reads each row's two x where the edge cores step two
 * 16.16 edges, and there are no segments to set up - the per-segment
 * set-up cost a third as much as the rows of a cached polygon. sp_go
 * fills d7 rows (1..) from the spans at a3 for fn in d2 (colour |
 * mode << 4): d0/d1 fill, d6 lim, a4 xo, a5 row, a6 prow (a bake),
 * a0 the state (pv at 32). Page modes 2 and 3: d5 the dbra count and
 * a6 the header, which each variant's tail loops back to (sp_mtab,
 * SP_ROW). Mode 1, a shape sprite's bake, marks coverage in the
 * priority plane like rs_run, with its row count in d7's high word
 * (sp1_mtab); mode 0 never reaches the cache (cutscene pages). */
"sp_go:\n"
"    moveq  #15,%d3\n"
"    and.w  %d2,%d3\n"
"    add.w  %d3,%d3\n"
"    btst   #5,%d2\n"
"    beq.s  sp_bake\n"
"    lea    sp_mtab(%pc),%a2\n"
"    move.w (%a2,%d3.w),%d3\n"
"    lea    (%a2,%d3.w),%a2\n"
"    move.w %d7,%d5\n"
"    subq.w #1,%d5\n"
"    lea    sp3_row(%pc),%a6\n"
"    btst   #4,%d2\n"
"    bne.s  1f\n"
"    lea    sp2_row(%pc),%a6\n"
"1:  jmp    (%a6)\n"                  /* the core returns for us */
"sp_bake:\n"
"    lea    sp1_mtab(%pc),%a2\n"
"    move.w (%a2,%d3.w),%d3\n"
"    lea    (%a2,%d3.w),%a2\n"
"    move.l 32(%a0),%d5\n"            /* pv */
"    subq.w #1,%d7\n"
"    swap   %d7\n"
"sp1_row:\n"
"    move.w (%a3)+,%d2\n"
"    move.w (%a3)+,%d3\n"
"    add.w  %a4,%d2\n"
"    add.w  %a4,%d3\n"
"    cmp.w  %d6,%d3\n"                /* xhi */
"    ble.s  1f\n"
"    move.w %d6,%d3\n"
"1:  swap   %d6\n"
"    cmp.w  %d6,%d2\n"                /* xlo */
"    bge.s  2f\n"
"    move.w %d6,%d2\n"
"2:  swap   %d6\n"
"    cmp.w  %d3,%d2\n"
"    bgt.s  sp1_skip\n"
"    move.w %d2,%d4\n"
"    asr.w  #4,%d4\n"                 /* g0 */
"    move.w %d3,%d7\n"                /* (the low word: the count is high) */
"    asr.w  #4,%d7\n"
"    sub.w  %d4,%d7\n"                /* d7 = g1 - g0 */
"    lea    rs_lmask(%pc),%a1\n"
"    and.w  #15,%d2\n"
"    add.w  %d2,%d2\n"
"    add.w  %d2,%d2\n"
"    move.l (%a1,%d2.w),%d2\n"        /* left mask m:m */
"    and.w  #15,%d3\n"
"    add.w  %d3,%d3\n"
"    add.w  %d3,%d3\n"
"    move.l 64(%a1,%d3.w),%d3\n"      /* right mask (rs_rmask) */
"    add.w  %d4,%d4\n"
"    lea    (%a6,%d4.w),%a1\n"        /* prio = prow + g0 */
"    lsl.w  #2,%d4\n"
"    lea    (%a5,%d4.w),%a0\n"        /* dst = row + g0*8 */
"    move.l %d3,%d4\n"
"    tst.w  %d7\n"
"    beq.s  3f\n"
"    subq.w #1,%d7\n"                 /* full groups between the ends */
"    move.l %d2,%d3\n"
"    not.l  %d3\n"
"    jmp    (%a2)\n"
"3:  and.l  %d2,%d4\n"                /* one group: m = lm & rm */
"    jmp    -20(%a2)\n"
"sp1_skip:\n"
"    lea    128(%a5),%a5\n"
"    lea    32(%a6),%a6\n"
"    swap   %d7\n"
"    dbra   %d7,1f\n"
"    rts\n"
"1:  swap   %d7\n"
"    bra    sp1_row\n"
"sp2_row:\n"                          /* clamped to the clip */
"    move.w (%a3)+,%d2\n"
"    move.w (%a3)+,%d3\n"
"    add.w  %a4,%d2\n"
"    add.w  %a4,%d3\n"
"    cmp.w  %d6,%d3\n"
"    ble.s  1f\n"
"    move.w %d6,%d3\n"
"1:  swap   %d6\n"
"    cmp.w  %d6,%d2\n"
"    bge.s  2f\n"
"    move.w %d6,%d2\n"
"2:  swap   %d6\n"
"    cmp.w  %d3,%d2\n"
"    bgt.s  sp2_skip\n"
"    RS_PAGE_BODY 26\n"
"sp2_skip:\n"
"    lea    128(%a5),%a5\n"
"    dbra   %d5,sp2_row\n"
"    rts\n"
"sp3_row:\n"                          /* inside it: nothing to clamp */
"    move.w (%a3)+,%d2\n"
"    move.w (%a3)+,%d3\n"
"    add.w  %a4,%d2\n"
"    add.w  %a4,%d3\n"
"    RS_PAGE_BODY 26\n"
/* segHit(key, x, y, colour8): a cached polygon drawn start to finish -
 * the lookup, ST_segDraw's clipping, and the spans - for op_drawShape's
 * loop, where the C path's setup cost as much as the fill. 0 when the
 * key is not cached (the caller records it). The shape's box grows in
 * g_segCtx and is applied once per shape (ST_segShapeEnd). SegCtx
 * offsets past RasterState: layer 44, remap 48, fill01 52, fill23 56,
 * hash 60, lim 64, crx 68, cry 70, xmaxv 72, ylast 74, box xlo 76 xhi
 * 78 y0 80 y1 82, hits 84, bake 88 (a shape sprite's bake: mode 1,
 * which marks coverage in the priority plane, and prow for it). */
"    .globl _segHit\n"                  /* called from cutscene.cpp */
"_segHit:\n"
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    lea    _g_segCtx,%a0\n"
"    move.l 48(%sp),%d0\n"             /* key */
"    move.l %d0,%d1\n"
"    lsr.l  #8,%d1\n"
"    eor.l  %d0,%d1\n"
"    and.w  #255,%d1\n"
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    move.l 60(%a0),%a1\n"
"    move.l (%a1,%d1.w),%a1\n"
"1:  move.l %a1,%d1\n"
"    beq.s  sh_miss\n"
"    cmp.l  (%a1),%d0\n"
"    beq.s  2f\n"
"    move.l 4(%a1),%a1\n"
"    bra.s  1b\n"
"sh_miss:\n"
"    moveq  #0,%d0\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
"2:  addq.l #1,84(%a0)\n"
"    move.w 54(%sp),%d2\n"             /* x */
"    move.w %d2,%d3\n"
"    add.w  12(%a1),%d2\n"             /* xlo */
"    add.w  14(%a1),%d3\n"             /* xhi */
"    tst.w  %d3\n"
"    bmi    sh_done\n"                 /* wholly left of the clip */
"    cmp.w  72(%a0),%d2\n"
"    bgt    sh_done\n"                 /* wholly right of it */
"    move.w 58(%sp),%d4\n"             /* y */
"    add.w  8(%a1),%d4\n"              /* ytop */
"    move.w %d4,%d6\n"                 /* first = max(ytop, 0) */
"    bpl.s  3f\n"
"    moveq  #0,%d6\n"
"3:  move.w %d4,%d7\n"                 /* last = min(ybot, ylast) */
"    add.w  10(%a1),%d7\n"
"    subq.w #1,%d7\n"
"    cmp.w  74(%a0),%d7\n"
"    ble.s  4f\n"
"    move.w 74(%a0),%d7\n"
"4:  cmp.w  %d7,%d6\n"
"    bgt    sh_done\n"                 /* wholly above or below */
"    cmp.w  76(%a0),%d2\n"             /* the shape's box */
"    bge.s  5f\n"
"    move.w %d2,76(%a0)\n"
"5:  cmp.w  78(%a0),%d3\n"
"    ble.s  5f\n"
"    move.w %d3,78(%a0)\n"
"5:  cmp.w  80(%a0),%d6\n"
"    bge.s  5f\n"
"    move.w %d6,80(%a0)\n"
"5:  cmp.w  82(%a0),%d7\n"
"    ble.s  5f\n"
"    move.w %d7,82(%a0)\n"
"5:  move.w %d6,%d5\n"                 /* skip = first - ytop */
"    sub.w  %d4,%d5\n"
"    add.w  %d5,%d5\n"
"    add.w  %d5,%d5\n"
"    lea    16(%a1,%d5.w),%a3\n"       /* the first row's span */
"    sub.w  %d6,%d7\n"                 /* rows = last - first + 1 */
"    addq.w #1,%d7\n"
"    move.w 54(%sp),%a4\n"             /* xo = x + crx */
"    add.w  68(%a0),%a4\n"
"    add.w  70(%a0),%d6\n"             /* row = layer + (cry + first) * 128 */
"    ext.l  %d6\n"
"    lsl.l  #7,%d6\n"
"    move.l 44(%a0),%a5\n"
"    add.l  %d6,%a5\n"
"    tst.b  88(%a0)\n"                 /* a bake: prow for the priority core */
"    beq.s  9f\n"
"    lsr.l  #2,%d6\n"
"    add.l  #28672,%d6\n"              /* kSTPlaneBytes */
"    move.l 44(%a0),%a6\n"
"    add.l  %d6,%a6\n"
"9:\n"
"    moveq  #0,%d1\n"                  /* v = remap[colour8] & 15 */
"    move.b 63(%sp),%d1\n"
"    move.l 48(%a0),%a1\n"
"    move.b (%a1,%d1.w),%d1\n"
"    and.w  #15,%d1\n"
"    moveq  #0x20,%d5\n"               /* mode 3 if it cannot need clamping */
"    tst.w  %d2\n"
"    bmi.s  6f\n"
"    cmp.w  72(%a0),%d3\n"
"    bgt.s  6f\n"
"    moveq  #0x30,%d5\n"
"6:  tst.b  88(%a0)\n"
"    beq.s  8f\n"
"    moveq  #0x10,%d5\n"               /* a bake: mode 1 marks coverage */
"8:  move.w %d1,%d2\n"
"    or.w   %d5,%d2\n"                 /* fn */
"    add.w  %d1,%d1\n"
"    add.w  %d1,%d1\n"
"    move.l 56(%a0),%a1\n"
"    move.l (%a1,%d1.w),%d3\n"         /* f23 */
"    move.l 52(%a0),%a1\n"
"    move.l (%a1,%d1.w),%d0\n"         /* f01 */
"    move.l %d3,%d1\n"
"    move.l 64(%a0),%d6\n"             /* lim */
"    bsr    sp_go\n"
"sh_done:\n"
"    moveq  #1,%d0\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
/* shpBlit(data, dst, rows): a shape sprite (ST_shpBakeEnd) onto a
 * page, rows of word ops from the first group of the sprite's box:
 * n (1..0x3FFF) groups wholly covered, their 4 plane words each;
 * 0x4000, one group part covered: a keep mask and its 4 words (zero
 * where not covered); 0x8000|n, n groups not covered; 0, end of row.
 * A covered group is two moves, where the polygons it came from each
 * paid a row of edge stepping and mask set-up. */
"_shpBlit:\n"
"    movem.l %d2/%d6/%a2,-(%sp)\n"
"    move.l 16(%sp),%a0\n"             /* data */
"    move.l 20(%sp),%a1\n"             /* dst: the box's first group */
"    move.l 24(%sp),%d6\n"             /* rows */
"    subq.w #1,%d6\n"
"sb_row:\n"
"    move.l %a1,%a2\n"
"sb_op:\n"
"    move.w (%a0)+,%d0\n"
"    beq.s  sb_eol\n"
"    bmi.s  sb_skip\n"
"    cmp.w  #0x4000,%d0\n"
"    beq.s  sb_part\n"
"    subq.w #1,%d0\n"
"1:  move.l (%a0)+,(%a2)+\n"
"    move.l (%a0)+,(%a2)+\n"
"    dbra   %d0,1b\n"
"    bra.s  sb_op\n"
"sb_skip:\n"
"    and.w  #0x7FFF,%d0\n"
"    lsl.w  #3,%d0\n"
"    add.w  %d0,%a2\n"
"    bra.s  sb_op\n"
"sb_part:\n"
"    move.w (%a0)+,%d1\n"              /* keep mask, both halves */
"    move.w %d1,%d2\n"
"    swap   %d1\n"
"    move.w %d2,%d1\n"
"    move.l (%a2),%d2\n"
"    and.l  %d1,%d2\n"
"    or.l   (%a0)+,%d2\n"
"    move.l %d2,(%a2)+\n"
"    move.l (%a2),%d2\n"
"    and.l  %d1,%d2\n"
"    or.l   (%a0)+,%d2\n"
"    move.l %d2,(%a2)+\n"
"    bra.s  sb_op\n"
"sb_eol:\n"
"    lea    128(%a1),%a1\n"
"    dbra   %d6,sb_row\n"
"    movem.l (%sp)+,%d2/%d6/%a2\n"
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
".macro RS_ROW pre,p0,p1,p2,p3,pm,dst,skip\n"
"\\pre\\()_t\\p0\\p1\\p2\\p3\\pm:\n"
"    move.l %d4,%d2\n"
"    move.l %d4,%d3\n"
"    not.l  %d3\n"
"    RS_END \\p0,\\p1,\\p2,\\p3,\\pm,\\dst\n"
"    bra    \\skip\n"
"    .space 20-(.-\\pre\\()_t\\p0\\p1\\p2\\p3\\pm)\n"
"\\pre\\()_v\\p0\\p1\\p2\\p3\\pm:\n"
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
"    bra    \\pre\\()_t\\p0\\p1\\p2\\p3\\pm\n"
".endm\n"
/* A span core's page variant (see sp_go): as RS_ROW's mode 2, but the
 * right end follows the run rather than being branched to, and each
 * tail steps the row and loops back to the header in a6 itself - two
 * branches a row fewer. The right-end-only entry is 26 bytes before
 * the main one. */
".macro SP_ROW p0,p1,p2,p3\n"
"sp_t\\p0\\p1\\p2\\p3:\n"
"    move.l %d4,%d2\n"
"    move.l %d4,%d3\n"
"    not.l  %d3\n"
"    RS_END \\p0,\\p1,\\p2,\\p3,2,%a1\n"
"    lea    128(%a5),%a5\n"
"    dbra   %d5,1f\n"
"    rts\n"
"1:  jmp    (%a6)\n"
"    .space 26-(.-sp_t\\p0\\p1\\p2\\p3)\n"
"sp_v\\p0\\p1\\p2\\p3:\n"
"    RS_END \\p0,\\p1,\\p2,\\p3,2,%a1\n"
"    add.w  %d7,%d7\n"
"    add.w  %d7,%d7\n"
"    neg.w  %d7\n"
"    jmp    sp_e\\@(%pc,%d7.w)\n"
"    .rept 16\n"
"    move.l %d0,(%a1)+\n"
"    move.l %d1,(%a1)+\n"
"    .endr\n"
"sp_e\\@:\n"
"    move.l %d4,%d2\n"
"    move.l %d4,%d3\n"
"    not.l  %d3\n"
"    RS_END \\p0,\\p1,\\p2,\\p3,2,%a1\n"
"    lea    128(%a5),%a5\n"
"    dbra   %d5,1f\n"
"    rts\n"
"1:  jmp    (%a6)\n"
".endm\n"
".macro SP_TAB p0,p1,p2,p3\n"
"    .word sp_v\\p0\\p1\\p2\\p3-sp_mtab\n"
".endm\n"
".macro RS_TAB pre,base,p0,p1,p2,p3,pm\n"
"    .word \\pre\\()_v\\p0\\p1\\p2\\p3\\pm-\\base\n"
".endm\n"
".irp p3,0,1\n"
".irp p2,0,1\n"
".irp p1,0,1\n"
".irp p0,0,1\n"
"    RS_ROW rs,\\p0,\\p1,\\p2,\\p3,0,%a0,rs_skip\n"
"    RS_ROW rs,\\p0,\\p1,\\p2,\\p3,1,%a0,rs_skip\n"
"    RS_ROW rs,\\p0,\\p1,\\p2,\\p3,2,%a1,rs2_skip\n"
"    RS_ROW rs,\\p0,\\p1,\\p2,\\p3,3,%a1,rs3_skip\n"
"    RS_ROW sb,\\p0,\\p1,\\p2,\\p3,1,%a0,sp1_skip\n"
"    SP_ROW \\p0,\\p1,\\p2,\\p3\n"
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
"    RS_TAB rs,rs_mtab,\\p0,\\p1,\\p2,\\p3,\\pm\n"
".endr\n"
".endr\n"
".endr\n"
".endr\n"
".endr\n"
"sp_mtab:\n"                         /* the span cores' (sp_go) */
".irp p3,0,1\n"
".irp p2,0,1\n"
".irp p1,0,1\n"
".irp p0,0,1\n"
"    SP_TAB \\p0,\\p1,\\p2,\\p3\n"
".endr\n"
".endr\n"
".endr\n"
".endr\n"
"sp1_mtab:\n"
".irp p3,0,1\n"
".irp p2,0,1\n"
".irp p1,0,1\n"
".irp p0,0,1\n"
"    RS_TAB sb,sp1_mtab,\\p0,\\p1,\\p2,\\p3,1\n"
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
static bool g_shpBaking;   // see ST_shpBakeBegin

static inline unsigned long prioMode(uint8_t colour8, bool xInside) {
	if (g_shpBaking) {
		return 1;                 // coverage, for the sprite's mask
	}
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
	uint8_t *rec;                // record mode: where the next row's span goes
};

// segReplay's state: a recorded polygon's spans to fill and where (see
// ST_segDraw)
struct ReplayState : RasterState {
	const int16_t *span;         // the first row to fill: xl, xr from the origin
	long rows;                   // rows to fill
	long xo;                     // the origin's layer x, clip origin included
};

struct SegEntry;

// segHit's context (offsets in the asm): the page and clip of the
// shape being drawn, its colour tables, and the box its hits grow
struct SegCtx : RasterState {
	uint8_t *layer;
	const uint8_t *remap;
	const unsigned long *fill01, *fill23;
	SegEntry **hash;
	unsigned long lim;
	int16_t crx, cry, xmaxv, ylast;
	int16_t bxlo, bxhi, by0, by1;
	uint32_t hits;
	uint8_t bake, pad[3];        // fill in mode 1, marking coverage
};
extern "C" SegCtx g_segCtx;
SegCtx g_segCtx;
// the asm addresses these fields by number: the sizes pin the layout
typedef char SegCtxLayout[(sizeof(ReplayState) == 56 && sizeof(SegCtx) == 92) ? 1 : -1];

extern "C" void rasterSeg(RasterState *st, long count);
extern "C" void polyWalk(PolyState *st);
extern "C" void segReplay(ReplayState *st);
extern "C" void shpBlit(const void *data, uint8_t *dst, long rows);

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
		for (int n = 0; n < nfull; ++n) {
			dst[n * 4 + 0] = f0;
			dst[n * 4 + 1] = f1;
			dst[n * 4 + 2] = f2;
			dst[n * 4 + 3] = f3;
			prio[n] = setPrio ? 0xFFFF : 0;
		}
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
	ST_prioTouchedRect(x, y, w, h);
	pageTouch(layer, x, x + w, y, y + h);
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
	ST_prioTouchedRect(x1, y, x2 - x1 + 1, 1);
	pageTouch(layer, x1, x2 + 1, y, y + 1);
	fillRow(layer, x1, x2 - x1 + 1, y, ST_getRemap()[colour8], (colour8 & 0x80) != 0);
}

// The whole polygon in one call: walks drawPolygon's run list
// (y, then x1,x2 pairs until x1 < 0), stepping the row pointers
// incrementally. Per-row address recomputation and the call chain
// were costing more than the fills for the average 15-pixel span.
void ST_fillArea(uint8_t *layer, const int16_t *pts, int crx, int cry,
		int crw, uint8_t v, bool setPrio) {
	ST_prioTouched();
	int y = cry + *pts++;
	int x1 = *pts++;
	if (x1 < 0) {
		return;
	}
	const int yFirst = y;
	int xMin = kSTLayerW, xMax = -1;
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
				if (a < xMin) {
					xMin = a;
				}
				if (b > xMax) {
					xMax = b;
				}
			}
		}
		++y;
		row += kSTRowBytes / 2;
		prow += kSTPrioRowBytes / 2;
		x1 = *pts++;
	} while (x1 >= 0);
	pageTouch(layer, xMin, xMax + 1, yFirst, y);
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
	// the vertex box, clipped, bounds every pixel written below
	pageTouch(layer, crx + (xlo < 1 ? 0 : xlo - 1), crx + (xhi >= xmaxv ? xmaxv : xhi + 1) + 1,
		cry + (ytop < 0 ? 0 : ytop), cry + (ybot > crh - 1 ? crh - 1 : ybot) + 1);
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
	st.rec = 0;
	polyWalk(&st);
	return true;
}

/*
 * Cutscene polygon cache. A scene redraws the same polygons frame after
 * frame at new positions - 87% of what the intro and level 1's opening
 * draw had been drawn before in the same scene, 93% through the
 * intro's camera moves - and for a whole-pixel move the walk is the
 * same one, shifted: the edges carry the vertices' integer x and only
 * differences in y. So the first draw records the walk, relative to
 * the shape's origin and with no clipping, as the span of each row
 * polyWalk fills, and every later draw hands those straight to the
 * span cores (sp_go) - from op_drawShape's loop, lookup to last row
 * in segHit, because the C route's per-polygon setup cost as much as
 * the parse and walk it replaced. Output is the same pixels, because
 * the same walker produced the spans (checked against the uncached
 * draw over every level's opening scene and the credits). Measured,
 * cycle-exact: a fifth to a quarter less cutscene work on every
 * machine - the intro 34.7s -> 26.4s on an STE, 20.8s -> 15.8s on a
 * Mega STE, level 1's opening 9.6s -> 7.5s. Spans rather than the
 * walk's segments (edge positions and steps) took another 9.6% off a
 * plain ST's polygon work (the intro 15.5%): no edges to step per
 * row, and no segments to set up per polygon.
 *
 * Keyed by the primitive's vertex data, which lives as long as the
 * scene's polygon data does: ST_segCacheBegin/End bracket one played
 * pass. The arena is emptied when full rather than managed - scenes
 * reuse what they drew recently, so 32K keeps 83-85% of the hits a
 * cache of everything would, as many with spans (four bytes a row)
 * as with segments.
 */
struct SegEntry {
	const void *key;
	SegEntry *next;
	int16_t ytop, rows;          // relative to the shape's origin
	int16_t xlo, xhi;            // vertex x extent, likewise
	// rows spans follow: each row's xl <= xr (words, from the origin)
};
enum {
	kSegArena = 32 * 1024,
	kSegArenaBig = 128 * 1024,     // a 4MB machine's (see ST_segCacheBegin)
	kSegBuckets = 256,
};
static uint8_t *g_segArena;
static uint32_t g_segUsed;
static uint32_t g_segArenaSize = kSegArena;

// A scene's caches four times the size on a machine with 3.5MB or more,
// where the RAM is there when the scene starts (with 400K left over):
// a long scene then draws more of its polygons and shapes from them
// rather than walking and baking them again after a flush.
static uint32_t segArenaSize(uint32_t small, uint32_t big) {
	if (ATARIST_ramK() >= kSTResidentRamK && !(g_options.ab & kAbCutSmallArenas)
	    && ST_freeK() >= (long)(big >> 10) + 400) {
		return big;
	}
	return small;
}
static SegEntry *g_segHash[kSegBuckets];
static uint32_t g_segMisses;
static Point g_segPts[256];

typedef char SegEntryLayout[(sizeof(SegEntry) == 16) ? 1 : -1];

static inline unsigned segBucket(const void *key) {
	const uintptr_t k = (uintptr_t)key;
	return (unsigned)(k ^ (k >> 8)) & (kSegBuckets - 1);
}

static void segFlush() {
	memset(g_segHash, 0, sizeof(g_segHash));
	g_segUsed = 0;
}

static void shpInit();
static void shpRelease();

bool ST_segCacheBegin() {
	g_segMisses = 0;
	shpInit();
	g_segCtx.hits = 0;
	g_segCtx.hash = g_segHash;
	g_segCtx.fill01 = kFill01;
	g_segCtx.fill23 = kFill23;
	if (!g_segArena) {
		g_segArenaSize = segArenaSize(kSegArena, kSegArenaBig);
		g_segArena = (uint8_t *)malloc(g_segArenaSize);
	}
	segFlush();
	return g_segArena != 0;
}

void ST_segCacheEnd() {
	free(g_segArena);
	g_segArena = 0;
	shpRelease();
}

bool ST_segCacheOn() {
	return g_segArena != 0 && ST_cutscenePalMode() && !(g_options.ab & kAbCutNoSegCache);
}

// the C route to a recorded polygon, for draws segHit does not see (a
// shape sprite's bake)
const void *ST_segFind(const void *key) {
	for (SegEntry *e = g_segHash[segBucket(key)]; e; e = e->next) {
		if (e->key == key) {
			++g_segCtx.hits;
			return e;
		}
	}
	return 0;
}

void ST_segCacheStats(uint32_t *hits, uint32_t *misses) {
	*hits = g_segCtx.hits;
	*misses = g_segMisses;
}

// A shape's primitives are drawn between these: Begin fixes the page,
// clip and colour table for segHit, End applies the box its hits grew
// - pageTouch once per shape rather than per polygon, over the union
// of what they could have drawn.
void ST_segShapeBegin(uint8_t *layer, int crx, int cry, int crw, int crh) {
	SegCtx &c = g_segCtx;
	const int xmaxv = (crw < kSTLayerW - crx ? crw : kSTLayerW - crx) - 1;
	int ylast = crh - 1;
	if (ylast > kSTLayerH - 1 - cry) {
		ylast = kSTLayerH - 1 - cry;
	}
	c.layer = layer;
	c.remap = ST_getRemap();
	c.lim = ((unsigned long)crx << 16) | (unsigned)(crx + xmaxv);
	c.crx = crx;
	c.cry = cry;
	c.xmaxv = xmaxv;
	c.ylast = ylast;
	c.bxlo = c.by0 = 0x7FFF;
	c.bxhi = c.by1 = -0x7FFF;
}

/*
 * Cutscene shape sprites, the level above the polygon cache. A shape
 * (op_drawShape's unit: a character, a head, a machine) is 15-20
 * overlapping polygons, and drawn that way each of its rows pays the
 * fill's row set-up once per polygon crossing it - 420-510 cycles
 * before a pixel is written - plus the overdraw between them. So a
 * foreground shape seen a second time at the same x phase is baked:
 * its polygons are drawn into a scratch layer through the same fill
 * code, in the priority mode, which marks coverage in the priority
 * plane, and the covered groups are kept as runs (see shpBlit). A
 * wholly covered group is then two moves, a part-covered one a
 * masked write, an empty one nothing. Second sighting, because most
 * shape-and-phase pairs in a camera move are drawn once (345 of 445
 * in the intro) and a bake costs more than the draw it replaces; when
 * a bake happens is Cutscene::stShapeSprite's call.
 *
 * Measured, cycle-exact, one binary with and without: an STE's intro
 * 27.1s -> 23.2s of work, level 1's opening 7.6s -> 6.5s; a Mega
 * STE's level 1 opening 5.0s -> 4.1s and its intro unchanged (it
 * bakes only in its spare time there). With frame skipping the STE
 * drops 138 intro frames rather than 155.
 *
 * Output is the polygons' pixels: an opaque shape drawn in order onto
 * a page is the page with its covered pixels replaced by the shape's
 * own. Colours are baked through the scene part's locked remap, so a
 * new part empties the cache (ST_shpFlush). The bake is unclipped and
 * keeps the exact bounds of what it covered: a shape is a sprite in
 * any frame where those lie inside the clip, and polygons in one where
 * the clip cuts it - bounds with a pixel's margin turned away the
 * shapes that reach the clip's edge, which were most of the work. A
 * shape with a shadow polygon, or too big for the scratch, is always
 * polygons.
 */
struct ShpEntry {
	ShpEntry *next;
	uint16_t key;
	int16_t x0, x1, y0, y1;      // the pixels it covers, from the origin
	int16_t bx, by;              // the stored box's first group and row, likewise
	int16_t groups, rows, pad;
	// the ops follow
};
enum {
	kShpArena = 64 * 1024,
	kShpArenaBig = 256 * 1024,
	kShpBuckets = 128,
	kShpSeen = 512,
};
static uint8_t *g_shpArena;
static uint32_t g_shpArenaSize = kShpArena;
static uint32_t g_shpUsed;
static ShpEntry *g_shpHash[kShpBuckets];
static uint16_t g_shpSeen[kShpSeen];
// planes for kShpRows rows, and their priority rows where a layer's
// are, so the fills address it as they do a page
static uint8_t *g_shpScratch;
static bool g_shpSavedTrk;
static uint32_t g_shpDrawn, g_shpBaked;

typedef char ShpEntryLayout[(sizeof(ShpEntry) == 24) ? 1 : -1];

// the cutscene colours (0xC0-0xFF) the sprites were baked through
static uint8_t g_shpRemap[64];

void ST_shpFlush() {
	memset(g_shpHash, 0, sizeof(g_shpHash));
	memset(g_shpSeen, 0, sizeof(g_shpSeen));
	g_shpUsed = 0;
	memcpy(g_shpRemap, ST_getRemap() + 0xC0, sizeof(g_shpRemap));
}

// a new scene part locks its own colours: the sprites stay if they
// came out the same (as they often do across a scene's parts)
void ST_shpColoursChanged() {
	if (memcmp(g_shpRemap, ST_getRemap() + 0xC0, sizeof(g_shpRemap)) != 0) {
		ST_shpFlush();
	}
}

static void shpInit() {
	g_shpDrawn = g_shpBaked = 0;
	if (!g_shpArena) {
		g_shpArenaSize = segArenaSize(kShpArena, kShpArenaBig);
		g_shpArena = (uint8_t *)malloc(g_shpArenaSize);
	}
	if (!g_shpScratch) {
		g_shpScratch = (uint8_t *)calloc(1, kSTLayerSize);
	}
	ST_shpFlush();
}

static void shpRelease() {
	free(g_shpArena);
	g_shpArena = 0;
	free(g_shpScratch);
	g_shpScratch = 0;
}

bool ST_shpCacheOn() {
	return g_shpArena != 0 && g_shpScratch != 0 && ST_segCacheOn() && !(g_options.ab & kAbCutNoShapeSprites);
}

void ST_shpStats(uint32_t *drawn, uint32_t *baked) {
	*drawn = g_shpDrawn;
	*baked = g_shpBaked;
}

const void *ST_shpFind(uint16_t key) {
	for (ShpEntry *e = g_shpHash[(key ^ (key >> 7)) & (kShpBuckets - 1)]; e; e = e->next) {
		if (e->key == key) {
			return e;
		}
	}
	return 0;
}

bool ST_shpSeen(uint16_t key) {
	uint16_t &s = g_shpSeen[(key ^ (key >> 9)) & (kShpSeen - 1)];
	if (s == (uint16_t)(key + 1)) {
		return true;
	}
	s = key + 1;
	return false;
}

// The scratch layer, with box tracking off - nothing drawn there is
// on a page - and the fills marking coverage.
uint8_t *ST_shpBakeBegin() {
	g_shpSavedTrk = g_trk;
	g_trk = false;
	g_shpBaking = true;
	return g_shpScratch;
}

// Keep what the bake drew in rows [soy + y0, soy + y1] and columns
// [sox + x0, sox + x1] of the scratch (bounds with a pixel's margin,
// from the shape's origin, which the bake put at (sox, soy)), clearing
// it for the next one. Null if it cannot be kept.
const void *ST_shpBakeEnd(uint16_t key, int sox, int soy, int x0, int y0, int x1, int y1) {
	g_trk = g_shpSavedTrk;
	g_shpBaking = false;
	const int r0 = soy + y0, r1 = soy + y1;
	const int g0 = (sox + x0) >> 4, g1 = (sox + x1) >> 4;
	uint8_t *planes = g_shpScratch;
	uint8_t *prio = g_shpScratch + kSTPlaneBytes;
	// the covered box, and the covered columns of its end groups
	int tr0 = kSTLayerH, tr1 = -1, tg0 = 16, tg1 = -1;
	uint16_t colOr[16];
	memset(colOr, 0, sizeof(colOr));
	for (int r = r0; r <= r1; ++r) {
		const uint16_t *cov = (const uint16_t *)(prio + r * kSTPrioRowBytes);
		for (int g = g0; g <= g1; ++g) {
			if (cov[g]) {
				if (r < tr0) tr0 = r;
				tr1 = r;
				if (g < tg0) tg0 = g;
				if (g > tg1) tg1 = g;
				colOr[g] |= cov[g];
			}
		}
	}
	const int rows = (tr1 >= tr0) ? tr1 - tr0 + 1 : 0;
	const int groups = (tg1 >= tg0) ? tg1 - tg0 + 1 : 0;
	// at worst an op and five words per group, and an end per row
	const uint32_t need = (sizeof(ShpEntry) + rows * (groups * 12 + 2) + 3) & ~3u;
	ShpEntry *e = 0;
	if (need <= g_shpArenaSize) {
		if (g_shpUsed + need > g_shpArenaSize) {
			ST_shpFlush();
		}
		e = (ShpEntry *)(g_shpArena + g_shpUsed);
	}
	uint16_t *out = e ? (uint16_t *)(e + 1) : 0;
	for (int r = tr0; r <= tr1; ++r) {
		uint16_t *cov = (uint16_t *)(prio + r * kSTPrioRowBytes);
		uint16_t *pl = (uint16_t *)(planes + r * kSTRowBytes);
		uint16_t *run = 0;       // the open run of covered groups
		int skip = 0;
		for (int g = tg0; g <= tg1; ++g) {
			const uint16_t c = cov[g];
			if (c == 0) {
				++skip;
				run = 0;
				continue;
			}
			uint16_t *w = pl + g * 4;
			if (out) {
				if (skip) {
					*out++ = 0x8000 | skip;
					skip = 0;
				}
				if (c == 0xFFFF) {
					if (run) {
						++*run;
					} else {
						run = out;
						*out++ = 1;
					}
				} else {
					run = 0;
					*out++ = 0x4000;
					*out++ = (uint16_t)~c;
				}
				*out++ = w[0];
				*out++ = w[1];
				*out++ = w[2];
				*out++ = w[3];
			}
			w[0] = w[1] = w[2] = w[3] = 0;
			cov[g] = 0;
		}
		if (out) {
			*out++ = 0;
		}
	}
	if (!e) {
		return 0;
	}
	e->key = key;
	if (rows > 0) {
		int lead = 0, trail = 0;
		while (!(colOr[tg0] & (0x8000 >> lead))) ++lead;
		while (!(colOr[tg1] & (1 << trail))) ++trail;
		e->x0 = tg0 * 16 + lead - sox;
		e->x1 = tg1 * 16 + 15 - trail - sox;
		e->y0 = tr0 - soy;
		e->y1 = tr1 - soy;
	} else {
		e->x0 = e->y0 = 0;
		e->x1 = e->y1 = -1;
	}
	e->bx = tg0 * 16 - sox;
	e->by = tr0 - soy;
	e->groups = groups;
	e->rows = rows;
	const unsigned b = (key ^ (key >> 7)) & (kShpBuckets - 1);
	e->next = g_shpHash[b];
	g_shpHash[b] = e;
	g_shpUsed += ((uint8_t *)out - (uint8_t *)e + 3) & ~3u;
	++g_shpBaked;
	return e;
}

// Blit a shape with its origin at (x, y) in the clip rect; false when
// the clip would cut it (the caller draws it as polygons).
bool ST_shpDraw(uint8_t *layer, const void *sprite, int x, int y,
		int crx, int cry, int crw, int crh) {
	const ShpEntry *e = (const ShpEntry *)sprite;
	if (e->rows > 0 && (x + e->x0 < 0 || x + e->x1 > crw - 1 || y + e->y0 < 0 || y + e->y1 > crh - 1)) {
		return false;
	}
	if (e->rows > 0) {
		const int ax = x + crx + e->bx;          // a group boundary: bx keeps the phase
		const int ay = cry + y + e->by;
		shpBlit(e + 1, layer + ay * kSTRowBytes + (ax >> 4) * 8, e->rows);
		pageTouch(layer, ax, ax + e->groups * 16, ay, ay + e->rows);
	}
	++g_shpDrawn;
	return true;
}

// segHit's fills for a shape sprite's bake (see ST_shpBakeBegin)
void ST_segBakeMode(bool on) {
	g_segCtx.bake = on;
	g_segCtx.pv = on ? 0xFFFFul : 0ul;
}

void ST_segShapeEnd() {
	const SegCtx &c = g_segCtx;
	if (c.by0 > c.by1) {
		return;
	}
	pageTouch(c.layer, c.crx + (c.bxlo < 1 ? 0 : c.bxlo - 1),
		c.crx + (c.bxhi >= c.xmaxv ? c.xmaxv : c.bxhi + 1) + 1,
		c.cry + c.by0, c.cry + c.by1 + 1);
}

// Record a polygon given relative to its shape's origin; the three
// kinds ST_drawPolygonFast draws, the same way. Null when it is not
// one of them, or cannot fit.
const void *ST_segRecord(const void *key, const void *ptsv, int n) {
	const Point *pts = (const Point *)ptsv;
	if (!g_segArena || n < 2 || n > (int)(sizeof(g_segPts) / sizeof(g_segPts[0]))) {
		return 0;
	}
	++g_segMisses;
	initRecip16();
	int imin = 0, imax = 0;
	int16_t xlo = pts[0].x, xhi = xlo;
	int16_t ytop = pts[0].y, ybot = ytop;
	for (int i = 1; i < n; ++i) {
		const int16_t x = pts[i].x, y = pts[i].y;
		if (y < ytop) { ytop = y; imin = i; }
		if (y > ybot) { ybot = y; imax = i; }
		if (x < xlo) xlo = x;
		if (x > xhi) xhi = x;
	}
	// a span per row
	const int rows = ybot - ytop + 1;
	const uint32_t need = (sizeof(SegEntry) + rows * 4 + 3) & ~3u;
	if (need > g_segArenaSize) {
		return 0;
	}
	if (g_segUsed + need > g_segArenaSize) {
		segFlush();
	}
	SegEntry *e = (SegEntry *)(g_segArena + g_segUsed);
	int16_t *span = (int16_t *)(e + 1);
	if (ytop == ybot) {
		span[0] = xlo;
		span[1] = xhi;
	} else if (n == 2) {
		// as the line path: a trapezoid row per row down to the one
		// above the far end, then the far end's pixel
		const int dy = ybot - ytop;
		const int32_t step = edgeStep(pts[imax].x - pts[imin].x, dy);
		int32_t fa = (int32_t)pts[imin].x << 16, fb = fa + step;
		for (int r = 0; r < dy; ++r) {
			const int16_t a = (int16_t)(fa >> 16), b = (int16_t)(fb >> 16);
			*span++ = a < b ? a : b;
			*span++ = a < b ? b : a;
			fa += step;
			fb += step;
		}
		span[0] = span[1] = pts[imax].x;
	} else {
		// the walker's own pass, from row 0 so nothing is above its
		// clip, filling nothing
		for (int i = 0; i < n; ++i) {
			g_segPts[i].x = pts[i].x;
			g_segPts[i].y = pts[i].y - ytop;
		}
		PolyState st;
		memset(&st, 0, sizeof(st));
		st.fn = 2 << 4;
		st.fa = st.fb = (int32_t)pts[imin].x << 16;
		st.pts = g_segPts;
		st.n = n;
		st.imax = imax;
		st.ia = st.ib = imin;
		st.ylast = ybot - ytop;
		st.recip = g_recip16;
		st.rec = (uint8_t *)span;
		polyWalk(&st);
	}
	e->key = key;
	e->ytop = ytop;
	e->rows = rows;
	e->xlo = xlo;
	e->xhi = xhi;
	const unsigned b = segBucket(key);
	e->next = g_segHash[b];
	g_segHash[b] = e;
	g_segUsed += need;
	return e;
}

// Draw a recorded polygon with its origin at (x, y) in the clip rect:
// ST_drawPolygonFast's clipping and box, then the fill cores.
void ST_segDraw(uint8_t *layer, const void *entry, int x, int y, uint8_t colour8,
		int crx, int cry, int crw, int crh) {
	const SegEntry *e = (const SegEntry *)entry;
	const int xlo = e->xlo + x, xhi = e->xhi + x;
	const int ytop = e->ytop + y, ybot = ytop + e->rows - 1;
	const int xmaxv = (crw < kSTLayerW - crx ? crw : kSTLayerW - crx) - 1;
	pageTouch(layer, crx + (xlo < 1 ? 0 : xlo - 1), crx + (xhi >= xmaxv ? xmaxv : xhi + 1) + 1,
		cry + (ytop < 0 ? 0 : ytop), cry + (ybot > crh - 1 ? crh - 1 : ybot) + 1);
	int ylast = crh - 1;
	if (ylast > kSTLayerH - 1 - cry) {
		ylast = kSTLayerH - 1 - cry;
	}
	if (ylast > ybot) {
		ylast = ybot;
	}
	const int first = (ytop < 0) ? 0 : ytop;
	if (first > ylast) {
		return;
	}
	const uint8_t v = ST_getRemap()[colour8] & 15;
	ReplayState st;
	st.f01 = kFill01[v];
	st.f23 = kFill23[v];
	st.pv = (colour8 & 0x80) ? 0xFFFFul : 0ul;
	st.lim = ((unsigned long)crx << 16) | (unsigned)(crx + xmaxv);
	st.fn = v | (prioMode(colour8, xlo >= 0 && xhi <= xmaxv) << 4);
	st.row = (uint16_t *)(layer + (cry + first) * kSTRowBytes);
	st.prow = (uint16_t *)(layer + kSTPlaneBytes + (cry + first) * kSTPrioRowBytes);
	st.span = (const int16_t *)(e + 1) + 2 * (first - ytop);
	st.rows = ylast - first + 1;
	st.xo = x + crx;
	segReplay(&st);
}

// Row fill with the colour already remapped: fillArea resolves the
// polygon colour once instead of per span (a cutscene frame emits
// hundreds of spans, and the remap fetch was pure overhead on each)
void ST_hspanV(uint8_t *layer, int x1, int x2, int y, uint8_t v, bool setPrio) {
	ST_prioTouched();
	if (y < 0 || y >= kSTLayerH) {
		return;
	}
	if (x1 < 0) x1 = 0;
	if (x2 >= kSTLayerW) x2 = kSTLayerW - 1;
	if (x1 > x2) {
		return;
	}
	pageTouch(layer, x1, x2 + 1, y, y + 1);
	fillRow(layer, x1, x2 - x1 + 1, y, v, setPrio);
}

// The chunky code's cutscene "shadow" effect ORs the colour's slot
// bits into the underlying pixel (*dst |= colour & 0xF8), selecting
// the artist's shadow variant of whatever is underneath. In hardware
// colour space that becomes a per-slot lookup (ST_getOrMap): read
// each covered pixel, translate, write back. Outside cutscene mode
// (no representative table) it falls back to an opaque fill.
void ST_hspanOr(uint8_t *layer, int x1, int x2, int y, uint8_t colour8) {
	ST_prioTouched();
	if (y < 0 || y >= kSTLayerH) {
		return;
	}
	if (x1 < 0) x1 = 0;
	if (x2 >= kSTLayerW) x2 = kSTLayerW - 1;
	if (x1 > x2) {
		return;
	}
	pageTouch(layer, x1, x2 + 1, y, y + 1);
	if (!ST_cutscenePalMode()) {
		fillRow(layer, x1, x2 - x1 + 1, y, ST_getRemap()[colour8], (colour8 & 0x80) != 0);
		return;
	}
	uint8_t orMap[16];
	ST_getOrMap(colour8, orMap);
	// The values the map moves, and the plane bits each move flips:
	// all sixteen pixels of a group are translated together, a value
	// at a time - the pixels holding value v are the AND of the four
	// planes (or their complements, by v's bits), and those pixels'
	// planes are XORed by v ^ orMap[v]. Values the map leaves alone
	// cost nothing. Pixel by pixel this was ~127 cycles a pixel: a
	// full-width shadow row 3.8ms, 2.2s of level 4's second scene.
	uint8_t mv[16], flip[16];
	int nMoved = 0;
	for (int v = 0; v < 16; ++v) {
		if (orMap[v] != v) {
			mv[nMoved] = (uint8_t)v;
			flip[nMoved] = (uint8_t)(orMap[v] ^ v);
			++nMoved;
		}
	}
	uint16_t *dst = groupPtr(layer, x1, y);
	uint16_t *prio = prioPtr(layer, x1, y);
	for (int g = x1 >> 4; g <= (x2 >> 4); ++g) {
		const uint16_t m = groupMask(g, x1, x2);
		const uint16_t p0 = dst[0], p1 = dst[1], p2 = dst[2], p3 = dst[3];
		uint16_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
		for (int k = 0; k < nMoved; ++k) {
			const int v = mv[k];
			const uint16_t sel = (uint16_t)(((v & 1) ? p0 : ~p0) & ((v & 2) ? p1 : ~p1)
			                    & ((v & 4) ? p2 : ~p2) & ((v & 8) ? p3 : ~p3));
			const int f = flip[k];
			if (f & 1) d0 |= sel;
			if (f & 2) d1 |= sel;
			if (f & 4) d2 |= sel;
			if (f & 8) d3 |= sel;
		}
		dst[0] = (uint16_t)(p0 ^ (d0 & m));
		dst[1] = (uint16_t)(p1 ^ (d1 & m));
		dst[2] = (uint16_t)(p2 ^ (d2 & m));
		dst[3] = (uint16_t)(p3 ^ (d3 & m));
		*prio |= m;
		dst += 4;
		++prio;
	}
}

void ST_drawPoint(uint8_t *layer, int x, int y, uint8_t colour8) {
	ST_prioTouched();
	if (x < 0 || x >= kSTLayerW || y < 0 || y >= kSTLayerH) {
		return;
	}
	pageTouch(layer, x, x + 1, y, y + 1);
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

// copyBlock for a length known only at run time (a multiple of 16,
// under 3MB: the burst count is a divu.w, not a __divsi3 call)
static void copyBytes(uint8_t *dst, const uint8_t *src, int bytes) {
#ifdef __m68k__
	int bursts = STDL_DivU16((uint32_t)bytes, 48);
	const int rest = bytes - (bursts << 5) - (bursts << 4);
	if (bursts > 0) {
		__asm__ volatile(
			"subq.w #1,%2\n"
			"1:\n\t"
			"movem.l (%1)+,%%d1-%%d7/%%a2-%%a6\n\t"
			"movem.l %%d1-%%d7/%%a2-%%a6,(%0)\n\t"
			"lea 48(%0),%0\n\t"
			"dbra %2,1b"
			: "+a"(dst), "+a"(src), "+d"(bursts)
			:
			: "d1","d2","d3","d4","d5","d6","d7",
			  "a2","a3","a4","a5","a6","memory","cc");
	}
	const uint32_t *s32 = (const uint32_t *)src;
	uint32_t *d32 = (uint32_t *)dst;
	for (int n = rest >> 2; --n >= 0; ) {
		*d32++ = *s32++;
	}
#else
	memcpy(dst, src, bytes);
#endif
}

// bytes (a multiple of 16) of the long pair a, b repeated: movem
// stores downward from the end, 48 bytes a burst
static void fillBytes(uint8_t *dst, int bytes, uint32_t a, uint32_t b) {
	uint32_t *d32 = (uint32_t *)dst;
#ifdef __m68k__
	int bursts = STDL_DivU16((uint32_t)bytes, 48);   // a divu.w, not __divsi3
	const int tail = (bytes - (bursts << 5) - (bursts << 4)) >> 2;
#else
	int bursts = bytes / 48;
	const int tail = (bytes % 48) / 4;
#endif
	// even: 48 and 16 keep the pair phase
	for (int n = tail / 2; --n >= 0; ) {
		*d32++ = a;
		*d32++ = b;
	}
#ifdef __m68k__
	if (bursts > 0) {
		uint8_t *end = (uint8_t *)d32 + bursts * 48;
		register uint32_t ra __asm__("d1") = a;
		register uint32_t rb __asm__("d2") = b;
		__asm__ volatile(
			"move.l %%d1,%%d3\n\t"
			"move.l %%d2,%%d4\n\t"
			"move.l %%d1,%%d5\n\t"
			"move.l %%d2,%%d6\n\t"
			"move.l %%d1,%%d7\n\t"
			"move.l %%d2,%%a2\n\t"
			"move.l %%d1,%%a3\n\t"
			"move.l %%d2,%%a4\n\t"
			"move.l %%d1,%%a5\n\t"
			"move.l %%d2,%%a6\n\t"
			"subq.w #1,%1\n"
			"1:\n\t"
			"movem.l %%d1-%%d7/%%a2-%%a6,-(%0)\n\t"
			"dbra %1,1b"
			: "+a"(end), "+d"(bursts), "+d"(ra), "+d"(rb)
			:
			: "d3","d4","d5","d6","d7","a2","a3","a4","a5","a6","memory","cc");
	}
#else
	for (int n = bytes / 8 - tail / 2; --n >= 0; ) {
		*d32++ = a;
		*d32++ = b;
	}
#endif
}

void ST_layerSaveRect(const uint8_t *layer, int x, int y, int h, uint32_t *out) {
	uint8_t *l = (uint8_t *)layer;
	for (int j = y; j < y + h; ++j, out += 5) {
		copyRowN<4>(out, (const uint32_t *)groupPtr(l, x, j));
		out[4] = *(const uint32_t *)prioPtr(l, x, j);
	}
}

void ST_layerLoadRect(uint8_t *layer, int x, int y, int h, const uint32_t *in) {
	ST_prioTouchedRect(x & ~15, y, 32, h);
	for (int j = y; j < y + h; ++j, in += 5) {
		copyRowN<4>((uint32_t *)groupPtr(layer, x, j), in);
		*(uint32_t *)prioPtr(layer, x, j) = in[4];
	}
}

void ST_layerCopyRect(uint8_t *dst, const uint8_t *src, int x, int y, int h) {
	uint8_t *s = (uint8_t *)src;
	ST_prioTouchedRect(x & ~15, y, 32, h);
	ST_copyGroupRows((uint8_t *)groupPtr(dst, x, y), kSTRowBytes, (const uint8_t *)groupPtr(s, x, y), kSTRowBytes, 2, h);
	for (int j = y; j < y + h; ++j) {
		*(uint32_t *)prioPtr(dst, x, j) = *(const uint32_t *)prioPtr(s, x, j);
	}
}

void ST_copyLayer(uint8_t *dst, const uint8_t *src) {
	ST_prioTouched();
	copyBlock<kSTPlaneBytes + kSTPrioRowBytes * kSTLayerH>(dst, src);
}

bool ST_copyGroupRows(uint8_t *d, int dStride, const uint8_t *s, int sStride, int groups, int h) {
	switch (groups) {
	case 1: copyRowsN<2>(d, dStride, s, sStride, h); return true;
	case 2: copyRowsN<4>(d, dStride, s, sStride, h); return true;
	case 3: copyRowsN<6>(d, dStride, s, sStride, h); return true;
	case 4: copyRowsN<8>(d, dStride, s, sStride, h); return true;
	case 5: copyRowsN<10>(d, dStride, s, sStride, h); return true;
	case 6: copyRowsN<12>(d, dStride, s, sStride, h); return true;
	case 7: copyRowsN<14>(d, dStride, s, sStride, h); return true;
	case 8: copyRowsN<16>(d, dStride, s, sStride, h); return true;
	case 9: copyRowsN<18>(d, dStride, s, sStride, h); return true;
	case 10: copyRowsN<20>(d, dStride, s, sStride, h); return true;
	case 11: copyRowsN<22>(d, dStride, s, sStride, h); return true;
	case 12: copyRowsN<24>(d, dStride, s, sStride, h); return true;
	case 13: copyRowsN<26>(d, dStride, s, sStride, h); return true;
	case 14: copyRowsN<28>(d, dStride, s, sStride, h); return true;
	case 15: copyRowsN<30>(d, dStride, s, sStride, h); return true;
	case 16: copyRowsN<32>(d, dStride, s, sStride, h); return true;
	}
	return false;
}

uint32_t g_stCutCopyGR;

// Copy a box, clipped to the band: the whole width as one run of
// rows, anything narrower its exact width a row at a time through
// copyRowN's movem bursts. Boxes of 10 groups or more used to be
// copied as whole rows (the narrower ones a long at a time in C),
// paying for up to 6 groups of clean pixels a row.
static void copyBox(uint8_t *dst, const uint8_t *src, const TrkBox *b) {
	int y0 = b->y0 < g_bandY0 ? g_bandY0 : b->y0;
	int y1 = b->y1 > g_bandY1 ? g_bandY1 : b->y1;
	if (y0 >= y1 || b->g0 >= b->g1) {
		return;
	}
	const int n = b->g1 - b->g0;
	g_stCutCopyGR += STDL_MulU16((uint16_t)n, (uint16_t)(y1 - y0));
	if (n == kSTLayerW / 16) {
		const int off = y0 * kSTRowBytes;
		copyBytes(dst + off, src + off, (y1 - y0) * kSTRowBytes);
		return;
	}
	const int off = y0 * kSTRowBytes + b->g0 * 8;
	ST_copyGroupRows(dst + off, kSTRowBytes, src + off, kSTRowBytes, n, y1 - y0);
}

static void markBand(TrkBox *b) {
	boxAdd(b, 0, 16, g_bandY0, g_bandY1);
}

// Cutscene page copy: the planes only. The scene's pages carry no
// live priority plane (see prioMode) and the copy is a fifth shorter
// without it. Inside a scene's band every copy is between tracked
// pages and moves only a box (see ST_pageBandStart); outside one the
// band is the whole page.
void ST_copyPage(uint8_t *dst, const uint8_t *src) {
	if (g_trk) {
		TrkBox *bd = trkBoxOf(dst);
		TrkBox *bs = trkBoxOf(src);
		if (src == g_trkAux && bd) {
			// the restore: put back what this page drew over
			copyBox(dst, src, bd);
			boxClear(bd);
			return;
		}
		if (dst == g_trkAux && bs) {
			// a new background: it moves only where src differed,
			// and everything else now differs from it there too
			copyBox(dst, src, bs);
			boxUnion(&g_trkBox[bs == &g_trkBox[0] ? 1 : 0], bs);
			boxUnion(&g_scrBox, bs);
			boxClear(bs);
			return;
		}
		if (bd && bs) {
			// page to page: they differ only where either differs
			// from the background
			TrkBox both = *bd;
			boxUnion(&both, bs);
			copyBox(dst, src, &both);
			*bd = *bs;
			return;
		}
	}
	copyBlock<kSTPlaneBytes>(dst, src);
}

void ST_pageBandStart(int y0, int y1, uint8_t colour8, uint8_t *front, uint8_t *back, uint8_t *aux) {
	g_bandY0 = (int16_t)(y0 < 0 ? 0 : y0);
	g_bandY1 = (int16_t)(y1 > kSTLayerH ? kSTLayerH : y1);
	g_bandFill = ST_getRemap()[colour8];
	g_trk = true;
	g_trkPage[0] = front;
	g_trkPage[1] = back;
	g_trkAux = aux;
	boxClear(&g_trkBox[0]);
	boxClear(&g_trkBox[1]);
	boxClear(&g_scrBox);
}

void ST_pageBandEnd() {
	g_bandY0 = 0;
	g_bandY1 = kSTLayerH;
	g_bandFill = -1;
	g_trk = false;
	g_trkPage[0] = g_trkPage[1] = g_trkAux = 0;
}

bool ST_pageShowRect(const uint8_t *page, bool full, int *x, int *y, int *w, int *h) {
	// untracked (no band in force): the whole page, as a band would be
	const TrkBox *bp = g_trk ? trkBoxOf(page) : 0;
	TrkBox r;
	if (full || !bp) {
		r.g0 = 0; r.g1 = 16; r.y0 = 0; r.y1 = kSTLayerH;
	} else {
		r = *bp;
		boxUnion(&r, &g_scrBox);
		if (r.y0 < g_bandY0) r.y0 = g_bandY0;
		if (r.y1 > g_bandY1) r.y1 = g_bandY1;
	}
	if (bp) {
		g_scrBox = *bp;
	}
	if (r.y0 >= r.y1 || r.g0 >= r.g1) {
		return false;
	}
	// the exact width: every width has a movem path now (copyRowN),
	// so widening a wide box to whole rows only copies clean groups
	*x = r.g0 * 16;
	*w = (r.g1 - r.g0) * 16;
	*y = r.y0;
	*h = r.y1 - r.y0;
	return true;
}

void ST_clearLayer(uint8_t *layer, uint8_t colour8) {
	ST_prioTouched();
	const uint8_t v = ST_getRemap()[colour8];
	uint16_t row[4];
	row[0] = (v & 1) ? 0xFFFF : 0;
	row[1] = (v & 2) ? 0xFFFF : 0;
	row[2] = (v & 4) ? 0xFFFF : 0;
	row[3] = (v & 8) ? 0xFFFF : 0;
	const uint32_t a = ((uint32_t)row[0] << 16) | row[1];
	const uint32_t b = ((uint32_t)row[2] << 16) | row[3];
	if (g_bandFill >= 0 && v != g_bandFill) {
		// the rows outside the band are in the old colour on every
		// page: from here on the band is the whole page
		g_bandY0 = 0;
		g_bandY1 = kSTLayerH;
	}
	const int off = g_bandY0 * kSTRowBytes;
	fillBytes(layer + off, (g_bandY1 - g_bandY0) * kSTRowBytes, a, b);
	if (g_trk) {
		// nothing is known about how a cleared page compares with
		// the background; a cleared background, likewise the others
		if (layer == g_trkAux) {
			markBand(&g_trkBox[0]);
			markBand(&g_trkBox[1]);
			markBand(&g_scrBox);
		} else if (TrkBox *b = trkBoxOf(layer)) {
			markBand(b);
		}
	}
	if (!ST_cutscenePalMode()) {         // cutscene pages: no live plane
		memset(layer + kSTPlaneBytes, (colour8 & 0x80) ? 0xFF : 0, kSTPrioRowBytes * kSTLayerH);
	}
}

#endif // ATARIST
