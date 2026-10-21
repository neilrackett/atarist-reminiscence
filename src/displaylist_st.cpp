/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Atari ST gameplay frames, redrawn only where they changed (see
 * video_st.h).
 */

#ifdef ATARIST

extern "C" {
#include <stdl/stdl.h>
}
#include "util.h"
#include "video_st.h"
#include "displaylist_st.h"

// The engine draws every object in view on every frame: the restore
// puts the room back under last frame's sprites, and everything is
// drawn again on top. In a busy room most of that is the same picture
// as before - doors, terminals, lights, a guard standing still -
// measured at 55-99% of the sprites drawn a frame, and in a room full
// of objects it is most of the frame (an 8MHz ST spent 38ms of 81 on
// sprites in the first room on Earth, nearly all of them unchanged).
//
// So a frame's draws on the front layer are written down rather than
// done: what to draw, where, how. At the end of the frame the list is
// compared with the previous one, in order. Where an entry has no twin
// - something moved, changed frame or came and went - its blocks, old
// and new, must be redrawn, and so must every entry that overlaps
// them, and every entry overlapping those, until it closes. Only that
// region is restored from the room and redrawn, in the original order,
// and only it goes to the screen. Everywhere else every entry covering
// a pixel has a twin drawn in the same order over the same room, so
// the pixel is already what a full redraw would make it.
//
// Glyphs come from the glyph cache, whose slots never change, and
// sprites name their source bytes (ST_sprDraw), so an entry is the
// whole description of its pixels. Anything that changes the room
// layer, draws on the front layer outside the list, or refreshes the
// whole screen tells this module, and the next frame redraws that.

static DlEntry g_dl[2][kDlMax];
static int g_dlN[2];
static int g_cur;                // the list being written
DlEntry *g_dlNext;
DlEntry *g_dlEnd;
bool g_stDlRec;                      // writing it (ST_dlRecording)
bool g_dlTokOpen;
uint8_t *g_stDlFront;
static bool g_prevValid;         // the other list is what the front layer shows
static bool g_flushed;           // drawn early this frame; the rest went straight to the layer
static bool g_warned;
static void (*g_flushFn)(void *);
static void *g_flushCtx;

void ST_dlSetFlush(void (*fn)(void *), void *ctx) {
	g_flushFn = fn;
	g_flushCtx = ctx;
}

void ST_dlBegin(uint8_t *front) {
	g_stDlFront = front;
	g_dlNext = g_dl[g_cur];
	g_dlEnd = g_dlNext + kDlMax;
	g_flushed = false;
	g_stDlRec = true;
	g_dlTokOpen = false;               // fixed at the first draw: the logic runs first, and may move the colours
}

// the end of the frame's fixed palette tokens: false if the colours
// moved under its draws
static bool closeTokens() {
	if (!g_dlTokOpen) {
		return true;
	}
	g_dlTokOpen = false;
	return ST_sprTokensEnd();
}

void ST_dlAbort() {
	g_stDlRec = false;
	closeTokens();
	g_prevValid = false;             // the front layer is no longer what a list says
}

void ST_dlInvalidate() {
	g_prevValid = false;
}

static void drawEntry(const DlEntry *e);

// Draw what is written down now and write no more this frame: the list
// is full, or the bytes its sprites name are about to be reused (the
// bank arena emptied mid-frame). The front layer is rebuilt from the
// room with every entry so far, and everything after goes straight to
// it; the next frame is redrawn whole, as this one's list is partial.
void ST_dlFlushNow() {
	if (!g_stDlRec) {
		return;
	}
	g_stDlRec = false;
	g_flushed = true;
	g_prevValid = false;
	if (g_flushFn) {
		g_flushFn(g_flushCtx);       // the room back on the front layer
	}
	for (const DlEntry *e = g_dl[g_cur]; e < g_dlNext; ++e) {
		drawEntry(e);
	}
}

DlEntry *ST_dlFull() {
	if (!g_warned) {
		g_warned = true;
		warning("Display list full: a frame drew more than %d things", (int)kDlMax);
	}
	ST_dlFlushNow();
	return 0;
}

bool ST_dlFlushed() {
	return g_flushed;
}

bool ST_dlAddGlyph(const uint8_t *src, int pitch, int x, int y, uint8_t v, bool setPrio) {
	DlEntry *e = dlNewEntry(x, y, 8, 8);
	if (!e) {
		return g_stDlRec;
	}
	e->type = 1;
	e->kind = 0;
	e->data = src;
	e->ident = (uint32_t)pitch;
	e->w = 8;
	e->h = 8;
	e->colMask = 0;
	e->token = v;
	e->flags = 0;
	e->setPrio = setPrio ? 1 : 0;
	e->hash = dlEntryHash(e);
	return true;
}

static inline bool sameEntry(const DlEntry *a, const DlEntry *b) {
	return a->hash == b->hash && a->data == b->data && a->x == b->x && a->y == b->y
	       && a->ident == b->ident && a->token == b->token && a->flags == b->flags
	       && a->type == b->type && a->w == b->w && a->h == b->h
	       && a->setPrio == b->setPrio && a->kind == b->kind && a->colMask == b->colMask;
}

