
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
#include <stdl/stdl_math.h>
}
#include <osbind.h>

#include "systemstub_atari.h"
#include "util.h"
#include "video_st.h"
#include "st_palette.h"
#include "palette_tables.h"


static const int kMaxSrcW = 320;
static const int kMaxSrcH = 224;
static const int kScreenStride = 160;
// the tallest surface an open border gives (both borders: 273 rows)
static const int kMaxScreenRows = 288;
// Fit's geometry, shared with ST_textRow: two rows cropped at each
// end, and between them every eleventh row dropped, the first at
// kFitFirstDrop - rows 9, 20, 31 ... 218. Where the drops fall is
// chosen: Conrad stands on floors 72 rows apart and his head bobs a
// row as he walks, and at 12, 23, 34 ... the row that went was the
// top of his hair on the top and bottom floors, so he walked about
// half bald. From 9 no floor loses the top of his head or his feet.
enum { kFitCrop = 2, kFitRun = 11, kFitFirstDrop = 9 };

// where row y falls in Fit's pattern: 0-9 a kept run, 10 dropped
static inline int fitPhase(int y) {
	return (y - kFitFirstDrop - 1 + kFitRun) % kFitRun;
}
// Fill's two windows: where the 200 shown rows start on the menu and
// in cutscenes, and where they start in play (see buildFillTable).
enum { kFillScreensTop = 12, kFillGameTop = 18 };

// the palette (STPalette, st_palette.cpp) is its own, shared with the
// experimental build
struct SystemStub_STDL : SystemStub_Atari, STPalette {
	STDL_Surface *_screen;
	STDL_Colour _hwShown[16];  // what the registers were last given
	int _srcW, _srcH;
	int _xOffset;           // horizontal centring, pixels (multiple of 16)
	uint8_t *_shadow;       // last converted chunky frame
	bool _shadowValid;
	uint8_t _yMap[kMaxSrcH];   // source line -> screen line
	uint16_t _yOff[kMaxSrcH];  // _yMap * the screen stride, see rowOffsets
	void rowOffsets();
	uint8_t _yDrop[kMaxSrcH];  // 1 = line not displayed
	bool _ovscOpen;            // top border open: display starts line 34
	bool _yLinear;             // no line dropped: rows step by a constant
	// double_buffer: frames go to the hidden page and a VBL-synced
	// flip shows them. What was written since the last flip is kept
	// as a mask of 16-pixel groups per screen row, and after the
	// flip those groups are copied from the page now shown to the
	// new hidden one, so the two pages never drift apart and every
	// caller can go on drawing as if there were only one.
	bool _dblBuf;
	uint32_t _dmg[kMaxScreenRows];
	int _dmgLo, _dmgHi;
	void addDamage(int row0, int row1, int g0, int g1);
	void damageSource(int y, int h, int g0, int g1);
	void clearScreen();
	void syncPages(const uint8_t *shown);

	virtual ~SystemStub_STDL() {}
	virtual void init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate);
	virtual void destroy();
	virtual void setScreenSize(int w, int h);
	virtual int setScreenMode(int mode);
	virtual int panScreen(int delta);
	virtual void useFillWindow(bool game);
	void buildFillTable();
	void applyFillTop(int top);
	int _mode;                 // the ScreenMode in effect
	int _fillTop;              // Fill: first of the 224 rows shown now
	int _fillTopGame;          // Fill: the row play starts from
	virtual void setPalette(const uint8_t *pal, int n);
	virtual void getPalette(uint8_t *pal, int n);
	virtual void setPaletteEntry(int i, const Color *c);
	virtual void getPaletteEntry(int i, Color *c);
	virtual void copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch);
	virtual void copyRectPlanar(int x, int y, int w, int h, const uint8_t *layer);
	virtual void fadeScreen();
	virtual void updateScreen(int shakeOffset);

	void writeHwPalette();
	void setHwColours(const STDL_Colour *c);
	void convertRegion(int x, int y, int w, int h, const uint8_t *buf, int pitch, bool useShadow);
	void reconvertFromShadow();
};

