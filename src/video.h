
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 * Atari ST port changes Copyright (C) 2026 Neil Rackett
 */

#ifndef VIDEO_H__
#define VIDEO_H__

#include "intern.h"

struct Resource;
struct SystemStub;

struct Video {
	typedef void (Video::*drawCharFunc)(uint8_t *, int, int, int, const uint8_t *, uint8_t, uint8_t);

	enum {
		GAMESCREEN_W = 256,
		GAMESCREEN_H = 224,
#ifdef ATARIST
		// ST_restoreDirty and copyRectPlanar both round to a 16-pixel
		// planar group, so a finer grid only costs scanning
		SCREENBLOCK_W = 16,
#else
		SCREENBLOCK_W = 8,
#endif
		SCREENBLOCK_H = 8,
		// dirty-block lifecycle: mark -> blit -> shown -> restore ->
		// owed (one more blit) -> blit -> clean
		kBlockClean = 0,
		kBlockOwed = 1,
		kBlockDirty = 2,
		kBlockShown = 0x80,
		CHAR_W = 8,
		CHAR_H = 8,
		PALETTE_INDEX_CONRAD = 4,
		PALETTE_INDEX_MONSTER = 5,
	};

	static const uint8_t _conradPal1[];
	static const uint8_t _conradPal2[];
	static const uint8_t _textPal[];
	static const uint8_t _palSlot0xF[];
	static const uint8_t _font8Jp[];

	Resource *_res;
	SystemStub *_stub;
	WidescreenMode _widescreenMode;

	int _w, _h;
	int _layerSize;
	int _layerScale; // 2 for Macintosh (512x448), 1 for other versions (256x224)
	uint8_t *_frontLayer;
	uint8_t *_backLayer;
	uint8_t *_tempLayer;
	uint8_t *_tempLayer2;
#ifdef ATARIST
	// Dirty-block bookkeeping, a bit per 16-pixel column and three
	// masks a row: what has been drawn since the last update, what
	// is on screen awaiting its restore, and what has been restored
	// and owes the screen one more blit. The same states used to be
	// a byte a block, which meant walking all 448 of them twice a
	// frame to find the handful that were set.
	enum { kBlockRows = GAMESCREEN_H / SCREENBLOCK_H };
	uint16_t _blkDirty[kBlockRows];
	uint16_t _blkShown[kBlockRows];
	uint16_t _blkOwed[kBlockRows];
	// put the room back in the front layer after a cutscene
	void ST_rebakeRoom();
	// bumped whenever the background layer is rebuilt: anything drawn
	// into it since is gone. In play the room decode is its only
	// writer (the title's copies happen with no level running, and a
	// level always starts with a decode); anything else that writes it
	// mid-level must bump this too, or the inventory icon's saved patch
	// goes back onto the wrong scenery.
	uint16_t _stBackGen;
	// give back the level's tile pool and sprite bakes
	void ST_releaseLevelCaches();
	// restore only recently drawn blocks from the back layer
	void ST_restoreDirty();
	// A gameplay frame: begin before the logic runs, end once everything
	// is drawn and before updateScreen, abort if it will not be drawn.
	// With the display list on (displaylist_st.cpp) only what changed
	// since the last frame is restored and redrawn; off, every frame
	// restores and redraws all it drew (ST_restoreDirty).
	void ST_frameBegin();
	void ST_frameEnd();
	void ST_frameAbort();
	bool ST_dlOn() const { return _dlOn; }
	// a rectangle of the back layer changed under the front one
	void ST_backChanged(int x, int y, int w, int h);
	uint16_t _blkForce[kBlockRows];    // blocks the next listed frame must redraw
	bool _dlOn;
	bool _dlFull;                      // ... all of them
	uint16_t _dlBackGen;               // _stBackGen the front layer was last rebuilt from
	void ST_restoreBlocks(const uint16_t *blk, const uint16_t *prio);
	uint16_t _prioDirty[kBlockRows];   // blocks whose priority plane may differ from the room's
	bool ST_roomFromCache(int level, int room);
	void ST_roomToCache(int level, int room, const uint8_t *hdr);
	// has anything been drawn in these blocks since the last update?
	bool ST_blocksDirty(int x, int y, int w, int h) const;
#endif
	uint8_t _unkPalSlot1, _unkPalSlot2;
	uint8_t _mapPalSlot1, _mapPalSlot2, _mapPalSlot3, _mapPalSlot4;
	uint8_t _charFrontColor;
	uint8_t _charTransparentColor;
	uint8_t _charShadowColor;
	uint8_t *_screenBlocks;
	bool _fullRefresh;
	uint8_t _shakeOffset;
	drawCharFunc _drawChar;

