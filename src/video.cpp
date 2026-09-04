
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
	_screenBlocks = (uint8_t *)calloc(1, (_w / SCREENBLOCK_W) * (_h / SCREENBLOCK_H));
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
	free(_screenBlocks);
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
// Which blocks of one grid row still owe the screen a blit, as a bit
// per column (leftmost is the top bit), moving each one on: a fresh
// mark blits and then awaits its restore, a restored block blits
// once more and is done. The grid is read a long at a time - a block
// is clean or already shown when its low seven bits are zero, so
// four are dismissed with one and-test, and in most frames most of
// them are.
static uint16_t blockRowToBlit(uint8_t *p, int cols) {
	uint16_t mask = 0;
	for (int i = 0; i < cols; i += 4) {
		const uint32_t v = *(const uint32_t *)(p + i);
		if ((v & 0x7f7f7f7fu) == 0) {
			continue;
		}
		for (int k = i; k < i + 4; ++k) {
			const uint8_t b = p[k];
			if ((b & 0x7f) == 0) {
				continue;
			}
			p[k] = (b == Video::kBlockOwed) ? Video::kBlockClean : Video::kBlockShown;
			mask |= (uint16_t)(0x8000 >> k);
		}
	}
	return mask;
}

// The same for the blocks that need their background back: those are
// the ones shown last frame, which is the one state with its top bit
// set, so four blocks test as one long again.
static uint16_t blockRowToRestore(uint8_t *p, int cols) {
	uint16_t mask = 0;
	for (int i = 0; i < cols; i += 4) {
		const uint32_t v = *(const uint32_t *)(p + i);
		if ((v & 0x80808080u) == 0) {
			continue;
		}
		for (int k = i; k < i + 4; ++k) {
			if ((p[k] & 0x80) == 0) {
				continue;
			}
			p[k] = Video::kBlockOwed;
			mask |= (uint16_t)(0x8000 >> k);
		}
	}
	return mask;
}

void Video::ST_restoreDirty() {
	if (_fullRefresh) {
		memcpy(_frontLayer, _backLayer, _layerSize);
		return;
	}
	const int cols = _w / SCREENBLOCK_W;
	const int rows = _h / SCREENBLOCK_H;
	// The priority plane is only worth putting back where something
	// wrote it: sprites mostly read it, and an ordinary frame leaves
	// all of it but the inventory icon as the room decoded it.
	int px0, py0, px1, py1;
	const bool prioAny = ST_takePrioRect(&px0, &py0, &px1, &py1);
	const int pbx0 = prioAny ? (px0 / SCREENBLOCK_W) : 0;
	const int pbx1 = prioAny ? (px1 / SCREENBLOCK_W) : -1;
	const int pby0 = prioAny ? (py0 / SCREENBLOCK_H) : 0;
	const int pby1 = prioAny ? (py1 / SCREENBLOCK_H) : -1;
	uint16_t rowMask[GAMESCREEN_H / SCREENBLOCK_H];
	uint8_t *p = _screenBlocks;
	for (int j = 0; j < rows; ++j) {
		rowMask[j] = blockRowToRestore(p, cols);
		p += cols;
	}
	// Rows that need the same blocks back are restored together, so a
	// tall sprite costs one setup instead of one per eight-pixel band.
	for (int j = 0; j < rows; ) {
		const uint16_t m = rowMask[j];
		if (m == 0) {
			++j;
			continue;
		}
		int j2 = j + 1;
		while (j2 < rows && rowMask[j2] == m) {
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
			const int gx0 = (i * SCREENBLOCK_W) >> 4;
			const int groups = ((i2 * SCREENBLOCK_W - 1) >> 4) - gx0 + 1;
			{
				const uint32_t *s = (const uint32_t *)(_backLayer + yy * kSTRowBytes + gx0 * 8);
				uint32_t *d = (uint32_t *)(_frontLayer + yy * kSTRowBytes + gx0 * 8);
				const int skip = (kSTRowBytes - groups * 8) >> 2;
				if (groups == 1) {
					// one 16-pixel group is the common case: two long
					// moves a line and nothing to step through
					for (int line = lines; --line >= 0; ) {
						d[0] = s[0];
						d[1] = s[1];
						s += kSTRowBytes / 4;
						d += kSTRowBytes / 4;
					}
				} else {
					for (int line = lines; --line >= 0; ) {
						for (int n = groups; --n >= 0; ) {
							*d++ = *s++;
							*d++ = *s++;
						}
						s += skip;
						d += skip;
					}
				}
				if (j > pby1 || j2 - 1 < pby0 || i > pbx1 || i2 - 1 < pbx0) {
					i = i2;
					continue;
				}
				// the priority plane, two bytes a group: long moves
				// where a pair of groups is aligned, words elsewhere
				const uint8_t *pb = _backLayer + kSTPlaneBytes + yy * kSTPrioRowBytes + gx0 * 2;
				uint8_t *pf = _frontLayer + kSTPlaneBytes + yy * kSTPrioRowBytes + gx0 * 2;
				const int longs = ((gx0 & 1) == 0) ? (groups >> 1) : 0;
				const int words = groups - longs * 2;
				for (int line = lines; --line >= 0; ) {
					const uint32_t *ls = (const uint32_t *)pb;
					uint32_t *ld = (uint32_t *)pf;
					for (int n = longs; --n >= 0; ) {
						*ld++ = *ls++;
					}
					const uint16_t *ws = (const uint16_t *)ls;
					uint16_t *wd = (uint16_t *)ld;
					for (int n = words; --n >= 0; ) {
						*wd++ = *ws++;
					}
					pb += kSTPrioRowBytes;
					pf += kSTPrioRowBytes;
				}
			}
			i = i2;
		}
		j = j2;
	}
}
#endif

