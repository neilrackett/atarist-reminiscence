
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The 030 build's platform layer (FBACK030.TOS). A TT or a Falcon shows
 * upstream's 256x224 chunky picture in 256 colours on STDL's responsive
 * screen (stdl_responsive.h): 320x240 in 8 interleaved planes, TT Low on
 * a TT, a Falcon's 8-bit mode on VGA, RGB or a TV, with its desktop back
 * at every exit, a crash's included. The keyboard, joystick and pads,
 * the clock, the sound and the machine set-up are the ST build's, shared
 * (systemstub_atari.h).
 *
 * The screen has three pages, or two (screen_pages=2, or where the
 * third does not fit), and each keeps its own picture. The engine pushes
 * boxes of its picture as if to one screen that keeps them all, so each
 * page owes every box pushed since it was last drawn on: a mask of
 * 16-pixel groups a row, the frame's boxes ORed into the other pages'
 * once a frame (into all of them at each push, a Falcon's replays took
 * 1.2ms a frame more, 47.21 against 46.03: play pushes a box for each
 * run of blocks of each band). Of what
 * the page drawn on owes, this frame's boxes are converted from the
 * engine's picture, and the rest copied from the page last put up,
 * which holds the screen as it stands. So the stub is one screen to the
 * engine, whatever the engine does with a picture between its pushes:
 * the title pushes its buffer black, decodes the picture into it and
 * reveals that a few rows a step, and converting what the other pages
 * owed from the buffer put the whole picture on them, every other frame
 * of the wipe. A frame that pushes nothing does not flip.
 *
 * Each box is converted straight into the page (c2p_030.h): on a TT
 * both of a row's lines, half the stride apart, so Flip has nothing to
 * copy; on a Falcon the page is the display. Rows and placement are as
 * the 030 build's own screen had them, so the screen's memory is the
 * same, byte for byte.
 *
 * Register 0 is black, everywhere. It is the colour of the border round
 * the screen memory, which nothing drawn can reach (a Falcon's on an RGB
 * monitor, outside the overscan picture), and the game's colour 0 is a
 * room's background: the jungle had a teal border. So every pixel is
 * XORed with kSwap as it is converted, and the palette goes to the
 * registers permuted to match, which moves colour 0 to register kSwap
 * and leaves register 0 for slot 7's first entry, which the Amiga data
 * never loads or draws in (Video::AMIGA_setLevelPalettes); it is held
 * black. The frame round the picture is register 0 too. The colours are
 * staged (STDL_StagePalette), set just before Flip, so each set is shown
 * with the page it came with.
 */

extern "C" {
#include <stdl/stdl_responsive.h>
}
#include "systemstub_atari.h"
#include "util.h"
#include "video_st.h"
#include "c2p_030.h"

enum {
	kFrameW = 320,
	kFrameH = 240,
	kMaxPages = 3,
	// what every pixel is XORed with on its way to the screen (see the
	// top of the file): colour 0 to register 0x70, slot 7 to 0-15. The
	// conversion has it built in (c2p_030.h).
	kSwap = 0x70
};

struct SystemStub_030 : SystemStub_Atari {
	int _srcW, _srcH;          // the picture the engine sends
	int _groups;               // its width in 16-pixel groups
	STDL_Surface *_screen;     // STDL's; its pixels the page drawn on
	int _dbl;                  // 1 on a TT: the conversion writes both of a row's lines
	int _margin;               // bytes of a line before the picture (a Falcon's overscan)
	Color _cols[256];          // what the engine asked for
	bool _colsDirty;           // not yet staged
	int _xOff, _yOff;          // the picture's place on the screen
	// The pages, as each is first drawn on, and what each owes.
	uint8_t *_page[kMaxPages];
	uint32_t _owed[kMaxPages][kFrameH];
	bool _clear[kMaxPages];
	// this frame's boxes: all of them, and those not yet converted (a
	// push ORs into the second alone; the other pages' masks take them
	// once a frame)
	uint32_t _now[kFrameH];
	uint32_t _conv[kFrameH];
	const uint8_t *_buf;       // the picture this frame's boxes come from
	int _pitch;
	bool _pushed;
	uint8_t *_shown;           // the page Flip was last handed
	// the colours as the registers get them: a TT's 12-bit words, a
	// Falcon's 0x00RRGGBB, as this build's own screen wrote them, for
	// ST_hwPaletteHash
	union {
		uint16_t tt[256];
		uint32_t f30[256];
	} _hw;
	bool _tt;

