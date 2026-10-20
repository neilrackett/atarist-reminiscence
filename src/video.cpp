
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 * Atari ST port changes Copyright (C) 2026 Neil Rackett
 */

#include "decode_mac.h"
#include "resource.h"
#include "systemstub.h"
#include "unpack.h"
#include "util.h"
#include "video.h"
#ifdef ATARIST
#include "palette_tables.h"
#include <mint/osbind.h>
#endif
#include "video_st.h"

Video::Video(Resource *res, SystemStub *stub, WidescreenMode widescreenMode)
	: _res(res), _stub(stub), _widescreenMode(widescreenMode) {
	_layerScale = g_features->resolution_scale;
	_w = GAMESCREEN_W * _layerScale;
	_h = GAMESCREEN_H * _layerScale;
#ifdef ATARIST
	_layerSize = kSTLayerSize;
#else
	_layerSize = _w * _h;
#endif
	_frontLayer = (uint8_t *)calloc(1, _layerSize);
	_backLayer = (uint8_t *)calloc(1,_layerSize);
	_tempLayer = (uint8_t *)calloc(1, _layerSize);
	_tempLayer2 = (uint8_t *)calloc(1, _layerSize);
#ifdef ATARIST
	_stBackGen = 0;
	memset(_blkForce, 0, sizeof(_blkForce));
	_dlOn = false;
	_dlFull = true;
	_dlBackGen = 0;
	memset(_prioDirty, 0xFF, sizeof(_prioDirty));
#endif
#ifndef ATARIST
	// the ST tracks block state in _blkDirty/_blkShown/_blkOwed instead
	_screenBlocks = (uint8_t *)calloc(1, (_w / SCREENBLOCK_W) * (_h / SCREENBLOCK_H));
#endif
#ifdef ATARIST
	memset(_blkDirty, 0, sizeof(_blkDirty));
	memset(_blkShown, 0, sizeof(_blkShown));
	memset(_blkOwed, 0, sizeof(_blkOwed));
#endif
	_fullRefresh = true;
	_shakeOffset = 0;
	_charFrontColor = 0;
	_charTransparentColor = 0;
	_charShadowColor = 0;
	_drawChar = 0;
	switch (_res->_type) {
	case kResourceTypeAmiga:
		_drawChar = &Video::AMIGA_drawStringChar;
		break;
	case kResourceTypeDOS:
	case kResourceTypePC98:
	case kResourceTypeSega:
		_drawChar = &Video::DOS_drawStringChar;
		break;
	case kResourceTypeMac:
		_drawChar = &Video::MAC_drawStringChar;
		break;
	}
}

Video::~Video() {
	free(_frontLayer);
	free(_backLayer);
	free(_tempLayer);
	free(_tempLayer2);
#ifndef ATARIST
	free(_screenBlocks);
#endif
}

#ifdef ATARIST
// The cutscene player used the front layer as a page; the back
// layer is the room as decoded, with the game's colour mapping,
// which ST_setCutscenePalMode(false) has just put back.
void Video::ST_rebakeRoom() {
	ST_copyLayer(_frontLayer, _backLayer);
}

// Restore only the screen blocks the last frame drew (anything drawn
// goes through markBlockAsDirty). Block lifecycle on the ST:
//
//   kBlockDirty --blit--> kBlockShown --restore--> kBlockOwed
//               --blit--> kBlockClean
//
// A mark is blitted and tagged; the next frame's restore puts the
// background back in the front layer and re-tags the block (owed) so
// the following blit shows the restored background on screen - skip
// that and a moving sprite leaves its old image on screen forever
// (the front layer is clean, the screen never hears about it).
// Compared with upstream's 2->1->0, blocks restore once instead of
// twice.
// bits for columns [i, 15] and for columns [0, i]
static const uint16_t kColsFrom[16] = {
	0xFFFF, 0x7FFF, 0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF,
	0x00FF, 0x007F, 0x003F, 0x001F, 0x000F, 0x0007, 0x0003, 0x0001
};
static const uint16_t kColsTo[16] = {
	0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
	0xFF80, 0xFFC0, 0xFFE0, 0xFFF0, 0xFFF8, 0xFFFC, 0xFFFE, 0xFFFF
};

// The copies live in their own functions on purpose: gcc 4.6 ran
// out of registers in the walk below and spilled the loop counters
// to the frame, so every iteration of these was three memory
// read-modify-writes before it moved a byte.
static void restorePlanes(uint8_t *dst, const uint8_t *src, int groups, int lines) {
	if (ST_copyGroupRows(dst, kSTRowBytes, src, kSTRowBytes, groups, lines)) {
		return;
	}
	uint32_t *d = (uint32_t *)dst;
	const uint32_t *s = (const uint32_t *)src;
	const int skip = (kSTRowBytes >> 2) - groups * 2;
	for (int line = lines; --line >= 0; ) {
		for (int n = groups; --n >= 0; ) {
			*d++ = *s++;
			*d++ = *s++;
		}
		s += skip;
		d += skip;
	}
}

// the priority plane, two bytes a group: long moves where a pair of
// groups is aligned, words for what is left
static void restorePrio(uint8_t *dst, const uint8_t *src, int groups, int lines, int longs) {
	const int words = groups - longs * 2;
	for (int line = lines; --line >= 0; ) {
		const uint32_t *ls = (const uint32_t *)src;
		uint32_t *ld = (uint32_t *)dst;
		for (int n = longs; --n >= 0; ) {
			*ld++ = *ls++;
		}
		const uint16_t *ws = (const uint16_t *)ls;
		uint16_t *wd = (uint16_t *)ld;
		for (int n = words; --n >= 0; ) {
			*wd++ = *ws++;
		}
		src += kSTPrioRowBytes;
		dst += kSTPrioRowBytes;
	}
}

void Video::ST_restoreDirty() {
	if (_fullRefresh) {
		ST_copyLayer(_frontLayer, _backLayer);
		return;
	}
	const int cols = _w / SCREENBLOCK_W;
	const int rows = _h / SCREENBLOCK_H;
	// The priority plane is only worth putting back where something
	// wrote it: sprites mostly read it, and an ordinary frame leaves
	// all of it as the room decoded it.
	int px0, py0, px1, py1;
	const bool prioAny = ST_takePrioRect(&px0, &py0, &px1, &py1);
	const int pbx0 = prioAny ? (px0 / SCREENBLOCK_W) : 0;
	const int pbx1 = prioAny ? (px1 / SCREENBLOCK_W) : -1;
	const int pby0 = prioAny ? (py0 / SCREENBLOCK_H) : 0;
	const int pby1 = prioAny ? (py1 / SCREENBLOCK_H) : -1;
	// Rows that need the same blocks back are restored together, so a
	// tall sprite costs one setup instead of one per eight-pixel band.
	for (int j = 0; j < rows; ) {
		const uint16_t m = _blkShown[j];
		if (m == 0) {
			++j;
			continue;
		}
		int j2 = j + 1;
		while (j2 < rows && _blkShown[j2] == m) {
			++j2;
		}
		const int yy = j * SCREENBLOCK_H;
		const int lines = (j2 - j) * SCREENBLOCK_H;
		int i = 0;
		uint16_t bit = 0x8000;
		while (i < cols) {
			if ((m & bit) == 0) {
				++i;
				bit >>= 1;
				continue;
			}
			int i2 = i + 1;
			uint16_t b2 = (uint16_t)(bit >> 1);
			while (i2 < cols && (m & b2) != 0) {
				++i2;
				b2 >>= 1;
			}
			bit = b2;
			// SCREENBLOCK_W is 16 here, so a block is exactly a group
			const int gx0 = i;
			const int groups = i2 - i;
			{
				const int off = yy * kSTRowBytes + gx0 * 8;
				restorePlanes(_frontLayer + off, _backLayer + off, groups, lines);
				if (j > pby1 || j2 - 1 < pby0 || i > pbx1 || i2 - 1 < pbx0) {
					i = i2;
					continue;
				}
				const int poff = kSTPlaneBytes + yy * kSTPrioRowBytes + gx0 * 2;
				restorePrio(_frontLayer + poff, _backLayer + poff, groups, lines,
					((gx0 & 1) == 0) ? (groups >> 1) : 0);
			}
			i = i2;
		}
		j = j2;
	}
	// what was on screen has its background back and owes one blit
	for (int j = 0; j < rows; ++j) {
		_blkOwed[j] |= _blkShown[j];
		_blkShown[j] = 0;
	}
}
#endif

#ifdef ATARIST
// Leading zeros of a byte (8 for none): the column of a block row's first
// set bit in two lookups, and the length of the run it starts in two
// more, where walking the 16 bits a column at a time was most of what
// finding a frame's runs cost.
static uint8_t g_clz8[256];

static inline int clz16(uint16_t m) {
	return (m >> 8) ? g_clz8[m >> 8] : 8 + g_clz8[m & 0xFF];
}

// The next run of set columns in m, [*i, *i + *n), taken out of m;
// false when there are none left
static inline bool nextRun(uint16_t &m, int *i, int *n) {
	if (!m) {
		return false;
	}
	const int c = clz16(m);
	const int len = clz16((uint16_t)~(uint16_t)(m << c));
	m = (c + len >= 16) ? 0 : (uint16_t)(m & (0xFFFFu >> (c + len)));
	*i = c;
	*n = len;
	return true;
}

static void initClz8() {
	if (g_clz8[0] == 0) {
		for (int b = 0; b < 256; ++b) {
			int n = 0;
			while (n < 8 && !(b & (0x80 >> n))) {
				++n;
			}
			g_clz8[b] = (uint8_t)n;
		}
	}
}

// The blocks set in blk back from the back layer: the planes, and the
// priority plane where prio says it may differ from the room's - only
// where a MARK wrote it (an icon, highlighted text), so mostly nowhere.
void Video::ST_restoreBlocks(const uint16_t *blk, const uint16_t *prio) {
	initClz8();
	const int rows = _h / SCREENBLOCK_H;
	for (int j = 0; j < rows; ) {
		const uint16_t m = blk[j];
		if (m == 0) {
			++j;
			continue;
		}
		int j2 = j + 1;
		while (j2 < rows && blk[j2] == m) {
			++j2;
		}
		const int yy = j * SCREENBLOCK_H;
		const int lines = (j2 - j) * SCREENBLOCK_H;
		{
			uint16_t mm = m;
			int i, groups;
			while (nextRun(mm, &i, &groups)) {
				const int off = yy * kSTRowBytes + i * 8;
				restorePlanes(_frontLayer + off, _backLayer + off, groups, lines);
			}
		}
		// the priority plane a block row at a time: its dirty columns
		// differ from row to row
		for (int r = j; r < j2; ++r) {
			uint16_t pm = (uint16_t)(m & prio[r]);
			const int py = r * SCREENBLOCK_H;
			int i, groups;
			while (nextRun(pm, &i, &groups)) {
				const int poff = kSTPlaneBytes + py * kSTPrioRowBytes + i * 2;
				restorePrio(_frontLayer + poff, _backLayer + poff, groups, SCREENBLOCK_H, ((i & 1) == 0) ? (groups >> 1) : 0);
			}
		}
		j = j2;
	}
}

// what a display list flush starts from: the room, planes and priority
static void ST_dlFlushLayer(void *ctx) {
	Video *v = (Video *)ctx;
	ST_copyLayer(v->_frontLayer, v->_backLayer);
	uint16_t gone[Video::kBlockRows];
	ST_takePrioBlocks(gone);             // all of it is the room's again
	memset(v->_prioDirty, 0, sizeof(v->_prioDirty));
}

void Video::ST_frameBegin() {
	_dlOn = !(g_options.ab & kAbNoDisplayList);
	if (!_dlOn) {
		ST_restoreDirty();
		return;
	}
	ST_dlSetFlush(ST_dlFlushLayer, this);
	ST_dlBegin(_frontLayer);
}

