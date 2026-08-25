
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Atari ST platform layer built on STDL.
 *
 * The engine renders into a 256x224 chunky 8bpp layer with a
 * 256-entry logical palette and hands us dirty strips through
 * copyRect(). The ST is 320x200, 4 bitplanes, 16 colours, so this
 * stub does three jobs the engine knows nothing about:
 *
 *  - palette reduction: the 256 logical entries (in practice ~2-3
 *    distinct 16-colour Amiga palettes at a time) are quantised to
 *    the 16 hardware registers, with a remap table applied during
 *    conversion. Pure brightness changes (fades) reuse the previous
 *    mapping and only rewrite the registers.
 *  - chunky-to-planar: dirty regions are converted straight into
 *    screen RAM. A shadow copy of the chunky frame lets us skip
 *    16-pixel groups that did not change, which is what makes the
 *    engine's full-frame cutscene blits affordable.
 *  - vertical squash: 224 source lines into 200 display lines by
 *    dropping every 28th/9th line pair (25-of-28 mapping). The game
 *    area is centred horizontally (256 -> +32px, group aligned).
 */

extern "C" {
#include <stdl/stdl.h>
}

#include "systemstub.h"
#include "util.h"
#include "video_st.h"

static const int kMaxSrcW = 320;
static const int kMaxSrcH = 224;
static const int kScreenStride = 160;

struct SystemStub_STDL : SystemStub {
	STDL_Surface *_screen;
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
	void quantiseClusters(uint16_t *col, uint16_t *cnt, int n, uint8_t *alias, uint8_t *slotOf);
	bool _hwDirty;          // hardware registers need reprogramming
	int _fade;              // 256 = full brightness (uniform scale)
	int _srcW, _srcH;
	int _xOffset;           // horizontal centring, pixels (multiple of 16)
	uint8_t *_shadow;       // last converted chunky frame
	bool _shadowValid;
	uint8_t _yMap[kMaxSrcH];   // source line -> screen line
	uint8_t _yDrop[kMaxSrcH];  // 1 = line not displayed
	bool _ovscOpen;            // top border open: display starts line 34
	// key releases are applied at the start of the *next* poll so a
	// press+release inside one slow frame is still seen by the game
	uint8_t _upDirMask;
	bool _upEnter, _upSpace, _upShift;

	virtual ~SystemStub_STDL() {}
	virtual void init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate);
	virtual void destroy();
	virtual bool hasWidescreen() const { return false; }
	virtual void setScreenSize(int w, int h);
	virtual void setPalette(const uint8_t *pal, int n);
	virtual void getPalette(uint8_t *pal, int n);
	virtual void setPaletteEntry(int i, const Color *c);
	virtual void getPaletteEntry(int i, Color *c);
	virtual void setOverscanColor(int i) {}
	virtual void copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch);
	virtual void copyRectPlanar(int x, int y, int w, int h, const uint8_t *layer);
	virtual void copyRectRgb24(int x, int y, int w, int h, const uint8_t *rgb) {}
	virtual void zoomRect(int x, int y, int w, int h) {}
	virtual void copyWidescreenLeft(int w, int h, const uint8_t *buf) {}
	virtual void copyWidescreenRight(int w, int h, const uint8_t *buf) {}
	virtual void copyWidescreenMirror(int w, int h, const uint8_t *buf) {}
	virtual void copyWidescreenBlur(int w, int h, const uint8_t *buf) {}
	virtual void copyWidescreenCDi(int w, int h, const uint8_t *buf, const uint8_t *pal) {}
	virtual void clearWidescreen() {}
	virtual void enableWidescreen(bool enable) {}
	virtual void fadeScreen();
	virtual void updateScreen(int shakeOffset);
	virtual void processEvents();
	virtual void sleep(int duration);
	virtual uint32_t getTimeStamp();
	virtual void startAudio(AudioCallback callback, void *param) {}
	virtual void stopAudio() {}
	virtual uint32_t getOutputSampleRate() { return 12517; }
	virtual void lockAudio() {}
	virtual void unlockAudio() {}

	void buildRemap();
	void fadedColours(STDL_Colour *c);
	void writeHwPalette();
	void convertRegion(int x, int y, int w, int h, const uint8_t *buf, int pitch, bool useShadow);
	void reconvertFromShadow();
};

static SystemStub_STDL *g_stub;

SystemStub *SystemStub_STDL_create() {
	g_stub = new SystemStub_STDL();
	return g_stub;
}

// Draw-time access to the logical->hardware colour map for the
// planar layer primitives (video_st.cpp).
const uint8_t *ST_getRemap() {
	if (g_stub->_palDirty) {
		g_stub->buildRemap();
	}
	return g_stub->_remap;
}