	virtual void init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate);
	virtual void destroy();
	virtual void setScreenSize(int w, int h);
	virtual int setScreenMode(int mode) { return mode; }
	virtual int panScreen(int delta) { return -1; }
	virtual void useFillWindow(bool game) {}
	virtual void setPalette(const uint8_t *pal, int n);
	virtual void getPalette(uint8_t *pal, int n);
	virtual void setPaletteEntry(int i, const Color *c);
	virtual void getPaletteEntry(int i, Color *c);
	virtual void copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch);
	virtual void fadeScreen();
	virtual void updateScreen(int shakeOffset);

	void clearScreen();
	int pageIndex();
	uint8_t *drawPage(int *p);
	void convert(uint32_t *mask, uint8_t *pix);
	void takeConv();
	void copyOwed(int p, uint8_t *pix);
	void setColours(int step);
	void checkScreen(const uint8_t *pix);
};

static SystemStub_030 *g_stub;

SystemStub *SystemStub_030_create() {
	g_stub = new SystemStub_030();
	return g_stub;
}

// With logging on: the conversion's assembler against its C twin on
// this machine, three rows of random pixels each way, said in the log.
static void checkPlanes() {
	enum { kGroups = 20, kRow = kGroups * 16, kRows = 3, kOut = 2 * kRows * kRow };
	uint8_t *buf = (uint8_t *)malloc(kRows * kRow + 2 * kOut);
	if (!buf) {
		return;
	}
	uint8_t *src = buf, *a = buf + kRows * kRow, *c = a + kOut;
	uint32_t seed = 0x12345678UL;
	for (int i = 0; i < kRows * kRow; ++i) {
		seed = seed * 1103515245UL + 12345UL;
		src[i] = (uint8_t)(seed >> 16);
	}
	int bad = 0;
	for (int dbl = 0; dbl <= 1; ++dbl) {
		memset(a, 0, kOut);
		memset(c, 0, kOut);
		c2p8(src, kRow, a, kRow, kGroups, kRows, dbl);
		if (dbl) {
			c2p8rowsC<1>(src, kRow, c, kRow, kGroups, kRows);
		} else {
			c2p8rowsC<0>(src, kRow, c, kRow, kGroups, kRows);
		}
		for (int i = 0; i < kOut; ++i) {
			if (a[i] != c[i]) {
				++bad;
			}
		}
	}
	free(buf);
	if (bad) {
		warning("Planes: the assembler and its C twin differ in %d bytes", bad);
	} else {
		info("Planes: the assembler matches its C twin");
	}
}

// replay_hash: every page, before it goes up, against a chunky model of
// the one screen the engine pushed to, converted (C twin) as a page
static uint8_t *g_shadow;              // 320x240, a byte a pixel
static uint8_t *g_shadowPage;          // the model converted
static uint32_t g_scrFlips, g_scrBad, g_scrBadFlip;
static int g_scrBadRow;

// Flip's time, the conversion's apart, in 1/38400s (STDL_GetHz38400)
static uint32_t g_flipTicks, g_convTicks;