void Video::ST_frameEnd() {
	if (!_dlOn) {
		return;
	}
	if (ST_dlFlushed()) {
		// drawn whole mid-frame (the list filled, or the banks it named
		// were about to go): it all goes to the screen
		ST_dlEndFlushed();
		int px0, py0, px1, py1;
		ST_takePrioRect(&px0, &py0, &px1, &py1);
		for (int j = 0; j < kBlockRows; ++j) {
			_blkDirty[j] = 0xFFFF;
			_blkOwed[j] = 0;
			_blkShown[j] = 0;
			_blkForce[j] = 0;
		}
		_dlFull = false;
		_dlBackGen = _stBackGen;
		return;
	}
	uint16_t R[kBlockRows];
	for (int j = 0; j < kBlockRows; ++j) {
		R[j] = _blkForce[j];
		_blkForce[j] = 0;
	}
	// a decoded room, a full refresh, anything drawn over the whole
	// front layer: nothing on it can be trusted, so it is all redrawn
	const bool full = _fullRefresh || _dlFull || _dlBackGen != _stBackGen;
	_dlFull = false;
	_dlBackGen = _stBackGen;
	ST_dlEnd(R, full);
	int px0, py0, px1, py1;
	ST_takePrioRect(&px0, &py0, &px1, &py1);   // the old restore's record; this one keeps blocks
	ST_takePrioBlocks(_prioDirty);             // where the priority plane may now differ from the room's
	if (full) {
		ST_copyLayer(_frontLayer, _backLayer);
		memset(_prioDirty, 0, sizeof(_prioDirty));
	} else {
		ST_restoreBlocks(R, _prioDirty);
		for (int j = 0; j < kBlockRows; ++j) {
			_prioDirty[j] &= (uint16_t)~R[j];
		}
	}
	ST_dlReplay();
	// the screen gets exactly what was redrawn, and owes nothing after
	for (int j = 0; j < kBlockRows; ++j) {
		_blkDirty[j] = R[j];
		_blkOwed[j] = 0;
		_blkShown[j] = 0;
	}
}

void Video::ST_frameAbort() {
	if (_dlOn) {
		ST_dlAbort();
		_dlFull = true;
	}
}

void Video::ST_backChanged(int x, int y, int w, int h) {
	if (!_dlOn || !ST_dlRecording(_frontLayer)) {
		markBlockAsDirty(x, y, w, h, _layerScale);
		return;
	}
	const uint16_t m = (uint16_t)(kColsFrom[x / SCREENBLOCK_W] & kColsTo[(x + w - 1) / SCREENBLOCK_W]);
	for (int j = y / SCREENBLOCK_H; j <= (y + h - 1) / SCREENBLOCK_H; ++j) {
		_blkForce[j] |= m;
	}
}

bool Video::ST_blocksDirty(int x, int y, int w, int h) const {
	const uint16_t m = (uint16_t)(kColsFrom[x / SCREENBLOCK_W] & kColsTo[(x + w - 1) / SCREENBLOCK_W]);
	for (int j = y / SCREENBLOCK_H; j <= (y + h - 1) / SCREENBLOCK_H; ++j) {
		if (_blkDirty[j] & m) {
			return true;
		}
	}
	return false;
}
#endif

void Video::markBlockAsDirty(int16_t x, int16_t y, uint16_t w, uint16_t h, int scale) {
	debug(DBG_VIDEO, "Video::markBlockAsDirty(%d, %d, %d, %d)", x, y, w, h);
#ifdef ATARIST
	if (_dlOn && ST_dlRecording(_frontLayer)) {
		return;                      // the display list knows what it drew
	}
#endif
	// 16-bit operands keep these as muls.w: a plain int product is a
	// __mulsi3 call, and this runs once per sprite piece, glyph and icon
	const int cols = _w / SCREENBLOCK_W;
	int bx1 = ((int16_t)scale * (int16_t)x) / SCREENBLOCK_W;
	int by1 = ((int16_t)scale * (int16_t)y) / SCREENBLOCK_H;
	int bx2 = ((int16_t)scale * (int16_t)(x + w - 1)) / SCREENBLOCK_W;
	int by2 = ((int16_t)scale * (int16_t)(y + h - 1)) / SCREENBLOCK_H;
	if (bx1 < 0) {
		bx1 = 0;
	}
	if (bx2 > cols - 1) {
		bx2 = cols - 1;
	}
	if (by1 < 0) {
		by1 = 0;
	}
	if (by2 > (_h / SCREENBLOCK_H) - 1) {
		by2 = (_h / SCREENBLOCK_H) - 1;
	}
	if (bx2 < bx1) {
		return;
	}
#ifdef ATARIST
	// one OR a row: the marks are what the two frame walks used to go
	// looking for, and this is where they were already known
	const uint16_t m = (uint16_t)(kColsFrom[bx1] & kColsTo[bx2]);
	if (_dlOn) {
		// drawn outside a listed frame - a menu, the inventory - so the
		// next listed frame must put these blocks back too
		for (int j = by1; j <= by2; ++j) {
			_blkForce[j] |= m;
		}
	}
	for (; by1 <= by2; ++by1) {
		_blkDirty[by1] |= m;
	}
#else
	uint8_t *row = _screenBlocks + (int16_t)by1 * (int16_t)cols;
	const int n = bx2 - bx1 + 1;
	for (; by1 <= by2; ++by1) {
		uint8_t *p = row + bx1;
		for (int i = n; --i >= 0; ) {
			*p++ = kBlockDirty;
		}
		row += cols;
	}
#endif
}

void Video::updateScreen() {
	debug(DBG_VIDEO, "Video::updateScreen()");
//	_fullRefresh = true;
#ifdef ATARIST
#define VIDEO_BLIT(x, y, w, h) _stub->copyRectPlanar((x), (y), (w), (h), _frontLayer)
#else
#define VIDEO_BLIT(x, y, w, h) _stub->copyRect((x), (y), (w), (h), _frontLayer, _w)
#endif
	if (_fullRefresh) {
		VIDEO_BLIT(0, 0, _w, _h);
		_stub->updateScreen(_shakeOffset);
		_fullRefresh = false;
#ifdef ATARIST
		// The sprites this frame drew went up with the page and owe
		// a restore like any other: zeroing the marks here left them
		// in the front layer for good - a Conrad ghost at the room
		// entry position whenever the next frame skipped him.
		for (int j = 0; j < (int)(sizeof(_blkDirty) / sizeof(_blkDirty[0])); ++j) {
			_blkShown[j] = _blkDirty[j];
			_blkDirty[j] = 0;
			_blkOwed[j] = 0;
		}
#endif
#ifdef ATARIST
	// A fresh mark blits and then awaits its restore; a restored
	// block blits once more and is done. Rows needing the same
	// blocks go up in one call: a sprite spans several eight-pixel
	// bands and used to cost a call for each.
	} else {
		const int rows = _h / SCREENBLOCK_H;
		int count = 0;
		initClz8();
		for (int j = 0; j < rows; ) {
			const uint16_t m = (uint16_t)(_blkDirty[j] | _blkOwed[j]);
			if (m == 0) {
				++j;
				continue;
			}
			int j2 = j + 1;
			while (j2 < rows && (uint16_t)(_blkDirty[j2] | _blkOwed[j2]) == m) {
				++j2;
			}
			uint16_t mm = m;
			int i, n;
			while (nextRun(mm, &i, &n)) {
				VIDEO_BLIT(i * SCREENBLOCK_W, j * SCREENBLOCK_H,
					n * SCREENBLOCK_W, (j2 - j) * SCREENBLOCK_H);
				++count;
			}
			j = j2;
		}
		// a fresh mark has been blitted and now awaits its restore;
		// a restored block has had its second blit and is done
		for (int j = 0; j < rows; ++j) {
			_blkShown[j] = _blkDirty[j];
			_blkDirty[j] = 0;
			_blkOwed[j] = 0;
		}
		// With the display list a still frame copies nothing, but a
		// palette change waiting for the stub must still reach the
		// hardware. A double-buffered stub would flip and wait for the
		// beam, so it only hears of frames that changed.
		if (count != 0 || !g_options.double_buffer) {
			_stub->updateScreen(_shakeOffset);
		}
	}
#else
	} else {
		int i, j;
		int count = 0;
		uint8_t *p = _screenBlocks;
		for (j = 0; j < _h / SCREENBLOCK_H; ++j) {
			int nh = 0;
			for (i = 0; i < _w / SCREENBLOCK_W; ++i) {
				if (p[i] != 0) {
					--p[i];
					++nh;
				} else if (nh != 0) {
					const int x = (i - nh) * SCREENBLOCK_W;
					VIDEO_BLIT(x, j * SCREENBLOCK_H, nh * SCREENBLOCK_W, SCREENBLOCK_H);
					nh = 0;
					++count;
				}
			}
			if (nh != 0) {
				const int x = (i - nh) * SCREENBLOCK_W;
				VIDEO_BLIT(x, j * SCREENBLOCK_H, nh * SCREENBLOCK_W, SCREENBLOCK_H);
				++count;
			}
			p += _w / SCREENBLOCK_W;
		}
		if (count != 0) {
			_stub->updateScreen(_shakeOffset);
		}
	}
#endif
#undef VIDEO_BLIT
	if (_shakeOffset != 0) {
		_shakeOffset = 0;
#ifndef ATARIST
		// the page was drawn offset; put it back
		_fullRefresh = true;
#endif
		// The ST stub ignores the offset, so there is nothing to put
		// back - and a full refresh there is a VBL wait plus a whole-
		// layer restore and copy, some 40ms on every frame a script
		// shakes the screen.
	}
}

void Video::updateWidescreen() {
	if (_stub->hasWidescreen()) {
		if (_widescreenMode == kWidescreenMirrorRoom) {
			_stub->copyWidescreenMirror(_w, _h, _backLayer);
		} else if (_widescreenMode == kWidescreenBlur) {
			_stub->copyWidescreenBlur(_w, _h, _backLayer);
		} else if (_widescreenMode == kWidescreenCDi) {
		} else {
			_stub->clearWidescreen();
		}
	}
}

void Video::fullRefresh() {
	debug(DBG_VIDEO, "Video::fullRefresh()");
	_fullRefresh = true;
#ifndef ATARIST
	memset(_screenBlocks, 0, (_w / SCREENBLOCK_W) * (_h / SCREENBLOCK_H));
#endif
#ifdef ATARIST
	memset(_blkDirty, 0, sizeof(_blkDirty));
	memset(_blkShown, 0, sizeof(_blkShown));
	memset(_blkOwed, 0, sizeof(_blkOwed));
	_dlFull = true;
#endif
}

void Video::fadeOut() {
	debug(DBG_VIDEO, "Video::fadeOut()");
	if (g_options.fade_out_palette && !_stub->hasWidescreen()) {
		fadeOutPalette();
	} else {
		_stub->fadeScreen();
	}
}

void Video::fadeOutPalette() {
	for (int step = 16; step >= 0; --step) {
		for (int c = 0; c < 256; ++c) {
			Color col;
			_stub->getPaletteEntry(c, &col);
			col.r = col.r * step >> 4;
			col.g = col.g * step >> 4;
			col.b = col.b * step >> 4;
			_stub->setPaletteEntry(c, &col);
		}
		fullRefresh();
		updateScreen();
		_stub->sleep(50);
	}
}

void Video::setPaletteColorBE(int num, int offset) {
	const int color = READ_BE_UINT16(_res->_pal + offset * 2);
	Color c = AMIGA_convertColor(color, true);
	_stub->setPaletteEntry(num, &c);
}

void Video::setPaletteSlotBE(int palSlot, int palNum) {
	debug(DBG_VIDEO, "Video::setPaletteSlotBE()");
	const uint8_t *p = _res->_pal + palNum * 32;
	for (int i = 0; i < 16; ++i) {
		const int color = READ_BE_UINT16(p); p += 2;
		Color c = AMIGA_convertColor(color, true);
		_stub->setPaletteEntry(palSlot * 16 + i, &c);
	}
}