// Copy a planar layer region to the screen: pure word moves with the
// 224->200 line squash and horizontal centring. x/w widen to 16px.
void SystemStub_STDL::copyRectPlanar(int x, int y, int w, int h, const uint8_t *layer) {
	if (_palDirty) {
		buildRemap();
	}
	const int gx0 = x >> 4;
	int gx1 = (x + w + 15) >> 4;
	if (gx1 > kSTLayerW / 16) {
		gx1 = kSTLayerW / 16;
	}
	if (y + h > kSTLayerH) {
		h = kSTLayerH - y;
	}
	const int bytes = (gx1 - gx0) << 3;
	if (bytes <= 0) {
		return;
	}
	const int xOffBytes = (_xOffset >> 4) << 3;
	// Large copies (full refreshes, room loads) go through
	// STDL_BlitSurface so a BLiTTER can take the aligned runs (same
	// 16px phase, unmasked; runs break where the 224->200 squash
	// drops a line). Small per-frame dirty runs stay on the word
	// loop below - measured faster than the per-call blit setup.
	// The layer is viewed maskless or the copy would key on the
	// priority plane.
	// With the top border open, display fetch starts at line 34
	// instead of 63: a full-page copy that used to hide in the
	// border now races the beam and loses intermittently (ghosting),
	// and the shared-mode BLiTTER (mandatory while a border is open,
	// see STDL) is slower than the CPU loop anyway. So while the
	// border is open, big copies skip the BLiTTER, and full-page
	// ones start at the VBL: the CPU loop writes rows faster than
	// the beam displays them, so the copy stays ahead - no tearing.
	if (_ovscOpen) {
		if (h >= kSTLayerH - 8) {
			STDL_WaitVBL();
		}
	} else
	if (h >= 32 && gx1 - gx0 >= 8 && _srcW == kSTLayerW
	    && STDL_GetMachineInfo()->has_blitter) {
		STDL_Surface *bare = ST_layerSurfaceBare((uint8_t *)layer);
		if (bare) {
			int j = 0;
			while (j < h) {
				if (_yDrop[y + j]) {
					++j;
					continue;
				}
				int j2 = j + 1;
				while (j2 < h && !_yDrop[y + j2]) {
					++j2;
				}
				STDL_Rect sr, dr;
				sr.x = (int16_t)(gx0 << 4);
				sr.y = (int16_t)(y + j);
				sr.w = (uint16_t)((gx1 - gx0) << 4);
				sr.h = (uint16_t)(j2 - j);
				dr.x = (int16_t)((gx0 << 4) + _xOffset);
				dr.y = _yMap[y + j];
				dr.w = sr.w;
				dr.h = sr.h;
				STDL_BlitSurface(bare, &sr, _screen, &dr);
				j = j2;
			}
			return;
		}
	}
	const int n32 = bytes >> 3;
	for (int j = 0; j < h; ++j) {
		const int sy = y + j;
		if (_yDrop[sy]) {
			continue;
		}
		const uint32_t *src = (const uint32_t *)(layer + sy * kSTRowBytes + (gx0 << 3));
		uint32_t *dst = (uint32_t *)(_screen->pixels + _yMap[sy] * kScreenStride + xOffBytes + (gx0 << 3));
		for (int n = n32; --n >= 0; ) {
			*dst++ = *src++;
			*dst++ = *src++;
		}
	}
}

void SystemStub_STDL::init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate) {
	memset(&_pi, 0, sizeof(_pi));
	_palLocked = false;
	_remapStale = true;
	_upDirMask = 0;
	_upEnter = _upSpace = _upShift = false;
	if (STDL_Init(0x20 | 0x200) != 0) { // VIDEO | JOYSTICK
		error("STDL_Init failed");
	}
	_screen = STDL_SetVideoMode(320, 200, 4, 0);
	if (!_screen) {
		error("STDL_SetVideoMode failed");
	}
	memset(_pal, 0, sizeof(_pal));
	memset(_basePal, 0, sizeof(_basePal));
	memset(_hwPal, 0, sizeof(_hwPal));
	memset(_remap, 0, sizeof(_remap));
	_palDirty = true;
	_hwDirty = true;
	_fade = 256;
	_shadow = (uint8_t *)malloc(kMaxSrcW * kMaxSrcH);
	_shadowValid = false;
	// 224 lines onto the ST's 200: open the top border so all 224
	// display natively (50Hz screens only - a 60Hz picture already
	// starts on the first possible line), or crop 12 lines off the
	// top and bottom (every displayed line is intact and screen
	// copies stay one contiguous run, but the edges of the playfield
	// are hidden), or squash with dst = y * 25 / 28, dropping
	// collisions
	_ovscOpen = false;
	if (g_options.overscan) {
		if (STDL_OpenTopBorder() != 0) {
			_ovscOpen = true;
			info("Top border open: 224 lines shown natively");
		} else {
			warning("Overscan unavailable (%s), using %s",
				STDL_GetError(),
				g_options.crop_screen ? "crop" : "squash");
		}
	}
	if (_ovscOpen) {
		for (int y = 0; y < kMaxSrcH; ++y) {
			_yDrop[y] = 0;
			_yMap[y] = y;
		}
	} else if (g_options.crop_screen) {
		for (int y = 0; y < kMaxSrcH; ++y) {
			const bool off = (y < 12) || (y >= kMaxSrcH - 12);
			_yDrop[y] = off ? 1 : 0;
			_yMap[y] = off ? 0 : (y - 12);
		}
	} else {
		int prev = -1;
		for (int y = 0; y < kMaxSrcH; ++y) {
			const int d = y * 25 / 28;
			_yDrop[y] = (d == prev) ? 1 : 0;
			_yMap[y] = d;
			prev = d;
		}
	}
	setScreenSize(w, h);
	// black screen until the first frame arrives
	memset(_screen->pixels, 0, kScreenStride * _screen->h);
	writeHwPalette();
	// Joystick and pad both drive the game as synthesised keypresses,
	// so the input code below needs to know nothing about either.
	//
	// Arrows and fire are all an ST joystick has, and fire is Shift,
	// Flashback's main verb. The rest exist only when a controller is
	// present; binding them on a machine without one is harmless, as
	// unbound inputs stay silent.
	//
	// Save, load, quit and the state-slot keys are deliberately absent:
	// they are Ctrl combinations, and emulation synthesises one key per
	// input, so they stay on the keyboard where they belong.
	STDL_JoyKeyMapping(STDLK_UP, STDLK_DOWN, STDLK_LEFT, STDLK_RIGHT, STDLK_RSHIFT);

	STDL_JoyKeyBind(STDL_JOYKEY_EAST,   STDLK_RETURN);     // use item
	STDL_JoyKeyBind(STDL_JOYKEY_NORTH,  STDLK_BACKSPACE);  // inventory
	STDL_JoyKeyBind(STDL_JOYKEY_WEST,   STDLK_SPACE);
	STDL_JoyKeyBind(STDL_JOYKEY_START,  STDLK_ESCAPE);     // options
	STDL_JoyKeyBind(STDL_JOYKEY_SELECT, STDLK_BACKSPACE);  // inventory

	// The two you reach for constantly, put where a thumb is not
	// needed: inventory and use, on the shoulders.
	STDL_JoyKeyBind(STDL_JOYKEY_TL, STDLK_BACKSPACE);
	STDL_JoyKeyBind(STDL_JOYKEY_TR, STDLK_RETURN);

	STDL_JoyKeyEmulation(1);
}