void SystemStub_030::init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate) {
	initMachine();
	logMachine();
	_tt = STDL_GetMachineInfo()->is_tt;
	// 320x240 in 256 colours, on a TT or a Falcon; STDL takes the
	// keyboard with the mode, and puts the desktop back at any exit
	STDL_UseResponsive(STDL_RESPONSIVE_COLOUR | STDL_RESPONSIVE_HEIGHT);
	// Three pages unless screen_pages=2: with two, Flip waits for the
	// VBL that shows its page, and a 60Hz display holds a frame of a
	// Falcon's 45ms to three VBLs, 50ms, past the 30Hz step's deadline
	// (a replay ran at 53.9ms a frame against 45.6 with three). STDL
	// gives two where the third page does not fit.
	const bool three = g_options.screen_pages != 2;
	_screen = STDL_SetVideoMode(kFrameW, kFrameH, 8, three ? STDL_TRIPLEBUF : STDL_DOUBLEBUF);
	if (!_screen || _screen->format->BitsPerPixel != 8 || _screen->h != kFrameH) {
		error("FBACK030.TOS needs a TT or a Falcon with a colour monitor: %s", STDL_GetError());
	}
	if (three && !(_screen->flags & STDL_TRIPLEBUF)) {
		warning("Screen: two pages, %s", STDL_GetError());
	}
	if (STDL_StagePalette(1) != 0) {
		warning("Screen: colours not staged, %s", STDL_GetError());
	}
	// a TT's stride is two display lines; a Falcon's overscan line has
	// the picture 32 bytes in
	_dbl = (_screen->stride == 2 * kFrameW) ? 1 : 0;
	_margin = (_screen->stride == 384) ? 32 : 0;
	if (g_options.logging) {
		info("Screen: %s, %d bytes a line, %d pages%s", _tt ? "TT Low" : "Falcon, 8 bits", _screen->stride >> _dbl,
			(_screen->flags & STDL_TRIPLEBUF) ? 3 : 2, _dbl ? ", rows doubled" : "");
		checkPlanes();
	}
	if (g_options.replay_hash) {
		g_shadow = (uint8_t *)malloc(kFrameW * kFrameH);
		g_shadowPage = (uint8_t *)malloc(_screen->stride * kFrameH);
		if (!g_shadow || !g_shadowPage) {
			error("No memory for the screen check");
		}
		memset(g_shadow, kSwap, kFrameW * kFrameH);
	}
	memset(_page, 0, sizeof(_page));
	memset(_owed, 0, sizeof(_owed));
	memset(_clear, 0, sizeof(_clear));
	memset(_now, 0, sizeof(_now));
	memset(_conv, 0, sizeof(_conv));
	memset(_cols, 0, sizeof(_cols));
	_colsDirty = true;
	_pushed = false;
	_buf = 0;
	_shown = 0;
	_srcW = _srcH = 0;
	setScreenSize(w, h);
	initJoystick();
}

void SystemStub_030::destroy() {
	if (g_options.replay_hash) {
		if (g_scrBad == 0) {
			info("SCREEN check: %u flips, every page as its pushes", (unsigned)g_scrFlips);
		} else {
			info("SCREEN check: %u of %u flips unlike their pushes, the first %u at row %d",
				(unsigned)g_scrBad, (unsigned)g_scrFlips, (unsigned)g_scrBadFlip, g_scrBadRow);
		}
	}
	STDL_Quit();
}

void SystemStub_030::setScreenSize(int w, int h) {
	if (w > kFrameW) {
		w = kFrameW;
	}
	if (h > kFrameH) {
		h = kFrameH;
	}
	if (w != _srcW || h != _srcH) {
		_srcW = w;
		_srcH = h;
		_groups = w >> 4;
		// whole groups: the picture's 16-pixel groups are the screen's
		_xOff = ((kFrameW - w) / 2) & ~15;
		_yOff = (kFrameH - h) / 2;
		clearScreen();
	}
}

