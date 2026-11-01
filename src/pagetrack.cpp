
/*
 * REminiscence - Flashback interpreter
 * Atari port Copyright (C) 2026 Neil Rackett
 *
 * Cutscene page tracking, for both Atari builds (see pagetrack.h), and
 * the 030 build's page copies, which move what it says to move.
 */

#ifdef ATARIST

#include "pagetrack.h"

int16_t g_bandY0 = 0, g_bandY1 = kSTLayerH;
int16_t g_bandFill = -1;
bool g_trk;
uint8_t *g_trkPage[2];
uint8_t *g_trkAux;
TrkBox g_trkBox[2];
TrkBox g_scrBox;

// group-rows the drawing on a tracked page touched, box by box (the
// per-frame stats: what one box per page costs against these)
uint32_t g_stCutTouchGR;
bool g_stCutStats;

// a colour as a page stores it: a hardware colour on the ST, the
// engine's own in the 030 build
static inline int storedColour(uint8_t colour8) {
#ifdef ST_PLANAR
	return ST_getRemap()[colour8];
#else
	return colour8;
#endif
}

#ifdef ATARIST_030
const uint8_t *g_touchLayer;
int16_t g_touchX0 = 0x7FFF, g_touchX1 = -0x7FFF, g_touchY0 = 0x7FFF, g_touchY1 = -0x7FFF;

void pageTouchFlush() {
	if (g_touchLayer && g_touchX0 < g_touchX1) {
		pageTouch(g_touchLayer, g_touchX0, g_touchX1, g_touchY0, g_touchY1);
	}
	g_touchX0 = g_touchY0 = 0x7FFF;
	g_touchX1 = g_touchY1 = -0x7FFF;
}
#define TOUCH_FLUSH() pageTouchFlush()
#else
#define TOUCH_FLUSH()
#endif

static void markBand(TrkBox *b) {
	boxAdd(b, 0, 16, g_bandY0, g_bandY1);
}

const TrkBox *ST_pageCopyBox(uint8_t *dst, const uint8_t *src) {
	static TrkBox r;
	TrkBox *move = &r;
	TOUCH_FLUSH();
	if (!g_trk) {
		return 0;
	}
	TrkBox *bd = trkBoxOf(dst);
	TrkBox *bs = trkBoxOf(src);
	if (src == g_trkAux && bd) {
		// the restore: put back what this page drew over
		*move = *bd;
		boxClear(bd);
	} else if (dst == g_trkAux && bs) {
		// a new background: it moves only where src differed,
		// and everything else now differs from it there too
		*move = *bs;
		boxUnion(&g_trkBox[bs == &g_trkBox[0] ? 1 : 0], bs);
		boxUnion(&g_scrBox, bs);
		boxClear(bs);
	} else if (bd && bs) {
		// page to page: they differ only where either differs
		// from the background
		*move = *bd;
		boxUnion(move, bs);
		*bd = *bs;
	} else {
		return 0;
	}
	if (move->y0 < g_bandY0) move->y0 = g_bandY0;
	if (move->y1 > g_bandY1) move->y1 = g_bandY1;
	return move;
}

void ST_pageClear(const uint8_t *layer, int fill) {
	TOUCH_FLUSH();
	if (g_bandFill >= 0 && fill != g_bandFill) {
		// the rows outside the band are in the old colour on every
		// page: from here on the band is the whole page
		g_bandY0 = 0;
		g_bandY1 = kSTLayerH;
	}
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
}

void ST_pageBandStart(int y0, int y1, uint8_t colour8, uint8_t *front, uint8_t *back, uint8_t *aux) {
	g_bandY0 = (int16_t)(y0 < 0 ? 0 : y0);
	g_bandY1 = (int16_t)(y1 > kSTLayerH ? kSTLayerH : y1);
	g_bandFill = (int16_t)storedColour(colour8);
	g_trk = true;
	g_trkPage[0] = front;
	g_trkPage[1] = back;
	g_trkAux = aux;
	boxClear(&g_trkBox[0]);
	boxClear(&g_trkBox[1]);
	boxClear(&g_scrBox);
}

void ST_pageBandEnd() {
	TOUCH_FLUSH();
	g_bandY0 = 0;
	g_bandY1 = kSTLayerH;
	g_bandFill = -1;
	g_trk = false;
	g_trkPage[0] = g_trkPage[1] = g_trkAux = 0;
}

bool ST_pageShowRect(const uint8_t *page, bool full, int *x, int *y, int *w, int *h) {
	TOUCH_FLUSH();
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

#ifdef ATARIST_030
// The 030 build's cutscene pages are upstream's, a byte a pixel and
// kSTLayerW to a row, and these are the ST's page functions for them.
// What they move, tracked as the ST's planar pages are, is what took
// the time: a Falcon's frame went mostly on its pages, in ST-RAM.

static void copyRows(uint8_t *dst, const uint8_t *src, const TrkBox *b) {
	if (b->y0 >= b->y1 || b->g0 >= b->g1) {
		return;
	}
	const int off = b->y0 * kSTLayerW + b->g0 * 16;
	const int w = (b->g1 - b->g0) * 16;
	if (w == kSTLayerW) {
		memcpy(dst + off, src + off, (b->y1 - b->y0) * kSTLayerW);
		return;
	}
	dst += off;
	src += off;
	for (int y = b->y0; y < b->y1; ++y) {
		memcpy(dst, src, w);
		dst += kSTLayerW;
		src += kSTLayerW;
	}
}

void ST_copyPage(uint8_t *dst, const uint8_t *src) {
	if (const TrkBox *b = ST_pageCopyBox(dst, src)) {
		copyRows(dst, src, b);
		return;
	}
	memcpy(dst, src, kSTLayerW * kSTLayerH);
}

void ST_clearLayer(uint8_t *layer, uint8_t colour8) {
	ST_pageClear(layer, colour8);
	memset(layer + g_bandY0 * kSTLayerW, colour8, (g_bandY1 - g_bandY0) * kSTLayerW);
}

void ST_fillRect(uint8_t *layer, int x, int y, int w, int h, uint8_t colour8) {
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
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
	pageTouch(layer, x, x + w, y, y + h);
	for (uint8_t *p = layer + y * kSTLayerW + x; --h >= 0; p += kSTLayerW) {
		memset(p, colour8, w);
	}
}
#endif

#endif // ATARIST