void SystemStub_STDL::destroy() {
	free(_shadow);
	_shadow = 0;
	STDL_Quit();
}

void SystemStub_STDL::setScreenSize(int w, int h) {
	if (w > kMaxSrcW) {
		w = kMaxSrcW;
	}
	if (h > kMaxSrcH) {
		h = kMaxSrcH;
	}
	if (w != _srcW || h != _srcH) {
		_srcW = w;
		_srcH = h;
		_xOffset = ((320 - w) / 2) & ~15;
		_shadowValid = false;
		memset(_screen->pixels, 0, kScreenStride * _screen->h);
	}
}

void SystemStub_STDL::setPalette(const uint8_t *pal, int n) {
	if (n > 256) {
		n = 256;
	}
	for (int i = 0; i < n; ++i) {
		_pal[i].r = pal[i * 3];
		_pal[i].g = pal[i * 3 + 1];
		_pal[i].b = pal[i * 3 + 2];
	}
	_palDirty = true;
}

void SystemStub_STDL::getPalette(uint8_t *pal, int n) {
	if (n > 256) {
		n = 256;
	}
	for (int i = 0; i < n; ++i) {
		pal[i * 3] = _pal[i].r;
		pal[i * 3 + 1] = _pal[i].g;
		pal[i * 3 + 2] = _pal[i].b;
	}
}

void SystemStub_STDL::setPaletteEntry(int i, const Color *c) {
	if (_pal[i].r != c->r || _pal[i].g != c->g || _pal[i].b != c->b) {
		_pal[i] = *c;
		_palDirty = true;
	}
}

void SystemStub_STDL::getPaletteEntry(int i, Color *c) {
	*c = _pal[i];
}

// Cutscene palette mode: shapes are authored against logical
// entries 0xC0-0xCF (8 base + 8 shadow colours), so map those to
// hardware slots 0-15 directly. Baked page content then follows
// palette changes exactly like the chunky original, and the shadow
// effect (index |= 8) becomes a plane-3 OR.
static bool g_cutscenePal;

// Bumped whenever _remap changes. Callers that bake the mapping into
// planar pixels (the sprite cache) key their entries on this.
static uint16_t g_remapGen = 1;

uint16_t ST_remapGen() {
	return g_remapGen;
}

void ST_setCutscenePalMode(bool enable) {
	if (g_cutscenePal != enable) {
		g_cutscenePal = enable;
		g_stub->_palDirty = true;
		g_stub->_remapStale = true;
	}
}

bool ST_rowVisible(int y) {
	if (y < 0 || y >= kMaxSrcH) {
		return false;
	}
	return g_stub->_yDrop[y] == 0;
}

bool ST_cutscenePalMode() {
	return g_cutscenePal;
}

// The cutscene shadow effect in hardware-colour space: the chunky op
// is L |= colour8 & 0xF8 in logical space, so for each hw slot take
// a representative logical entry, apply the OR, and see where the
// result lands. Built per shadow shape - 16 lookups.
void ST_getOrMap(uint8_t colour8, uint8_t *orMap) {
	if (g_stub->_palDirty) {
		g_stub->buildRemap();
	}
	for (int s = 0; s < 16; ++s) {
		const uint8_t L = g_stub->_cutsceneRep[s];
		orMap[s] = g_stub->_remap[L | (colour8 & 0xF8)];
	}
}

// Which logical entries the current mode quantises: the cutscene
// banks 0xC0-0xEF in cutscene mode, everything but 0xC0-0xDF in game
// mode (a finished cutscene's colours must not crowd out the game's).
static inline bool excludedEntry(int i) {
	const bool cutEntry = (i >= 0xC0 && i < 0xF0);
	return g_cutscenePal ? !cutEntry : (i >= 0xC0 && i < 0xE0);
}

// Manhattan colour distance (green-weighted): the quantiser runs on
// every cutscene palette step, and squared distances cost a 32-bit
// multiply library call each on the 68000.
static inline int colDist(const Color &a, const Color &b) {
	int dr = a.r - b.r; if (dr < 0) dr = -dr;
	int dg = a.g - b.g; if (dg < 0) dg = -dg;
	int db = a.b - b.b; if (db < 0) db = -db;
	return dr + 2 * dg + db;
}