// Register 0 everywhere: the frame round the picture, and the picture
// until the engine sends it. The page up now goes at once, as the one
// screen did; the others when next drawn on.
void SystemStub_030::clearScreen() {
	for (int p = 0; p < kMaxPages; ++p) {
		if (_page[p] && _page[p] == _shown) {
			memset(_shown - _margin, 0, (long)_screen->stride * kFrameH);
			_clear[p] = false;
		} else {
			_clear[p] = true;
		}
		memset(_owed[p], 0, sizeof(_owed[p]));
	}
	memset(_now, 0, sizeof(_now));
	memset(_conv, 0, sizeof(_conv));
	_pushed = false;
	if (g_shadow) {
		memset(g_shadow, kSwap, kFrameW * kFrameH);
	}
}

void SystemStub_030::setPalette(const uint8_t *pal, int n) {
	for (int i = 0; i < n && i < 256; ++i, pal += 3) {
		_cols[i].r = pal[0];
		_cols[i].g = pal[1];
		_cols[i].b = pal[2];
	}
	_colsDirty = true;
}

void SystemStub_030::getPalette(uint8_t *pal, int n) {
	for (int i = 0; i < n && i < 256; ++i, pal += 3) {
		pal[0] = _cols[i].r;
		pal[1] = _cols[i].g;
		pal[2] = _cols[i].b;
	}
}

void SystemStub_030::setPaletteEntry(int i, const Color *c) {
	_cols[i] = *c;
	_colsDirty = true;
}

void SystemStub_030::getPaletteEntry(int i, Color *c) {
	*c = _cols[i];
}

void SystemStub_030::copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch) {
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > _srcW) {
		w = _srcW - x;
	}
	if (y + h > _srcH) {
		h = _srcH - y;
	}
	if (w <= 0 || h <= 0) {
		return;
	}
	// a frame's boxes from a second picture: the first's go into the
	// page drawn on now, before that picture can change
	if (_pushed && (buf != _buf || pitch != _pitch)) {
		int p;
		takeConv();
		convert(_conv, drawPage(&p));
	}
	_buf = buf;
	_pitch = pitch;
	_pushed = true;
	// the groups the box touches: the picture's width is whole groups
	// and its place on the screen is on one, so they are the source's too
	const int g0 = x >> 4, g1 = (x + w + 15) >> 4;
	const uint32_t m = (g1 >= 32 ? ~0UL : ((1UL << g1) - 1)) & ~((1UL << g0) - 1);
	for (int r = y; r < y + h; ++r) {
		_conv[r] |= m;
	}
	if (g_shadow) {
		for (int r = y; r < y + h; ++r) {
			memcpy(g_shadow + (_yOff + r) * kFrameW + _xOff + g0 * 16, buf + r * pitch + g0 * 16, (g1 - g0) * 16);
		}
	}
}

// the slot of the page drawn on, learnt as each comes round
int SystemStub_030::pageIndex() {
	uint8_t *pix = (uint8_t *)_screen->pixels;
	for (int p = 0; p < kMaxPages; ++p) {
		if (_page[p] == pix) {
			return p;
		}
		if (!_page[p]) {
			_page[p] = pix;
			return p;
		}
	}
	return 0;
}

// The page drawn on, cleared first if it is to be
uint8_t *SystemStub_030::drawPage(int *p) {
	*p = pageIndex();
	uint8_t *pix = (uint8_t *)_screen->pixels;
	if (_clear[*p]) {
		memset(pix - _margin, 0, (long)_screen->stride * kFrameH);
		_clear[*p] = false;
	}
	return pix;
}

// The groups a mask names, from this frame's picture, the mask cleared:
// each run of rows with the same groups, a run of groups at a time
void SystemStub_030::convert(uint32_t *mask, uint8_t *pix) {
	const int stride = _screen->stride, dp = stride >> _dbl;
	uint8_t *row0 = pix + _yOff * stride + _xOff;
	for (int y = 0; y < _srcH; ) {
		const uint32_t m = mask[y];
		if (m == 0) {
			++y;
			continue;
		}
		int y1 = y + 1;
		while (y1 < _srcH && mask[y1] == m) {
			++y1;
		}
		for (int g = 0; g < _groups; ) {
			if (!(m & (1UL << g))) {
				++g;
				continue;
			}
			int g1 = g + 1;
			while (g1 < _groups && (m & (1UL << g1))) {
				++g1;
			}
			c2p8(_buf + y * _pitch + g * 16, _pitch, row0 + y * stride + g * 16, dp, g1 - g, y1 - y, _dbl);
			g = g1;
		}
		for (int k = y; k < y1; ++k) {
			mask[k] = 0;
		}
		y = y1;
	}
}

