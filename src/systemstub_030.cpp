
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The 030 build's platform layer (FBACK030.TOS). A TT or a Falcon shows
 * upstream's 256x224 chunky picture in 256 colours through STDL's SDL2
 * add-on (stdl/addons/sdl2, stdl/docs/sdl2.md), on STDL's responsive
 * screen: 320x240 in 8 interleaved planes, TT Low on a TT, a Falcon's
 * 8-bit mode on VGA, RGB or a TV, with its desktop back at every exit, a
 * crash's included. The keyboard, joystick and pads, the clock, the
 * sound and the machine set-up are the ST build's, shared
 * (systemstub_atari.h).
 *
 * The engine pushes boxes of its pictures as if to one screen that keeps
 * them all, and the add-on's screen is one: a texture lies over each of
 * the engine's buffers, with no copy, and a push is a copy of its box.
 * Boxes are rounded out to their 16-pixel groups, as the engine's
 * pictures are whole groups wide and sit on one, so the pages hold what
 * this build's own conversion put there, byte for byte. The add-on
 * converts a frame's boxes at the present, and those from one picture
 * before a box from another, before that picture can change; it catches
 * each page up from the page last shown, which holds the screen as it
 * stands (the title pushes its buffer black, decodes the picture into it
 * and reveals that a few rows a step); and a frame that pushes nothing
 * does not flip. Three pages, or two (screen_pages=2, or where the third
 * does not fit).
 *
 * Register 0 is black, everywhere. It is the colour of the border round
 * the screen memory, which nothing drawn can reach (a Falcon's on an RGB
 * monitor, outside the overscan picture), and the game's colour 0 is a
 * room's background: the jungle had a teal border. So the border index
 * is kSwap, slot 7's first entry, which the Amiga data never loads or
 * draws in (Video::AMIGA_setLevelPalettes): every pixel is XORed with it
 * as it is converted, the palette goes to the registers permuted to
 * match, and register 0 is held black. The frame round the picture is
 * register 0 too. The colours are staged, set at the present, so each
 * set is shown with the page it came with.
 */

#include "SDL.h"
#include "SDL_stdl.h"
#include "SDL_stdl_bench.h"
#include "systemstub_atari.h"
#include "util.h"
#include "video_st.h"

enum {
	kFrameW = 320,
	kFrameH = 240,
	// the engine's buffers: the front layer, the cutscene's pages, the
	// title's and the splash's
	kTextures = 8,
	// what every pixel is XORed with on its way to the screen (see the
	// top of the file): colour 0 to register 0x70, slot 7 to 0-15
	kSwap = 0x70
};

struct SystemStub_030 : SystemStub_Atari {
	int _srcW, _srcH;          // the picture the engine sends
	SDL_Window *_win;
	SDL_Renderer *_ren;
	SDL_Palette *_pal;         // the renderer's: what the engine asked for
	Color _cols[256];          // the same, full strength
	// the colours changed since the renderer last had them, sent once
	// before the present: a cutscene sets 32 entries a frame, and a call
	// of the renderer's each cost a Falcon's intro 1.8ms a frame
	int _palLo, _palHi;
	struct Tex {
		const uint8_t *buf;
		int pitch, w, h;
		SDL_Texture *tex;
	} _tex[kTextures];
	int _nextTex;
	// the one pushed from last: play pushes a dozen boxes a frame, all
	// from the front layer
	const uint8_t *_lastBuf;
	int _lastPitch;
	SDL_Texture *_lastTex;
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
	SDL_Texture *texture(const uint8_t *buf, int pitch);
	void setColours(int first, int n, int step);
	void sendColours();
};

static SystemStub_030 *g_stub;

SystemStub *SystemStub_030_create() {
	g_stub = new SystemStub_030();
	return g_stub;
}

