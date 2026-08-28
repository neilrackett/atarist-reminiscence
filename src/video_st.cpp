
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

// Positioned-pattern table: for pixel position i and 4-bit value v,
// the plane bits that pixel contributes, packed two planes to a long.
// Building the four plane words a bit at a time cost five
// accumulators and about 220 cycles a pixel - gcc 4.6 spills that
// shape - where an indexed OR of two longs costs a fraction.
struct CvtCell { uint32_t d01, d23; };
static CvtCell g_cvt[16][16];
static bool g_cvtReady;

static void initCvt() {
	for (int i = 0; i < 16; ++i) {
		const uint16_t bit = (uint16_t)(1 << (15 - i));
		for (int v = 0; v < 16; ++v) {
			const uint16_t p0 = (v & 1) ? bit : 0;
			const uint16_t p1 = (v & 2) ? bit : 0;
			const uint16_t p2 = (v & 4) ? bit : 0;
			const uint16_t p3 = (v & 8) ? bit : 0;
			g_cvt[i][v].d01 = ((uint32_t)p0 << 16) | p1;
			g_cvt[i][v].d23 = ((uint32_t)p2 << 16) | p3;
		}
	}
	g_cvtReady = true;
}

void ST_convertChunky(uint8_t *layer, const uint8_t *src, int h) {
	if (!g_cvtReady) {
		initCvt();
	}
	const uint8_t *remap = ST_getRemap();
	uint16_t *dst = (uint16_t *)layer;
	uint16_t *prio = (uint16_t *)(layer + kSTPlaneBytes);
	for (int y = 0; y < h; ++y) {
		for (int g = 0; g < kSTLayerW / 16; ++g) {
			uint32_t a01 = 0, a23 = 0;
			uint16_t pr = 0;
			const CvtCell *row = &g_cvt[0][0];
			for (int i = 0; i < 16; ++i) {
				const uint8_t c = *src++;
				const CvtCell *cell = row + remap[c];
				a01 |= cell->d01;
				a23 |= cell->d23;
				pr = (uint16_t)(pr + pr + (c >> 7));
				row += 16;
			}
			dst[0] = (uint16_t)(a01 >> 16);
			dst[1] = (uint16_t)a01;
			dst[2] = (uint16_t)(a23 >> 16);
			dst[3] = (uint16_t)a23;
			*prio++ = pr;
			dst += 4;
		}
	}
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
/* Run one polygon edge segment: count rows with both edges linear.
 * All stepping state lives in the caller's RasterState, which this
 * routine advances in place - fa/fb, row and prio pointers walk on
 * across segments with no C-side arithmetic (a segment averages
 * under three rows, so per-call multiplies were most of the bill).
 * struct offsets: fa 0, sa 4, fb 8, sb 12, row 16, prow 20, f01 24,
 * f23 28, pv 32, crx 36, xmax 40 (all longs).
 * args: 4(sp) = state, 8(sp) = count */
"    .even\n"
"_rasterSeg:\n"
"    movem.l %d2-%d7/%a2,-(%sp)\n"
"    move.l 32(%sp),%a2\n"           /* state */
"    move.l 36(%sp),%d6\n"           /* count */
"rs_row:\n"
"    move.l (%a2),%d4\n"             /* fa */
"    move.l 8(%a2),%d5\n"            /* fb */
"    swap   %d4\n"
"    swap   %d5\n"
"    cmp.w  %d5,%d4\n"
"    ble.s  rs_ord\n"
"    exg    %d4,%d5\n"
"rs_ord:\n"
"    tst.w  %d4\n"
"    bge.s  rs_lo\n"
"    moveq  #0,%d4\n"
"rs_lo:\n"
"    cmp.w  42(%a2),%d5\n"           /* xmax (low word) */
"    ble.s  rs_hi\n"
"    move.w 42(%a2),%d5\n"
"rs_hi:\n"
"    cmp.w  %d5,%d4\n"
"    bgt.s  rs_skip\n"
"    add.w  38(%a2),%d4\n"           /* + crx */
"    add.w  38(%a2),%d5\n"
"    move.w %d4,%d2\n"
"    ext.l  %d2\n"
"    move.w %d5,%d3\n"
"    ext.l  %d3\n"
"    move.l 16(%a2),%a0\n"           /* row  */
"    move.l 20(%a2),%a1\n"           /* prow */
"    move.l 24(%a2),%d0\n"           /* f01  */
"    move.l 28(%a2),%d1\n"           /* f23  */
"    move.l 32(%a2),%d5\n"           /* pv   */
"    move.l %d6,-(%sp)\n"            /* count survives the core */
"    bsr    fra_core\n"
"    move.l (%sp)+,%d6\n"
"rs_skip:\n"
"    move.l 4(%a2),%d4\n"
"    add.l  %d4,(%a2)\n"             /* fa += sa */
"    move.l 12(%a2),%d4\n"
"    add.l  %d4,8(%a2)\n"            /* fb += sb */
"    add.l  #128,16(%a2)\n"          /* row += one line */
"    moveq  #32,%d4\n"
"    add.l  %d4,20(%a2)\n"           /* prow += one line */
"    subq.l #1,%d6\n"
"    bne.s  rs_row\n"
"    movem.l (%sp)+,%d2-%d7/%a2\n"
"    rts\n"
);