void Video::setPaletteSlotLE(int palSlot, const uint8_t *palData) {
	debug(DBG_VIDEO, "Video::setPaletteSlotLE()");
	for (int i = 0; i < 16; ++i) {
		const uint16_t color = READ_LE_UINT16(palData + i * 2);
		Color c = AMIGA_convertColor(color);
		_stub->setPaletteEntry(palSlot * 16 + i, &c);
	}
	if (palSlot == 4 && g_options.use_white_tshirt) {
		const Color color12 = AMIGA_convertColor(0x888);
		const Color color13 = AMIGA_convertColor((palData == _conradPal2) ? 0x888 : 0xCCC);
		_stub->setPaletteEntry(palSlot * 16 + 12, &color12);
		_stub->setPaletteEntry(palSlot * 16 + 13, &color13);
	}
}

void Video::setTextPalette() {
	debug(DBG_VIDEO, "Video::setTextPalette()");
	setPaletteSlotLE(0xE, _textPal);
	if (_res->isAmiga()) {
		Color c;
		c.r = c.g = 0xEE;
		c.b = 0;
		_stub->setPaletteEntry(0xE7, &c);
	}
}

void Video::setPalette0xF() {
	debug(DBG_VIDEO, "Video::setPalette0xF()");
	const uint8_t *p = _palSlot0xF;
	for (int i = 0; i < 16; ++i) {
		Color c;
		c.r = *p++;
		c.g = *p++;
		c.b = *p++;
		_stub->setPaletteEntry(0xF0 + i, &c);
	}
}

void Video::DOS_decodeLev(int level, int room) {
	uint8_t *tmp = _res->_mbk;
	_res->_mbk = _res->_bnq;
	_res->clearBankData();
	AMIGA_decodeLev(level, room);
	_res->_mbk = tmp;
	_res->clearBankData();
}

static void DOS_decodeMapPlane(int sz, const uint8_t *src, uint8_t *dst) {
	const uint8_t *end = src + sz;
	while (src < end) {
		int code = (int8_t)*src++;
		if (code < 0) {
			const int len = 1 - code;
			memset(dst, *src++, len);
			dst += len;
		} else {
			++code;
			memcpy(dst, src, code);
			src += code;
			dst += code;
		}
	}
}

void Video::DOS_decodeMap(int level, int room) {
	debug(DBG_VIDEO, "Video::DOS_decodeMap(%d)", room);
	assert(room < 0x40);
	int32_t off = READ_LE_UINT32(_res->_map + room * 6);
	if (off == 0) {
		error("Invalid room %d", room);
	}
	// int size = READ_LE_UINT16(_res->_map + room * 6 + 4);
	bool packed = true;
	if (off < 0) {
		off = -off;
		packed = false;
	}
	const uint8_t *p = _res->_map + off;
	_mapPalSlot1 = *p++;
	_mapPalSlot2 = *p++;
	_mapPalSlot3 = *p++;
	_mapPalSlot4 = *p++;
	if (level == 4 && room == 60) {
		// workaround for wrong palette colors (fire)
		_mapPalSlot4 = 5;
	}
	static const int kPlaneSize = GAMESCREEN_W * GAMESCREEN_H / 4;
	if (packed) {
		for (int i = 0; i < 4; ++i) {
			const int sz = READ_LE_UINT16(p); p += 2;
			DOS_decodeMapPlane(sz, p, _res->_scratchBuffer); p += sz;
			memcpy(_frontLayer + i * kPlaneSize, _res->_scratchBuffer, kPlaneSize);
		}
	} else {
		for (int i = 0; i < 4; ++i, p += kPlaneSize) {
			for (int y = 0; y < GAMESCREEN_H; ++y) {
				for (int x = 0; x < 64; ++x) {
					_frontLayer[i + x * 4 + GAMESCREEN_W * y] = p[x + 64 * y];
				}
			}
		}
	}
	memcpy(_backLayer, _frontLayer, _layerSize);
	DOS_setLevelPalettes();
}

void Video::DOS_setLevelPalettes() {
	debug(DBG_VIDEO, "Video::DOS_setLevelPalettes()");
	if (_unkPalSlot2 == 0) {
		_unkPalSlot2 = _mapPalSlot3;
	}
	if (_unkPalSlot1 == 0) {
		_unkPalSlot1 = _mapPalSlot3;
	}
	// background
	setPaletteSlotBE(0x0, _mapPalSlot1);
	// objects
	setPaletteSlotBE(0x1, _mapPalSlot2);
	setPaletteSlotBE(0x2, _mapPalSlot3);
	setPaletteSlotBE(0x3, _mapPalSlot4);
	// conrad
	if (_unkPalSlot1 == _mapPalSlot3) {
		setPaletteSlotLE(4, _conradPal1);
	} else {
		setPaletteSlotLE(4, _conradPal2);
	}
	// slot 5 is monster palette
	// foreground
	setPaletteSlotBE(0x8, _mapPalSlot1);
	setPaletteSlotBE(0x9, _mapPalSlot2);
	// inventory
	setPaletteSlotBE(0xA, _unkPalSlot2);
	setPaletteSlotBE(0xB, _mapPalSlot4);
	// slots 0xC and 0xD are cutscene palettes
	setTextPalette();
}

void Video::DOS_decodeIcn(const uint8_t *src, int num, uint8_t *dst) {
	const int offset = READ_LE_UINT16(src + num * 2);
	const uint8_t *p = src + offset + 2;
	for (int i = 0; i < 16 * 16 / 2; ++i) {
		*dst++ = p[i] >> 4;
		*dst++ = p[i] & 15;
	}
}

void Video::DOS_decodeSpc(const uint8_t *src, int w, int h, uint8_t *dst) {
	const int size = w * h / 2;
	for (int i = 0; i < size; ++i) {
		*dst++ = src[i] >> 4;
		*dst++ = src[i] & 15;
	}
}

void Video::DOS_decodeSpm(const uint8_t *dataPtr, uint8_t *dst) {
	const int len = 2 * READ_BE_UINT16(dataPtr); dataPtr += 2;
	uint8_t *dst2 = dst + 1024;
	for (int i = 0; i < len; ++i) {
		*dst2++ = dataPtr[i] >> 4;
		*dst2++ = dataPtr[i] & 15;
	}
	const uint8_t *src = dst + 1024;
	const uint8_t *end = src + len;
	do {
		const uint8_t code = *src++;
		if (code == 0xF) {
			uint8_t color = *src++;
			int count = *src++;
			if (color == 0xF) {
				count = (count << 4) | *src++;
				color = *src++;
			}
			count += 4;
			memset(dst, color, count);
			dst += count;
		} else {
			*dst++ = code;
		}
	} while (src < end);
}

#ifdef ATARIST
// Planes to chunky four pixels at a time: g_spread[p][n] puts the four
// bits of nibble n at bit p of four bytes, leftmost pixel in the
// first, so a plane's share of four pixels is one lookup. Testing a
// bit at a time cost ~700 cycles a pixel on a 68000 - the title
// picture took over six seconds to decode on an 8MHz ST.
static uint32_t g_spread[5][16];

// a long store where the 68000 allows one (big-endian, an even
// address: the sprite and icon buffers make no promise)
static inline void storeBE32(uint8_t *p, uint32_t v, bool aligned) {
#ifdef __m68k__
	if (aligned) {
		*(uint32_t *)p = v;
		return;
	}
#endif
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void initSpread() {
	if (g_spread[0][15] == 0) {
		for (int p = 0; p < 5; ++p) {
			for (int n = 0; n < 16; ++n) {
				uint32_t v = 0;
				for (int i = 0; i < 4; ++i) {
					if (n & (8 >> i)) {
						v |= 1u << (8 * (3 - i) + p);
					}
				}
				g_spread[p][n] = v;
			}
		}
	}
}

static void AMIGA_planar16(uint8_t *dst, int w, int h, int depth, const uint8_t *src) {
	initSpread();
	const int pitch = w * 16;
	const int planarSize = w * 2 * h;
	const bool aligned = ((uintptr_t)dst & 1) == 0;   // pitch keeps every row the same
	for (int y = 0; y < h; ++y) {
		uint8_t *d = dst;
		for (int x = 0; x < w; ++x) {
			uint32_t q0 = 0, q1 = 0, q2 = 0, q3 = 0;
			const uint8_t *s = src;
			for (int bit = 0; bit < depth; ++bit, s += planarSize) {
				const uint32_t *t = g_spread[bit];
				const uint8_t hi = s[0], lo = s[1];
				q0 |= t[hi >> 4];
				q1 |= t[hi & 15];
				q2 |= t[lo >> 4];
				q3 |= t[lo & 15];
			}
			storeBE32(d, q0, aligned);
			storeBE32(d + 4, q1, aligned);
			storeBE32(d + 8, q2, aligned);
			storeBE32(d + 12, q3, aligned);
			d += 16;
			src += 2;
		}
		dst += pitch;
	}
}
#else
static void AMIGA_planar16(uint8_t *dst, int w, int h, int depth, const uint8_t *src) {
	const int pitch = w * 16;
	const int planarSize = w * 2 * h;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			for (int i = 0; i < 16; ++i) {
				int color = 0;
				const int mask = 1 << (15 - i);
				for (int bit = 0; bit < depth; ++bit) {
					if (READ_BE_UINT16(src + bit * planarSize) & mask) {
						color |= 1 << bit;
					}
				}
				dst[x * 16 + i] = color;
			}
			src += 2;
		}
		dst += pitch;
	}
}
#endif

#ifdef ATARIST
// Eight pixels from four interleaved plane bytes, by the same table as
// AMIGA_planar16: two lookups a plane where the bit loop below tested
// every bit, ~320 cycles a pixel. Object pieces 8 and 24 pixels wide
// come this way - about half of them - each time one is decoded.
static void AMIGA_planar8(uint8_t *dst, int w, int h, const uint8_t *src) {
	assert((w & 7) == 0);
	initSpread();
	const bool aligned = ((uintptr_t)dst & 1) == 0;   // units are 8 bytes, so every one alike
	for (int n = (w >> 3) * h; --n >= 0; ) {
		const uint8_t b0 = src[0], b1 = src[1], b2 = src[2], b3 = src[3];
		const uint32_t q0 = g_spread[0][b0 >> 4] | g_spread[1][b1 >> 4] | g_spread[2][b2 >> 4] | g_spread[3][b3 >> 4];
		const uint32_t q1 = g_spread[0][b0 & 15] | g_spread[1][b1 & 15] | g_spread[2][b2 & 15] | g_spread[3][b3 & 15];
		storeBE32(dst, q0, aligned);
		storeBE32(dst + 4, q1, aligned);
		src += 4;
		dst += 8;
	}
	g_stPerf.planar8Px += (uint32_t)(w >> 3) * (uint32_t)(h << 3);
}

#else
static void AMIGA_planar8(uint8_t *dst, int w, int h, const uint8_t *src) {
	assert((w & 7) == 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w / 8; ++x) {
			for (int i = 0; i < 8; ++i) {
				int color = 0;
				const int mask = 1 << (7 - i);
				for (int bit = 0; bit < 4; ++bit) {
					if (src[bit] & mask) {
						color |= 1 << bit;
					}
				}
				dst[i] = color;
			}
			src += 4;
			dst += 8;
		}
	}
}
#endif

#ifndef ATARIST
// bit i of a plane byte (i = 0 leftmost) -> bit 0 of nibble i, so
// ORing the four planes shifted 0..3 builds eight 4-bit pixels in
// one 32-bit word instead of 32 bit tests.
static uint32_t g_planeSpread[256];

static void initPlaneSpread() {
	for (int b = 0; b < 256; ++b) {
		uint32_t v = 0;
		for (int i = 0; i < 8; ++i) {
			if (b & (1 << (7 - i))) {
				v |= 1u << (i * 4);
			}
		}
		g_planeSpread[b] = v;
	}
}

