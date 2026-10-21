
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
#include <string.h>

enum {
	kSTLayerW = 256,
	kSTLayerH = 224,
	kSTRowBytes = 128,
	kSTPrioRowBytes = 32,
	kSTPlaneBytes = kSTRowBytes * kSTLayerH,
	kSTLayerSize = kSTPlaneBytes + kSTPrioRowBytes * kSTLayerH,
};

// One row of N longs: a group (two longs) as two moves, 2-5 groups -
// a sprite's dirty run - as one movem pair where a word loop runs ~50
// cycles a group, and a whole 128-byte layer row as three, at movem's
// 4.5 cycles a byte against the loop's 5+ (the cutscene page flip).
template <int N> static inline void copyRowN(uint32_t *dst, const uint32_t *src);
template <> inline void copyRowN<2>(uint32_t *dst, const uint32_t *src) {
	dst[0] = src[0];
	dst[1] = src[1];
}
#ifdef __m68k__
#define COPY_ROW_N(N, REGS, ...) \
	template <> inline void copyRowN<N>(uint32_t *dst, const uint32_t *src) { \
		__asm__ volatile("movem.l (%1)," REGS "\n\tmovem.l " REGS ",(%0)" \
			: : "a"(dst), "a"(src) : "memory", __VA_ARGS__); \
	}
