
/*
 * REminiscence - Flashback interpreter
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

void ST_convertChunky(uint8_t *layer, const uint8_t *src, int h) {
	const uint8_t *remap = ST_getRemap();
	uint16_t *dst = (uint16_t *)layer;
	uint16_t *prio = (uint16_t *)(layer + kSTPlaneBytes);
	for (int y = 0; y < h; ++y) {
		for (int g = 0; g < kSTLayerW / 16; ++g) {
			uint16_t p0 = 0, p1 = 0, p2 = 0, p3 = 0, pr = 0;
			for (int i = 0; i < 16; ++i) {
				const uint8_t c = src[i];
				const uint8_t v = remap[c];
				p0 += p0 + (v & 1);
				p1 += p1 + ((v >> 1) & 1);
				p2 += p2 + ((v >> 2) & 1);
				p3 += p3 + ((v >> 3) & 1);
				pr += pr + (c >> 7);
			}
			dst[0] = p0;
			dst[1] = p1;
			dst[2] = p2;
			dst[3] = p3;
			*prio++ = pr;
			dst += 4;
			src += 16;
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
	uint16_t *planes;       // groups*4 words per row
	uint16_t *mask;         // groups words per row, 1 = opaque
	int cap;                // words allocated at planes
};

static SprEntry g_spr[kSprSlots];

void ST_flushSpriteCache() {
	for (int i = 0; i < kSprSlots; ++i) {
		g_spr[i].src = 0;
	}
}

static bool bakeSprite(SprEntry *e, const uint8_t *src, int pitch, int w, int h, const uint8_t *map16, unsigned f) {
	const int groups = (w + 15) >> 4;
	const int planeWords = groups * 4 * h;
	const int words = planeWords + groups * h;
	if (e->cap < words) {
		// grow only: a slot large enough for one frame is large
		// enough for the next one that lands in it, so after warm-up
		// a miss costs no allocation at all
		free(e->planes);
		e->planes = (uint16_t *)malloc(words * sizeof(uint16_t));
		e->cap = e->planes ? words : 0;
	}
	if (!e->planes) {
		return false;
	}
	e->mask = e->planes + planeWords;
	e->groups = groups;
	// Only the mask has to start clear: BlitIndexed8 skips transparent
	// pixels, and plane bits outside the mask are never merged.
	memset(e->mask, 0, groups * h * sizeof(uint16_t));

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
	STDL_BlitIndexed8(scratch, src, pitch, 0, 0, w, h, map16, f | STDL_I8_MARK);
	return true;
}

// Move one baked frame into the layer. gcc 4.6 does not unswitch
// loops, so the aligned and shifted cases are separate loops rather
// than a test inside the group loop, and each keeps its live values
// down to what the 68000's registers hold.
static void mergeGroup(uint16_t *d, uint16_t *pr, uint16_t m,
                       uint16_t o0, uint16_t o1, uint16_t o2, uint16_t o3,
                       bool respectPrio, bool setPrio) {
	if (respectPrio) {
		m = (uint16_t)(m & ~*pr);
	}
	if (m == 0) {
		return;
	}
	const uint16_t keep = (uint16_t)~m;
	d[0] = (uint16_t)((d[0] & keep) | (o0 & m));
	d[1] = (uint16_t)((d[1] & keep) | (o1 & m));
	d[2] = (uint16_t)((d[2] & keep) | (o2 & m));
	d[3] = (uint16_t)((d[3] & keep) | (o3 & m));
	*pr = setPrio ? (uint16_t)(*pr | m) : (uint16_t)(*pr & keep);
}

static void blitBaked(uint8_t *layer, const SprEntry *e, int x, int y, bool respectPrio, bool setPrio) {
	const int groups = e->groups;
	const int r = x & 15;
	const int gx = x >> 4;
	const int lastGroup = (kSTLayerW >> 4) - 1;
	int j0 = 0, j1 = e->h;
	if (y < 0) {
		j0 = -y;
	}
	if (y + j1 > kSTLayerH) {
		j1 = kSTLayerH - y;
	}
	if (j0 >= j1) {
		return;
	}
	// the output groups that land on the layer, computed once
	int g0 = 0, g1 = (r == 0) ? groups : groups + 1;
	if (gx + g0 < 0) {
		g0 = -gx;
	}
	if (gx + g1 > lastGroup + 1) {
		g1 = lastGroup + 1 - gx;
	}
	if (g0 >= g1) {
		return;
	}
	const uint16_t *srcRow = e->planes + (uint32_t)j0 * groups * 4;
	const uint16_t *maskRow = e->mask + (uint32_t)j0 * groups;
	uint8_t *dstRow = layer + (uint32_t)(y + j0) * kSTRowBytes + gx * 8;
	uint8_t *prioRow = layer + kSTPlaneBytes + (uint32_t)(y + j0) * kSTPrioRowBytes + gx * 2;

	if (r == 0) {
		for (int j = j0; j < j1; ++j) {
			const uint16_t *sp = srcRow + g0 * 4;
			const uint16_t *mp = maskRow + g0;
			uint16_t *d = (uint16_t *)dstRow + g0 * 4;
			uint16_t *pr = (uint16_t *)prioRow + g0;
			for (int g = g0; g < g1; ++g) {
				const uint16_t m = *mp++;
				if (m != 0) {
					mergeGroup(d, pr, m, sp[0], sp[1], sp[2], sp[3], respectPrio, setPrio);
				}
				sp += 4;
				d += 4;
				++pr;
			}
			srcRow += groups * 4;
			maskRow += groups;
			dstRow += kSTRowBytes;
			prioRow += kSTPrioRowBytes;
		}
		return;
	}

	const int l = 16 - r;
	for (int j = j0; j < j1; ++j) {
		const uint16_t *sp = srcRow;
		const uint16_t *mp = maskRow;
		uint16_t *d = (uint16_t *)dstRow;
		uint16_t *pr = (uint16_t *)prioRow;
		uint16_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, cm = 0;
		for (int g = 0; g < g1; ++g) {
			uint16_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, sm = 0;
			if (g < groups) {
				s0 = sp[0]; s1 = sp[1]; s2 = sp[2]; s3 = sp[3];
				sm = *mp++;
				sp += 4;
			}
			const uint16_t om = (uint16_t)((cm << l) | (sm >> r));
			if (g >= g0 && om != 0) {
				mergeGroup(d, pr, om,
					(uint16_t)((c0 << l) | (s0 >> r)),
					(uint16_t)((c1 << l) | (s1 >> r)),
					(uint16_t)((c2 << l) | (s2 >> r)),
					(uint16_t)((c3 << l) | (s3 >> r)),
					respectPrio, setPrio);
			}
			c0 = s0; c1 = s1; c2 = s2; c3 = s3; cm = sm;
			d += 4;
			++pr;
		}
		srcRow += groups * 4;
		maskRow += groups;
		dstRow += kSTRowBytes;
		prioRow += kSTPrioRowBytes;
	}
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
			enum { kSeen = 48 };
			static const uint8_t *seenSrc[kSeen];
			static uint8_t seenMask[kSeen];
			static int seenPos;
			int slot = -1;
			for (int i = 0; i < kSeen; ++i) {
				if (seenSrc[i] == src && seenMask[i] == colMask) {
					slot = i;
					break;
				}
			}
			if (slot < 0) {
				seenSrc[seenPos] = src;
				seenMask[seenPos] = colMask;
				seenPos = (seenPos + 1) % kSeen;
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