static inline void addBlocks(uint16_t *R, const DlEntry *e) {
	for (int j = e->by0; j <= e->by1; ++j) {
		R[j] |= e->bmask;
	}
}

static inline bool hitsBlocks(const uint16_t *R, const DlEntry *e) {
	for (int j = e->by0; j <= e->by1; ++j) {
		if (R[j] & e->bmask) {
			return true;
		}
	}
	return false;
}

// R (kSTBlockRows rows of block columns) comes in holding the blocks
// that must be redrawn whatever the lists say, and goes out holding
// every block that is. Entries to redraw are marked for ST_dlReplay.
void ST_dlEnd(uint16_t *R, bool full) {
	g_stDlRec = false;
	if (!closeTokens()) {
		full = true;                 // the colours moved under the frame's draws
	}
	DlEntry *C = g_dl[g_cur];
	g_dlN[g_cur] = (int)(g_dlNext - C);
	const int nc = g_dlN[g_cur];
	DlEntry *P = g_dl[g_cur ^ 1];
	const int np = g_dlN[g_cur ^ 1];
	if (full || !g_prevValid) {
		for (int j = 0; j < kSTBlockRows; ++j) {
			R[j] = 0xFFFF;
		}
		for (int i = 0; i < nc; ++i) {
			C[i].in = 1;
		}
	} else {
		// Twins, in order: walk both lists together, and where they part
		// look a few entries ahead in the old one for the new entry -
		// one thing appearing or vanishing should not unpair everything
		// after it. Entries that stay unpaired just get redrawn.
		enum { kLookAhead = 8 };
		DlEntry *const pEnd = P + np;
		DlEntry *const cEnd = C + nc;
		for (DlEntry *p = P; p < pEnd; ++p) {
			p->matched = 0;
		}
		DlEntry *pj = P;
		for (DlEntry *c = C; c < cEnd; ++c) {
			c->matched = 0;
			c->in = 0;
			DlEntry *end = pj + kLookAhead;
			if (end > pEnd) {
				end = pEnd;
			}
			for (DlEntry *p = pj; p < end; ++p) {
				if (sameEntry(c, p)) {
					c->matched = 1;
					p->matched = 1;
					pj = p + 1;
					break;
				}
			}
		}
		for (DlEntry *p = P; p < pEnd; ++p) {
			if (!p->matched) {
				addBlocks(R, p);
			}
		}
		for (DlEntry *c = C; c < cEnd; ++c) {
			if (!c->matched) {
				addBlocks(R, c);
				c->in = 1;
			}
		}
		// Close over overlaps: an entry touching the region is redrawn
		// whole, so all of it joins the region. Nothing to close over in
		// a frame where nothing changed - most frames of a still scene -
		// and an entry outside the region's columns altogether cannot
		// touch it, which spares the row-by-row test for most.
		uint16_t cols = 0;
		for (int j = 0; j < kSTBlockRows; ++j) {
			cols |= R[j];
		}
		bool grew = cols != 0;
		while (grew) {
			grew = false;
			for (DlEntry *c = C; c < cEnd; ++c) {
				if (!c->in && (c->bmask & cols) && hitsBlocks(R, c)) {
					addBlocks(R, c);
					cols |= c->bmask;
					c->in = 1;
					grew = true;
				}
			}
		}
	}
	g_prevValid = true;
}

static void drawEntry(const DlEntry *e) {
	++g_stPerf.dlDrawn;
	if (e->type == 0) {
		ST_sprDraw(g_stDlFront, e->kind, e->data, e->ident, e->w, e->h, e->x, e->y, e->colMask, e->flags, e->setPrio != 0);
	} else {
		uint8_t map16[16];
		memset(map16, e->token, sizeof(map16));
		ST_drawSprite(g_stDlFront, e->data, (int)e->ident, e->x, e->y, 8, 8, map16, 0, e->setPrio != 0);
	}
}

void ST_dlReplay() {
	DlEntry *C = g_dl[g_cur];
	const int nc = g_dlN[g_cur];
	// ST_dlEnd has just looked at the colours and nothing moves them
	// before this is drawn: the palette tokens stay as they are, which
	// spares every redrawn sprite the look
	ST_sprTokensBegin();
	for (int i = 0; i < nc; ++i) {
		const DlEntry *e = &C[i];
		if (!e->in) {
			++g_stPerf.dlKept;
			continue;
		}
		drawEntry(e);
	}
	ST_sprTokensEnd();
	g_cur ^= 1;                      // this frame's list is the one to compare with next
}

// The end of a frame ST_dlFlushNow already drew: nothing to compare,
// and the next frame starts from nothing
void ST_dlEndFlushed() {
	g_stDlRec = false;
	closeTokens();
	g_flushed = false;
	g_prevValid = false;
}

#endif