static SystemStub_STDL *g_stub;

SystemStub *SystemStub_STDL_create() {
	g_stub = new SystemStub_STDL();
	g_stpal = g_stub;
	return g_stub;
}

// Layer rows onto a squashed screen: each line's screen offset from
// the table, and the lines the squash drops skipped.
template <int N> static void copyMappedRows(uint8_t *base, const uint8_t *s, int y, int h, const uint16_t *yOff, const uint8_t *yDrop) {
	for (int j = y; j < y + h; ++j, s += kSTRowBytes) {
		if (!yDrop[j]) {
			copyRowN<N>((uint32_t *)(base + yOff[j]), (const uint32_t *)s);
		}
	}
}

// Copy a planar layer region to the screen: pure word moves with the
// 224->200 line squash and horizontal centring. x/w widen to 16px.
void SystemStub_STDL::copyRectPlanar(int x, int y, int w, int h, const uint8_t *layer) {
	if (_palDirty) {
		buildRemap();
	}
	if (_cutPending && !_dblBuf) {
		// A cutscene's cut: a new part's first page, under a new
		// mapping and new colours, onto a screen still showing the
		// last part. The two cannot change at the same instant -
		// the copy takes longer than the blanking, and on an STE
		// playing a module the VBL's mixing leaves the beam 30-45
		// lines into the picture before any copy can start - so
		// whichever goes first, a frame showed one picture in the
		// other's colours (the intro's corridor in brown, its
		// jungle in red). Black between them instead: the registers
		// go black, the page goes up, and its colours follow at the
		// next VBL. One black frame at a cut.
		_cutPending = false;
		STDL_Colour black[16];
		memset(black, 0, sizeof(black));
		// with a border open STDL holds palette writes for the next
		// VBL's blanking, so they go in before the wait (as below)
		if (!_ovscOpen) {
			STDL_WaitVBL();
		}
		setHwColours(black);
		if (_ovscOpen) {
			STDL_WaitVBL();
		}
		_hwDirty = false;               // not before the page
		copyRectPlanar(x, y, w, h, layer);
		if (!_ovscOpen) {
			STDL_WaitVBL();
		}
		writeHwPalette();
		if (_ovscOpen) {
			STDL_WaitVBL();
		}
		return;
	}
	const int gx0 = x >> 4;
	int gx1 = (x + w + 15) >> 4;
	if (gx1 > kSTLayerW / 16) {
		gx1 = kSTLayerW / 16;
	}
	if (y + h > kSTLayerH) {
		h = kSTLayerH - y;
	}
	const int groups = gx1 - gx0;
	if (groups <= 0) {
		return;
	}
	const int xOffBytes = (_xOffset >> 4) << 3;
	if (_dblBuf) {
		damageSource(y, h, gx0 + (_xOffset >> 4), gx1 + (_xOffset >> 4));
	}
	// Large copies to a hidden page (double buffering) go through
	// STDL_BlitSurface so a BLiTTER can take the aligned runs (same
	// 16px phase, unmasked; runs break where the 224->200 squash
	// drops a line). Small per-frame dirty runs stay on the word
	// loop below - measured faster than the per-call blit setup.
	// The layer is viewed maskless or the copy would key on the
	// priority plane.
	// The BLiTTER is used with a border open as well: STDL now runs
	// every blit in hog mode and places it from the beam so it ends
	// before a border ISR window (or splits it around one), so it
	// neither shares the bus nor delays the flip.
	// Never to the shown screen, though: the BLiTTER moves one plane
	// per pass, and the beam shows the page between passes - planes
	// 0-2 new over an old plane 3 are other colours. That put the
	// Delphine logo up in magenta and green for a frame before its
	// fade, and flashed patches of the intro red, on every STE and
	// Mega STE. The CPU puts a group's four planes down together.
	// Measured in one binary: STE gameplay and cutscenes unchanged,
	// Mega STE intro work +2.5%, level 1's opening +8%, both still
	// inside their budgets.
	const bool fullPage = h >= kSTLayerH - 8 && !_dblBuf;
	if (fullPage && (_ovscOpen || _hwDirty)) {
		// Beam race: with a border open the display starts
		// fetching at line 34 instead of 63, so a full-page copy
		// has to start at the VBL to stay ahead of it - unsynced
		// it loses intermittently and tears. This sync is not
		// negotiable against frame pacing: skipping it when a
		// scene runs late (tried Aug 2026) brought the tearing
		// straight back. A cutscene's smaller pushes sync through
		// ST_beamSync instead.
		//
		// A new page's colours go in at the same VBL, border or
		// not. Written after the copy (updateScreen), they reached
		// the screen up to a frame after the picture: a level's
		// first room showed in the colours put back from before its
		// opening cutscene, all or part of one frame depending on
		// where the copy met the VBL. With a border open STDL holds
		// the write for the next VBL's blanking, so it is made
		// before the wait; without one it goes straight to the
		// registers, so after it, in the blanking before the
		// picture starts.
		// Only a page that brings new colours waits without a
		// border: scene changes, not frames.
		if (_ovscOpen && _hwDirty) {
			writeHwPalette();
		}
		STDL_WaitVBL();
		if (_hwDirty) {
			writeHwPalette();
		}
	}
	if (_dblBuf && h >= 32 && groups >= 8 && _srcW == kSTLayerW
	    && STDL_GetMachineInfo()->has_blitter) {
		STDL_Surface *bare = ST_layerView((uint8_t *)layer, false);
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
				sr.w = (uint16_t)(groups << 4);
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
	const uint8_t *s = layer + y * kSTRowBytes + (gx0 << 3);
	uint8_t *base = _screen->pixels + xOffBytes + (gx0 << 3);
	// With every line displayed the destination row is a constant
	// step from the last, so the per-row address arithmetic is
	// hoisted out of the loop. These runs are small (a sprite's worth
	// of blocks) and were spending more time finding their rows than
	// copying them.
	if (_yLinear) {
		ST_copyGroupRows(base + _yOff[y], kScreenStride, s, kSTRowBytes, groups, h);
		return;
	}
	switch (groups) {                   // the widths ST_copyGroupRows takes
	case 1: copyMappedRows<2>(base, s, y, h, _yOff, _yDrop); return;
	case 2: copyMappedRows<4>(base, s, y, h, _yOff, _yDrop); return;
	case 3: copyMappedRows<6>(base, s, y, h, _yOff, _yDrop); return;
	case 4: copyMappedRows<8>(base, s, y, h, _yOff, _yDrop); return;
	case 5: copyMappedRows<10>(base, s, y, h, _yOff, _yDrop); return;
	case 6: copyMappedRows<12>(base, s, y, h, _yOff, _yDrop); return;
	case 7: copyMappedRows<14>(base, s, y, h, _yOff, _yDrop); return;
	case 8: copyMappedRows<16>(base, s, y, h, _yOff, _yDrop); return;
	case 9: copyMappedRows<18>(base, s, y, h, _yOff, _yDrop); return;
	case 10: copyMappedRows<20>(base, s, y, h, _yOff, _yDrop); return;
	case 11: copyMappedRows<22>(base, s, y, h, _yOff, _yDrop); return;
	case 12: copyMappedRows<24>(base, s, y, h, _yOff, _yDrop); return;
	case 13: copyMappedRows<26>(base, s, y, h, _yOff, _yDrop); return;
	case 14: copyMappedRows<28>(base, s, y, h, _yOff, _yDrop); return;
	case 15: copyMappedRows<30>(base, s, y, h, _yOff, _yDrop); return;
	case 16: copyMappedRows<32>(base, s, y, h, _yOff, _yDrop); return;   // the cutscene page flip
	}
}

void SystemStub_STDL::init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate) {
	resetColours();
	initMachine();
	_dblBuf = g_options.double_buffer;
	_dmgLo = kMaxScreenRows;
	_dmgHi = -1;
	memset(_dmg, 0, sizeof(_dmg));
	_screen = STDL_SetVideoMode(320, 200, 4, _dblBuf ? STDL_DOUBLEBUF : 0);
	if (!_screen) {
		error("STDL_SetVideoMode failed");
	}
	_shadow =(uint8_t *)malloc(kMaxSrcW * kMaxSrcH);
	_shadowValid = false;
	_fillTop = kFillScreensTop;
	_fillTopGame = kFillGameTop;
	// refresh_rate: 50 or 60, or else whatever the machine booted at,
	// which is then what the title's Options shows. Asked for before
	// any border opens; an open one holds 50Hz and the rate asked for
	// comes in when it closes.
	if (g_options.refresh_rate == 50 || g_options.refresh_rate == 60) {
		STDL_SetRefresh(g_options.refresh_rate);
	} else {
		g_options.refresh_rate = STDL_SetRefresh(-1);
	}
	// what the screen IS, which is what Options shows: overscan can be
	// out of reach (kOverscanMinK, a 60Hz display) whatever RS.CFG
	// asked for. The RAM goes in the log either way, for bug reports.
	logMachine();
	g_options.screen = setScreenMode(g_options.screen);
	setScreenSize(w, h);
	// black screen until the first frame arrives
	clearScreen();
	writeHwPalette();
	initJoystick();
}