COPY_ROW_N(4, "%%d0-%%d3", "d0","d1","d2","d3")
COPY_ROW_N(6, "%%d0-%%d5", "d0","d1","d2","d3","d4","d5")
COPY_ROW_N(8, "%%d0-%%d7", "d0","d1","d2","d3","d4","d5","d6","d7")
COPY_ROW_N(10, "%%d0-%%d7/%%a2-%%a3", "d0","d1","d2","d3","d4","d5","d6","d7","a2","a3")
#undef COPY_ROW_N
// 6-15 groups: two or three bursts of up to eleven longs, the most
// movem can carry here; a C loop of longs ran ~21 cycles a long
template <> inline void copyRowN<12>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d5\n\t"
		"movem.l %%d0-%%d5,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d5\n\t"
		"movem.l %%d0-%%d5,24(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "memory");
}
template <> inline void copyRowN<14>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d6\n\t"
		"movem.l %%d0-%%d6,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d6\n\t"
		"movem.l %%d0-%%d6,28(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "memory");
}
template <> inline void copyRowN<16>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7\n\t"
		"movem.l %%d0-%%d7,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7\n\t"
		"movem.l %%d0-%%d7,32(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory");
}
template <> inline void copyRowN<18>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7/%%a2\n\t"
		"movem.l %%d0-%%d7/%%a2,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2\n\t"
		"movem.l %%d0-%%d7/%%a2,36(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a2", "memory");
}
template <> inline void copyRowN<20>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a3\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a3,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a3\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a3,40(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a2", "a3", "memory");
}
template <> inline void copyRowN<22>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a4\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a4,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a4\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a4,44(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a2", "a3", "a4", "memory");
}
template <> inline void copyRowN<24>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7\n\t"
		"movem.l %%d0-%%d7,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7\n\t"
		"movem.l %%d0-%%d7,32(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7\n\t"
		"movem.l %%d0-%%d7,64(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory");
}
template <> inline void copyRowN<26>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7/%%a2\n\t"
		"movem.l %%d0-%%d7/%%a2,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2\n\t"
		"movem.l %%d0-%%d7/%%a2,36(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7\n\t"
		"movem.l %%d0-%%d7,72(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a2", "memory");
}
template <> inline void copyRowN<28>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a3\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a3,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2\n\t"
		"movem.l %%d0-%%d7/%%a2,40(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2\n\t"
		"movem.l %%d0-%%d7/%%a2,76(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a2", "a3", "memory");
}
template <> inline void copyRowN<30>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a3\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a3,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a3\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a3,40(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a3\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a3,80(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a2", "a3", "memory");
}
template <> inline void copyRowN<32>(uint32_t *dst, const uint32_t *src) {
	__asm__ volatile(
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a4\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a4,(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a4\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a4,44(%0)\n\t"
		"movem.l (%1)+,%%d0-%%d7/%%a2-%%a3\n\t"
		"movem.l %%d0-%%d7/%%a2-%%a3,88(%0)"
		: "+a"(dst), "+a"(src)
		:
		: "d0","d1","d2","d3","d4","d5","d6","d7","a2","a3","a4","memory");
}
#else
template <int N> static inline void copyRowN(uint32_t *dst, const uint32_t *src) {
	memcpy(dst, src, N * 4);
}
#endif

// h rows of N longs, each side a fixed stride apart
template <int N> static inline void copyRowsN(uint8_t *d, int dStride, const uint8_t *s, int sStride, int h) {
	for (int j = h; --j >= 0; ) {
		copyRowN<N>((uint32_t *)d, (const uint32_t *)s);
		s += sStride;
		d += dStride;
	}
}

// h rows of a run of whole groups (1-16), through copyRowN; false for
// any other width, which the caller copies its own way
// one copy, not inlined into each caller: with every width it is
// a few K a copy, and each caller pays one call per copy, not per row
bool ST_copyGroupRows(uint8_t *d, int dStride, const uint8_t *s, int sStride, int groups, int h);

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

struct STDL_Surface;

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
void ST_cutscenePalCollect(const uint16_t *packed);   // 32 colours, 0x0RGB
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

// frames whose overscan border flip was missed (see STDL); 0 when
// overscan is off
uint32_t ST_overscanMisses();
// the colours the hardware shows, hashed (cutscene replay_hash)
uint32_t ST_hwPaletteHash();

// What the gameplay sprite path did since the last log_fps line, so a
// slow frame says which cache gave out instead of only how slow it
// was. Increments only, and always compiled in: a build with them
// lays out like one without, which the Mega STE's cache cares about.
struct STPerf {
	uint16_t bakeHits;      // baked sprite found
	uint16_t bakeMakes;     // baked from a decode: a miss
	uint16_t chunky;        // drawn from chunky: no ring, or no room in it
	uint16_t shBuilds;      // pre-shifted copies made
	uint16_t shRefused;     // ... not made, to keep their own bake in the ring
	uint16_t promotes;      // bakes copied forward out of an old segment
	uint16_t segFlush;      // ring segments emptied
	uint16_t unused;        // bakes emptied without being drawn again
	uint16_t monsterLoads;  // monster SPM files read
	uint16_t bankLoads;     // banks unpacked into the arena
	uint16_t bankClears;    // arena resets
	uint32_t bankBytes;     // bytes those loads produced
	uint32_t planar8Px;     // pixels through AMIGA_planar8
	uint16_t dlDrawn;       // display list entries redrawn
	uint16_t dlKept;        // ... and left as they were
	uint16_t blitUnderAl;   // bakes blitted behind foreground, aligned
	uint16_t blitUnderSh;   // ... through the shift chain
	uint16_t blitPlainAl;   // bakes blitted with no foreground under them, aligned
	uint16_t blitPlainSh;   // ... through the shift chain
	uint16_t bakePlanar;    // bakes made straight from the Amiga planes
	uint8_t roomHits;       // rooms copied back from the room cache
	uint8_t roomRemap;      // ... found, but drawn in other colours since
	uint8_t roomDecodes;    // rooms decoded
	uint8_t pad;
	uint16_t tilePreps;     // room tiles packed for the tile cache: a miss
};
extern STPerf g_stPerf;
// cutscenes: group-rows (8 bytes x 1 row) the page copies have moved,
// for the per-frame stats
extern uint32_t g_stCutCopyGR;
// ... and what the drawing touched, a box per shape (overlaps counted twice)
extern uint32_t g_stCutTouchGR;
// a set of counters as one log fragment
void ST_perfFormat(const STPerf &p, char *buf, int len);

// ST_hspan with the colour pre-remapped (see fillArea)
void ST_hspanV(uint8_t *layer, int x1, int x2, int y, uint8_t v, bool setPrio);

// walk a drawPolygon run list (y, then x1,x2 pairs, x1 < 0 ends) in
// one call with incremental row pointers
void ST_fillArea(uint8_t *layer, const int16_t *pts, int crx, int cry,
		int crw, uint8_t v, bool setPrio);

// The cutscene polygon cache (see video_st.cpp): Begin/End bracket a
// played scene (false: no memory, drawn the ordinary way). A shape's
// primitives go between ShapeBegin and ShapeEnd; segHit draws one
// recorded under its key and returns 1, or returns 0 and the caller
// records it - RecordDraw takes it relative to the shape's origin,
// keeps it and draws it as segHit would at an origin in the clip rect
// (false if it cannot be kept: the caller draws it).
bool ST_segCacheBegin();
void ST_segCacheEnd();
bool ST_segCacheOn();
void ST_segCacheStats(uint32_t *hits, uint32_t *misses);
void ST_segShapeBegin(uint8_t *layer, int crx, int cry, int crw, int crh);
void ST_segShapeEnd();
void ST_segBakeMode(bool on);

// Cutscene shape sprites (see video_st.cpp): Find a baked shape by
// key (shape number | x phase << 11), Draw it (false: the clip cuts
// it), Seen says whether a key came before (and marks it). A bake
// draws the shape into the layer BakeBegin returns, then BakeEnd
// keeps it (null if it cannot). Flush when the colours change.
bool ST_shpCacheOn();
const void *ST_shpFind(uint16_t key);
bool ST_shpSeen(uint16_t key);
uint8_t *ST_shpBakeBegin();
const void *ST_shpBakeEnd(uint16_t key, int sox, int soy, int x0, int y0, int x1, int y1);
bool ST_shpDraw(uint8_t *layer, const void *sprite, int x, int y,
		int crx, int cry, int crw, int crh);
void ST_shpFlush();
void ST_shpColoursChanged();
void ST_shpStats(uint32_t *drawn, uint32_t *baked);
extern "C" int segHit(const void *key, int x, int y, int colour8);
// op_drawShape's primitive loop over the cache (segWalk): it draws the
// cached polygons itself and returns 1 with a primitive it leaves to
// the caller (new, an ellipse or point, a shadow), 0 at the end;
// colour and alpha are the last primitive's either way
struct SegWalk {
	const uint8_t *data;         // the next primitive's header
	const uint8_t *vot, *vdt;    // the vertices offset and data tables
	int16_t count;               // primitives left
	int16_t x, y;                // the shape's origin
	uint8_t cadd, pad;           // added to a primitive's colour byte
	const uint8_t *verts;        // the primitive handed back
	int16_t px, py;
	uint8_t colour, alpha;
};
extern "C" int segWalk(SegWalk *w);
bool ST_segRecordDraw(const void *key, const void *pts, int n, int x, int y, uint8_t colour8);
// the same for rows given as spans (xl, xr pairs) from (ox, oy), an
// ellipse's (Graphics::ellipseSpans)
bool ST_segRecordSpans(const void *key, int ox, int oy, int rows, const int16_t *spans,
	int x, int y, uint8_t colour8);

// fast path for opaque 3+-point non-flat polygons; returns false
// when the caller must use the reference converter
bool ST_drawPolygonFast(uint8_t *layer, const void *pts, int n,
		uint8_t colour8, int crx, int cry, int crw, int crh);

// The priority plane changes only where something marks or clears
// it; the rest of a frame need not have it restored. Writers report
// what they touched, and the restore asks once a frame.
void ST_prioTouchedRect(int x, int y, int w, int h);
// OR the blocks whose priority plane was touched since the last call into
// acc (kSTBlockRows rows of block columns), and forget them
void ST_takePrioBlocks(uint16_t *acc);
// ... or let them go, for a taker that has its own record
void ST_prioDropBlocks();
void ST_prioDropRect();
// a row of block columns (16 pixels each): bits for columns [i, 15] and
// for [0, i], so columns [a, b] are kSTColsFrom[a] & kSTColsTo[b]
extern const uint16_t kSTColsFrom[16], kSTColsTo[16];
void ST_prioTouched();
bool ST_takePrioRect(int *x0, int *y0, int *x1, int *y1);

// the two 16-pixel groups from the one holding x, h rows from y,
// planes and priority plane, to and from a buffer of 5 longs a row
void ST_layerSaveRect(const uint8_t *layer, int x, int y, int h, uint32_t *out);
void ST_layerLoadRect(uint8_t *layer, int x, int y, int h, const uint32_t *in);
void ST_layerCopyRect(uint8_t *dst, const uint8_t *src, int x, int y, int h);

// whole-layer copy (planes via the BLiTTER where present)
void ST_copyLayer(uint8_t *dst, const uint8_t *src);
// planes only: cutscene pages, whose priority plane is not kept
void ST_copyPage(uint8_t *dst, const uint8_t *src);

// Cutscene row band. A scene draws its shapes inside a 128-row
// window, yet every page clear, page copy and screen push moved all
// 224 rows. Between ST_pageBandStart and ST_pageBandEnd those three
// work on the band only: rows outside it are the same on every page
// and on screen, because the scene was started on cleared pages and
// every primitive that draws widens the band to cover what it
// touches first. A clear in another colour than the one the pages
// started in widens it to the whole page, which is plain behaviour.
//
// Within the band, the two pages that take turns on screen are also
// tracked against aux, the background each frame is restored from:
// a box around where each differs. ST_copyPage then moves only that
// box for a restore or a new background, and ST_pageShowRect gives
// the rectangle a push to the screen has to copy (full = the whole
// page, for the first push; false = nothing changed) and records the
// page as what the screen now shows.
void ST_pageBandStart(int y0, int y1, uint8_t colour8, uint8_t *front, uint8_t *back, uint8_t *aux);
void ST_pageBandEnd();
bool ST_pageShowRect(const uint8_t *page, bool full, int *x, int *y, int *w, int *h);

// palette_custom (palette_st.cpp): pick the room's PALETTE\ file, if
// any, before its palettes go in (level from 1; levNum the level file
// in use; room < 0 for none), and Ctrl+P's dump of the current colours
void ST_paletteForRoom(int level, int levNum, int room);
// for each logical entry, which of the Amiga's 32 colours it is (0-31,
// 0xFF for none): set with each room's palettes
void ST_setAmigaColourMap(const uint8_t *map256);
// what each logical entry is worth to the automatic palette in play
// (see Video::AMIGA_setLevelPalettes); null for none - the title, and
// cutscenes, which have their own mapping - so every entry counts once
void ST_setColourWeights(const uint16_t *weights256);
// the halves of the object palette a level's enemies are drawn with,
// from its monster list: bit 0 entries 0-7, bit 1 entries 8-15
uint8_t ST_enemyHalves(int level);
const char *ST_paletteDump(int level, int levNum, int room);
// Ctrl+Shift+P: rescan PALETTE\ and read the room's file again on its
// next load; and the file in use, "" for none
bool ST_paletteReload();
const char *ST_paletteInUse();

// with a border open, wait for the VBL before a big screen update
void ST_beamSync();

// Amiga planar sources into a layer with the remap applied (room
// build). A unit is 8 pixels; a prepared cell is 8 bytes (the
// remapped plane bytes 0-3, each followed by the mask), and cell
// buffers sit at even addresses. ST_amigaPrepare reads contiguous planes (plane k
// at planes + k * planeSize, rows of `units` bytes) and a 1bpp mask
// of the same shape; ST_amigaTile8 packs one 32-byte 8x8 tile with
// colour 0 transparent when keyZero, through the pair table
// ST_tilePalette gives for its palette base. ST_amigaPlace clips to
// the layer; x is normally a multiple of 8.
void ST_amigaPrepare(uint8_t *out, const uint8_t *planes, int planeSize, int units, int rows, const uint8_t *mask, uint8_t pal);
void ST_amigaTile8(uint8_t *out, const uint8_t *tile, const uint32_t *pair, bool xflip, bool yflip, bool keyZero);
void ST_amigaPlace(uint8_t *layer, int x, int y, const uint8_t *prep, int units, int rows, bool setPrio);

// the tile packer's pair table for palette base pal (0x00, 0x10, 0x80
// or 0x90), and a version of the colours it maps to that changes only
// when they do: a tile packed at one version is good while it lasts
uint16_t ST_tilePalette(uint8_t pal, const uint32_t **pair);
// the pair table (see packCellsAsm) for a sixteen-colour mapping
void ST_buildPairTable(const uint8_t *map16, uint32_t *t);

// ST_amigaPlace for one ST_amigaTile8 cell block that is known to
// lie inside the layer at a multiple of 8 (the room tile grid), given
// the tile's plane byte d (the even or odd byte of its group in row 0)
// and priority byte pr, prio 0xFF or 0. The caller marks the priority
// plane touched (ST_prioTouched). The opaque one is for a cell block
// whose every mask byte is 0xFF.
extern "C" void placeTile8Asm(uint8_t *d, uint8_t *pr, const uint8_t *cells, long prio);
extern "C" void placeTile8OpaqueAsm(uint8_t *d, uint8_t *pr, const uint8_t *cells, long prio);

// as ST_amigaPlace for a front-to-back build: the layer must start
// black with a zero priority plane, which serves as per-unit
// coverage; a cell paints only pixels no earlier call has, so
// placing a room's list last to first gives the same picture
// without the overdraw. ST_amigaPlaceCovEnd finishes the build:
// colour8 goes into every pixel no cell reached (the chunky room's
// colour 0 was a colour like any other) and the priority plane is
// cleared for its real job.
void ST_amigaPlaceCov(uint8_t *layer, int x, int y, const uint8_t *prep, int units, int rows);
void ST_amigaPlaceCovEnd(uint8_t *layer, uint8_t colour8);

// drawSpriteSub replacement; src indexing matches the chunky code:
// row-major src[j*pitch +/- i], column-major src[+/-i*pitch + j].
// map16[c] = hardware colour for source value c (0 = transparent).
// setPrio marks drawn pixels as foreground.
void ST_drawSprite(uint8_t *layer, const uint8_t *src, int pitch, int x, int y, int w, int h, const uint8_t *map16, unsigned flags, bool setPrio);

// A gameplay sprite, baked to planar the first time it is drawn and
// moved into the layer from then on (spritecache_st.cpp). `data` names
// the bytes it comes from - a frame in the level's data, a piece of an
// object bank - and `ident` which load of them it is (kSprId* | epoch):
// together they key the bake, so whatever recycles those bytes moves
// the epoch. A miss decodes through the decoder the game registers, by
// `kind`. The sprite is drawn whole at (x, y) - w x h, unclipped - and
// clipped to the layer here. flags are kSTSprite*.
enum {
	kSprKindSPM = 0,      // a character frame (Conrad, a monster)
	kSprKindSPC = 1,      // an object piece from a bank
	kSprKindIcon = 2      // an icon: data is the icon number + 1
};
enum {
	kSprIdPerso = 1UL << 24,
	kSprIdMonster = 2UL << 24,
	kSprIdBank = 3UL << 24,
	kSprIdIcon = 4UL << 24,
	kSprIdEpochMask = (1UL << 24) - 1
};
// dst: chunky w x h, or with planes set the four Amiga bitplanes one
// after another, (w / 8) * h bytes each; false when it cannot be done
// that way (the bake then asks for chunky)
typedef bool (*STSprDecodeFn)(void *ctx, int kind, const uint8_t *data, int w, int h, uint8_t *dst, bool planes);
// the spread and bit-reverse tables the Amiga-planes kernels use, and
// packCellsAsm itself (video_st.cpp)
const uint32_t *ST_spreadTable();
const uint8_t *ST_bitrevTable();
const uint32_t *ST_repTable();           // a byte in all four bytes of a long
// STDL_BlitIndexed8's flags for a sprite's x flip and column-major layout
unsigned ST_i8Flags(unsigned flags);
extern "C" void packCellsAsm(uint8_t *out, const uint8_t *s0, long planeSize,
	const uint8_t *mask, long n, const uint32_t *spread, const uint32_t *pair);
void ST_sprSetDecoder(STSprDecodeFn fn, void *ctx, uint8_t *scratch, uint32_t scratchSize);
void ST_sprDraw(uint8_t *layer, int kind, const uint8_t *data, uint32_t ident, int w, int h,
                int x, int y, uint8_t colMask, unsigned flags, bool setPrio);
// give the bakes' memory back: a level is about to load, or a cutscene
// is short of room (Game::playCutscene); the next draw asks for it again
void ST_sprRelease();
// give back the decoded rooms kept for revisits (Video::AMIGA_decodeLev)
// and the room tile table
void ST_roomCacheRelease();
// both of the above: the caches that take what RAM there is and come
// back as play needs them
void ST_releaseRamCaches();
// the layer as an STDL surface; masked: with the priority plane as its mask
STDL_Surface *ST_layerView(uint8_t *layer, bool masked);
// the palette tokens sprites are baked with: between Begin and End the
// remap is taken as fixed, and End says whether it really was
void ST_sprTokensBegin();
bool ST_sprTokensEnd();

// Gameplay frames redrawn only where they changed (displaylist_st.cpp).
// Between ST_dlBegin and ST_dlEnd, sprites and glyphs drawn on `front`
// are written down instead of drawn; ST_dlEnd compares the list with
// the previous frame's and works out the blocks to redraw - R comes in
// with the blocks that must be, and goes out with all of them - and
// ST_dlReplay draws what touches them. ST_dlAbort stops a frame that
// will not be finished; ST_dlInvalidate says the front layer no longer
// shows the last list (a full refresh, a menu drawn over it).
enum { kSTBlockRows = kSTLayerH / 8 };
// The Add calls return false when the list could not take the draw -
// it was full and has just been drawn (ST_dlFlushNow) - so the caller
// draws it there and then. ST_dlFlushNow draws the list so far and
// stops writing for the frame (before the bytes its entries name are
// reused); a frame that did is ended with ST_dlEndFlushed. The flush
// function rebuilds the front layer from the room first.
void ST_dlSetFlush(void (*fn)(void *), void *ctx);
void ST_dlBegin(uint8_t *front);
// a draw onto layer is to be written down rather than drawn: inline, as
// every sprite and glyph asks
extern bool g_stDlRec;
extern uint8_t *g_stDlFront;
inline bool ST_dlRecording(const uint8_t *layer) {
	return g_stDlRec && layer == g_stDlFront;
}
bool ST_dlAddGlyph(const uint8_t *src, int pitch, int x, int y, uint8_t v, bool setPrio);
void ST_dlFlushNow();
bool ST_dlFlushed();
void ST_dlEnd(uint16_t *R, bool full);
void ST_dlEndFlushed();
void ST_dlReplay();
void ST_dlAbort();
void ST_dlInvalidate();

// with logging on, the largest block malloc can still hand out
void ST_logFreeMemory(const char *where);
// the hashes' step: a 32-bit rotate left by 5
static inline uint32_t ST_rol5(uint32_t h) {
	return (h << 5) | (h >> 27);
}

// the largest free block, in K, as GEMDOS gives it (see ST_logFreeMemory)
long ST_freeK();
// the RAM the machine has, as found at start-up (0 before then), for
// code in user mode; kSTResidentRamK is what a machine needs to keep
// a level's room data and the big tile table (4MB, with some to spare)
int ATARIST_ramK();
enum { kSTResidentRamK = 3584 };

// The room cache keeps a room's two layers for a revisit while this
// much would stay free after it (video.cpp); the sprite ring, sized
// first, leaves room for this many such rooms on top (spritecache_st.cpp).
// Sized independently, the 1MB ring left the room cache one slot or
// none, and a few K either way decided which: a build 9K bigger lost
// level 5's revisits (65ms to 320ms), as did a Mega STE with 550K of
// drivers resident (245ms).
enum { kSTRoomCacheSpareK = 500, kSTRoomSlotsKept = 3 };
bool ST_screenModeAvailable(int mode);   // see kOverscanMinK

// 8x8 glyph from a chunky source (the glyph cache's): nonzero source
// pixels painted in colour8; rows are the same pixels as a byte a row,
// the leftmost in bit 7
void ST_drawGlyph(uint8_t *layer, const uint8_t *src, int pitch, const uint8_t *rows,
	int x, int y, uint8_t colour8);

void ST_fillRect(uint8_t *layer, int x, int y, int w, int h, uint8_t colour8);
void ST_hspan(uint8_t *layer, int x1, int x2, int y, uint8_t colour8);
void ST_hspanOrArea(uint8_t *layer, const int16_t *pts, int crx, int cry, int crw, uint8_t colour8);
void ST_drawPoint(uint8_t *layer, int x, int y, uint8_t colour8);
void ST_clearLayer(uint8_t *layer, uint8_t colour8);

#endif // ATARIST

#endif // VIDEO_ST_H__