static void AMIGA_planar_mask(uint8_t *dst, int x0, int y0, int w, int h, uint8_t *src, uint8_t *mask, int size) {
	static bool spreadReady;
	if (!spreadReady) {
		initPlaneSpread();
		spreadReady = true;
	}
	dst += y0 * Video::GAMESCREEN_W + x0;
	const int size2 = size * 2;
	const int size3 = size * 3;
	for (int y = 0; y < h; ++y) {
		const int py = y0 + y;
		const bool rowVisible = (py >= 0 && py < Video::GAMESCREEN_H);
		for (int x = 0; x < w * 2; ++x) {
			// The four plane bytes were addressed as mask[j * size],
			// a runtime 32-bit multiply - a __mulsi3 call on 68000 -
			// per plane per PIXEL. Read them once per byte instead,
			// and only build the colour for pixels actually drawn.
			const uint8_t s0 = *src++;
			if (s0 != 0 && rowVisible) {
				uint32_t nib = g_planeSpread[mask[0]]
				             | (g_planeSpread[mask[size]] << 1)
				             | (g_planeSpread[mask[size2]] << 2)
				             | (g_planeSpread[mask[size3]] << 3);
				uint32_t drawn = g_planeSpread[s0];
				uint8_t *d = dst + 8 * x;
				const int px0 = x0 + 8 * x;
				// walk the nibbles from pixel 0 up; a constant >>= 4
				// beats a variable shift per pixel
				for (int i = 0; i < 8; ++i) {
					if (drawn & 1) {
						const int px = px0 + i;
						if (px >= 0 && px < Video::GAMESCREEN_W) {
							d[i] = (uint8_t)(nib & 15);
						}
					}
					nib >>= 4;
					drawn >>= 4;
				}
			}
			++mask;
		}
		dst += Video::GAMESCREEN_W;
	}
}
#endif

#if defined(ATARIST) && defined(__m68k__)
// The RLE runs are short (a few bytes each), so the memcpy/memset
// calls of the C version cost more than the bytes they moved: a
// room's tiles decoded at over a hundred cycles a byte. Straight
// byte loops, in place.
__asm__(
"    .text\n"
"    .even\n"
"_AMIGA_decodeRle:\n"
"    move.l 4(%sp),%a1\n"            /* dst */
"    move.l 8(%sp),%a0\n"            /* src */
"    move.l %a2,-(%sp)\n"
"    moveq  #0,%d0\n"
"    move.b (%a0)+,%d0\n"
"    lsl.w  #8,%d0\n"
"    move.b (%a0)+,%d0\n"
"    and.w  #0x7FFF,%d0\n"
"    lea    (%a0,%d0.w),%a2\n"       /* end of the packed data */
"rle_next:\n"
"    cmp.l  %a2,%a0\n"
"    bhs.s  rle_done\n"
"    moveq  #0,%d0\n"
"    move.b (%a0)+,%d0\n"
"    bmi.s  rle_rep\n"
"    move.l %a0,%d1\n"               /* literal of code+1 bytes, clipped at the end */
"    add.l  %d0,%d1\n"
"    addq.l #1,%d1\n"
"    cmp.l  %a2,%d1\n"
"    bls.s  rle_lit\n"
"    move.l %a2,%d0\n"
"    sub.l  %a0,%d0\n"
"    subq.w #1,%d0\n"
"    bmi.s  rle_next\n"
"rle_lit:\n"
"    move.b (%a0)+,(%a1)+\n"
"    dbra   %d0,rle_lit\n"
"    bra.s  rle_next\n"
"rle_rep:\n"
"    neg.w  %d0\n"                   /* 257 - code copies, less one for dbra */
"    add.w  #256,%d0\n"
"    move.b (%a0)+,%d1\n"
"rle_fill:\n"
"    move.b %d1,(%a1)+\n"
"    dbra   %d0,rle_fill\n"
"    bra.s  rle_next\n"
"rle_done:\n"
"    move.l (%sp)+,%a2\n"
"    rts\n"
);
extern "C" void AMIGA_decodeRle(uint8_t *dst, const uint8_t *src);
#else
static void AMIGA_decodeRle(uint8_t *dst, const uint8_t *src) {
	const int size = READ_BE_UINT16(src) & 0x7FFF; src += 2;
	for (int i = 0; i < size; ) {
		int code = src[i++];
		if ((code & 0x80) == 0) {
			++code;
			if (i + code > size) {
				code = size - i;
			}
			memcpy(dst, &src[i], code);
			i += code;
		} else {
			code = 1 - ((int8_t)code);
			memset(dst, src[i], code);
			++i;
		}
		dst += code;
	}
}
#endif

#ifndef ATARIST
static void DOS_drawTileMask(uint8_t *dst, int x0, int y0, int w, int h, uint8_t *m, uint8_t *p, int size) {
	assert(size == (w * 2 * h));
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const int bits = READ_BE_UINT16(m); m += 2;
			for (int bit = 0; bit < 8; ++bit) {
				const int j = y0 + y;
				const int i = x0 + 2 * (x * 8 + bit);
				if (i >= 0 && i < Video::GAMESCREEN_W && j >= 0 && j < Video::GAMESCREEN_H) {
					const uint8_t color = *p;
					if (bits & (1 << (15 - (bit * 2)))) {
						dst[j * Video::GAMESCREEN_W + i] = color >> 4;
					}
					if (bits & (1 << (15 - (bit * 2 + 1)))) {
						dst[j * Video::GAMESCREEN_W + i + 1] = color & 15;
					}
				}
				++p;
			}
		}
	}
}

#endif

#ifdef ATARIST
// SGD scenery for the planar layer. A room's placement list names
// the same tile again and again, out of order, and the original
// decoder (below) re-unpacks the tile every time the index changes:
// the jungle rooms place two or three hundred tiles from about a
// hundred distinct ones, so most of the room build was spent
// decoding and remapping the same tile for the fifth time.
//
// Tiles are prepared once into cells (video_st.cpp) and kept for
// the whole level, keyed on the SGD data and the version of the
// colours of palette base 0, the only one they are drawn in (a room
// whose other slots change keeps them): the jungle rooms share most
// of their scenery, so a
// second visit or a neighbouring room finds nearly everything
// ready. The cells live in one pool filled front to back and
// wrapped; a wrap forgets whatever it overwrites. The pool holds
// the largest room's set with room to spare, and is released when
// a level without SGD scenery loads.
enum {
	kSgdPoolSize = 96 * 1024,
	kSgdSlots = 256                     // more than any SGD has tiles
};

struct SgdSlot {
	uint8_t *cells;                     // 0: nothing prepared here
	uint32_t size;                      // bytes of cells
	uint16_t id;                        // tile index the cells are for
	uint16_t units;
	uint16_t rows;
};

static uint8_t *g_sgdPool;
static uint32_t g_sgdPos;
static bool g_sgdWrapped;               // cells beyond g_sgdPos may be live
static SgdSlot g_sgdSlots[kSgdSlots];
static const uint8_t *g_sgdData;
static uint16_t g_sgdGen;

static void ST_sgdForgetAll() {
	memset(g_sgdSlots, 0, sizeof(g_sgdSlots));
	g_sgdPos = 0;
	g_sgdWrapped = false;
}

static void ST_sgdRelease() {
	free(g_sgdPool);
	g_sgdPool = 0;
	g_sgdData = 0;
	ST_sgdForgetAll();
}

void Video::ST_releaseLevelCaches() {
	ST_sgdRelease();
	ST_sprRelease();
	ST_roomCacheRelease();
}

// the tile's cells, preparing them if they are not in the pool
static const SgdSlot *ST_sgdTile(const uint8_t *data, int num, uint8_t *buf, int bufSize) {
	SgdSlot *slot = &g_sgdSlots[num & (kSgdSlots - 1)];
	if (slot->cells && slot->id == num) {
		return slot;
	}
	const int32_t offset = READ_BE_UINT32(data + num * 4);
	if (offset < 0) {
		const uint8_t *ptr = data - offset;
		const int size = READ_BE_UINT16(ptr); ptr += 2;
		assert(size <= bufSize);
		memcpy(buf, ptr, size);
	} else {
		const int size = READ_BE_UINT16(data + offset) & 0x7FFF;
		assert(size <= bufSize);
		AMIGA_decodeRle(buf, data + offset);
	}
	const int units = ((buf[0] + 1) >> 1) * 2;
	const int rows = buf[1] + 1;
	const int planarSize = READ_BE_UINT16(buf + 2);
	const uint32_t need = (uint32_t)units * rows * 8;
	assert(need <= kSgdPoolSize);
	if (g_sgdPos + need > kSgdPoolSize) {
		g_sgdPos = 0;
		g_sgdWrapped = true;
	}
	uint8_t *cells = g_sgdPool + g_sgdPos;
	g_sgdPos += need;
	// whatever was prepared where these cells go is gone
	if (g_sgdWrapped) {
		for (int i = 0; i < kSgdSlots; ++i) {
			SgdSlot *s = &g_sgdSlots[i];
			if (s->cells && s->cells < cells + need && cells < s->cells + s->size) {
				s->cells = 0;
			}
		}
	}
	ST_amigaPrepare(cells, buf + 4 + planarSize, planarSize, units, rows, buf + 4, 0);
	slot->cells = cells;
	slot->size = need;
	slot->id = (uint16_t)num;
	slot->units = (uint16_t)units;
	slot->rows = (uint16_t)rows;
	return slot;
}

// The list is walked twice: once to resolve every entry to its
// cells, then backwards to place them front to back with coverage
// (ST_amigaPlaceCov), so the scenery under later tiles is never
// painted. Rooms with more entries than the table holds - none in
// the shipped data - take the plain back-to-front route.
enum { kSgdMaxPlace = 512 };

struct SgdPlace {
	const SgdSlot *tile;
	int16_t x, y;
};

static void ST_decodeSgd(uint8_t *dst, const uint8_t *src, const uint8_t *data, int level, int room) {
	if (!g_sgdPool) {
		g_sgdPool = (uint8_t *)malloc(kSgdPoolSize);
		if (!g_sgdPool) {
			error("Unable to allocate SGD cell pool");
		}
	}
	const uint32_t *pair;
	const uint16_t ver = ST_tilePalette(0, &pair);
	if (g_sgdData != data || g_sgdGen != ver) {
		ST_sgdForgetAll();
		g_sgdData = data;
		g_sgdGen = ver;
	}
	uint8_t buf[256 * 32];
	SgdPlace list[kSgdMaxPlace];
	const SgdSlot *tile = 0;
	const int count = READ_BE_UINT16(src); src += 2;
	const bool covered = count <= kSgdMaxPlace;
	if (!covered) {
		ST_clearLayer(dst, 0);
	}
	int n = 0;
	for (int i = 0; i < count; ++i) {
		int d2 = READ_BE_UINT16(src); src += 2;
		const int x_pos = (int16_t)READ_BE_UINT16(src); src += 2;
		int y_pos = (int16_t)READ_BE_UINT16(src); src += 2;
		if (d2 != 0xFFFF) {
			d2 &= ~(1 << 15);
			tile = ST_sgdTile(data, d2, buf, sizeof(buf));
		} else {
			d2 = tile ? tile->id : -1;
		}
		if (level == 0 && room == 26 && d2 == 38 && y_pos == 176) {
			y_pos += 8;
		}
		if (!tile) {
			continue;
		}
		if (!covered) {
			ST_amigaPlace(dst, x_pos, y_pos, tile->cells, tile->units, tile->rows, false);
			continue;
		}
		list[n].tile = tile;
		list[n].x = (int16_t)x_pos;
		list[n].y = (int16_t)y_pos;
		++n;
	}
	if (covered) {
		while (--n >= 0) {
			const SgdPlace &p = list[n];
			ST_amigaPlaceCov(dst, p.x, p.y, p.tile->cells, p.tile->units, p.tile->rows);
		}
		ST_amigaPlaceCovEnd(dst, 0);
	}
}
#else
typedef void (*DrawTileMaskProc)(uint8_t *dst, int x0, int y0, int w, int h, uint8_t *src, uint8_t *mask, int size);