void SystemStub_030::init(const char *title, int w, int h, bool fullscreen, int widescreenMode, bool maximized, const ScalerParameters *scalerParameters, int outputRate) {
	initMachine();
	logMachine();
	_tt = STDL_GetMachineInfo()->is_tt;
	// Three pages unless screen_pages=2: with two, Flip waits for the
	// VBL that shows its page, and a 60Hz display holds a frame of a
	// Falcon's 45ms to three VBLs, 50ms, past the 30Hz step's deadline
	// (a replay ran at 53.9ms a frame against 45.6 with three). STDL
	// gives two where the third page does not fit.
	const bool three = g_options.screen_pages != 2;
	SDL_SetHint(STDL_SDL2_HINT_PAGES, three ? "3" : "2");
	// replay_hash: every page, before it goes up, against a chunky model
	// of the screen the engine pushed to
	if (g_options.replay_hash) {
		SDL_SetHint(STDL_SDL2_HINT_CHECK, "1");
	}
	// SDL_Init takes STDL as initMachine left it; the window takes the
	// screen, and with it the keyboard
	_win = 0;
	_ren = 0;
	if (SDL_Init(SDL_INIT_VIDEO) < 0 ||
		!(_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kFrameW, kFrameH, 0)) ||
		!(_ren = SDL_CreateRenderer(_win, -1, 0))) {
		error("FBACK030.TOS needs a TT or a Falcon with a colour monitor: %s", SDL_GetError());
	}
	STDL_SDL2_SetBorderIndex(_ren, kSwap);
	_pal = SDL_AllocPalette(256);
	if (!_pal) {
		error("No memory for the palette");
	}
	memset(_cols, 0, sizeof(_cols));
	setColours(0, 256, 8);
	_palLo = 256;
	_palHi = -1;
	STDL_SDL2_SetRenderPalette(_ren, _pal);
	const STDL_Surface *s = STDL_GetVideoSurface();
	const int dbl = (s->stride == 2 * kFrameW) ? 1 : 0;
	if (three && !(s->flags & STDL_TRIPLEBUF)) {
		warning("Screen: two pages, %s", STDL_GetError());
	}
	if (g_options.logging) {
#ifdef ATARIST_030_ST
		// the experimental build on an ST: the add-on reduces the 8-bit
		// picture to the ST's 16 colours, on 240 rows with the bottom
		// border open or ST Low's 200. An open border flips its own
		// two pages whatever the flags say.
		if (s->format->BitsPerPixel == 4) {
			info("Screen: ST, 8 bits (emulated), %d rows, %d bytes a line, %d pages", s->h, s->stride,
				(s->h == 200 && (s->flags & STDL_TRIPLEBUF)) ? 3 : 2);
		} else
#endif
		info("Screen: %s, %d bytes a line, %d pages%s", _tt ? "TT Low" : "Falcon, 8 bits", s->stride >> dbl,
			(s->flags & STDL_TRIPLEBUF) ? 3 : 2, dbl ? ", rows doubled" : "");
	}
	memset(_tex, 0, sizeof(_tex));
	_nextTex = 0;
	_lastBuf = 0;
	_lastTex = 0;
	_srcW = _srcH = 0;
	setScreenSize(w, h);
	initJoystick();
}