// (dx << 16) / dy without __divsi3: dy is 1..255 on these shapes,
// so dx * (65536/dy) as a hardware 16x16 multiply is exact to under
// 0.005 pixel per row - and every edge change reloads the exact
// vertex, so error cannot accumulate across edges.
static uint16_t g_recip16[256];

static void initRecip16() {
	if (g_recip16[1] == 0) {
		g_recip16[1] = 0xFFFF;              // 65536 clipped, <1px/224 rows
		for (int dy = 2; dy < 256; ++dy) {
			g_recip16[dy] = (uint16_t)(65536 / dy);
		}
	}
}

static inline int32_t edgeStep(int dx, int dy) {
	if (dy >= 256) {                        // taller than any shape row
		return ((int32_t)dx << 16) / dy;    // rare: keep it exact
	}
	return (int32_t)((int16_t)dx * (int32_t)g_recip16[dy]);
}

struct RasterState {
	int32_t fa, sa, fb, sb;
	uint16_t *row;
	uint16_t *prow;
	unsigned long f01, f23, pv;
	long crx, xmax;
};

extern "C" void rasterSeg(RasterState *st, long count);

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
bool ST_drawPolygonFast(uint8_t *layer, const void *ptsv, int n,
		uint8_t colour8, int crx, int cry, int crw, int crh) {
	const Point *pts = (const Point *)ptsv;
	if (n < 2) {
		return false;
	}
	initRecip16();
	int imin = 0, imax = 0;
	for (int i = 1; i < n; ++i) {
		if (pts[i].y < pts[imin].y) imin = i;
		if (pts[i].y > pts[imax].y) imax = i;
	}
	const int ytop = pts[imin].y;
	const int ybot = pts[imax].y;
	if (ytop == ybot) {
		// flat: one row across the x extent
		const int sy = cry + ytop;
		if ((unsigned)sy >= (unsigned)kSTLayerH
		    || (unsigned)ytop >= (unsigned)crh) {
			return true;
		}
		int xlo = pts[0].x, xhi = pts[0].x;
		for (int i = 1; i < n; ++i) {
			if (pts[i].x < xlo) xlo = pts[i].x;
			if (pts[i].x > xhi) xhi = pts[i].x;
		}
		const int xmaxv = (crw < kSTLayerW - crx ? crw : kSTLayerW - crx) - 1;
		if (xlo < 0) xlo = 0;
		if (xhi > xmaxv) xhi = xmaxv;
		if (xlo > xhi) {
			return true;
		}
		const uint8_t fv = ST_getRemap()[colour8];
		ST_hspanV(layer, crx + xlo, crx + xhi, sy, fv,
			(colour8 & 0x80) != 0);
		return true;
	}

	if (n == 2) {
		// A line, drawn as a one-row-step-wide trapezoid: each row
		// fills the horizontal run the line crosses, like a solid
		// Bresenham. Mid-line rows span [f, f+step]; the final row
		// lands exactly on the far endpoint.
		const uint8_t fv = ST_getRemap()[colour8];
		const bool sp = (colour8 & 0x80) != 0;
		const int xmaxv = (crw < kSTLayerW - crx ? crw : kSTLayerW - crx) - 1;
		int ylast = crh - 1;
		if (ylast > kSTLayerH - 1 - cry) {
			ylast = kSTLayerH - 1 - cry;
		}
		const int dy = ybot - ytop;
		RasterState st;
		st.sa = st.sb = edgeStep(pts[imax].x - pts[imin].x, dy);
		st.fa = (int32_t)pts[imin].x << 16;
		st.fb = st.fa + st.sa;
		st.f01 = ((fv & 1) ? 0xFFFF0000ul : 0) | ((fv & 2) ? 0xFFFFul : 0);
		st.f23 = ((fv & 4) ? 0xFFFF0000ul : 0) | ((fv & 8) ? 0xFFFFul : 0);
		st.pv = sp ? 0xFFFFul : 0ul;
		st.crx = crx;
		st.xmax = xmaxv;
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
		st.row = (uint16_t *)(layer + (cry + y) * kSTRowBytes);
		st.prow = (uint16_t *)(layer + kSTPlaneBytes + (cry + y) * kSTPrioRowBytes);
		if (count > 0) {
			rasterSeg(&st, count);
		}
		if (ybot >= 0 && ybot <= ylast) {
			int xe = pts[imax].x;
			if (xe >= 0 && xe <= xmaxv) {
				ST_hspanV(layer, crx + xe, crx + xe, cry + ybot, fv, sp);
			}
		}
		return true;
	}

	// chain a walks backwards through the vertex list, chain b
	// forwards; both start at the top vertex
	int ia = imin, ib = imin;
	int ya = ytop, yb = ytop;            // y where each edge ends
	int32_t fa = (int32_t)pts[imin].x << 16, fb = fa;
	int32_t sa = 0, sb = 0;

	const uint8_t v = ST_getRemap()[colour8];
	const bool setPrio = (colour8 & 0x80) != 0;
	const uint16_t f0 = (v & 1) ? 0xFFFF : 0;
	const uint16_t f1 = (v & 2) ? 0xFFFF : 0;
	const uint16_t f2 = (v & 4) ? 0xFFFF : 0;
	const uint16_t f3 = (v & 8) ? 0xFFFF : 0;

	int y = ytop;
	const int xmaxv = (crw < kSTLayerW - crx ? crw : kSTLayerW - crx) - 1;
	int ylast = crh - 1;
	if (ylast > kSTLayerH - 1 - cry) {
		ylast = kSTLayerH - 1 - cry;
	}
	if (ylast > ybot) {
		ylast = ybot;
	}
	if (ytop > ylast) {
		return true;                  // fully below the clip
	}

	RasterState st;
	st.fa = fa;
	st.sa = 0;
	st.fb = fb;
	st.sb = 0;
	st.row = (uint16_t *)(layer + (cry + y) * kSTRowBytes);
	st.prow = (uint16_t *)(layer + kSTPlaneBytes + (cry + y) * kSTPrioRowBytes);
	st.f01 = ((unsigned long)f0 << 16) | f1;
	st.f23 = ((unsigned long)f2 << 16) | f3;
	st.pv = setPrio ? 0xFFFFul : 0ul;
	st.crx = crx;
	st.xmax = xmaxv;

	for (;;) {
		while (ya <= y && ia != imax) {
			const int j = (ia == 0) ? n - 1 : ia - 1;
			const int dy = pts[j].y - pts[ia].y;
			st.fa = (int32_t)pts[ia].x << 16;
			ya = pts[j].y;
			st.sa = dy > 0 ? edgeStep(pts[j].x - pts[ia].x, dy) : 0;
			ia = j;
		}
		while (yb <= y && ib != imax) {
			const int j = (ib == n - 1) ? 0 : ib + 1;
			const int dy = pts[j].y - pts[ib].y;
			st.fb = (int32_t)pts[ib].x << 16;
			yb = pts[j].y;
			st.sb = dy > 0 ? edgeStep(pts[j].x - pts[ib].x, dy) : 0;
			ib = j;
		}
		int stop = ya < yb ? ya : yb;
		if (stop > ylast) {
			stop = ylast + 1;
		}
		if (stop <= y) {
			stop = y + 1;
		}
		int count = stop - y;
		if (y < 0) {
			// above the clip: step without drawing (rare)
			const int skip = (stop <= 0 ? count : -y);
			st.fa += st.sa * skip;
			st.fb += st.sb * skip;
			y += skip;
			st.row += (kSTRowBytes / 2) * skip;
			st.prow += (kSTPrioRowBytes / 2) * skip;
			count -= skip;
		}
		if (count > 0) {
			rasterSeg(&st, count);    // steps fa/fb/row/prow itself
			y += count;
		}
		if (y > ylast) {
			break;
		}
	}
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
	memset(layer + kSTPlaneBytes, (colour8 & 0x80) ? 0xFF : 0, kSTPrioRowBytes * kSTLayerH);
}

#endif // ATARIST