static void decodeSgd(uint8_t *dst, const uint8_t *src, const uint8_t *data, DrawTileMaskProc drawTileMask, int level, int room) {
	int num = -1;
	uint8_t buf[256 * 32];
	int count = READ_BE_UINT16(src) - 1; src += 2;
	do {
		int d2 = READ_BE_UINT16(src); src += 2;
		int x_pos = (int16_t)READ_BE_UINT16(src); src += 2;
		int y_pos = (int16_t)READ_BE_UINT16(src); src += 2;
		if (d2 != 0xFFFF) {
			d2 &= ~(1 << 15);
			const int32_t offset = READ_BE_UINT32(data + d2 * 4);
			if (offset < 0) {
				const uint8_t *ptr = data - offset;
				const int size = READ_BE_UINT16(ptr); ptr += 2;
				if (num != d2) {
					num = d2;
					assert(size <= (int)sizeof(buf));
					memcpy(buf, ptr, size);
				}
                        } else {
				if (num != d2) {
					num = d2;
					const int size = READ_BE_UINT16(data + offset) & 0x7FFF;
					assert(size <= (int)sizeof(buf));
					AMIGA_decodeRle(buf, data + offset);
				}
			}
		}
		if (level == 0 && room == 26 && d2 == 38 && y_pos == 176) {
			y_pos += 8;
		}
		const int w = (buf[0] + 1) >> 1;
		const int h = buf[1] + 1;
		const int planarSize = READ_BE_UINT16(buf + 2);
		drawTileMask(dst, x_pos, y_pos, w, h, buf + 4, buf + 4 + planarSize, planarSize);
	} while (--count >= 0);
}
#endif

#ifndef ATARIST
static const uint8_t *AMIGA_mirrorTileY(const uint8_t *a2) {
	static uint8_t buf[32];

        a2 += 24;
	for (int j = 0; j < 4; ++j) {
		for (int i = 0; i < 8; ++i) {
			buf[31 - j * 8 - i] = *a2++;
		}
		a2 -= 16;
	}
	return buf;
}

static const uint8_t *AMIGA_mirrorTileX(const uint8_t *a2) {
	static uint8_t buf[32];

	for (int i = 0; i < 32; ++i) {
		uint8_t mask = 0;
		for (int bit = 0; bit < 8; ++bit) {
			if (a2[i] & (1 << bit)) {
				mask |= 1 << (7 - bit);
			}
		}
		buf[i] = mask;
	}
	return buf;
}

static void AMIGA_drawTile(uint8_t *dst, int x, int y, const uint8_t *src, int pal, const bool xflip, const bool yflip, int colorKey) {
	const int pitch = Video::GAMESCREEN_W;
	dst += y * pitch + x;
	if (yflip) {
		src = AMIGA_mirrorTileY(src);
	}
	if (xflip) {
		src = AMIGA_mirrorTileX(src);
	}
	for (int y = 0; y < 8; ++y) {
		for (int i = 0; i < 8; ++i) {
			const int mask = 1 << (7 - i);
			int color = 0;
			for (int bit = 0; bit < 4; ++bit) {
				if (src[8 * bit] & mask) {
					color |= 1 << bit;
				}
			}
			if (color != colorKey) {
				dst[i] = pal + color;
			}
		}
		++src;
		dst += pitch;
	}
}

static void DOS_drawTile(uint8_t *dst, int x, int y, const uint8_t *src, int mask, const bool xflip, const bool yflip, int colorKey) {
	int pitch = Video::GAMESCREEN_W;
	dst += y * pitch + x;
	if (yflip) {
		dst += 7 * pitch;
		pitch = -pitch;
	}
	int inc = 1;
	if (xflip) {
		dst += 7;
		inc = -inc;
	}
	for (int y = 0; y < 8; ++y) {
		for (int i = 0; i < 8; ++src) {
			int color = *src >> 4;
			if (color != colorKey) {
				dst[inc * i] = mask | color;
			}
			++i;
			color = *src & 15;
			if (color != colorKey) {
				dst[inc * i] = mask | color;
			}
			++i;
		}
		dst += pitch;
	}
}
#endif

#ifdef ATARIST
// Room tiles straight into the planar layer (see video_st.cpp):
// prepared once per tile, palette and flip through the colour remap,
// placed by byte stores. A room draws about a thousand tiles, three
// to five hundred of them different, and packing one costs about a
// millisecond on an 8MHz ST, so the prepared cells are kept from room
// to room: two ways a set, keyed on where the tile is in its bank
// (the room's own buffer is a copy, made afresh for every room), its
// flags, and the version of its palette base's colours. A bank stays
// where it is until the arena is emptied, which the bank epoch counts;
// then they all go. A machine with 3.5MB keeps 1024, enough for most
// of what neighbouring rooms share, in one GEMDOS block taken at a
// level's first room and given back with the room cache (a level load,
// the title, a cutscene short of RAM), so it never sits between one
// level's data and the next; others 256, about one room's.
// Parallel arrays rather than a struct: a 64-byte cell block indexes
// by a shift where a struct's odd size costs a multiply call.
enum { kTilesSmall = 256, kTilesBig = 1024, kTileBufTiles = 1024 };
static const uint8_t *g_stTileSrcS[kTilesSmall];
static uint8_t g_stTileFlagsS[kTilesSmall];
static uint16_t g_stTileVerS[kTilesSmall];
static uint8_t g_stTileCellsS[kTilesSmall][8 * 8];
static const uint8_t **g_stTileSrc = g_stTileSrcS;
static uint8_t *g_stTileFlags = g_stTileFlagsS;
static uint16_t *g_stTileVer = g_stTileVerS;
static uint8_t (*g_stTileCells)[8 * 8] = g_stTileCellsS;
static int g_stTileSets = kTilesSmall / 2;
static bool g_stTileSized;
static uint8_t g_stTileNext[kTilesBig / 2];      // per set, the way to replace next
static uint32_t g_stTileEpoch = 0xFFFFFFFF;      // the bank epoch the keys are good for
// this room: which bank tile each 32 bytes of its buffer copies, and
// the palette bases' pair tables and versions
static const uint8_t *g_stTileOrig[kTileBufTiles];
static const uint8_t *g_stTileBuf;
static int g_stTileCount;
static const uint32_t *g_stTilePair[4];
static uint16_t g_stTileVerNow[4];

static uint8_t *g_stTileBlock;                   // the big table's block, from GEMDOS

static void ST_clearTiles() {
	memset(g_stTileSrc, 0, g_stTileSets * 2 * sizeof(*g_stTileSrc));
}

static void ST_tilesRelease() {
	if (g_stTileBlock) {
		Mfree(g_stTileBlock);
		g_stTileBlock = 0;
		g_stTileSrc = g_stTileSrcS;
		g_stTileFlags = g_stTileFlagsS;
		g_stTileVer = g_stTileVerS;
		g_stTileCells = g_stTileCellsS;
		g_stTileSets = kTilesSmall / 2;
	}
	g_stTileSized = false;
	g_stTileEpoch = 0xFFFFFFFF;                  // whichever table is used next starts empty
}

// before a room's tiles are drawn, with its palettes set: count tiles
// copied into buf, stable when the bank arena was not emptied while
// they were (two tiles might otherwise have had the same address)
static void ST_tilesForRoom(const uint8_t *buf, int count, bool stable, uint32_t epoch) {
	if (!g_stTileSized) {
		g_stTileSized = true;
		extern int ATARIST_ramK();
		enum { kBlock = kTilesBig * (8 * 8 + sizeof(*g_stTileSrc) + sizeof(*g_stTileVer) + 1) };
		if (ATARIST_ramK() >= 3584) {
			// cells first: 64 bytes each keeps what follows aligned
			uint8_t *p = (uint8_t *)Malloc(kBlock);
			if ((long)p > 0) {
				g_stTileBlock = p;
				g_stTileCells = (uint8_t (*)[8 * 8])p;
				p += kTilesBig * 8 * 8;
				g_stTileSrc = (const uint8_t **)p;
				p += kTilesBig * sizeof(*g_stTileSrc);
				g_stTileVer = (uint16_t *)p;
				p += kTilesBig * sizeof(*g_stTileVer);
				g_stTileFlags = p;
				g_stTileSets = kTilesBig / 2;
				g_stTileEpoch = 0xFFFFFFFF;
			}
		}
	}
	g_stTileBuf = buf;
	g_stTileCount = count;
	if (!stable) {
		// this room's go by their place in buf, and the next room's
		// start afresh
		for (int i = 0; i < count; ++i) {
			g_stTileOrig[i] = buf + i * 32;
		}
		ST_clearTiles();
		g_stTileEpoch = 0xFFFFFFFF;
	} else if (epoch != g_stTileEpoch) {
		ST_clearTiles();
		g_stTileEpoch = epoch;
	}
	static const uint8_t kBase[4] = { 0x00, 0x10, 0x80, 0x90 };
	for (int b = 0; b < 4; ++b) {
		g_stTileVerNow[b] = ST_tilePalette(kBase[b], &g_stTilePair[b]);
	}
	ST_prioTouched();
}

