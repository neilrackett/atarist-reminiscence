
/*
 * REminiscence - Flashback interpreter
 * Atari port Copyright (C) 2026 Neil Rackett
 *
 * Cutscene page tracking, for both Atari builds: which rows of a page
 * hold the scene (the band), and a box around where each page differs
 * from the background. It knows nothing of how a page is stored; the
 * ST's planar pages (video_st.cpp) and the 030 build's byte-a-pixel
 * ones (pagetrack.cpp) move what it says to move. See ST_pageBandStart
 * in video_st.h.
 */

#ifndef PAGETRACK_H__
#define PAGETRACK_H__

#ifdef ATARIST

extern "C" {
#include <stdl/stdl_math.h>
}

#include "video_st.h"

// Groups [g0, g1) of 16 pixels, rows [y0, y1).
struct TrkBox {
	int16_t g0, g1, y0, y1;
};

// the band; the whole page when no scene is in progress
extern int16_t g_bandY0, g_bandY1;
extern int16_t g_bandFill;   // what a page holds outside the band, as stored
// For the two pages that take turns on screen, and for the screen
// itself, a box around everything that differs from the aux page - the
// background every frame is restored from. A restore then copies only
// what the last frame on that page drew over, and a push only what
// changed since the last one, instead of the whole band every time.
// One box, not a mask of blocks: marking every block of every shape,
// and copying a region in pieces, cost more on a 68000 than the few
// clean blocks a box takes along.
extern bool g_trk;
extern uint8_t *g_trkPage[2];
extern uint8_t *g_trkAux;
extern TrkBox g_trkBox[2];
extern TrkBox g_scrBox;

static inline void bandTouch(int y0, int y1) {
	if (y0 < g_bandY0) {
		g_bandY0 = (int16_t)(y0 < 0 ? 0 : y0);
	}
	if (y1 > g_bandY1) {
		g_bandY1 = (int16_t)(y1 > kSTLayerH ? kSTLayerH : y1);
	}
}

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
		if (g_stCutStats) {
			g_stCutTouchGR += STDL_MulU16((uint16_t)(g1 - g0), (uint16_t)(y1 - y0));
		}
	}
}

#ifdef ATARIST_030
// The 030 build's drawing touches through this: a box gathered on the
// side, a few compares a primitive, and given to pageTouch before
// anything reads the tracking (ST_pageCopyBox, ST_pageClear,
// ST_pageShowRect, ST_pageBandEnd) or the drawing moves to another
// layer. A Falcon runs its code from ST-RAM, and pageTouch inlined at
// every polygon and ellipse cost its level 1 opening (0x00) 7-8ms a
// frame, more than the smaller restore saved.
extern const uint8_t *g_touchLayer;
extern int16_t g_touchX0, g_touchX1, g_touchY0, g_touchY1;
void pageTouchFlush();

static inline void pageTouchLater(const uint8_t *layer, int x0, int x1, int y0, int y1) {
	if (layer != g_touchLayer) {
		pageTouchFlush();
		g_touchLayer = layer;
	}
	if (x0 < g_touchX0) g_touchX0 = (int16_t)x0;
	if (x1 > g_touchX1) g_touchX1 = (int16_t)x1;
	if (y0 < g_touchY0) g_touchY0 = (int16_t)y0;
	if (y1 > g_touchY1) g_touchY1 = (int16_t)y1;
}
#endif

// A page copy, src to dst: the box that need go (rows clipped to the
// band, and possibly empty), or null for the whole page. Neither this
// nor ST_pageClear hands anything back through a caller's local: one
// whose address is taken gave the ST's ST_clearLayer a frame pointer
// in a6, which its fill's movem burst uses, and the game bombed.
const TrkBox *ST_pageCopyBox(uint8_t *dst, const uint8_t *src);
// A page about to be cleared to fill (as stored): the rows to fill are
// then [g_bandY0, g_bandY1).
void ST_pageClear(const uint8_t *layer, int fill);

#endif // ATARIST

#endif // PAGETRACK_H__
