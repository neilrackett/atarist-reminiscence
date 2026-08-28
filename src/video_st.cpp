
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

// fill helper: writes the constant colour to [x, x+w) of one row
static void fillRow(uint8_t *layer, int x, int w, int y, uint8_t v, bool setPrio) {
	int x1 = x + w - 1;
	uint16_t *dst = groupPtr(layer, x, y);
	uint16_t *prio = prioPtr(layer, x, y);
	const uint16_t f0 = (v & 1) ? 0xFFFF : 0;
	const uint16_t f1 = (v & 2) ? 0xFFFF : 0;
	const uint16_t f2 = (v & 4) ? 0xFFFF : 0;
	const uint16_t f3 = (v & 8) ? 0xFFFF : 0;
	for (int g = x >> 4; g <= (x1 >> 4); ++g) {
		const uint16_t m = groupMask(g, x, x1);
		if (m == 0xFFFF) {
			dst[0] = f0;
			dst[1] = f1;
			dst[2] = f2;
			dst[3] = f3;
			*prio = setPrio ? 0xFFFF : 0;
		} else {
			const uint16_t keep = ~m;
			dst[0] = (dst[0] & keep) | (f0 & m);
			dst[1] = (dst[1] & keep) | (f1 & m);
			dst[2] = (dst[2] & keep) | (f2 & m);
			dst[3] = (dst[3] & keep) | (f3 & m);
			if (setPrio) {
				*prio |= m;
			} else {
				*prio &= keep;
			}
		}
		dst += 4;
		++prio;
	}
}

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