void SystemStub_STDL::destroy() {
	free(_shadow);
	_shadow = 0;
	if (_ovscOpen) {
		// Close the borders before quitting rather than leaving it to
		// STDL_Quit's teardown. Both end by reseeding the Shifter's
		// plane phase - a sync pulse that ran into the next line
		// leaves it rotated, and on hardware the rotation survives
		// the program - but the close waits for the blanking first,
		// where the teardown reseeds wherever the beam happens to be.
		// The desktop came back with red/blue fringed icons after
		// quitting from the title screen on a real Mega STE.
		_ovscOpen = false;
		STDL_CloseBottomBorder();
		STDL_CloseTopBorder();
	}
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
		clearScreen();
	}
}

// 224 lines onto the ST's 200. Fill crops 12 lines off the top and
// bottom (every displayed line is intact and screen copies stay one
// contiguous run, but the edges of the playfield are hidden); Fit
// crops two at each end and drops every eleventh row between; Top opens the
// top border so all 224 display natively (50Hz screens only - a 60Hz
// picture already starts on the first possible line); Full opens both
// borders and centres the picture between them.
//
// Called once from init and again from the title menu, so it has to
// undo whatever the previous mode left open. Each border transition
// reshapes the screen surface, so the caller repaints afterwards.
// The nearest row to y on which an 8-pixel line of text loses nothing
// in Fit: the first three rows of a kept run, so the line ends inside
// it (and not the short runs either end, which are too short). The engine's fixed-row text - the continue screen, the score,
// a level code - was laid out for a screen that shows every row; on
// Fit most of it straddled a dropped one and came out ragged. The
// shift is at most five rows and is applied in every mode, so a
// screen looks the same whichever the player chose.
int ST_textRow(int y) {
	int best = y;
	int bestDist = kFitRun;
	for (int d = -(kFitRun / 2); d <= kFitRun / 2; ++d) {
		const int c = y + d;
		if (c < kFitFirstDrop || c + 8 > kMaxSrcH - kFitCrop) {
			continue;
		}
		const int dist = d < 0 ? -d : d;
		if (fitPhase(c) <= 2 && dist < bestDist) {
			best = c;
			bestDist = dist;
		}
	}
	return best;
}

