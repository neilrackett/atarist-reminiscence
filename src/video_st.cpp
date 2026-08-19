
/*
 * REminiscence - Flashback interpreter
 * Atari ST planar layer primitives (see video_st.h).
 */

#ifdef ATARIST

#include "video_st.h"

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

// Apply one group's worth of sprite pixels. drawn = bits to write.
static inline void applyGroup(uint16_t *dst, uint16_t *prio, uint16_t drawn, uint16_t d0, uint16_t d1, uint16_t d2, uint16_t d3, bool respectPrio, bool setPrio) {
	if (respectPrio) {
		drawn &= ~*prio;
	}
	if (!drawn) {
		return;
	}
	const uint16_t keep = ~drawn;
	dst[0] = (dst[0] & keep) | (d0 & drawn);
	dst[1] = (dst[1] & keep) | (d1 & drawn);
	dst[2] = (dst[2] & keep) | (d2 & drawn);
	dst[3] = (dst[3] & keep) | (d3 & drawn);
	if (setPrio) {
		*prio |= drawn;
	} else {
		*prio &= keep;
	}
}

void ST_drawSprite(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, int w, int h, const uint8_t *map16, unsigned flags, bool setPrio) {
	// clip (callers pre-clip; this is a safety net)
	if (x < 0) {
		w += x;
		src += (flags & kSTSpriteColMajor) ? -x * ((flags & kSTSpriteXflip) ? -pitch : pitch) : ((flags & kSTSpriteXflip) ? x : -x);
		x = 0;
	}
	if (y < 0) {
		h += y;
		src += (flags & kSTSpriteColMajor) ? -y : -y * pitch;
		y = 0;
	}
	if (x + w > kSTLayerW) {
		w = kSTLayerW - x;
	}
	if (y + h > kSTLayerH) {
		h = kSTLayerH - y;
	}
	if (w <= 0 || h <= 0) {
		return;
	}
	const bool respectPrio = (flags & kSTSpriteRespectPrio) != 0;
	const int gx0 = x >> 4;
	const int gx1 = (x + w - 1) >> 4;
	for (int j = 0; j < h; ++j) {
		// gather the source row (handles flip / column-major once)
		uint8_t row[kSTLayerW];
		if (flags & kSTSpriteColMajor) {
			if (flags & kSTSpriteXflip) {
				for (int i = 0; i < w; ++i) {
					row[i] = src[-i * pitch];
				}
			} else {
				for (int i = 0; i < w; ++i) {
					row[i] = src[i * pitch];
				}
			}
			++src;
		} else {
			if (flags & kSTSpriteXflip) {
				for (int i = 0; i < w; ++i) {
					row[i] = src[-i];
				}
			} else {
				for (int i = 0; i < w; ++i) {
					row[i] = src[i];
				}
			}
			src += pitch;
		}
		uint16_t *dst = groupPtr(layer, gx0 << 4, y + j);
		uint16_t *prio = prioPtr(layer, gx0 << 4, y + j);
		for (int g = gx0; g <= gx1; ++g) {
			const int base = (g << 4) - x; // source index of the group's first pixel
			uint16_t drawn = 0, d0 = 0, d1 = 0, d2 = 0, d3 = 0;
			uint16_t bit = 0x8000;
			for (int i = 0; i < 16; ++i, bit >>= 1) {
				const int s = base + i;
				if (s < 0 || s >= w) {
					continue;
				}
				const uint8_t c = row[s];
				if (c == 0) {
					continue;
				}
				const uint8_t v = map16[c & 15];
				drawn |= bit;
				if (v & 1) d0 |= bit;
				if (v & 2) d1 |= bit;
				if (v & 4) d2 |= bit;
				if (v & 8) d3 |= bit;
			}
			applyGroup(dst, prio, drawn, d0, d1, d2, d3, respectPrio, setPrio);
			dst += 4;
			++prio;
		}
	}
}

void ST_drawGlyph(uint8_t *layer, const uint8_t *src, int x, int y, uint8_t colour8) {
	const uint8_t v = ST_getRemap()[colour8];
	uint8_t map16[16];
	memset(map16, v, sizeof(map16));
	ST_drawSprite(layer, src, 16, x, y, 8, 8, map16, 0, (colour8 & 0x80) != 0);
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
		uint16_t m = 0xFFFF;
		if ((g << 4) < x) {
			m >>= (x & 15);
		}
		const int gend = (g << 4) + 15;
		if (gend > x1) {
			m &= 0xFFFF << (gend - x1);
		}
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
// bits into the underlying pixel (*dst |= colour & 0xF8) - for the
// usual colours 0xC8-0xCF that means "index |= 8". In cutscene
// palette mode (identity remap of 0xC0-0xCF onto slots 0-15) that is
// exactly an OR into plane 3. Outside that case fall back to an
// opaque fill.
void ST_hspanOr(uint8_t *layer, int x1, int x2, int y, uint8_t colour8) {
	if (y < 0 || y >= kSTLayerH) {
		return;
	}
	if (x1 < 0) x1 = 0;
	if (x2 >= kSTLayerW) x2 = kSTLayerW - 1;
	if (x1 > x2) {
		return;
	}
	if (!(ST_cutscenePalMode() && (colour8 & 0xF8) == 0xC8)) {
		fillRow(layer, x1, x2 - x1 + 1, y, ST_getRemap()[colour8], (colour8 & 0x80) != 0);
		return;
	}
	uint16_t *dst = groupPtr(layer, x1, y);
	uint16_t *prio = prioPtr(layer, x1, y);
	for (int g = x1 >> 4; g <= (x2 >> 4); ++g) {
		uint16_t m = 0xFFFF;
		if ((g << 4) < x1) {
			m >>= (x1 & 15);
		}
		const int gend = (g << 4) + 15;
		if (gend > x2) {
			m &= 0xFFFF << (gend - x2);
		}
		dst[3] |= m;
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
