
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
