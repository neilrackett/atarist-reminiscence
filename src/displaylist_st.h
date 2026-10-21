/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The display list's entries (see displaylist_st.cpp), for the two
 * modules that write them: displaylist_st.cpp, and spritecache_st.cpp,
 * which writes a sprite's in ST_sprDraw itself - two of every three
 * sprites drawn are only written down, and through a call into the
 * other module each paid for its arguments twice and its registers three
 * times over.
 */

#ifndef DISPLAYLIST_ST_H__
#define DISPLAYLIST_ST_H__

#include "video_st.h"

// A frame draws a few dozen things at most - sprites, an icon or two, a
// line of text; one that draws more is redrawn whole
enum { kDlMax = 160 };

struct DlEntry {
	const uint8_t *data;        // sprite bytes, or the glyph's pixels
	uint32_t ident;             // sprite identity, or the glyph's pitch
	int16_t x, y;
	uint16_t hash;              // the fields below folded, compared first
	uint16_t bmask;             // block columns covered
	uint8_t by0, by1;           // block rows covered
	uint8_t type;               // 0 sprite, 1 glyph
	uint8_t kind;               // sprite kind
	uint8_t w, h;
	uint8_t colMask;            // sprite colour bank
	uint8_t token;              // sprite palette token, or the glyph's hardware colour
	uint8_t flags;              // kSTSprite*
	uint8_t setPrio;
	uint8_t matched;            // has a twin in the other frame
	uint8_t in;                 // redrawn this frame
	uint8_t pad[4];             // 32 bytes: an index is a shift, not a multiply
};
typedef char DlEntrySizeCheck[(sizeof(DlEntry) == 32) ? 1 : -1];

extern DlEntry *g_dlNext;       // where the frame's next entry goes
extern DlEntry *g_dlEnd;
extern bool g_dlTokOpen;        // the palette tokens are fixed for the frame's draws

// the list is full: what it holds is drawn now, and nothing more is
// written this frame (ST_dlFlushNow); 0
DlEntry *ST_dlFull();

// A new entry for something drawn over [x, x+w) x [y, y+h), clipped to
// the layer: 0 when none of it is on the layer, or when there is no
// room left - then the list is drawn there and then, and g_stDlRec is
// false: whoever asked draws it directly.
static inline __attribute__((always_inline)) DlEntry *dlNewEntry(int x, int y, int w, int h) {
	int x1 = x + w, y1 = y + h;
	int cx = x, cy = y;
	if (cx < 0) {
		cx = 0;
	}
	if (cy < 0) {
		cy = 0;
	}
	if (x1 > kSTLayerW) {
		x1 = kSTLayerW;
	}
	if (y1 > kSTLayerH) {
		y1 = kSTLayerH;
	}
	if (x1 <= cx || y1 <= cy) {
		return 0;
	}
	if (g_dlNext == g_dlEnd) {
		return ST_dlFull();
	}
	DlEntry *e = g_dlNext++;
	e->by0 = (uint8_t)(cy >> 3);
	e->by1 = (uint8_t)((y1 - 1) >> 3);
	e->bmask = (uint16_t)(kSTColsFrom[cx >> 4] & kSTColsTo[(x1 - 1) >> 4]);
	e->x = (int16_t)x;
	e->y = (int16_t)y;
	return e;
}

// compared before the fields; the same function every frame is all it
// needs (shifts by 16 are a swap)
static inline uint16_t dlEntryHash(const DlEntry *e) {
	const uint32_t d = (uint32_t)e->data;
	return (uint16_t)((uint16_t)(d >> 2) ^ (uint16_t)(d >> 16) ^ (uint16_t)e->x ^ (uint16_t)(e->y << 7)
	                  ^ (uint16_t)(e->token << 3) ^ e->flags ^ (uint16_t)e->ident);
}

#endif