// the cells for one room tile, from the cache or packed into it; flags
// as the cache keys them: the palette base (0x00, 0x10, 0x80 or 0x90),
// 1 for an x flip, 2 for a y flip, 4 for colour 0 transparent
static const uint8_t *ST_tileCells(const uint8_t *src, uint8_t flags) {
	const int base = ((flags >> 4) & 1) | ((flags >> 6) & 2);
	const unsigned long n = (unsigned long)((src - g_stTileBuf) >> 5);
	if (n >= (unsigned long)g_stTileCount) {
		// past what the room copied (upstream reads the buffer's
		// leftovers there): drawn, never kept
		static uint8_t cells[8 * 8];
		ST_amigaTile8(cells, src, g_stTilePair[base], (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0);
		return cells;
	}
	const uint8_t *key = g_stTileOrig[n];
	const uint16_t ver = g_stTileVerNow[base];
	// tiles a bank's width apart would share the low bits: fold the
	// next ones in (16-bit, constant shifts)
	const uint16_t h = (uint16_t)((uintptr_t)key >> 5);
	const int set = (h ^ (h >> 4) ^ flags) & (g_stTileSets - 1);
	int i = set << 1;
	if (g_stTileSrc[i] == key && g_stTileFlags[i] == flags && g_stTileVer[i] == ver) {
		g_stTileNext[set] = 1;
	} else if (g_stTileSrc[i + 1] == key && g_stTileFlags[i + 1] == flags && g_stTileVer[i + 1] == ver) {
		++i;
		g_stTileNext[set] = 0;
	} else {
		i += g_stTileNext[set];
		g_stTileNext[set] ^= 1;
		ST_amigaTile8(g_stTileCells[i], src, g_stTilePair[base], (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0);
		g_stTileSrc[i] = key;
		g_stTileFlags[i] = flags;
		g_stTileVer[i] = ver;
		++g_stPerf.tilePreps;
	}
	return g_stTileCells[i];
}

// decodeLevHelper's two passes for the ST: the 32 x 28 grid of 8x8
// tiles behind, then in front, each from the tile cache and placed by
// its kernel with nothing between. A call through a pointer for each
// grid word and eight arguments for each tile cost as much as placing
// it. The background is drawn with no transparent colour, so its
// tiles are opaque throughout and take the kernel without the mask
// tests. The grid words are upstream's: the tile (11 bits), x flip
// (bit 11), y flip (12), the palette bits (13-15).
static void ST_decodeLevTiles(uint8_t *dst, const uint8_t *src, int offset10, int offset12, const uint8_t *a5, bool sgdBuf) {
	if (offset10 != 0) {
		const uint8_t *a0 = src + offset10;
		uint8_t *planes = dst;
		uint8_t *prio = dst + kSTPlaneBytes;
		for (int y = 0; y < Video::GAMESCREEN_H; y += 8, planes += 8 * kSTRowBytes, prio += 8 * kSTPrioRowBytes) {
			for (int c = 0; c < Video::GAMESCREEN_W / 8; ++c, a0 += 2) {
				const int d3 = READ_BE_UINT16(a0);
				const int d0 = d3 & 0x7FF;
				if (d0 != 0) {
					uint8_t mask = 0;
					if ((d3 & 0x8000) != 0) {
						mask = 0x80 + ((d3 >> 6) & 0x10);
					}
					const uint8_t *cells = ST_tileCells(a5 + d0 * 32, (uint8_t)(mask | ((d3 >> 11) & 3)));
					placeTile8OpaqueAsm(planes + ((c >> 1) << 3) + (c & 1), prio + c, cells, (mask & 0x80) ? 0xFF : 0);
				}
			}
		}
	}
	if (offset12 != 0) {
		const uint8_t *a0 = src + offset12;
		uint8_t *planes = dst;
		uint8_t *prio = dst + kSTPlaneBytes;
		for (int y = 0; y < Video::GAMESCREEN_H; y += 8, planes += 8 * kSTRowBytes, prio += 8 * kSTPrioRowBytes) {
			for (int c = 0; c < Video::GAMESCREEN_W / 8; ++c, a0 += 2) {
				const int d3 = READ_BE_UINT16(a0);
				int d0 = d3 & 0x7FF;
				if (d0 != 0 && sgdBuf) {
					d0 -= 0x380;
				}
				if (d0 != 0) {
					uint8_t mask = 0;
					if ((d3 & 0x6000) != 0 && sgdBuf) {
						mask = 0x10;
					} else if ((d3 & 0x8000) != 0) {
						mask = 0x80 + ((d3 >> 6) & 0x10);
						if (d3 & 0x4000) {
							mask = 0x90;
						}
					}
					const uint8_t *cells = ST_tileCells(a5 + d0 * 32, (uint8_t)(mask | ((d3 >> 11) & 3) | 4));
					placeTile8Asm(planes + ((c >> 1) << 3) + (c & 1), prio + c, cells, (mask & 0x80) ? 0xFF : 0);
				}
			}
		}
	}
}
#endif

typedef void (*DrawTileProc)(uint8_t *dst, int x, int y, const uint8_t *src, int pal, const bool xflip, const bool yflip, int colorKey);

#ifndef ATARIST
static void decodeLevHelper(uint8_t *dst, const uint8_t *src, int offset10, int offset12, const uint8_t *a5, bool sgdBuf, DrawTileProc drawTile, uint16_t (*read16)(const void *)) {
	if (offset10 != 0) {
		const uint8_t *a0 = src + offset10;
		for (int y = 0; y < Video::GAMESCREEN_H; y += 8) {
			for (int x = 0; x < Video::GAMESCREEN_W; x += 8) {
				const int d3 = read16(a0); a0 += 2;
				const int d0 = d3 & 0x7FF;
				if (d0 != 0) {
					const uint8_t *a2 = a5 + d0 * 32;
					const bool yflip = (d3 & (1 << 12)) != 0;
					const bool xflip = (d3 & (1 << 11)) != 0;
					int mask = 0;
					if ((d3 & 0x8000) != 0) {
						mask = 0x80 + ((d3 >> 6) & 0x10);
					}
					drawTile(dst, x, y, a2, mask, xflip, yflip, -1);
				}
			}
		}
	}
	if (offset12 != 0) {
		const uint8_t *a0 = src + offset12;
		for (int y = 0; y < Video::GAMESCREEN_H; y += 8) {
			for (int x = 0; x < Video::GAMESCREEN_W; x += 8) {
				const int d3 = read16(a0); a0 += 2;
				int d0 = d3 & 0x7FF;
				if (d0 != 0 && sgdBuf) {
					d0 -= 0x380;
				}
				if (d0 != 0) {
					const uint8_t *a2 = a5 + d0 * 32;
					const bool yflip = (d3 & (1 << 12)) != 0;
					const bool xflip = (d3 & (1 << 11)) != 0;
					int mask = 0;
					if ((d3 & 0x6000) != 0 && sgdBuf) {
						mask = 0x10;
					} else if ((d3 & 0x8000) != 0) {
						mask = 0x80 + ((d3 >> 6) & 0x10);
						if (d3 & 0x4000) {
							mask = 0x90;
						}
					}
					drawTile(dst, x, y, a2, mask, xflip, yflip, 0);
				}
			}
		}
	}
}
#endif

void Video::PC98_decodeMap(int level, int room) {
	const uint16_t size = READ_LE_UINT16(_res->_map + room * 6 + 4);
	if (size == 0) {
		return;
	}
	const uint32_t offset = READ_LE_UINT32(_res->_map + room * 6);
	const uint8_t *p = _res->_map + offset;
	_mapPalSlot1 = *p++;
	_mapPalSlot2 = *p++;
	_mapPalSlot3 = *p++;
	_mapPalSlot4 = *p++;
	static const int kPlaneSize = GAMESCREEN_W * GAMESCREEN_H / 4;
	for (int i = 0; i < 4; ++i) {
		const int plane_size = READ_LE_UINT16(p); p += 2;
		pc98_unpack(_frontLayer + i * kPlaneSize, kPlaneSize, p, plane_size);
		p += plane_size;
	}
	for (int i = 0; i < GAMESCREEN_W * GAMESCREEN_H; ++i) {
		_frontLayer[i] &= ~0x40;
	}
	memcpy(_backLayer, _frontLayer, _layerSize);
	DOS_setLevelPalettes();
}

#ifdef ATARIST
// Decoded rooms, kept for going back. Entering a room unpacks its data,
// unpacks the banks its tiles come from and prepares and places about a
// thousand tiles: a second or more on an 8MHz ST, half that on a Mega
// STE, every time - and walking back and forth between two rooms is
// most of what Flashback is. The room as drawn (planes and priority)
// is kept with its header, keyed on the room and the colour remap it
// was drawn with: the header sets the palettes up again, the remap they
// give is checked, and if it is the same the room is copied back. Only
// where RAM is to spare (a 4MB machine), and given back with the level.
struct STRoomCache {
	uint8_t *layer;             // kSTLayerSize, from GEMDOS; 0 = empty
	uint32_t remapHash;
	uint32_t used;
	int16_t level, levNum, room;
	uint8_t hdr[16];            // the room data's header: SGD flag, palette slots
};
enum { kRoomCacheMax = 16, kRoomCacheSpareK = 500 };
static STRoomCache g_roomCache[kRoomCacheMax];
static uint32_t g_roomUse;

// What a room is drawn from: its tiles and scenery take their colours
// from bases 0x00, 0x10, 0x80 and 0x90 and nothing else. Hashing just
// those keeps a room good when a monster's colours or the text's move.
static uint32_t ST_remapHash() {
	const uint32_t *p = (const uint32_t *)ST_getRemap();
	uint32_t h = 0;
	for (int n = 32 / 4; --n >= 0; ) {
		h = ((h << 5) | (h >> 27)) + p[n];
		h = ((h << 5) | (h >> 27)) + p[(0x80 / 4) + n];
	}
	return h;
}

void ST_roomCacheRelease() {
	for (int i = 0; i < kRoomCacheMax; ++i) {
		if (g_roomCache[i].layer) {
			Mfree(g_roomCache[i].layer);
			g_roomCache[i].layer = 0;
		}
	}
	ST_tilesRelease();
}

static STRoomCache *ST_roomFind(int level, int levNum, int room) {
	for (int i = 0; i < kRoomCacheMax; ++i) {
		STRoomCache *e = &g_roomCache[i];
		if (e->layer && e->level == level && e->levNum == levNum && e->room == room) {
			return e;
		}
	}
	return 0;
}

bool Video::ST_roomFromCache(int level, int room) {
	if (g_options.ab & kAbNoRoomCache) {
		return false;
	}
	STRoomCache *e = ST_roomFind(level, _res->_levNum, room);
	if (!e) {
		return false;
	}
	ST_paletteForRoom(level + 1, _res->_levNum, room);
	AMIGA_setLevelPalettes(level, e->hdr);
	if (ST_remapHash() != e->remapHash) {
		++g_stPerf.roomRemap;
		return false;              // other colours now: drawn afresh, the same setup again
	}
	++g_stPerf.roomHits;
	if (!_res->_sgd) {
		ST_sgdRelease();
	}
	memcpy(_frontLayer, e->layer, kSTLayerSize);
	ST_copyLayer(_backLayer, _frontLayer);
	++_stBackGen;
	e->used = ++g_roomUse;
	return true;
}

void Video::ST_roomToCache(int level, int room, const uint8_t *hdr) {
	if (g_options.ab & kAbNoRoomCache) {
		return;
	}
	STRoomCache *e = ST_roomFind(level, _res->_levNum, room);
	if (!e) {
		// an empty slot if the RAM is there, else the one longest unused
		STRoomCache *lru = 0;
		for (int i = 0; i < kRoomCacheMax && !e; ++i) {
			STRoomCache *c = &g_roomCache[i];
			if (!c->layer) {
				if (((long)Malloc(-1) >> 10) >= (kSTLayerSize >> 10) + 1 + kRoomCacheSpareK) {
					c->layer = (uint8_t *)Malloc(kSTLayerSize);
					if (c->layer) {
						e = c;
					}
				}
				break;
			}
			if (!lru || (int32_t)(c->used - lru->used) < 0) {
				lru = c;
			}
		}
		if (!e) {
			e = lru;
		}
		if (!e || !e->layer) {
			return;
		}
	}
	e->level = (int16_t)level;
	e->levNum = (int16_t)_res->_levNum;
	e->room = (int16_t)room;
	memcpy(e->hdr, hdr, sizeof(e->hdr));
	e->remapHash = ST_remapHash();
	e->used = ++g_roomUse;
	memcpy(e->layer, _frontLayer, kSTLayerSize);
}
#endif

void Video::AMIGA_decodeLev(int level, int room) {
#ifdef ATARIST
	if (ST_roomFromCache(level, room)) {
		return;
	}
#endif
	uint8_t *tmp = _res->_scratchBuffer;
#ifdef ATARIST
	++g_stPerf.roomDecodes;
#endif
	const int offset = READ_BE_UINT32(_res->_lev + room * 4);
#ifdef ATARIST
	if (!_res->ST_unpackedRoom(room, tmp))
#endif
	if (!bytekiller_unpack(tmp, Resource::kScratchBufferSize, _res->_lev, offset)) {
		warning("Bad CRC for level %d room %d", level, room);
		return;
	}
	uint16_t offset10 = READ_BE_UINT16(tmp + 10);
	const uint16_t offset12 = READ_BE_UINT16(tmp + 12);
	const uint16_t offset14 = READ_BE_UINT16(tmp + 14);
	static const int kTempMbkSize = 1024;
	uint8_t *buf = (uint8_t *)malloc(kTempMbkSize * 32);
	if (!buf) {
		error("Unable to allocate mbk temporary buffer");
	}
	int sz = 0;
	memset(buf, 0, 32);
	sz += 32;
#ifdef ATARIST
	const uint32_t tileEpoch = _res->_stBankEpoch;
	g_stTileOrig[0] = buf;
#endif
	const uint8_t *a1 = tmp + offset14;
	int d0;
	do {
		d0 = READ_BE_UINT16(a1); a1 += 2;
		const int num = d0 & ~0x8000;
		const uint8_t *a6 = _res->findBankData(num);
		if (!a6) {
			a6 = _res->loadBankData(num);
		}
		const int d3 = *a1++;
		if (d3 == 255) {
			const int d1 = _res->getBankDataSize(num);
			assert(sz + d1 <= kTempMbkSize * 32);
			memcpy(buf + sz, a6, d1);
#ifdef ATARIST
			for (int k = 0; k < d1; k += 32) {
				g_stTileOrig[(sz + k) >> 5] = a6 + k;
			}
#endif
			sz += d1;
		} else {
			for (int i = 0; i < d3 + 1; ++i) {
				const int d4 = *a1++;
				assert(sz + 32 <= kTempMbkSize * 32);
				memcpy(buf + sz, a6 + d4 * 32, 32);
#ifdef ATARIST
				g_stTileOrig[sz >> 5] = a6 + d4 * 32;
#endif
				sz += 32;
			}
		}
	} while((d0 & 0x8000) == 0);
#ifdef ATARIST
	// The ST draws the room straight into the planar layer with the
	// colour remap applied, so the palettes (and therefore the
	// remap) have to be known before the first tile is drawn.
	ST_paletteForRoom(level + 1, _res->_levNum, room);
	AMIGA_setLevelPalettes(level, tmp);
	ST_getRemap();                      // the remap is lazy: build it now, so the
	                                    // generation the caches key on is current
	ST_tilesForRoom(buf, sz >> 5, _res->_stBankEpoch == tileEpoch, tileEpoch);
	if (!_res->_sgd) {
		ST_sgdRelease();
	}
#endif
#ifdef ATARIST
	// the chunky room started as colour 0, a colour like any other
	// once remapped; the SGD decoder fills the layer itself
	if (tmp[1] != 0) {
		memset(_frontLayer, 0, _layerSize);
	} else {
		ST_clearLayer(_frontLayer, 0);
	}
#else
	memset(_frontLayer, 0, _layerSize);
#endif
	if (tmp[1] != 0) {
		assert(_res->_sgd);
#ifdef ATARIST
		ST_decodeSgd(_frontLayer, tmp + offset10, _res->_sgd, level, room);
#else
		DrawTileMaskProc drawTileMask = _res->isAmiga() ? AMIGA_planar_mask : DOS_drawTileMask;
		decodeSgd(_frontLayer, tmp + offset10, _res->_sgd, drawTileMask, level, room);
#endif
		offset10 = 0;
	}
#ifdef ATARIST
	ST_decodeLevTiles(_frontLayer, tmp, offset10, offset12, buf, tmp[1] != 0);
#else
	DrawTileProc drawTile = _res->isAmiga() ? AMIGA_drawTile : DOS_drawTile;
	decodeLevHelper(_frontLayer, tmp, offset10, offset12, buf, tmp[1] != 0, drawTile, _res->_readUint16);
#endif
	free(buf);
#ifdef ATARIST
	ST_roomToCache(level, room, tmp);
	ST_copyLayer(_backLayer, _frontLayer);
	++_stBackGen;
#else
	memcpy(_backLayer, _frontLayer, _layerSize);
	AMIGA_setLevelPalettes(level, tmp);
#endif
}

void Video::AMIGA_setLevelPalettes(int level, const uint8_t *tmp) {
	_mapPalSlot1 = READ_BE_UINT16(tmp + 2);
	_mapPalSlot2 = READ_BE_UINT16(tmp + 4);
	_mapPalSlot3 = READ_BE_UINT16(tmp + 6);
	_mapPalSlot4 = READ_BE_UINT16(tmp + 8);
	if (_res->isDOS()) {
		DOS_setLevelPalettes();
		if (level == 0) { // tiles with color slot 0x9
			setPaletteSlotBE(0x9, _mapPalSlot1);
		}
		return;
	}
	// background
	setPaletteSlotBE(0x0, _mapPalSlot1);
	// objects
	setPaletteSlotBE(0x1, (level == 0) ? _mapPalSlot3 : _mapPalSlot2);
	setPaletteSlotBE(0x2, _mapPalSlot3);
	setPaletteSlotBE(0x3, _mapPalSlot3);
	// conrad
	setPaletteSlotBE(0x4, _mapPalSlot3);
	// foreground
	setPaletteSlotBE(0x8, _mapPalSlot1);
	setPaletteSlotBE(0x9, (level == 0) ? _mapPalSlot1 : _mapPalSlot3);
	// inventory
	setPaletteSlotBE(0xA, _mapPalSlot3);
#ifdef ATARIST
	// Which logical entries are the Amiga's 32 colours: the background
	// palette is colours 0-15 wherever it is loaded, the object palette
	// (Conrad, items, the inventory) 16-31. Level 2's extra object
	// palette is neither, and custom palettes match it by colour.
	{
		const int slotPal[16] = {
			_mapPalSlot1, (level == 0) ? _mapPalSlot3 : _mapPalSlot2, _mapPalSlot3, _mapPalSlot3,
			_mapPalSlot3, -1, -1, -1,
			_mapPalSlot1, (level == 0) ? _mapPalSlot1 : _mapPalSlot3, _mapPalSlot3, -1,
			-1, -1, -1, -1
		};
		uint8_t amiga[256];
		memset(amiga, 0xFF, sizeof(amiga));
		for (int s = 0; s < 16; ++s) {
			const int base = (slotPal[s] == _mapPalSlot1) ? 0 : (slotPal[s] == _mapPalSlot3) ? 16 : -1;
			if (slotPal[s] >= 0 && base >= 0) {
				for (int k = 0; k < 16; ++k) {
					amiga[s * 16 + k] = (uint8_t)(base + k);
				}
			}
		}
		ST_setAmigaColourMap(amiga);

		// What each colour is worth to the automatic palette: how
		// much of the level's rooms it covers on average (measured
		// from every room, see tools/gen-palette-tables.py), and for
		// the object palette a fixed importance on top - it is what
		// Conrad, the enemies and the items are drawn in, which cover
		// few pixels but are what the player watches. Conrad's own
		// colours and the level's enemy half count most. Tenths of a
		// percent of the screen, given to every slot the palette is
		// loaded into - equal colours add up, so the object palette's
		// share counts five times over and the background's two or
		// three; the importance values were tuned with that, on the
		// measured renders of every level.
		if (level >= 0 && level < 7 && !_res->_isDemo) {   // measured on the full game's levels
			uint16_t weight[256];
			memset(weight, 0, sizeof(weight));
			for (int s = 0; s < 16; ++s) {
				if (slotPal[s] >= 0 && slotPal[s] < 6) {
					for (int k = 0; k < 16; ++k) {
						weight[s * 16 + k] = kSTColourUse[level][slotPal[s]][k];
					}
				}
			}
			static const uint8_t kConrad[] = { 1, 3, 4, 5, 6, 7 };
			const uint8_t enemyHalves = ST_enemyHalves(level);
			for (int k = 0; k < 16; ++k) {
				weight[0x40 + k] += 30;                  // objects
				if (enemyHalves & (1 << (k >> 3))) {
					weight[0x40 + k] += 120;             // enemies
				}
			}
			for (unsigned i = 0; i < sizeof(kConrad); ++i) {
				weight[0x40 + kConrad[i]] += 120;        // Conrad
			}
			ST_setColourWeights(weight);
		} else {
			ST_setColourWeights(0);
		}
	}
#endif
}

void Video::AMIGA_decodeSpm(const uint8_t *src, uint8_t *dst) {
	uint8_t buf[256 * 32];
	const int size = READ_BE_UINT16(src + 3) & 0x7FFF;
	assert(size <= (int)sizeof(buf));
	AMIGA_decodeRle(buf, src + 3);
	const int w = (src[2] >> 7) + 1;
	const int h = src[2] & 0x7F;
	AMIGA_planar16(dst, w, h, 3, buf);
}

#ifdef ATARIST
// The planes of a character frame or an object piece, unconverted: a
// frame's three RLE-packed planes with a fourth of zeros, a piece's
// four as they are - plane after plane for 16 and 32 pixels wide, a
// byte of each plane per eight pixels for 8 and 24, which is put in
// plane order here. False for anything else, which the caller then
// bakes from chunky.
bool Video::ST_amigaPlanes(int kind, const uint8_t *src, int w, int h, uint8_t *dst) {
	const int planeSize = (w >> 3) * h;
	switch (kind) {
	case kSprKindSPM: {
			const int size = READ_BE_UINT16(src + 3) & 0x7FFF;
			if (size > 256 * 32 || ((src[2] >> 7) + 1) * 16 != w || (src[2] & 0x7F) != h) {
				return false;
			}
			AMIGA_decodeRle(dst, src + 3);
			memset(dst + 3 * planeSize, 0, planeSize);
		}
		return true;
	case kSprKindSPC:
		if (w == 16 || w == 32) {
			memcpy(dst, src, 4 * planeSize);
			return true;
		}
		if (w == 8 || w == 24) {
			uint8_t *d0 = dst, *d1 = dst + planeSize, *d2 = d1 + planeSize, *d3 = d2 + planeSize;
			for (int n = planeSize; --n >= 0; src += 4) {
				*d0++ = src[0];
				*d1++ = src[1];
				*d2++ = src[2];
				*d3++ = src[3];
			}
			return true;
		}
		return false;
	}
	return false;
}
#endif

void Video::AMIGA_decodeIcn(const uint8_t *src, int num, uint8_t *dst) {
	for (int i = 0; i < num; ++i) {
		const int h = 1 + *src++;
		const int w = 1 + *src++;
		const int size = w * h * 8;
		src += 4 + size;
	}
	const int h = 1 + *src++;
	const int w = 1 + *src++;
	AMIGA_planar16(dst, w, h, 4, src + 4);
}

void Video::AMIGA_decodeSpc(const uint8_t *src, int w, int h, uint8_t *dst) {
	switch (w) {
	case 8:
	case 24:
		AMIGA_planar8(dst, w, h, src);
		break;
	case 16:
	case 32:
		AMIGA_planar16(dst, w / 16, h, 4, src);
		break;
	default:
		warning("AMIGA_decodeSpc w=%d unimplemented", w);
		break;
	}
}

void Video::AMIGA_decodeCmp(const uint8_t *src, uint8_t *dst) {
	AMIGA_planar16(dst, 20, GAMESCREEN_H, 5, src);
}

void Video::SEGA_decodeIcn(const uint8_t *src, int num, uint8_t *dst) {
	static const int W = 16;
	static const int H = 16;
	int offset = num * W * H / 2;
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < 8; ++x) {
			const uint8_t color = src[offset + ((x & 4) << 4) + (x & 3)];
			*dst++ = color >> 4;
			*dst++ = color & 15;
		}
		offset += 4;
	}
}

