
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The 030 build's platform layer (FBACK030.TOS). A TT or a Falcon shows
 * upstream's 256x224 chunky picture in 256 colours, the mode and the
 * colours set through the XBIOS here. The keyboard, joystick and pads,
 * the clock, the sound and the machine set-up are the ST build's,
 * shared (systemstub_atari.h).
 *
 * The modes are the ones SDL 1.2's XBIOS driver picks for 320x240,
 * which this build used until the stub converted the picture itself
 * and SDL was left setting the mode and the colours. That took 240K of
 * its code into the binary, and a 75-150K chunky surface it allocates
 * for its own conversion, unused, and SDL could choose a mode the
 * conversion does not write (a chunky one on a graphics card):
 *  - A TT's 256-colour mode, TT Low, is 480 lines. Each row of the
 *    picture is converted once and written to two lines. (SDL's 320x240
 *    mode doubles them too, but puts an update that does not start at
 *    the top at line y rather than 2y.)
 *  - A Falcon on VGA has 320x240, its lines doubled by the VIDEL. On an
 *    RGB monitor or a TV, 240 lines take overscan, 384 pixels wide; the
 *    picture is centred in the line. The PAL and VGA bits come from the
 *    desktop's mode.
 * The desktop's mode, screen and colours go back at exit (atexit, so an
 * error's exit puts them back too).
 *
 * Register 0 is black, everywhere. It is the colour of the border round
 * the screen memory, which nothing drawn can reach (a Falcon's on an RGB
 * monitor, outside the overscan picture), and the game's colour 0 is a
 * room's background: the jungle had a teal border. So every pixel is
 * XORed with kSwap as it is converted, and the palette goes to the
 * registers permuted to match, which moves colour 0 to register kSwap
 * and leaves register 0 for slot 7's first entry, which the Amiga data
 * never loads or draws in (Video::AMIGA_setLevelPalettes); it is held
 * black. The frame round the picture is register 0 too.
 */

#include <mint/cookie.h>
#include <mint/falcon.h>
#include <mint/osbind.h>
#include "systemstub_atari.h"
#include "util.h"
#include "video_st.h"
#include "c2p_030.h"

enum {
	kFrameW = 320,
	kFrameH = 240,
	// a frame's dirty runs, one per band of eight rows: more than this
	// and those so far go up there and then
	kMaxRects = 64,
	// what every pixel is XORed with on its way to the screen (see the
	// top of the file): colour 0 to register 0x70, slot 7 to 0-15. The
	// conversion has it built in (c2p_030.h).
	kSwap = 0x70
};

struct SystemStub_030 : SystemStub_Atari {
	// A box the engine handed over, waiting for updateScreen: the
	// engine's own pixels, read when the colours are in. Every caller
	// updates the screen before it touches that buffer again.
	struct Pending {
		const uint8_t *src;
		uint8_t *dst;
		int pitch;
		int groups, h;
	};
	int _srcW, _srcH;          // the picture the engine sends
	bool _tt;                  // a TT, else a Falcon
	uint8_t *_screen;          // the planar screen, ours
	int _frameW;               // its width: a line's bytes, 384 on a Falcon's RGB mode
	int _dbl;                  // 1 when a frame row is two lines (a TT)
	Color _cols[256];          // what the engine asked for
	bool _colsDirty;           // not yet in the registers
	int _xOff, _yOff;          // the picture's place in the frame
	Pending _pending[kMaxRects];
	int _numPending;
	// what goes to the XBIOS: a TT's 12-bit words, a Falcon's 0x00RRGGBB
	// longs; kept, as either may be read at the next VBL
	union {
		uint16_t tt[256];
		uint32_t f30[256];
	} _hw;

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

	void setMode();
	void clearScreen();
	void show();
	void setColours(int step);
};

static SystemStub_030 *g_stub;

SystemStub *SystemStub_030_create() {
	g_stub = new SystemStub_030();
	return g_stub;
}

// The palette registers: a TT's 256 (its ST modes show a bank of them),
// a Falcon's 256 and the 16 its ST-compatible modes show
static volatile uint16_t *const kTTPal = (volatile uint16_t *)0xFF8400L;
static volatile uint32_t *const kF30Pal = (volatile uint32_t *)0xFF9800L;
static volatile uint16_t *const kSTPal = (volatile uint16_t *)0xFF8240L;