void SystemStub_030::destroy() {
	STDL_SDL2_Check chk;
	if (g_options.replay_hash && STDL_SDL2_GetCheck(_ren, &chk) == 0) {
		if (chk.bad == 0) {
			info("SCREEN check: %u flips, every page as its pushes", (unsigned)chk.flips);
		} else {
			info("SCREEN check: %u of %u flips unlike their pushes, the first %u at row %d",
				(unsigned)chk.bad, (unsigned)chk.flips, (unsigned)chk.first_flip, chk.first_row);
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
		// a picture's boxes are dropped with the screen, then its place:
		// x on a 16-pixel group, as the add-on centres it
		clearScreen();
		SDL_RenderSetLogicalSize(_ren, w, h);
		// textures are the picture's size
		for (int i = 0; i < kTextures; ++i) {
			if (_tex[i].tex) {
				SDL_DestroyTexture(_tex[i].tex);
			}
		}
		memset(_tex, 0, sizeof(_tex));
		_nextTex = 0;
		_lastBuf = 0;
		_lastTex = 0;
	}
}

// Register 0 everywhere: the frame round the picture, and the picture
// until the engine sends it. The page up now goes at once, as the one
// screen did; the others when next drawn on.
void SystemStub_030::clearScreen() {
	STDL_SDL2_ClearScreenNow(_ren);
}

// The engine's colours [first, first + n) to the renderer's palette,
// scaled by step eighths; the renderer permutes them by kSwap and stages
// them at the next present, or at once for a fade's step
void SystemStub_030::setColours(int first, int n, int step) {
	SDL_Color c[256];
	for (int i = 0; i < n; ++i) {
		const Color &s = _cols[first + i];
		c[i].r = (uint8_t)(s.r * step >> 3);
		c[i].g = (uint8_t)(s.g * step >> 3);
		c[i].b = (uint8_t)(s.b * step >> 3);
		c[i].a = 255;
	}
	SDL_SetPaletteColors(_pal, c, first, n);
}

void SystemStub_030::sendColours() {
	if (_palLo <= _palHi) {
		setColours(_palLo, _palHi - _palLo + 1, 8);
		_palLo = 256;
		_palHi = -1;
	}
}

void SystemStub_030::setPalette(const uint8_t *pal, int n) {
	if (n > 256) {
		n = 256;
	}
	for (int i = 0; i < n; ++i, pal += 3) {
		_cols[i].r = pal[0];
		_cols[i].g = pal[1];
		_cols[i].b = pal[2];
	}
	if (n > 0) {
		_palLo = 0;
		if (n - 1 > _palHi) {
			_palHi = n - 1;
		}
	}
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
	if (i < _palLo) {
		_palLo = i;
	}
	if (i > _palHi) {
		_palHi = i;
	}
}

void SystemStub_030::getPaletteEntry(int i, Color *c) {
	*c = _cols[i];
}

// The texture over one of the engine's buffers, made the first time it
// is pushed from; the oldest goes if there are more than kTextures
SDL_Texture *SystemStub_030::texture(const uint8_t *buf, int pitch) {
	_lastBuf = buf;
	_lastPitch = pitch;
	for (int i = 0; i < kTextures; ++i) {
		const Tex &t = _tex[i];
		if (t.tex && t.buf == buf && t.pitch == pitch && t.w == _srcW && t.h == _srcH) {
			_lastTex = t.tex;
			return t.tex;
		}
	}
	Tex &t = _tex[_nextTex];
	_nextTex = (_nextTex + 1) % kTextures;
	if (t.tex) {
		SDL_DestroyTexture(t.tex);
	}
	t.buf = buf;
	t.pitch = pitch;
	t.w = _srcW;
	t.h = _srcH;
	t.tex = STDL_SDL2_CreateTextureFrom(_ren, (void *)buf, _srcW, _srcH, pitch);
	if (!t.tex) {
		error("Screen: %s", SDL_GetError());
	}
	_lastTex = t.tex;
	return t.tex;
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
	// the groups the box touches: the picture's width is whole groups
	// and its place on the screen is on one
	const int g0 = x >> 4, g1 = (x + w + 15) >> 4;
	SDL_Rect r;
	r.x = g0 * 16;
	r.y = y;
	r.w = (g1 - g0) * 16;
	r.h = h;
	SDL_RenderCopy(_ren, (buf == _lastBuf && pitch == _lastPitch && _lastTex) ? _lastTex : texture(buf, pitch), &r, &r);
}

void SystemStub_030::updateScreen(int shakeOffset) {
	sendColours();
	SDL_RenderPresent(_ren);
}

// Fade to black on the palette alone, as the ST does: each step staged
// for the next blank, and the colours back at the next present, which
// the engine redraws for first.
void SystemStub_030::fadeScreen() {
	for (int step = 7; step >= 0; --step) {
		setColours(0, 256, step);
		STDL_SDL2_StagePalette(_ren);
		STDL_Delay(40);
	}
	clearScreen();
	setColours(0, 256, 8);
	_palLo = 256;
	_palHi = -1;
}

// What a frame put up (replay_hash, Cutscene::updateScreen and the
// replays): the page Flip was last handed, the whole of its memory, and
// the colours as the registers got them, folded h * 33 + each long as
// the ST build's page hash is
uint32_t ST_screenHash() {
	const uint8_t *base;
	long bytes;
	if (STDL_SDL2_GetShownPage(g_stub->_ren, &base, &bytes) != 0 || !base) {
		return 0;
	}
	const uint32_t *p = (const uint32_t *)base;
	uint32_t h = 0x811C9DC5;
	for (long n = bytes / 4; --n >= 0; ) {
		h = (h << 5) + h + *p++;
	}
	return h;
}

// a TT's registers take 4 bits a gun and a Falcon's are 0x00RRGGBB here,
// as this build's own screen wrote them
uint32_t ST_hwPaletteHash() {
	const SDL_Color *c = STDL_SDL2_GetRegisterColours(g_stub->_ren);
	uint32_t h = 0x811C9DC5;
	for (int i = 0; i < 256; ++i) {
		const uint32_t w = g_stub->_tt
			? (uint32_t)(((c[i].r >> 4) << 8) | ((c[i].g >> 4) << 4) | (c[i].b >> 4))
			: ((uint32_t)c[i].r << 16) | ((uint32_t)c[i].g << 8) | c[i].b;
		h = (h << 5) + h + w;
	}
	return h;
}

// the conversion's and Flip's time over a replay, in ms (game.cpp's
// REPLAY push line)
void ST_screenTimes(uint32_t *convMs, uint32_t *flipMs) {
	Uint32 conv, flip;
	STDL_SDL2_GetPresentTimes(g_stub->_ren, &conv, &flip, 1);
	*convMs = conv / 384 * 10;
	*flipMs = flip / 384 * 10;
}