// The least RAM, in K, on which the overscan modes are offered (0: on
// any machine). Overscan needs a screen page of its own, 44K of ST RAM,
// where Fit and Fill draw on the screen TOS already has. That page is
// what failed level 5 on a 2MB STE with music ("Unable to allocate SPM
// temporary buffer" in Overscan Full, 136K free in Fit). 2MB is below
// what the game supports, so the limit sits at 2.5MB and only gives an
// unsupported machine a better chance - on 2.5MB level 5 still has
// 463K free in Overscan Full. Raise it (4096 was the first thought) if
// optimising ever leaves a supported machine short; a real machine has
// about 70K less than the emulator, for its hard disk driver. Alt-RAM
// counts: the game's own allocations go there when a machine has some.
// Deliberately not in the docs or RS.CFG.template.
static const int kOverscanMinK = 2560;

bool ST_screenModeAvailable(int mode) {
	return (mode != kScreenTop && mode != kScreenFull) || ST_ramK() >= kOverscanMinK;
}

int SystemStub_STDL::setScreenMode(int mode) {
	if (_ovscOpen) {
		_ovscOpen = false;
		STDL_CloseBottomBorder();
		STDL_CloseTopBorder();
	}
	if (!ST_screenModeAvailable(mode)) {
		warning("Overscan needs %dK of RAM and this machine has %dK, so the screen is fit", kOverscanMinK, ST_ramK());
		mode = kScreenFit;
	}
	int ovscY = 0;   // screen row the first source line goes to
	if (mode == kScreenTop || mode == kScreenFull) {
		const int topH = STDL_OpenTopBorder();
		if (topH != 0) {
			_ovscOpen = true;
			// Both borders (273 rows) centre the picture: the
			// 224 lines sit in the middle with black bands above
			// and below, instead of hanging off the top of the
			// tube. Costs about 2.5% of an 8MHz frame against
			// 1.4% for the top border alone.
			const int both = (mode == kScreenFull) ? STDL_OpenBottomBorder() : 0;
			if (both != 0) {
				ovscY = (both - kMaxSrcH) / 2;
				info("Both borders open: %d lines, picture centred at row %d", both, ovscY);
			} else {
				// Top border alone: sit the picture on the bottom
				// of the screen rather than the top. The first two
				// or three displayed lines come up shifted a few
				// pixels left while the Shifter settles after the
				// sync-rate switch, and the spare rows are the only
				// place to put that where nobody sees it.
				ovscY = topH - kMaxSrcH;
				if (ovscY < 0) {
					ovscY = 0;
				}
				if (mode == kScreenFull) {
					warning("Bottom border unavailable (%s), top border only",
						STDL_GetError());
					mode = kScreenTop;
				}
				info("Top border only: %d lines, picture at row %d", topH, ovscY);
			}
		} else {
			warning("Overscan unavailable (%s), using fit", STDL_GetError());
			mode = kScreenFit;
		}
	}
	if (_ovscOpen) {
		for (int y = 0; y < kMaxSrcH; ++y) {
			_yDrop[y] = 0;
			_yMap[y] = (uint8_t)(y + ovscY);
		}
	} else if (mode == kScreenFill) {
		buildFillTable();
	} else {
		// Fit: two rows cropped at each end, then every eleventh of
		// the 220 between dropped - twenty of them, 200 rows exactly,
		// placed as kFitFirstDrop explains.
		// The old dst = y * 25 / 28 lost the same twenty-four lines
		// but in an 8/8/9 rhythm, and it is the rhythm that shows:
		// edges wobble where the gap changes length. Regular ten-row
		// runs read as a faint even texture instead, and an 8-pixel
		// glyph placed on the run phase (see the title menu) stays
		// whole. A dropped row maps to the last row that was kept,
		// the same convention the squash used.
		int d = 0;
		for (int y = 0; y < kMaxSrcH; ++y) {
			const bool cropped = (y < kFitCrop) || (y >= kMaxSrcH - kFitCrop);
			const bool dropped = !cropped && fitPhase(y) == kFitRun - 1;
			if (cropped || dropped) {
				_yDrop[y] = 1;
				_yMap[y] = (uint8_t)(d ? d - 1 : 0);
			} else {
				_yDrop[y] = 0;
				_yMap[y] = (uint8_t)d++;
			}
		}
	}
	_yLinear = true;
	for (int y = 0; y < kMaxSrcH; ++y) {
		if (_yDrop[y]) {
			_yLinear = false;
			break;
		}
	}
	// The surface may have changed shape and the shadow no longer
	// describes what is on it: black until the caller repaints.
	rowOffsets();
	_shadowValid = false;
	clearScreen();
	writeHwPalette();
	_mode = mode;
	return mode;
}