	Video(Resource *res, SystemStub *stub, WidescreenMode widescreenMode);
	~Video();

	void markBlockAsDirty(int16_t x, int16_t y, uint16_t w, uint16_t h, int scale);
	void updateScreen();
	void updateWidescreen();
	void fullRefresh();
	void fadeOut();
	void fadeOutPalette();
	void setPaletteColorBE(int num, int offset);
	void setPaletteSlotBE(int palSlot, int palNum);
	void setPaletteSlotLE(int palSlot, const uint8_t *palData);
	void setTextPalette();
	void setPalette0xF();
	void DOS_decodeLev(int level, int room);
	void DOS_decodeMap(int level, int room);
	void DOS_setLevelPalettes();
	void DOS_decodeIcn(const uint8_t *src, int num, uint8_t *dst);
	void DOS_decodeSpc(const uint8_t *src, int w, int h, uint8_t *dst);
	void DOS_decodeSpm(const uint8_t *dataPtr, uint8_t *dstPtr);
	void PC98_decodeMap(int level, int room);
	void AMIGA_setLevelPalettes(int level, const uint8_t *tmp);
	void AMIGA_decodeLev(int level, int room);
	void AMIGA_decodeSpm(const uint8_t *src, uint8_t *dst);
	void AMIGA_decodeIcn(const uint8_t *src, int num, uint8_t *dst);
	void AMIGA_decodeSpc(const uint8_t *src, int w, int h, uint8_t *dst);
#ifdef ATARIST
	// a sprite's Amiga bitplanes as four planes one after another,
	// (w / 8) * h bytes each: what the planar bakes read (ST_sprDraw)
	bool ST_amigaPlanes(int kind, const uint8_t *src, int w, int h, uint8_t *dst);
#endif
	void AMIGA_decodeCmp(const uint8_t *src, uint8_t *dst);
	void SEGA_decodeIcn(const uint8_t *src, int num, uint8_t *dst);
	void SEGA_decodeSpc(const uint8_t *src, int w, int h, uint8_t *dst);
	void SEGA_decodeSpm(const uint8_t *src, uint8_t *dst);
	void drawSpriteSub1(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask);
	void drawSpriteSub2(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask);
	void drawSpriteSub3(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask);
	void drawSpriteSub4(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask);
	void drawSpriteSub5(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask);
	void drawSpriteSub6(const uint8_t *src, uint8_t *dst, int pitch, int h, int w, uint8_t colMask);
	void DOS_drawChar(uint8_t c, int16_t y, int16_t x, bool forceDefaultFont = false);
	void DOS_drawStringChar(uint8_t *dst, int pitch, int x, int y, const uint8_t *src, uint8_t color, uint8_t chr);
	void AMIGA_drawStringChar(uint8_t *dst, int pitch, int x, int y, const uint8_t *src, uint8_t color, uint8_t chr);
	void MAC_drawStringChar(uint8_t *dst, int pitch, int x, int y, const uint8_t *src, uint8_t color, uint8_t chr);
	const char *drawString(const char *str, int16_t x, int16_t y, uint8_t col);
	void drawStringLen(const char *str, int len, int x, int y, uint8_t color);
	static Color AMIGA_convertColor(const uint16_t color, bool bgr = false);
	void MAC_decodeMap(int level, int room);
	void fillRect(int x, int y, int w, int h, uint8_t color);
	void MAC_drawSprite(int x, int y, const uint8_t *data, int frame, bool xflip, bool eraseBackground);
};

#endif // VIDEO_H__