// Merge n distinct 4-bit colours down to 16 clusters and give each
// surviving cluster a hardware slot, preferring the slot whose current
// colour is nearest: planar pages bake slots into pixels, so content
// that outlives a palette change only stays correct if unchanged
// colours keep their slots. Fills alias[] (cluster union-find) and
// slotOf[] (cluster -> slot), and updates _hwPal.
void SystemStub_STDL::quantiseClusters(uint16_t *col, uint16_t *cnt, int n, uint8_t *alias, uint8_t *slotOf) {
	// greedy merge to 16 clusters
	for (int i = 0; i < n; ++i) {
		alias[i] = i;
	}
	int live = n;
	while (live > 16) {
		int bi = -1, bj = -1;
		long best = 0x7FFFFFFF;
		for (int i = 0; i < n; ++i) {
			if (alias[i] != i) {
				continue;
			}
			const int r1 = (col[i] >> 8) & 15, g1 = (col[i] >> 4) & 15, b1 = col[i] & 15;
			for (int j = i + 1; j < n; ++j) {
				if (alias[j] != j) {
					continue;
				}
				int dr = r1 - ((col[j] >> 8) & 15); if (dr < 0) dr = -dr;
				int dg = g1 - ((col[j] >> 4) & 15); if (dg < 0) dg = -dg;
				int db = b1 - (col[j] & 15); if (db < 0) db = -db;
				// weight by the smaller usage count so rare
				// colours give way to common ones (16-bit multiply:
				// a mulu.w, not a __mulsi3 library call)
				const uint16_t w = (uint16_t)(1 + MIN(MIN(cnt[i], cnt[j]), (uint16_t)63));
				const long d = (long)((uint16_t)(dr + 2 * dg + db) * w);
				if (d < best) {
					best = d;
					bi = i;
					bj = j;
				}
			}
		}
		// merge bj into bi, weighted average
		const long w1 = cnt[bi], w2 = cnt[bj];
		const int r = (((col[bi] >> 8) & 15) * w1 + ((col[bj] >> 8) & 15) * w2) / (w1 + w2);
		const int g = (((col[bi] >> 4) & 15) * w1 + ((col[bj] >> 4) & 15) * w2) / (w1 + w2);
		const int b = ((col[bi] & 15) * w1 + (col[bj] & 15) * w2) / (w1 + w2);
		col[bi] = (r << 8) | (g << 4) | b;
		cnt[bi] += cnt[bj];
		alias[bj] = bi;
		--live;
	}

	// Assign hardware slots stickily: match each cluster to the slot
	// whose previous colour is nearest. Planar layers bake the remap
	// into their pixels at draw time, so content that survives a
	// palette change keeps its colours only if unchanged colours
	// keep their slots.
	int clusters[16];
	int nc = 0;
	for (int i = 0; i < n; ++i) {
		if (alias[i] == i) {
			clusters[nc++] = i;
		}
	}
	bool slotUsed[16];
	bool clusterDone[16];
	memset(slotUsed, 0, sizeof(slotUsed));
	memset(clusterDone, 0, sizeof(clusterDone));
	Color newHw[16];
	memset(newHw, 0, sizeof(newHw));
	for (int pass = 0; pass < nc; ++pass) {
		long best = 0x7FFFFFFF;
		int bc = -1, bs = -1;
		for (int c = 0; c < nc; ++c) {
			if (clusterDone[c]) {
				continue;
			}
			const int i = clusters[c];
			const int r1 = (col[i] >> 8) & 15, g1 = (col[i] >> 4) & 15, b1 = col[i] & 15;
			for (int s = 0; s < 16; ++s) {
				if (slotUsed[s]) {
					continue;
				}
				int dr = r1 - (_hwPal[s].r >> 4); if (dr < 0) dr = -dr;
				int dg = g1 - (_hwPal[s].g >> 4); if (dg < 0) dg = -dg;
				int db = b1 - (_hwPal[s].b >> 4); if (db < 0) db = -db;
				const long d = dr + 2 * dg + db;
				if (d < best) {
					best = d;
					bc = c;
					bs = s;
				}
			}
		}
		const int i = clusters[bc];
		slotOf[i] = bs;
		const uint8_t r = (col[i] >> 8) & 15, g = (col[i] >> 4) & 15, b = col[i] & 15;
		newHw[bs].r = (r << 4) | r;
		newHw[bs].g = (g << 4) | g;
		newHw[bs].b = (b << 4) | b;
		slotUsed[bs] = true;
		clusterDone[bc] = true;
	}
	// unused slots keep their previous colours so stale pixels at
	// least stay stable
	for (int s = 0; s < 16; ++s) {
		if (!slotUsed[s]) {
			newHw[s] = _hwPal[s];
		}
	}
	memcpy(_hwPal, newHw, sizeof(_hwPal));
}

// --- cutscene palette collection and locking -------------------
//
// The 32 cutscene entries change colour over the scene (fades, and
// new palettes at scene-part boundaries), so the mapping is decided
// per ENTRY INDEX rather than per colour: each entry is represented
// by the brightest colour it is ever given (fades are scalings of
// that), those are merged to 16 clusters, and each entry keeps its
// slot for the whole scene.

// Scenes are made of parts separated by a full screen clear, and a
// part reuses the same 32 entries for very different colours, so the
// collection is bucketed by part and the mapping is re-locked as each
// part starts - when the page has just been cleared and nothing on it
// depends on the old mapping.
//
// Two entries may share a hardware slot only if they are similar in
// EVERY palette the part loads: the logo's camera and its caption
// both ramp through greys as the scene fades up, but they are never
// the same grey at the same moment, so they need separate slots.
// Clustering therefore runs on worst-case distance between entries
// (complete linkage) over the part's palettes, not on one snapshot.
enum { kCutParts = 16, kCutEntries = 32 };