// Each source row's byte offset on the screen, kept with _yMap: the
// copies below read it once a row where they multiplied the mapped
// row by the stride, most of a sprite-sized copy's per-row overhead.
void SystemStub_STDL::rowOffsets() {
	for (int y = 0; y < kMaxSrcH; ++y) {
		_yOff[y] = (uint16_t)(_yMap[y] * kScreenStride);
	}
}

// Fill shows 200 of the 224 rows, from _fillTop down - and where that
// window sits depends on what is being shown. In play it starts at
// row 18, six from the bottom: the rooms put their floors and lower
// platforms in the last rows, and a centred window lost the whole
// bottom platform of the first room, while the top rows are canopy.
// The menu and the cutscenes were laid out for a centred window and
// keep one. The pan (Ctrl+Up/Down in play) moves only the play window
// and is there for trying other positions.
void SystemStub_STDL::buildFillTable() {
	for (int y = 0; y < kMaxSrcH; ++y) {
		const bool off = (y < _fillTop) || (y >= _fillTop + 200);
		_yDrop[y] = off ? 1 : 0;
		_yMap[y] = off ? 0 : (uint8_t)(y - _fillTop);
	}
}

void SystemStub_STDL::applyFillTop(int top) {
	if (top == _fillTop) {
		return;
	}
	_fillTop = top;
	if (_mode == kScreenFill) {
		buildFillTable();
		rowOffsets();
		_shadowValid = false;
		clearScreen();
	}
}