void Video::markBlockAsDirty(int16_t x, int16_t y, uint16_t w, uint16_t h, int scale) {
	debug(DBG_VIDEO, "Video::markBlockAsDirty(%d, %d, %d, %d)", x, y, w, h);
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
	uint8_t *row = _screenBlocks + (int16_t)by1 * (int16_t)cols;
	// the runs are a few blocks wide, where a memset call costs more
	// than the stores it makes
	const int n = bx2 - bx1 + 1;
	for (; by1 <= by2; ++by1) {
		uint8_t *p = row + bx1;
		for (int i = n; --i >= 0; ) {
			*p++ = kBlockDirty;
		}
		row += cols;
	}
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
	// A fresh mark blits and then awaits its restore; a restored
	// block blits once more and is done. Rows needing the same
	// blocks go up in one call: a sprite spans several eight-pixel
	// bands and used to cost a call for each.
	} else {
		const int cols = _w / SCREENBLOCK_W;
		const int rows = _h / SCREENBLOCK_H;
		uint16_t rowMask[GAMESCREEN_H / SCREENBLOCK_H];
		uint8_t *p = _screenBlocks;
		int count = 0;
		for (int j = 0; j < rows; ++j) {
			rowMask[j] = blockRowToBlit(p, cols);
			p += cols;
		}
		for (int j = 0; j < rows; ) {
			const uint16_t m = rowMask[j];
			if (m == 0) {
				++j;
				continue;
			}
			int j2 = j + 1;
			while (j2 < rows && rowMask[j2] == m) {
				++j2;
			}
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
				VIDEO_BLIT(i * SCREENBLOCK_W, j * SCREENBLOCK_H,
					(i2 - i) * SCREENBLOCK_W, (j2 - j) * SCREENBLOCK_H);
				++count;
				i = i2;
			}
			j = j2;
		}
		if (count != 0) {
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
		_fullRefresh = true;
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
	memset(_screenBlocks, 0, (_w / SCREENBLOCK_W) * (_h / SCREENBLOCK_H));
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
// the whole level, keyed on the SGD data and the room colour
// mapping: the jungle rooms share most of their scenery, so a
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
	if (g_sgdData != data || g_sgdGen != ST_roomGen()) {
		ST_sgdForgetAll();
		g_sgdData = data;
		g_sgdGen = ST_roomGen();
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
// prepared once per tile, palette and flip through the colour
// remap, placed by byte stores. Direct-mapped on the tile's address
// in the room's bank buffer, and cleared per room, since the cells
// have that room's remap baked in and the buffer is reallocated.
// Parallel arrays rather than a struct: a 64-byte cell block
// indexes by a shift where a struct's odd size costs a multiply
// call on the 68000.
static const uint8_t *g_stTileSrc[256];
static uint8_t g_stTileFlags[256];
static uint8_t g_stTileCells[256][8 * 8];

static void ST_clearTiles() {
	memset(g_stTileSrc, 0, sizeof(g_stTileSrc));
}

static void ST_drawTile(uint8_t *layer, int x, int y, const uint8_t *src, int pal, const bool xflip, const bool yflip, int colorKey) {
	// pal is 0x00, 0x10, 0x80 or 0x90: bits 4 and 7 only
	const uint8_t flags = (uint8_t)pal | (xflip ? 1 : 0) | (yflip ? 2 : 0) | (colorKey == 0 ? 4 : 0);
	const int i = (((uintptr_t)src >> 5) ^ flags) & 255;
	if (g_stTileSrc[i] != src || g_stTileFlags[i] != flags) {
		ST_amigaTile8(g_stTileCells[i], src, (uint8_t)pal, xflip, yflip, colorKey == 0);
		g_stTileSrc[i] = src;
		g_stTileFlags[i] = flags;
	}
	ST_amigaPlaceTile8(layer, x, y, g_stTileCells[i], (pal & 0x80) != 0);
}
#endif

typedef void (*DrawTileProc)(uint8_t *dst, int x, int y, const uint8_t *src, int pal, const bool xflip, const bool yflip, int colorKey);

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

void Video::AMIGA_decodeLev(int level, int room) {
	uint8_t *tmp = _res->_scratchBuffer;
	const int offset = READ_BE_UINT32(_res->_lev + room * 4);
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
			sz += d1;
		} else {
			for (int i = 0; i < d3 + 1; ++i) {
				const int d4 = *a1++;
				assert(sz + 32 <= kTempMbkSize * 32);
				memcpy(buf + sz, a6 + d4 * 32, 32);
				sz += 32;
			}
		}
	} while((d0 & 0x8000) == 0);
#ifdef ATARIST
	// The ST draws the room straight into the planar layer with the
	// colour remap applied, so the palettes (and therefore the
	// remap) have to be known before the first tile is drawn.
	AMIGA_setLevelPalettes(level, tmp);
	ST_getRemap();                      // the remap is lazy: build it now, so the
	                                    // generation the caches key on is current
	ST_clearTiles();
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
	DrawTileProc drawTile = ST_drawTile;
#else
	DrawTileProc drawTile = _res->isAmiga() ? AMIGA_drawTile : DOS_drawTile;
#endif
	decodeLevHelper(_frontLayer, tmp, offset10, offset12, buf, tmp[1] != 0, drawTile, _res->_readUint16);
	free(buf);
#ifdef ATARIST
	ST_copyLayer(_backLayer, _frontLayer);
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
	static const uint8_t *cachedFnt;
	static uint8_t cache[96][8 * 16];
	static uint8_t cached[96];
	const int idx = chr - 32;
	if (idx < 96) {
		if (src != cachedFnt) {
			memset(cached, 0, sizeof(cached));
			cachedFnt = src;
		}
		if (!cached[idx]) {
			AMIGA_decodeIcn(src, idx, _res->_scratchBuffer);
			memcpy(cache[idx], _res->_scratchBuffer, sizeof(cache[idx]));
			cached[idx] = 1;
		}
		src = cache[idx];
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
		ST_drawGlyph(dst, src, x, y, color);
		return;
	}
#endif
	dst += y * pitch + x;
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			if (src[x] != 0) {
				dst[x] = color;
			}
		}
		src += 16;
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
