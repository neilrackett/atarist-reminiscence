/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The ST's 16 colours from the engine's 256 logical ones: the colour
 * quantiser, the map from logical entries to hardware slots, the fade
 * that rides on it, the cutscene lock and the custom palettes. The ST
 * build's screen (systemstub_stdl.cpp) has it, and so does the
 * experimental build's on an ST (systemstub_030.cpp), which hands its
 * map and colours to STDL's SDL2 add-on, so that both show the same
 * colours. st_palette.cpp has the code; video_st.h declares the calls
 * the rest of the game makes on it.
 */

#ifndef ST_PALETTE_H__
#define ST_PALETTE_H__

extern "C" {
#include <stdl/stdl.h>
}
#include "intern.h"

struct STPalette {
	Color _pal[256];        // logical palette (engine side)
	Color _basePal[256];    // palette when the remap was last built
	Color _hwPal[16];       // quantised colours at last build
	uint8_t _remap[256];    // logical index -> hardware index
	bool _palDirty;         // logical palette changed, remap stale
	// set when the palette MODE changed (cutscene <-> game): the
	// uniform-fade shortcut must not reuse the other mode's remap
	bool _remapStale;
	// per hardware slot, a cutscene logical entry mapping to it
	// (for the shadow-effect hw->hw lookup)
	uint8_t _cutsceneRep[16];
	bool _palLocked;        // fixed cutscene mapping, see video_st.h
	bool _cutPending;       // a new mapping's first page is still to go up
	void quantiseClusters(uint16_t *col, uint16_t *cnt, int n, uint8_t *alias, uint8_t *slotOf, bool perceptual);
	bool _hwDirty;          // hardware registers need reprogramming
	int _fade;              // 256 = full brightness (uniform scale)
	void buildRemap();
	void applyRemap(const uint8_t *newRemap);
	int customSlotFor(int i);
	// palette_custom: the room's own 16 colours, used in place of the
	// quantiser's in game mode (see ST_setCustomPalette)
	bool _customPal;
	Color _customCols[16];
	// the file's "= n,n" lists: Amiga colour 0-31 -> slot, 0xFF for
	// none; and which Amiga colour each logical entry is, per room
	uint8_t _customMap[32];
	uint8_t _amigaIdx[256];
	// what each entry is worth to the automatic palette in play (see
	// ST_setColourWeights); without them every entry counts once
	uint16_t _colW[256];
	bool _haveColW;
	// all black, no mapping yet, nothing custom: a screen's start
	void resetColours();
	void fadedColours(STDL_Colour *c);
};

// the one the game's ST_ palette calls (video_st.h) act on; none in the
// experimental build on a TT or a Falcon, which shows the 256 colours
extern STPalette *g_stpal;

// custom palettes and Ctrl+P's dump (palette_st.cpp), and the replay's
// frame dump
void ST_setCustomPalette(const Color *cols, const uint8_t *map32);
void ST_getHwColours(Color *cols);
void ST_getAmigaColours(Color *cols, uint8_t *slots, bool *have);

#ifdef ST_PLANAR
// a new map moved colours to other slots: what the screen holds was
// converted with the old one, so the ST build converts its shadow frame
// again (systemstub_stdl.cpp); the experimental build's add-on does
// that itself
void ST_reconvertScreen();
#endif

#endif // ST_PALETTE_H__