// Per part: the worst-case distance seen between every pair of
// entries, accumulated over every palette the part loads, plus each
// entry's brightest colour (used only to choose which slot a cluster
// lands in). Two entries may share a slot only if they never differ.
static uint8_t g_partDist[kCutParts][kCutEntries][kCutEntries];
static uint16_t g_partRep[kCutParts][kCutEntries];
static bool g_partUsed[kCutParts];
static int g_collectPart;

static inline uint16_t pack4(const Color &c) {
	return (uint16_t)(((c.r >> 4) << 8) | ((c.g >> 4) << 4) | (c.b >> 4));
}

static inline int dist4(uint16_t a, uint16_t b) {
	int dr = ((a >> 8) & 15) - ((b >> 8) & 15); if (dr < 0) dr = -dr;
	int dg = ((a >> 4) & 15) - ((b >> 4) & 15); if (dg < 0) dg = -dg;
	int db = (a & 15) - (b & 15); if (db < 0) db = -db;
	return dr + 2 * dg + db;
}

void ST_cutscenePalReset() {
	memset(g_partUsed, 0, sizeof(g_partUsed));
	g_collectPart = 0;
}

void ST_cutscenePalPart(int part) {
	g_collectPart = (part < kCutParts) ? part : (kCutParts - 1);
}

void ST_cutscenePalCollect(const Color *entries, int n) {
	if (n > 32) {
		n = 32;
	}
	const int part = g_collectPart;
	uint16_t packed[kCutEntries];
	memset(packed, 0, sizeof(packed));
	for (int i = 0; i < n; ++i) {
		packed[i] = pack4(entries[i]);
	}
	if (!g_partUsed[part]) {
		g_partUsed[part] = true;
		memset(g_partDist[part], 0, sizeof(g_partDist[part]));
		memset(g_partRep[part], 0, sizeof(g_partRep[part]));
	}
	uint8_t (*dm)[kCutEntries] = g_partDist[part];
	uint16_t *rep = g_partRep[part];
	for (int i = 0; i < kCutEntries; ++i) {
		if (dist4(packed[i], 0) > dist4(rep[i], 0)) {
			rep[i] = packed[i];
		}
		for (int j = i + 1; j < kCutEntries; ++j) {
			int d = dist4(packed[i], packed[j]);
			if (d > 255) {
				d = 255;
			}
			if (d > dm[i][j]) {
				dm[i][j] = dm[j][i] = (uint8_t)d;
			}
		}
	}
}

void ST_cutscenePalLock(int part) {
	if (part >= kCutParts) {
		part = kCutParts - 1;
	}
	if (part < 0 || !g_partUsed[part]) {
		// A part the prescan never reached plays under the previous
		// part's 16 colours, which looks exactly like a quantiser
		// fault, so say so rather than degrade silently.
		if (part > 0) {
			warning("Cutscene part %d has no collected palette", part);
		}
		return;
	}
	SystemStub_STDL *stub = g_stub;
	const uint16_t *rep = g_partRep[part];
	static uint8_t dm[kCutEntries][kCutEntries];
	memcpy(dm, g_partDist[part], sizeof(dm));

	uint8_t alias[kCutEntries];
	for (int i = 0; i < kCutEntries; ++i) {
		alias[i] = (uint8_t)i;
	}
	int live = kCutEntries;
	while (live > 16) {
		int bi = -1, bj = -1, best = 0x7FFF;
		for (int i = 0; i < kCutEntries; ++i) {
			if (alias[i] != i) {
				continue;
			}
			for (int j = i + 1; j < kCutEntries; ++j) {
				if (alias[j] != j) {
					continue;
				}
				if (dm[i][j] < best) {
					best = dm[i][j];
					bi = i;
					bj = j;
				}
			}
		}
		if (bi < 0) {
			break;
		}
		for (int k = 0; k < kCutEntries; ++k) {
			if (dm[bj][k] > dm[bi][k]) {
				dm[bi][k] = dm[k][bi] = dm[bj][k];
			}
		}
		alias[bj] = (uint8_t)bi;
		--live;
	}

	// give each surviving cluster the free slot whose current colour
	// is nearest, so slots stay put across re-locks where they can
	int clusters[16];
	int nc = 0;
	for (int i = 0; i < kCutEntries && nc < 16; ++i) {
		if (alias[i] == i) {
			clusters[nc++] = i;
		}
	}
	bool slotUsed[16], clusterDone[16];
	memset(slotUsed, 0, sizeof(slotUsed));
	memset(clusterDone, 0, sizeof(clusterDone));
	uint8_t slotOfCluster[16];
	for (int pass = 0; pass < nc; ++pass) {
		int best = 0x7FFF, bc = -1, bs = -1;
		for (int c = 0; c < nc; ++c) {
			if (clusterDone[c]) {
				continue;
			}
			const uint16_t rc = rep[clusters[c]];
			for (int sl = 0; sl < 16; ++sl) {
				if (slotUsed[sl]) {
					continue;
				}
				const int d = dist4(rc, pack4(stub->_hwPal[sl]));
				if (d < best) {
					best = d;
					bc = c;
					bs = sl;
				}
			}
		}
		if (bc < 0) {
			break;
		}
		slotOfCluster[bc] = (uint8_t)bs;
		slotUsed[bs] = true;
		clusterDone[bc] = true;
	}

	bool repSet[16];
	uint16_t slotColour[16];
	memset(repSet, 0, sizeof(repSet));
	memset(slotColour, 0, sizeof(slotColour));
	for (int i = 0; i < kCutEntries; ++i) {
		int r = i;
		while (alias[r] != r) {
			r = alias[r];
		}
		int c = 0;
		while (c < nc && clusters[c] != r) {
			++c;
		}
		if (c == nc) {
			continue;
		}
		const uint8_t slot = slotOfCluster[c];
		stub->_remap[0xC0 + i] = slot;
		// the slot shows its representative entry's LIVE colour, so
		// fades and colour cycling need no re-quantisation at all
		if (!repSet[slot]) {
			stub->_cutsceneRep[slot] = (uint8_t)(0xC0 + i);
			slotColour[slot] = rep[i];
			repSet[slot] = true;
		}
	}
	for (int sl = 0; sl < 16; ++sl) {
		if (!repSet[sl]) {
			stub->_cutsceneRep[sl] = 0xC0;
		}
	}
	// Captions draw with the text/menu entries, which the scene never
	// sets. They stay out of the clustering - 16 static colours would
	// take every slot from the art - and take the nearest slot the
	// scene's own palette provides.
	for (int i = 0; i < 16; ++i) {
		const uint16_t want = pack4(stub->_pal[0xE0 + i]);
		int best = 0x7FFF, bs = 0;
		for (int sl = 0; sl < 16; ++sl) {
			const int d = dist4(want, slotColour[sl]);
			if (d < best) {
				best = d;
				bs = sl;
			}
		}
		stub->_remap[0xE0 + i] = (uint8_t)bs;
	}
	++g_remapGen;
	stub->_palLocked = true;
	stub->_remapStale = false;
	stub->_palDirty = true;
}

