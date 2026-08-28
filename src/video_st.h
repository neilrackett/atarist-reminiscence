
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Atari ST planar layer primitives.
 *
 * Layers are ST interleaved planar: 224 rows of 128 bytes (4 plane
 * words per 16-pixel group), followed by a priority plane of 224
 * rows of 32 bytes (bit set = foreground, sprites must not draw).
 * This reproduces the chunky engine's 0x80 pixel bit.
 *
 * Colours are remapped from the 256-entry logical palette to the 16
 * hardware colours at draw time via the quantiser in
 * systemstub_stdl.cpp; chunky pixel data survives only at load time
 * (room decode, title screen).
 */

#ifndef VIDEO_ST_H__
#define VIDEO_ST_H__

#ifdef ATARIST

#include "intern.h"

enum {
	kSTLayerW = 256,
	kSTLayerH = 224,
	kSTRowBytes = 128,
	kSTPrioRowBytes = 32,
	kSTPlaneBytes = kSTRowBytes * kSTLayerH,
	kSTLayerSize = kSTPlaneBytes + kSTPrioRowBytes * kSTLayerH,
};

// sprite draw flags (replacing Video::drawSpriteSub1..6)
enum {
	kSTSpriteXflip = 1 << 0,
	kSTSpriteColMajor = 1 << 1,
	kSTSpriteRespectPrio = 1 << 2,
};

// current logical->hardware colour map, rebuilt if the palette
// changed (owned by systemstub_stdl.cpp)
const uint8_t *ST_getRemap();

// changes whenever ST_getRemap()'s contents change; anything holding
// pixels with the mapping already baked in must re-bake when it moves
uint16_t ST_remapGen();

// maskless STDL surface view of a layer (screen copies, layer
// copies); returns a cached wrapper
struct STDL_Surface;
STDL_Surface *ST_layerSurfaceBare(uint8_t *layer);

// cutscene palette mode: the colour quantisation runs over only the
// cutscene banks (0xC0-0xDF, scenes use up to 32 colours) plus the
// text slot, instead of the game's slots
void ST_setCutscenePalMode(bool enable);
bool ST_cutscenePalMode();

// Cutscene palette locking. A scene's script is walked once before
// it plays (see Cutscene::play) to collect every palette it loads;
// the 32 cutscene entries are then mapped to the 16 hardware slots
// once and that mapping is held for the whole scene. Planar pages
// can then bake slots at draw time, and palette changes - including
// fades - become writes to the 16 hardware registers.
void ST_cutscenePalReset();
void ST_cutscenePalPart(int part);
void ST_cutscenePalCollect(const Color *entries, int n);
void ST_cutscenePalLock(int part);
void ST_cutscenePalUnlock();

// hardware-space translation table for the cutscene shadow effect
// (chunky semantics: pixel |= colour8 & 0xF8)
void ST_getOrMap(uint8_t colour8, uint8_t *orMap);

// map16[c] = hardware colour of logical entry (colMask | c)
inline void ST_buildMap16(uint8_t colMask, uint8_t *map16) {
	const uint8_t *remap = ST_getRemap();
	for (int c = 0; c < 16; ++c) {
		map16[c] = remap[colMask | c];
	}
}

// Is source row y actually displayed? The 224->200 mapping (squash
// drop list, or the crop window) lives in the stub; callers placing
// content by hand must ask instead of reproducing the arithmetic.
bool ST_rowVisible(int y);
void ST_invalidateBakedRange(const uint8_t *p, uint32_t len);

// frames whose overscan border flip was missed (see STDL); 0 when
// overscan is off
uint32_t ST_overscanMisses();

// ST_hspan with the colour pre-remapped (see fillArea)
void ST_hspanV(uint8_t *layer, int x1, int x2, int y, uint8_t v, bool setPrio);

// walk a drawPolygon run list (y, then x1,x2 pairs, x1 < 0 ends) in
// one call with incremental row pointers
void ST_fillArea(uint8_t *layer, const int16_t *pts, int crx, int cry,
		int crw, uint8_t v, bool setPrio);

// fast path for opaque 3+-point non-flat polygons; returns false
// when the caller must use the reference converter
bool ST_drawPolygonFast(uint8_t *layer, const void *pts, int n,
		uint8_t colour8, int crx, int cry, int crw, int crh);

// Conrad's jacket: logical entry 7 of the Amiga sprite palette
// (bank 1). The quantiser pins this entry's cluster so a screenful
// of scenery can never vote the jacket into another colour's slot.
enum { kSTConradJacketEntry = 0x17 };

// whole-layer copy (planes via the BLiTTER where present)
void ST_copyLayer(uint8_t *dst, const uint8_t *src);

// full chunky (256-wide, 8bpp) to planar conversion - load time only
void ST_convertChunky(uint8_t *layer, const uint8_t *src, int h);

// drawSpriteSub replacement; src indexing matches the chunky code:
// row-major src[j*pitch +/- i], column-major src[+/-i*pitch + j].
// map16[c] = hardware colour for source value c (0 = transparent).
// setPrio marks drawn pixels as foreground.
void ST_drawSprite(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, int w, int h, const uint8_t *map16, unsigned flags, bool setPrio);

// As ST_drawSprite, but the planar form is cached: the same frame,
// colour bank, flags and palette produce the same plane words every
// time, so bake them once and then just move words. Falls back to
// ST_drawSprite when a frame is too large to cache.
void ST_drawSpriteCached(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, int w, int h, uint8_t colMask, unsigned flags, bool setPrio);

// drop every cached planar frame (level data reloaded)
void ST_flushSpriteCache();

// 8x8 glyph from a 16-byte-stride chunky source (AMIGA_decodeIcn
// output): nonzero source pixels painted in colour8
void ST_drawGlyph(uint8_t *layer, const uint8_t *src, int x, int y, uint8_t colour8);

void ST_fillRect(uint8_t *layer, int x, int y, int w, int h, uint8_t colour8);
void ST_hspan(uint8_t *layer, int x1, int x2, int y, uint8_t colour8);
void ST_hspanOr(uint8_t *layer, int x1, int x2, int y, uint8_t colour8);
void ST_drawPoint(uint8_t *layer, int x, int y, uint8_t colour8);
void ST_clearLayer(uint8_t *layer, uint8_t colour8);

#endif // ATARIST

#endif // VIDEO_ST_H__