void SystemStub_STDL::useFillWindow(bool game) {
	applyFillTop(game ? _fillTopGame : kFillScreensTop);
}

int SystemStub_STDL::panScreen(int delta) {
	if (_mode != kScreenFill) {
		return -1;
	}
	int top = _fillTopGame + delta;
	if (top < 0) {
		top = 0;
	} else if (top > kMaxSrcH - 200) {
		top = kMaxSrcH - 200;
	}
	_fillTopGame = top;
	applyFillTop(top);
	return _fillTopGame;
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

bool ST_rowVisible(int y) {
	if (y < 0 || y >= kMaxSrcH) {
		return false;
	}
	return g_stub->_yDrop[y] == 0;
}


// the registers' sixteen colours, and what they were given kept for
// ST_hwPaletteHash
void SystemStub_STDL::setHwColours(const STDL_Colour *c) {
	STDL_SetColours(_screen, c, 0, 16);
	memcpy(_hwShown, c, sizeof(_hwShown));
}

void SystemStub_STDL::writeHwPalette() {
	STDL_Colour c[16];
	fadedColours(c);
	setHwColours(c);
	_hwDirty = false;
}

// the colours the screen shows, folded: with the pixel hash, the check
// that a frame looks exactly as it did (see Cutscene::stStatsEnd)
uint32_t ST_hwPaletteHash() {
	uint32_t h = 0x811C9DC5;
	for (int i = 0; i < 16; ++i) {
		const STDL_Colour &c = g_stub->_hwShown[i];
		h = ST_rol5(h) ^ (((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b);
	}
	return h;
}

// The frame hashes are of the layers and pages, so a box a push left
// out changed none of them: this compares the screen itself with the
// layer it was last pushed from, over the rows the mode displays.
int ST_screenDiffRow(const uint8_t *layer, int y0) {
	const SystemStub_STDL *s = g_stub;
	const uint8_t *base = s->_screen->pixels + ((s->_xOffset >> 4) << 3);
	for (int y = y0; y < kSTLayerH; ++y) {
		if (!s->_yDrop[y] && memcmp(base + s->_yOff[y], layer + y * kSTRowBytes, kSTRowBytes) != 0) {
			return y;
		}
	}
	return -1;
}

// Chunky to planar by table: g_c2p[c] holds the four plane bits of
// the slot colour c maps to, one at the bottom of each byte with
// plane 0 in the top one, so eight pixels shift into a long whose
// bytes are their plane bytes, and movep lays those into the four
// plane words. Building each word a bit at a time cost ~200 cycles a
// pixel: a whole 320x224 screen took 1.8s on an 8MHz ST, and the
// title's black clear and its wipe were seconds of its delay.
static uint32_t g_c2p[256];
static uint32_t g_c2pRemap[64];         // the remap g_c2p was built from
static bool g_c2pValid;

static const uint32_t *c2pTable(const uint8_t *remap) {
	const uint32_t *r = (const uint32_t *)remap;
	if (g_c2pValid) {
		int i = 0;
		while (i < 64 && r[i] == g_c2pRemap[i]) {
			++i;
		}
		if (i == 64) {
			return g_c2p;
		}
	}
	for (int i = 0; i < 64; ++i) {
		g_c2pRemap[i] = r[i];
	}
	for (int c = 0; c < 256; ++c) {
		const uint32_t v = remap[c];
		g_c2p[c] = ((v & 1) << 24) | ((v & 2) << 15) | ((v & 4) << 6) | ((v >> 3) & 1);
	}
	g_c2pValid = true;
	return g_c2p;
}

// hi's bytes are a group's four plane bytes for its left eight
// pixels, lo's for its right eight
static inline void storePlanes(uint16_t *dst, uint32_t hi, uint32_t lo) {
	__asm__ volatile("movep.l %1,0(%0)\n\tmovep.l %2,1(%0)" : : "a"(dst), "d"(hi), "d"(lo) : "memory");
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
	const uint32_t *tab = c2pTable(_remap);
	if (_dblBuf && h > 0) {
		damageSource(y, h, (x0 + _xOffset) >> 4, (x1 + _xOffset) >> 4);
	}
	for (int j = 0; j < h; ++j) {
		const int sy = y + j;
		if (_yDrop[sy]) {
			continue;
		}
		const uint8_t *src = buf + sy * pitch + x0;
		uint32_t *sh = (uint32_t *)(_shadow + sy * kMaxSrcW + x0);
		uint16_t *dst = (uint16_t *)(_screen->pixels + _yOff[sy] + ((x0 + _xOffset) >> 4) * 8);
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
			const uint32_t w0 = s32[0];
			sh[0] = w0;
			sh[1] = s32[1];
			sh[2] = s32[2];
			sh[3] = s32[3];
			uint32_t hi, lo;
			if (w0 == s32[1] && w0 == s32[2] && w0 == s32[3] && ((w0 << 8) | (w0 >> 24)) == w0) {
				// sixteen pixels of one colour (a black clear, the
				// title's surround): each plane byte is all set or
				// clear, and a byte of 1 times 255 is 0xFF
				const uint32_t u = tab[w0 & 0xFF];
				hi = lo = (u << 8) - u;
			} else {
				hi = lo = 0;
				for (int i = 0; i < 8; ++i) {
					hi = (hi << 1) | tab[src[i]];
					lo = (lo << 1) | tab[src[i + 8]];
				}
			}
			storePlanes(dst, hi, lo);
			src += 16;
			sh += 4;
			dst += 4;
		}
	}
}

void ST_reconvertScreen() {
	g_stub->reconvertFromShadow();
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

// A cutscene push with a border open: a box over half the page races
// the beam like a full copy. (A full one syncs in copyRectPlanar.)
void ST_beamSync() {
	if (g_stub->_ovscOpen && !g_stub->_dblBuf) {
		STDL_WaitVBL();
	}
}

uint32_t ST_overscanMisses() {
	return STDL_OverscanMisses();
}

void SystemStub_STDL::updateScreen(int shakeOffset) {
	if (_palDirty) {
		buildRemap();
	}
	if (_dblBuf) {
		// the palette goes in after the flip, at the top of the
		// frame it belongs to: written first, it would recolour the
		// old picture until the VBL
		const uint8_t *shown = _screen->pixels;
		STDL_Flip();
		if (_hwDirty) {
			writeHwPalette();
		}
		if (_screen->pixels != shown) {
			syncPages(shown);
		}
		return;
	}
	if (_hwDirty) {
		writeHwPalette();
	}
}

// Source rows [y, y + h) were copied to the hidden page: damage the
// screen rows they landed on. Not simply _yMap[y].._yMap[y + h - 1]:
// Fill maps the rows outside its window to row 0, so a copy running
// past the window would come out as a backwards range and damage
// nothing.
void SystemStub_STDL::damageSource(int y, int h, int g0, int g1) {
	int r0 = -1, r1 = -1;
	for (int sy = y; sy < y + h && sy < kMaxSrcH; ++sy) {
		if (!_yDrop[sy]) {
			if (r0 < 0) {
				r0 = _yMap[sy];
			}
			r1 = _yMap[sy];
		}
	}
	if (r0 >= 0) {
		addDamage(r0, r1, g0, g1);
	}
}

// groups [g0, g1) of rows [row0, row1] were written on the hidden page
void SystemStub_STDL::addDamage(int row0, int row1, int g0, int g1) {
	if (row0 < 0) {
		row0 = 0;
	}
	if (row1 >= _screen->h) {
		row1 = _screen->h - 1;
	}
	if (g1 > kScreenStride / 8) {
		g1 = kScreenStride / 8;
	}
	if (row1 < row0 || g1 <= g0) {
		return;
	}
	const uint32_t m = ((1UL << (g1 - g0)) - 1) << g0;
	for (int r = row0; r <= row1; ++r) {
		_dmg[r] |= m;
	}
	if (row0 < _dmgLo) {
		_dmgLo = row0;
	}
	if (row1 > _dmgHi) {
		_dmgHi = row1;
	}
}

void SystemStub_STDL::clearScreen() {
	memset(_screen->pixels, 0, kScreenStride * _screen->h);
	if (_dblBuf) {
		addDamage(0, _screen->h - 1, 0, kScreenStride / 8);
	}
}

// Bring the new hidden page up to the one just shown: copy what the
// last frame wrote, group runs at a time, whole rows through movem.
void SystemStub_STDL::syncPages(const uint8_t *shown) {
	const int hi = (_dmgHi < _screen->h) ? _dmgHi : _screen->h - 1;
	const uint32_t kAll = (1UL << (kScreenStride / 8)) - 1;
	for (int r = _dmgLo; r <= hi; ++r) {
		uint32_t m = _dmg[r];
		if (m == 0) {
			continue;
		}
		_dmg[r] = 0;
		const uint32_t *s = (const uint32_t *)(shown + r * kScreenStride);
		uint32_t *d = (uint32_t *)(_screen->pixels + r * kScreenStride);
		if (m == kAll) {
			copyRowN<32>(d, s);
			d += kSTRowBytes / 4;
			s += kSTRowBytes / 4;
			for (int n = (kScreenStride - kSTRowBytes) / 4; --n >= 0; ) {
				*d++ = *s++;
			}
			continue;
		}
		for (int g = 0; m != 0; ++g, m >>= 1) {
			if (m & 1) {
				d[g * 2] = s[g * 2];
				d[g * 2 + 1] = s[g * 2 + 1];
			}
		}
	}
	for (int r = hi + 1; r < kMaxScreenRows && r <= _dmgHi; ++r) {
		_dmg[r] = 0;
	}
	_dmgLo = kMaxScreenRows;
	_dmgHi = -1;
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
		setHwColours(c);
		STDL_Delay(40);
	}
	clearScreen();
	_shadowValid = false;
	_hwDirty = true;
}