void ST_cutscenePalUnlock() {
	g_stub->_palLocked = false;
	g_stub->_remapStale = true;
	g_stub->_palDirty = true;
}

// Quantise the 256-entry logical palette to 16 hardware colours.
// Colours are reduced to STE 4-bit per channel first (the hardware
// cannot do better), deduplicated, then greedily merged by nearest
// pair until 16 remain.
void SystemStub_STDL::buildRemap() {
	_palDirty = false;

	// Locked cutscene mapping: the logical->slot assignment is fixed
	// for the scene, so a palette change is just a refresh of the 16
	// registers from each slot's representative entry. Fades cost
	// nothing and drawn pixels never need re-baking.
	if (_palLocked) {
		for (int s = 0; s < 16; ++s) {
			_hwPal[s] = _pal[_cutsceneRep[s]];
		}
		_fade = 256;
		_hwDirty = true;
		return;
	}

	// Estimate the overall brightness scale against the palette the
	// mapping was last built for: fades ramp every entry by the same
	// factor, so measuring the brightest base entry recovers it
	// (fade-ins ramp FROM the base upward - brightening rides the
	// same path with the registers clamping per channel).
	int num = 0, den = 0;
	for (int i = 0; i < 256; ++i) {
		if (excludedEntry(i)) {
			continue;
		}
		const int bsum = _basePal[i].r + _basePal[i].g + _basePal[i].b;
		const int nsum = _pal[i].r + _pal[i].g + _pal[i].b;
		if (bsum > den) {
			den = bsum;
			num = nsum;
		}
	}
	// Patch fast path: cutscene scripts animate a handful of entries
	// (blinking lights, shading variants of one palette) while the
	// bulk of the palette stands still. A full requantise on every
	// such step flips merge decisions and the whole scene jumps
	// between two or three looks, so when only a few entries moved,
	// keep the standing mapping and point just the changed entries
	// at their nearest current hardware colour. A real scene change
	// replaces most of the palette and still rebuilds below.
	if (!_remapStale && den > 0) {
		// Scale each base channel by num/den through a table: inline
		// that is three multiplies and three DIVIDES per entry, all
		// library calls on 68000 - about 1350 of them per palette
		// change, just to decide whether anything moved.
		const int fade = num * 256 / den;
		uint8_t scaled[256];
		for (int v = 0; v < 256; ++v) {
			const int t = v * fade >> 8;
			scaled[v] = (uint8_t)(t > 255 ? 255 : t);
		}
		uint8_t changed[16];
		int nchg = 0;
		for (int i = 0; i < 256; ++i) {
			if (excludedEntry(i)) {
				continue;
			}
			const int er = scaled[_basePal[i].r] - _pal[i].r;
			const int eg = scaled[_basePal[i].g] - _pal[i].g;
			const int eb = scaled[_basePal[i].b] - _pal[i].b;
			const int d = (er < 0 ? -er : er) + 2 * (eg < 0 ? -eg : eg) + (eb < 0 ? -eb : eb);
			if (d > 40) {
				if (nchg == 16) {
					++nchg;
					break;
				}
				changed[nchg++] = (uint8_t)i;
			}
		}
		if (nchg <= 16) {
			for (int c = 0; c < nchg; ++c) {
				const int i = changed[c];
				long best = 0x7FFFFFFF;
				int bs = 0;
				for (int s = 0; s < 16; ++s) {
					const long d = colDist(_pal[i], _hwPal[s]);
					if (d < best) {
						best = d;
						bs = s;
					}
				}
				_remap[i] = (uint8_t)bs;
			}
			++g_remapGen;
			_fade = (fade > 1023) ? 1023 : fade;
			_hwDirty = true;
			return;
		}
	}
	_fade = 256;

	// Gather distinct 4-bit colours with usage counts. Logical
	// entries 0xC0-0xDF belong to cutscenes only (the game's slots
	// are 0x0-0xB plus the text slots): in game mode they are
	// excluded so a finished cutscene's palette cannot crowd the
	// game's colours out of the 16 hardware slots, and in cutscene
	// mode the quantisation runs over ONLY the cutscene banks and
	// the text slot (scenes use up to 32 colours - DEBUT draws
	// Conrad from the upper bank). Excluded entries are mapped to
	// the nearest surviving colour below.
	uint16_t col[256];
	uint16_t cnt[256];
	uint8_t entryCluster[256];
	int n = 0;
	for (int i = 0; i < 256; ++i) {
		if (excludedEntry(i)) {
			entryCluster[i] = 0xFF;
			continue;
		}
		const uint16_t c = ((_pal[i].r >> 4) << 8) | ((_pal[i].g >> 4) << 4) | (_pal[i].b >> 4);
		int j = 0;
		for (; j < n; ++j) {
			if (col[j] == c) {
				break;
			}
		}
		if (j == n) {
			col[n] = c;
			cnt[n] = 0;
			++n;
		}
		++cnt[j];
		entryCluster[i] = j;
	}

	uint8_t alias[256];
	uint8_t slotOf[256];
	quantiseClusters(col, cnt, n, alias, slotOf);
	uint8_t newRemap[256];
	memset(_cutsceneRep, 0xC0, sizeof(_cutsceneRep));
	bool repSet[16];
	memset(repSet, 0, sizeof(repSet));
	for (int i = 0; i < 256; ++i) {
		if (entryCluster[i] == 0xFF) {
			if (g_cutscenePal) {
				// game entries are never drawn during a cutscene
				newRemap[i] = 0;
				continue;
			}
			// excluded cutscene entry: nearest surviving colour
			long best = 0x7FFFFFFF;
			int bs = 0;
			for (int s = 0; s < 16; ++s) {
				const long d = colDist(_pal[i], _hwPal[s]);
				if (d < best) {
					best = d;
					bs = s;
				}
			}
			newRemap[i] = bs;
			continue;
		}
		int c = entryCluster[i];
		while (alias[c] != c) {
			c = alias[c];
		}
		newRemap[i] = slotOf[c];
		if (i >= 0xC0 && i < 0xE0 && !repSet[slotOf[c]]) {
			_cutsceneRep[slotOf[c]] = (uint8_t)i;
			repSet[slotOf[c]] = true;
		}
	}
	memcpy(_basePal, _pal, sizeof(_pal));
	_hwDirty = true;
	_remapStale = false;
	if (memcmp(newRemap, _remap, sizeof(_remap)) != 0) {
		memcpy(_remap, newRemap, sizeof(_remap));
		++g_remapGen;
		// stale planar data on screen uses the old mapping:
		// re-run the conversion from the shadow frame
		reconvertFromShadow();
	}
}

