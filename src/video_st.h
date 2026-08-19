
/*
 * REminiscence - Flashback interpreter
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

// cutscene palette mode: logical 0xC0-0xCF map to hardware slots
// 0-15 directly, making the shadow effect a plane-3 OR and letting
// baked pages follow palette changes exactly
void ST_setCutscenePalMode(bool enable);
bool ST_cutscenePalMode();

// map16[c] = hardware colour of logical entry (colMask | c)
inline void ST_buildMap16(uint8_t colMask, uint8_t *map16) {
	const uint8_t *remap = ST_getRemap();
	for (int c = 0; c < 16; ++c) {
		map16[c] = remap[colMask | c];
	}
}

// full chunky (256-wide, 8bpp) to planar conversion - load time only
void ST_convertChunky(uint8_t *layer, const uint8_t *src, int h);

// drawSpriteSub replacement; src indexing matches the chunky code:
// row-major src[j*pitch +/- i], column-major src[+/-i*pitch + j].
// map16[c] = hardware colour for source value c (0 = transparent).
// setPrio marks drawn pixels as foreground.
void ST_drawSprite(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, int w, int h, const uint8_t *map16, unsigned flags, bool setPrio);

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