// The desktop's screen, mode and colours, put back at exit. The colours
// are copied from the registers, which the program reaches in supervisor
// mode (STDL's, from initMachine until it quits): on a Falcon with TOS
// 4.04 VgetRGB read all black under a desktop that had set its colours
// through the VDI, and putting that back left the desktop black (Hatari).
static struct {
	bool saved;
	bool tt;
	void *log, *phys;
	int16_t mode;              // a TT's Getrez, a Falcon's mode word
	uint16_t st[16];
	union {
		uint16_t tt[256];
		uint32_t f30[256];
	} pal;
} g_desk;

static void restoreDesktop() {
	if (!g_desk.saved) {
		return;
	}
	g_desk.saved = false;
	if (g_desk.tt) {
		(void)Setscreen(g_desk.log, g_desk.phys, g_desk.mode);
	} else {
		(void)Setscreen(g_desk.log, g_desk.phys, -1);
		(void)VsetMode(g_desk.mode);
	}
	// after the VBL, which lands any colours the XBIOS still holds for it
	Vsync();
	for (int i = 0; i < 256; ++i) {
		if (g_desk.tt) {
			kTTPal[i] = g_desk.pal.tt[i];
		} else {
			kF30Pal[i] = g_desk.pal.f30[i];
		}
	}
	if (!g_desk.tt) {
		for (int i = 0; i < 16; ++i) {
			kSTPal[i] = g_desk.st[i];
		}
	}
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

void SystemStub_030::init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate) {
	initMachine();
	// STDL takes the keyboard when it sets a mode, which this build
	// leaves to setMode; this takes it without one
	if (STDL_OpenInput() < 0) {
		error("STDL_OpenInput failed: %s", STDL_GetError());
	}
	logMachine();
	setMode();
	if (g_options.logging) {
		info("Screen: %s, %d pixels a line, %d lines%s", _tt ? "TT Low" : "Falcon, 8 bits", _frameW,
			kFrameH << _dbl, _dbl ? ", rows doubled" : "");
		checkPlanes();
	}
	memset(_cols, 0, sizeof(_cols));
	_colsDirty = true;
	_numPending = 0;
	_srcW = _srcH = 0;
	setScreenSize(w, h);
	initJoystick();
}

void SystemStub_030::setMode() {
	long mch = 0;
	if (Getcookie(C__MCH, &mch) != C_FOUND) {
		mch = 0;
	}
	_tt = ((mch >> 16) == 2);
	g_desk.tt = _tt;
	g_desk.log = Logbase();
	g_desk.phys = Physbase();
	int16_t mode;
	if (_tt) {
		// TT High is the only mode a TT's mono monitor shows; its colour
		// monitor shows every other, ST High included
		g_desk.mode = Getrez();
		if (g_desk.mode == (TT_HIGH >> 8)) {
			error("FBACK030.TOS needs a colour monitor");
		}
		for (int i = 0; i < 256; ++i) {
			g_desk.pal.tt[i] = kTTPal[i];
		}
		mode = TT_LOW >> 8;
		_frameW = kFrameW;
		_dbl = 1;
	} else {
		const int16_t mon = VgetMonitor();
		if (mon == MON_MONO) {
			error("FBACK030.TOS needs a colour monitor");
		}
		g_desk.mode = VsetMode(-1);
		for (int i = 0; i < 256; ++i) {
			g_desk.pal.f30[i] = kF30Pal[i];
		}
		for (int i = 0; i < 16; ++i) {
			g_desk.st[i] = kSTPal[i];
		}
		mode = (int16_t)(BPS8 | COL40 | (g_desk.mode & (VGA | PAL)) | (mon == MON_VGA ? VERTFLAG : OVERSCAN));
		_frameW = (mode & OVERSCAN) ? 384 : kFrameW;
		_dbl = 0;
	}
	// ST-RAM, which the video sees, on a 256-byte boundary
	const long size = (long)_frameW * (kFrameH << _dbl);
	const long mem = Mxalloc(size + 255, 0);
	if (mem <= 0) {
		error("No room in ST-RAM for the screen (%ldK)", size >> 10);
	}
	_screen = (uint8_t *)((mem + 255) & ~255L);
	memset(_screen, 0, size);
	g_desk.saved = true;
	atexit(restoreDesktop);
	// A TT changes mode with the screen, and the console moves to it; a
	// Falcon's VsetMode leaves the console on the desktop's screen.
	// Nothing prints (util.cpp's diagnostics go to RS.LOG), and the
	// console's cursor goes off.
	if (_tt) {
		(void)Setscreen(_screen, _screen, mode);
	} else {
		(void)Setscreen(-1L, _screen, -1);
		(void)VsetMode(mode);
	}
	(void)Cconws("\033f");
	Vsync();
}