// the 16 hardware colours at the current fade level; _fade can
// exceed 256 (brightening fades), so every channel is clamped
void SystemStub_STDL::fadedColours(STDL_Colour *c) {
	for (int i = 0; i < 16; ++i) {
		int r = _hwPal[i].r * _fade >> 8;
		int g = _hwPal[i].g * _fade >> 8;
		int b = _hwPal[i].b * _fade >> 8;
		c[i].r = (uint8_t)(r > 255 ? 255 : r);
		c[i].g = (uint8_t)(g > 255 ? 255 : g);
		c[i].b = (uint8_t)(b > 255 ? 255 : b);
		c[i].unused = 0;
	}
}

void SystemStub_STDL::writeHwPalette() {
	STDL_Colour c[16];
	fadedColours(c);
	STDL_SetColours(_screen, c, 0, 16);
	_hwDirty = false;
}

// Convert a chunky region to the planar screen. x/w are widened to
// 16-pixel group boundaries (buf is the whole layer, so neighbouring
// pixels are always available). With useShadow, groups whose 16
// source bytes match the shadow copy are skipped.
void SystemStub_STDL::convertRegion(int x, int y, int w, int h, const uint8_t *buf, int pitch, bool useShadow) {
	int x0 = x & ~15;
	int x1 = (x + w + 15) & ~15;
	if (x1 > _srcW) {
		x1 = _srcW;
	}
	if (y + h > _srcH) {
		h = _srcH - y;
	}
	const int groups = (x1 - x0) >> 4;
	if (groups <= 0) {
		return;
	}
	const uint8_t *remap = _remap;
	for (int j = 0; j < h; ++j) {
		const int sy = y + j;
		if (_yDrop[sy]) {
			continue;
		}
		const uint8_t *src = buf + sy * pitch + x0;
		uint32_t *sh = (uint32_t *)(_shadow + sy * kMaxSrcW + x0);
		uint16_t *dst = (uint16_t *)(_screen->pixels + _yMap[sy] * kScreenStride + ((x0 + _xOffset) >> 4) * 8);
		for (int g = 0; g < groups; ++g) {
			const uint32_t *s32 = (const uint32_t *)src;
			if (useShadow) {
				if (sh[0] == s32[0] && sh[1] == s32[1] && sh[2] == s32[2] && sh[3] == s32[3]) {
					src += 16;
					sh += 4;
					dst += 4;
					continue;
				}
			}
			sh[0] = s32[0];
			sh[1] = s32[1];
			sh[2] = s32[2];
			sh[3] = s32[3];
			uint16_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
			for (int i = 0; i < 16; ++i) {
				const uint8_t v = remap[src[i]];
				p0 += p0 + (v & 1);
				p1 += p1 + ((v >> 1) & 1);
				p2 += p2 + ((v >> 2) & 1);
				p3 += p3 + ((v >> 3) & 1);
			}
			dst[0] = p0;
			dst[1] = p1;
			dst[2] = p2;
			dst[3] = p3;
			src += 16;
			sh += 4;
			dst += 4;
		}
	}
}