static void SEGA_decode8x8(const uint8_t *src, uint8_t *dst, int pitch) {
	for (int j = 0; j < 8; ++j) {
		for (int i = 0; i < 4; ++i) {
			const uint8_t color = *src++;
			dst[i * 2]     = color >> 4;
			dst[i * 2 + 1] = color & 15;
		}
		dst += pitch;
	}
}

void Video::SEGA_decodeSpc(const uint8_t *src, int w, int h, uint8_t *dst) {
	for (int x = 0; x < w; x += 8) {
		for (int y = 0; y < h; y += 8) {
			SEGA_decode8x8(src, dst, w);
			src += 8 * 8 / 2;
		}
	}
}

void Video::SEGA_decodeSpm(const uint8_t *src, uint8_t *dst) {
	static const int W = 32;
	static const int H = 24;
	static const int SPM_SIZE = 2048;
	const uint8_t *p = src;
	uint16_t len = READ_BE_UINT16(p + 2) + 1;
	p += 4;
	int uncompressed = SPM_SIZE;
	for (int j = 0; j < len; ++j) {
		if ((p[j] & 0xF0) == 0xF0) {
			const uint8_t color = p[j] & 15;
			++j;
			const int count = p[j] + 1;
			memset(dst + uncompressed, (color << 4) | color, count);
			uncompressed += count;
		} else {
			assert((p[j] & 15) != 15);
			dst[uncompressed] = p[j];
			++uncompressed;
		}
	}
	src = dst + SPM_SIZE;
	for (int part = 0; part < 2; ++part) { /* top, bottom */
		for (int x = 0; x < W; x += 8) {
			for (int y = 0; y < H; y += 8) {
				SEGA_decode8x8(src, dst + part * W * H + y * W + x, W);
				src += 8 * 8 / 2;
			}
		}
	}
}

void Video::drawSpriteSub1(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask) {
	debug(DBG_VIDEO, "Video::drawSpriteSub1(0x%X, 0x%X, 0x%X, 0x%X)", pitch, w, h, colMask);
	while (h--) {
		for (int i = 0; i < w; ++i) {
			if (src[i] != 0) {
				dst[i] = src[i] | colMask;
			}
		}
		src += pitch;
		dst += GAMESCREEN_W;
	}
}

void Video::drawSpriteSub2(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask) {
	debug(DBG_VIDEO, "Video::drawSpriteSub2(0x%X, 0x%X, 0x%X, 0x%X)", pitch, w, h, colMask);
	while (h--) {
		for (int i = 0; i < w; ++i) {
			if (src[-i] != 0) {
				dst[i] = src[-i] | colMask;
			}
		}
		src += pitch;
		dst += GAMESCREEN_W;
	}
}

void Video::drawSpriteSub3(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask) {
	debug(DBG_VIDEO, "Video::drawSpriteSub3(0x%X, 0x%X, 0x%X, 0x%X)", pitch, w, h, colMask);
	while (h--) {
		for (int i = 0; i < w; ++i) {
			if (src[i] != 0 && !(dst[i] & 0x80)) {
				dst[i] = src[i] | colMask;
			}
		}
		src += pitch;
		dst += GAMESCREEN_W;
	}
}