// the boxes waiting to be converted, among this frame's
void SystemStub_030::takeConv() {
	for (int y = 0; y < _srcH; ++y) {
		_now[y] |= _conv[y];
	}
}

// What page p owes from earlier frames, from the page last put up: it
// holds the screen as it stands there, as nothing pushed it since.
// Whole groups as longs; on a TT a row's first line, written to both of
// its lines as the conversion writes them (a TT's replays 24.41ms a
// frame of work, against 24.54 reading both).
void SystemStub_030::copyOwed(int p, uint8_t *pix) {
	const uint8_t *from = _shown;
	if (!from || from == pix) {
		return;
	}
	const int stride = _screen->stride, odd = (stride >> 1) >> 2;
	const long off0 = (long)_yOff * stride + _xOff;
	for (int y = 0; y < _srcH; ++y) {
		const uint32_t m = _owed[p][y] & ~_now[y];
		if (m == 0) {
			continue;
		}
		for (int g = 0; g < _groups; ) {
			if (!(m & (1UL << g))) {
				++g;
				continue;
			}
			int g1 = g + 1;
			while (g1 < _groups && (m & (1UL << g1))) {
				++g1;
			}
			const long off = off0 + (long)y * stride + g * 16;
			uint32_t *d = (uint32_t *)(pix + off);
			const uint32_t *s = (const uint32_t *)(from + off);
			if (_dbl) {
				for (int n = (g1 - g) * 4; --n >= 0; ++d) {
					d[0] = d[odd] = *s++;
				}
			} else {
				for (int n = (g1 - g) * 4; --n >= 0; ) {
					*d++ = *s++;
				}
			}
			g = g1;
		}
	}
}

// The engine's colours, staged for the next blank: permuted as the
// pixels are (kSwap), register 0 black, scaled by step eighths. A TT's
// registers take 4 bits a gun, a Falcon's 6.
void SystemStub_030::setColours(int step) {
	STDL_Colour c[256];
	for (int i = 0; i < 256; ++i) {
		const Color &s = _cols[i ^ kSwap];
		c[i].r = (uint8_t)(s.r * step >> 3);
		c[i].g = (uint8_t)(s.g * step >> 3);
		c[i].b = (uint8_t)(s.b * step >> 3);
		c[i].unused = 0;
		if (_tt) {
			_hw.tt[i] = (uint16_t)(((s.r * step >> 7) << 8) | ((s.g * step >> 7) << 4) | (s.b * step >> 7));
		} else {
			_hw.f30[i] = ((uint32_t)c[i].r << 16) | ((uint32_t)c[i].g << 8) | c[i].b;
		}
	}
	c[0].r = c[0].g = c[0].b = 0;
	if (_tt) {
		_hw.tt[0] = 0;
	} else {
		_hw.f30[0] = 0;
	}
	STDL_SetColours(_screen, c, 0, 256);
}

// replay_hash: the page about to go up against the model of the screen
void SystemStub_030::checkScreen(const uint8_t *pix) {
	const int stride = _screen->stride;
	memset(g_shadowPage, 0, (long)stride * kFrameH);
	if (_dbl) {
		c2p8rowsC<1>(g_shadow, kFrameW, g_shadowPage, stride >> 1, kFrameW >> 4, kFrameH);
	} else {
		c2p8rowsC<0>(g_shadow, kFrameW, g_shadowPage + _margin, stride, kFrameW >> 4, kFrameH);
	}
	const uint8_t *page = pix - _margin;
	for (int y = 0; y < kFrameH; ++y) {
		if (memcmp(page + (long)y * stride, g_shadowPage + (long)y * stride, stride) != 0) {
			if (g_scrBad++ == 0) {
				g_scrBadFlip = g_scrFlips;
				g_scrBadRow = y;
			}
			break;
		}
	}
	++g_scrFlips;
}