void SystemStub_STDL::reconvertFromShadow() {
	if (_shadowValid) {
		convertRegion(0, 0, _srcW, _srcH, _shadow, kMaxSrcW, false);
	}
}

void SystemStub_STDL::copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch) {
	if (_palDirty) {
		buildRemap();
	}
	const bool full = (x == 0 && y == 0 && w == _srcW && h == _srcH);
	convertRegion(x, y, w, h, buf, pitch, _shadowValid);
	if (full) {
		_shadowValid = true;
	}
}

uint32_t ST_overscanMisses() {
	return STDL_OverscanMisses();
}

void SystemStub_STDL::updateScreen(int shakeOffset) {
	if (_palDirty) {
		buildRemap();
	}
	if (_hwDirty) {
		writeHwPalette();
	}
}

// Hardware fade to black; the next updateScreen restores the palette
// (in practice the engine always redraws and re-sets palettes after
// fading out).
void SystemStub_STDL::fadeScreen() {
	STDL_Colour base[16];
	fadedColours(base);
	for (int step = 7; step >= 0; --step) {
		STDL_Colour c[16];
		for (int i = 0; i < 16; ++i) {
			c[i].r = (uint8_t)(base[i].r * step >> 3);
			c[i].g = (uint8_t)(base[i].g * step >> 3);
			c[i].b = (uint8_t)(base[i].b * step >> 3);
			c[i].unused = 0;
		}
		STDL_SetColours(_screen, c, 0, 16);
		STDL_Delay(40);
	}
	memset(_screen->pixels, 0, kScreenStride * _screen->h);
	_shadowValid = false;
	_hwDirty = true;
}

void SystemStub_STDL::processEvents() {
	_pi.dirMask &= ~_upDirMask;
	_upDirMask = 0;
	if (_upEnter) { _pi.enter = false; _upEnter = false; }
	if (_upSpace) { _pi.space = false; _upSpace = false; }
	if (_upShift) { _pi.shift = false; _upShift = false; }
	STDL_Event ev;
	while (STDL_PollEvent(&ev)) {
		switch (ev.type) {
		case STDL_QUIT:
			_pi.quit = true;
			break;
		case STDL_KEYDOWN:
		case STDL_KEYUP: {
			const bool down = (ev.type == STDL_KEYDOWN);
			const uint16_t sym = ev.key.keysym.sym;
			const uint16_t mod = ev.key.keysym.mod;
			if (down) {
				_pi.anyKey = true;
			}
			switch (sym) {
			case STDLK_UP:
				if (down) { _pi.dirMask |= PlayerInput::DIR_UP; _upDirMask &= ~PlayerInput::DIR_UP; } else _upDirMask |= PlayerInput::DIR_UP;
				break;
			case STDLK_DOWN:
				if (down) { _pi.dirMask |= PlayerInput::DIR_DOWN; _upDirMask &= ~PlayerInput::DIR_DOWN; } else _upDirMask |= PlayerInput::DIR_DOWN;
				break;
			case STDLK_LEFT:
				if (down) { _pi.dirMask |= PlayerInput::DIR_LEFT; _upDirMask &= ~PlayerInput::DIR_LEFT; } else _upDirMask |= PlayerInput::DIR_LEFT;
				break;
			case STDLK_RIGHT:
				if (down) { _pi.dirMask |= PlayerInput::DIR_RIGHT; _upDirMask &= ~PlayerInput::DIR_RIGHT; } else _upDirMask |= PlayerInput::DIR_RIGHT;
				break;
			case STDLK_RETURN:
			case STDLK_KP_ENTER:
				if (down) { _pi.enter = true; _upEnter = false; } else _upEnter = true;
				break;
			case STDLK_SPACE:
				if (down) { _pi.space = true; _upSpace = false; } else _upSpace = true;
				break;
			case STDLK_LSHIFT:
			case STDLK_RSHIFT:
				if (down) { _pi.shift = true; _upShift = false; } else _upShift = true;
				break;
			// backspace and escape are edge-consumed by the game
			// (it writes false back). Frames can be slow enough
			// that press and release arrive in the same poll, so
			// never clear them here or the game misses the press.
			case STDLK_BACKSPACE:
			case STDLK_TAB:
				if (down) {
					_pi.backspace = true;
				}
				break;
			case STDLK_ESCAPE:
				if (down) {
					_pi.escape = true;
				}
				break;
			default:
				if (down) {
					if (mod & (STDL_KMOD_LCTRL | STDL_KMOD_RCTRL)) {
						switch (sym) {
						case STDLK_s:
							_pi.save = true;
							break;
						case STDLK_l:
							_pi.load = true;
							break;
						case STDLK_q:
							_pi.quit = true;
							break;
						case STDLK_KP_PLUS:
						case STDLK_EQUALS:
							_pi.stateSlot = 1;
							break;
						case STDLK_KP_MINUS:
						case STDLK_MINUS:
							_pi.stateSlot = -1;
							break;
						}
					} else if (sym >= STDLK_a && sym <= STDLK_z) {
						_pi.lastChar = sym - STDLK_a + 'A';
					} else if (sym >= STDLK_0 && sym <= STDLK_9) {
						_pi.lastChar = sym - STDLK_0 + '0';
					}
				}
				break;
			}
			break;
		}
		default:
			break;
		}
	}
}

void SystemStub_STDL::sleep(int duration) {
	STDL_Delay(duration);
}

uint32_t SystemStub_STDL::getTimeStamp() {
	return STDL_GetTicks();
}