void Video::drawSpriteSub4(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask) {
	debug(DBG_VIDEO, "Video::drawSpriteSub4(0x%X, 0x%X, 0x%X, 0x%X)", pitch, w, h, colMask);
	while (h--) {
		for (int i = 0; i < w; ++i) {
			if (src[-i] != 0 && !(dst[i] & 0x80)) {
				dst[i] = src[-i] | colMask;
			}
		}
		src += pitch;
		dst += GAMESCREEN_W;
	}
}

void Video::drawSpriteSub5(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask) {
	debug(DBG_VIDEO, "Video::drawSpriteSub5(0x%X, 0x%X, 0x%X, 0x%X)", pitch, w, h, colMask);
	while (h--) {
		for (int i = 0; i < w; ++i) {
			if (src[i * pitch] != 0 && !(dst[i] & 0x80)) {
				dst[i] = src[i * pitch] | colMask;
			}
		}
		++src;
		dst += GAMESCREEN_W;
	}
}

void Video::drawSpriteSub6(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask) {
	debug(DBG_VIDEO, "Video::drawSpriteSub6(0x%X, 0x%X, 0x%X, 0x%X)", pitch, w, h, colMask);
	while (h--) {
		for (int i = 0; i < w; ++i) {
			if (src[-i * pitch] != 0 && !(dst[i] & 0x80)) {
				dst[i] = src[-i * pitch] | colMask;
			}
		}
		++src;
		dst += GAMESCREEN_W;
	}
}

void Video::DOS_drawChar(uint8_t c, int16_t y, int16_t x, bool forceDefaultFont) {
	debug(DBG_VIDEO, "Video::DOS_drawChar(0x%X, %d, %d)", c, y, x);
	const uint8_t *fnt = ((_res->_lang == LANG_JP && !forceDefaultFont) || _res->isPC98()) ? _font8Jp : _res->_fnt;
	y *= CHAR_W;
	x *= CHAR_H;
	assert(c >= 32);
	const uint8_t *src = fnt + (c - 32) * 32;
	uint8_t *dst = _frontLayer + x + _w * y;
	for (int h = 0; h < CHAR_H; ++h) {
		for (int i = 0; i < 4; ++i, ++src) {
			const uint8_t c1 = *src >> 4;
			if (c1 != 0) {
				if (c1 != 2) {
					*dst = _charFrontColor;
				} else {
					*dst = _charShadowColor;
				}
			} else if (_charTransparentColor != 0xFF) {
				*dst = _charTransparentColor;
			}
			++dst;
			const uint8_t c2 = *src & 15;
			if (c2 != 0) {
				if (c2 != 2) {
					*dst = _charFrontColor;
				} else {
					*dst = _charShadowColor;
				}
			} else if (_charTransparentColor != 0xFF) {
				*dst = _charTransparentColor;
			}
			++dst;
		}
		dst += _w - CHAR_W;
	}
}

void Video::AMIGA_drawStringChar(uint8_t *dst, int pitch, int x, int y, const uint8_t *src, uint8_t color, uint8_t chr) {
	assert(chr >= 32);
#ifdef ATARIST
	// AMIGA_decodeIcn walks the icon list from the start and then
	// unpacks a whole 16x16 cell one bit at a time, which costs 5ms
	// a character on a Mega STE: drawing a screenful of text took
	// over a second. Only the top-left 8x8 of each cell is ever
	// drawn, so keep that much per character and decode each one
	// once. The font pointer says which font the cache holds, which
	// is enough here: the Amiga resources load one font and keep it.
	// Rows are kept 8 bytes wide, the glyph's own width, rather than
	// the decoded cell's 16: the other half was 6K of padding. Every
	// character has a slot, accented ones included, so a glyph's pixels
	// stay put once decoded: a drawn frame's text may be redrawn from
	// them later (ST_frameEnd).
	enum { kGlyphs = 256 - 32 };
	static const uint8_t *cachedFnt;
	static uint8_t cache[kGlyphs][8 * 8];
	static uint8_t cached[kGlyphs];
	const int idx = chr - 32;
	int stride = 16;
	if (idx < kGlyphs) {
		if (src != cachedFnt) {
			memset(cached, 0, sizeof(cached));
			cachedFnt = src;
		}
		if (!cached[idx]) {
			AMIGA_decodeIcn(src, idx, _res->_scratchBuffer);
			for (int row = 0; row < 8; ++row) {
				memcpy(cache[idx] + row * 8, _res->_scratchBuffer + row * 16, 8);
			}
			cached[idx] = 1;
		}
		src = cache[idx];
		stride = 8;
	} else {
		AMIGA_decodeIcn(src, idx, _res->_scratchBuffer);
		src = _res->_scratchBuffer;
	}
#else
	AMIGA_decodeIcn(src, chr - 32, _res->_scratchBuffer);
	src = _res->_scratchBuffer;
#endif
#ifdef ATARIST
	// pitch 256 = a planar layer / cutscene page; anything else is a
	// plain chunky buffer (the 320-wide Amiga title screen)
	if (pitch == GAMESCREEN_W) {
		ST_drawGlyph(dst, src, stride, x, y, color);
		return;
	}
#else
	const int stride = 16;
#endif
	dst += y * pitch + x;
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			if (src[x] != 0) {
				dst[x] = color;
			}
		}
		src += stride;
		dst += pitch;
	}
}

void Video::DOS_drawStringChar(uint8_t *dst, int pitch, int x, int y, const uint8_t *src, uint8_t color, uint8_t chr) {
	dst += y * pitch + x;
	assert(chr >= 32);
	src += (chr - 32) * 8 * 4;
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 4; ++x, ++src) {
			const uint8_t c1 = *src >> 4;
			if (c1 != 0) {
				*dst = (c1 == 15) ? color : (0xE0 + c1);
			}
			++dst;
			const uint8_t c2 = *src & 15;
			if (c2 != 0) {
				*dst = (c2 == 15) ? color : (0xE0 + c2);
			}
			++dst;
		}
		dst += pitch - CHAR_W;
	}
}

static void MAC_drawFont(const DecodeBuffer &buf, uint8_t *dst, int dstPitch, const uint8_t frontColor, const uint8_t shadowColor) {
	const uint8_t *src = buf.clip_buf + buf.clip_y * buf.orig_w + buf.clip_x;
	dst += buf.dst_y * dstPitch + buf.dst_x;
	for (int j = 0; j < buf.clip_h; ++j) {
		for (int i = 0; i < buf.clip_w; ++i) {
			switch (src[i]) {
			case 0xC0:
				dst[i] = shadowColor;
				break;
			case 0xC1:
				dst[i] = frontColor;
				break;
			}
		}
		src += buf.orig_w;
		dst += dstPitch;
	}
}

void Video::MAC_drawStringChar(uint8_t *dst, int pitch, int x, int y, const uint8_t *src, uint8_t color, uint8_t chr) {
	DecodeBuffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.dst_w = _w;
	buf.dst_h = _h;
	buf.dst_x = x * _layerScale;
	buf.dst_y = y * _layerScale;
	assert(chr >= 32);
	_res->MAC_decodeImageData(_res->_fnt, chr - 32, &buf);
	MAC_drawFont(buf, dst, _w, color, _charShadowColor);
}

const char *Video::drawString(const char *str, int16_t x, int16_t y, uint8_t col) {
	debug(DBG_VIDEO, "Video::drawString('%s', %d, %d, 0x%X)", str, x, y, col);
	const uint8_t *fnt = (_res->_lang == LANG_JP || _res->isPC98()) ? _font8Jp : _res->_fnt;
	int len = 0;
	while (1) {
		const uint8_t c = *str++;
		if (c == 0 || c == 0xB || c == 0xA) {
			break;
		}
		(this->*_drawChar)(_frontLayer, _w, x + len * CHAR_W, y, fnt, col, c);
		++len;
	}
	markBlockAsDirty(x, y, len * CHAR_W, CHAR_H, _layerScale);
	return str - 1;
}

void Video::drawStringLen(const char *str, int len, int x, int y, uint8_t color) {
	const uint8_t *fnt = (_res->_lang == LANG_JP || _res->isPC98()) ? _font8Jp : _res->_fnt;
	for (int i = 0; i < len; ++i) {
		(this->*_drawChar)(_frontLayer, _w, x + i * CHAR_W, y, fnt, color, str[i]);
	}
	markBlockAsDirty(x, y, len * CHAR_W, CHAR_H, _layerScale);
}

Color Video::AMIGA_convertColor(const uint16_t color, bool bgr) { // 4bits to 8bits
	int r = (color & 0xF00) >> 8;
	int g = (color & 0xF0)  >> 4;
	int b =  color & 0xF;
	if (bgr) {
		SWAP(r, b);
	}
	Color c;
	c.r = (r << 4) | r;
	c.g = (g << 4) | g;
	c.b = (b << 4) | b;
	return c;
}

void Video::MAC_decodeMap(int level, int room) {
	DecodeBuffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.ptr = _frontLayer;
	buf.dst_w = _w;
	buf.dst_h = _h;
	_res->MAC_loadLevelRoom(level, room, &buf);
	memcpy(_backLayer, _frontLayer, _layerSize);
	Color roomPalette[256];
	_res->MAC_setupRoomClut(level, room, roomPalette);
	for (int j = 0; j < 16; ++j) {
		if (j == 5 || j == 7 || j == 14 || j == 15) {
			continue;
		}
		for (int i = 0; i < 16; ++i) {
			const int color = j * 16 + i;
			_stub->setPaletteEntry(color, &roomPalette[color]);
		}
	}
}

void Video::fillRect(int x, int y, int w, int h, uint8_t color) {
#ifdef ATARIST
	ST_fillRect(_frontLayer, x, y, w, h, color);
#else
	uint8_t *p = _frontLayer + y * _layerScale * _w + x * _layerScale;
	for (int j = 0; j < h * _layerScale; ++j) {
		memset(p, color, w * _layerScale);
		p += _w;
	}
#endif
}

static void fixOffsetDecodeBuffer(DecodeBuffer *buf, const uint8_t *dataPtr, bool xflip) {
	if (xflip) {
		buf->dst_x += (int16_t)READ_BE_UINT16(dataPtr + 4) - READ_BE_UINT16(dataPtr) - 1;
	} else {
		buf->dst_x -= (int16_t)READ_BE_UINT16(dataPtr + 4);
	}
	buf->dst_y -= (int16_t)READ_BE_UINT16(dataPtr + 6);
}

void Video::MAC_drawSprite(int x, int y, const uint8_t *data, int frame, bool xflip, bool eraseBackground) {
	const uint8_t *dataPtr = _res->MAC_getImageData(data, frame);
	if (dataPtr) {
		DecodeBuffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.dst_w = _w;
		buf.dst_h = _h;
		buf.dst_x = x * _layerScale;
		buf.dst_y = y * _layerScale;
		fixOffsetDecodeBuffer(&buf, dataPtr, xflip);
		_res->MAC_decodeImageData(data, frame, &buf);

		const uint8_t *src = buf.clip_buf + buf.clip_y * buf.orig_w;
		if (xflip) {
			src += buf.orig_w - 1 - buf.clip_x;
		} else {
			src += buf.clip_x;
		}
		uint8_t *dst = _frontLayer + buf.dst_y * _w + buf.dst_x;
		for (int j = 0; j < buf.clip_h; ++j) {
			if (xflip) {
				for (int i = 0; i < buf.clip_w; ++i) {
					if (src[-i] && (eraseBackground || (dst[i] & 0x80) == 0)) {
						dst[i] = src[-i];
					}
				}
			} else {
				for (int i = 0; i < buf.clip_w; ++i) {
					if (src[i] && (eraseBackground || (dst[i] & 0x80) == 0)) {
						dst[i] = src[i];
					}
				}
			}
			src += buf.orig_w;
			dst += _w;
		}

		markBlockAsDirty(buf.dst_x, buf.dst_y, buf.clip_w, buf.clip_h, 1);
	}
}