void SystemStub_030::updateScreen(int shakeOffset) {
	if (!_pushed) {
		// the colours alone, on the page up now: staged, the next blank
		if (_colsDirty) {
			setColours(8);
			_colsDirty = false;
		}
		return;
	}
	const uint32_t t0 = STDL_GetHz38400();
	int p;
	uint8_t *pix = drawPage(&p);
	takeConv();
	copyOwed(p, pix);
	convert(_conv, pix);
	// this frame's boxes owed by the other pages, none by this one
	for (int q = 0; q < kMaxPages; ++q) {
		if (q != p) {
			uint32_t *o = _owed[q];
			for (int y = 0; y < _srcH; ++y) {
				o[y] |= _now[y];
			}
		}
	}
	memset(_owed[p], 0, sizeof(_owed[p]));
	memset(_now, 0, sizeof(_now));
	if (g_shadow) {
		checkScreen(pix);
	}
	// set just before Flip, staged: shown with this page
	if (_colsDirty) {
		setColours(8);
		_colsDirty = false;
	}
	const uint32_t t1 = STDL_GetHz38400();
	_pushed = false;
	_shown = pix;
	STDL_Flip();
	g_convTicks += t1 - t0;
	g_flipTicks += STDL_GetHz38400() - t1;
}

// Fade to black on the palette alone, as the ST does: the next
// updateScreen puts the colours back, and the engine redraws first.
void SystemStub_030::fadeScreen() {
	for (int step = 7; step >= 0; --step) {
		setColours(step);
		STDL_Delay(40);
	}
	clearScreen();
	_colsDirty = true;
}

// The counters the ST build's log lines carry (video_st.cpp keeps them
// there). Of its sprite path the 030 build has none, so what it counts
// is the rooms decoded (video.cpp) and the banks and monsters loaded
// (resource.cpp, game.cpp).
STPerf g_stPerf;

void ST_perfFormat(const STPerf &p, char *buf, int len) {
	snprintf(buf, len, "room %u mon %u bank %u/%u/%luK",
		(unsigned)p.roomDecodes, (unsigned)p.monsterLoads,
		(unsigned)p.bankLoads, (unsigned)p.bankClears, (unsigned long)(p.bankBytes >> 10));
}

// What a frame put up (replay_hash, Cutscene::updateScreen and the
// replays): the page Flip was last handed, the whole of its memory, and
// the colours as the registers got them, folded h * 33 + each long as
// the ST build's page hash is
uint32_t ST_screenHash() {
	if (!g_stub->_shown) {
		return 0;
	}
	const uint32_t *p = (const uint32_t *)(g_stub->_shown - g_stub->_margin);
	uint32_t h = 0x811C9DC5;
	for (long n = (long)g_stub->_screen->stride * kFrameH / 4; --n >= 0; ) {
		h = (h << 5) + h + *p++;
	}
	return h;
}

uint32_t ST_hwPaletteHash() {
	uint32_t h = 0x811C9DC5;
	for (int i = 0; i < 256; ++i) {
		h = (h << 5) + h + (g_stub->_tt ? g_stub->_hw.tt[i] : g_stub->_hw.f30[i]);
	}
	return h;
}

// the conversion's and Flip's time over a replay, in ms (game.cpp's
// REPLAY push line)
void ST_screenTimes(uint32_t *convMs, uint32_t *flipMs) {
	*convMs = g_convTicks / 384 * 10;
	*flipMs = g_flipTicks / 384 * 10;
	g_convTicks = g_flipTicks = 0;
}