void SystemStub_030::destroy() {
	restoreDesktop();
	STDL_Quit();
}

void SystemStub_030::setScreenSize(int w, int h) {
	if (w > _frameW) {
		w = _frameW;
	}
	if (h > kFrameH) {
		h = kFrameH;
	}
	if (w != _srcW || h != _srcH) {
		_srcW = w;
		_srcH = h;
		// whole groups: the picture's 16-pixel groups are the screen's
		_xOff = ((_frameW - w) / 2) & ~15;
		_yOff = (kFrameH - h) / 2;
		clearScreen();
	}
}

// Register 0 everywhere: the frame round the picture, and the picture
// until the engine sends it
void SystemStub_030::clearScreen() {
	memset(_screen, 0, _frameW * (kFrameH << _dbl));
	_numPending = 0;
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
	if (_numPending == kMaxRects) {
		show();
	}
	// the groups the box touches: the picture's width is whole groups
	// and its place in the frame is on one, so they are the source's too
	const int x0 = x & ~15;
	const int x1 = (x + w + 15) & ~15;
	Pending &p = _pending[_numPending++];
	p.src = buf + y * pitch + x0;
	p.dst = _screen + ((_yOff + y) << _dbl) * _frameW + _xOff + x0;
	p.pitch = pitch;
	p.groups = (x1 - x0) >> 4;
	p.h = h;
}

// The colours, then what changed, so a new picture never goes up in
// the old colours
void SystemStub_030::show() {
	if (_colsDirty) {
		setColours(8);
		_colsDirty = false;
	}
	for (int i = 0; i < _numPending; ++i) {
		const Pending &p = _pending[i];
		c2p8(p.src, p.pitch, p.dst, _frameW, p.groups, p.h, _dbl);
	}
	_numPending = 0;
}

// The engine's colours into the registers: permuted as the pixels are
// (kSwap), register 0 black, scaled by step eighths. A TT's are 4 bits a
// gun, a Falcon's 6, given to the XBIOS as 8.
void SystemStub_030::setColours(int step) {
	if (_tt) {
		for (int i = 0; i < 256; ++i) {
			const Color &c = _cols[i ^ kSwap];
			_hw.tt[i] = (uint16_t)(((c.r * step >> 7) << 8) | ((c.g * step >> 7) << 4) | (c.b * step >> 7));
		}
		_hw.tt[0] = 0;
		EsetPalette(0, 256, _hw.tt);
	} else {
		for (int i = 0; i < 256; ++i) {
			const Color &c = _cols[i ^ kSwap];
			_hw.f30[i] = ((uint32_t)(c.r * step >> 3) << 16) | ((c.g * step >> 3) << 8) | (c.b * step >> 3);
		}
		_hw.f30[0] = 0;
		VsetRGB(0, 256, _hw.f30);
	}
}

void SystemStub_030::updateScreen(int shakeOffset) {
	show();
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

// What a cutscene frame put up (replay_hash, Cutscene::updateScreen):
// the screen memory as converted, and the colours as the registers got
// them, folded h * 33 + each long as the ST build's page hash is
uint32_t ST_screenHash() {
	const uint32_t *p = (const uint32_t *)g_stub->_screen;
	uint32_t h = 0x811C9DC5;
	for (long n = (long)g_stub->_frameW * (kFrameH << g_stub->_dbl) / 4; --n >= 0; ) {
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
