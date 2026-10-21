
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 * Atari ST port changes Copyright (C) 2026 Neil Rackett
 */

#include <time.h>
#include "file.h"
#include "fs.h"
#include "game.h"
#include "screenshot.h"
#include "seq_player.h"
#include "systemstub.h"
#include "util.h"
#include "video_st.h"
#include <stddef.h>
#ifdef ATARIST
extern "C" {
#include <stdl/stdl.h>
}
#include <mint/osbind.h>
#endif

// the input a byte of a .DEM stream stands for (a replay's too)
static void setKeymask(PlayerInput &pi, int keymask) {
	pi.dirMask = keymask & 0xF;
	pi.enter = (keymask & 0x10) != 0;
	pi.space = (keymask & 0x20) != 0;
	pi.shift = (keymask & 0x40) != 0;
	pi.backspace = (keymask & 0x80) != 0;
}

#ifdef ATARIST
#define GAME_COPYRECT_FRONT() _stub->copyRectPlanar(0, 0, _vid._w, _vid._h, _vid._frontLayer)
#else
#define GAME_COPYRECT_FRONT() _stub->copyRect(0, 0, _vid._w, _vid._h, _vid._frontLayer, _vid._w)
#endif

#ifdef ATARIST
// The sprite decoder the planar bakes call on a miss (ST_sprDraw): a
// frame is only decoded when it has to be baked, into the bakes'
// scratch buffer, never per draw.
static bool stDecode(void *ctx, int kind, const uint8_t *data, int w, int h, uint8_t *dst, bool planes) {
	Game *g = (Game *)ctx;
	if (planes) {
		return g->_vid.ST_amigaPlanes(kind, data, w, h, dst);
	}
	switch (kind) {
	case kSprKindSPM:
		g->_vid.AMIGA_decodeSpm(data, dst);
		break;
	case kSprKindSPC:
		g->_vid.AMIGA_decodeSpc(data, w, h, dst);
		break;
	case kSprKindIcon:
		g->ST_decodeIcon((uint8_t)((uintptr_t)data - 1), dst);
		break;
	}
	return true;
}

// the part of a sprite on the layer, for the dirty blocks
static inline void stMarkClipped(Video &vid, int x, int y, int w, int h) {
	if (vid.ST_dlOn() && ST_dlRecording(vid._frontLayer)) {
		return;                      // the display list keeps its own marks
	}
	int x1 = x + w, y1 = y + h;
	if (x < 0) {
		x = 0;
	}
	if (y < 0) {
		y = 0;
	}
	if (x1 > Video::GAMESCREEN_W) {
		x1 = Video::GAMESCREEN_W;
	}
	if (y1 > Video::GAMESCREEN_H) {
		y1 = Video::GAMESCREEN_H;
	}
	if (x1 > x && y1 > y) {
		vid.markBlockAsDirty(x, y, x1 - x, y1 - y, vid._layerScale);
	}
}

// --- record and replay (record=N, replay=N) ------------------------
//
// A benchmark that plays. bench=true times an idle Conrad in a warm
// room, the one case where every sprite cache hits, so it never sees a
// fight. A recording is the input of a real session - running,
// shooting, rooms changing - one keymask byte per inp_update, the
// format the demos use; a replay feeds it back with no pacing and logs
// what each frame cost. Both start from a title Load, so the state is
// the save's, and the seed travels in the file because no save holds
// one. Catch-ups are off while recording, so a step is a frame in both.
enum { kRecMaxSteps = 32768, kRecVersion = 1 };
static const uint32_t TAG_RSRC = 0x52535243;   // 'RSRC'

// Per 64 frames of a replay, kept in memory and logged at the end so
// no log write lands inside the measurement.
struct STRepWindow {
	uint32_t ms;
	uint32_t hash;          // replay_hash: the frames' layers, folded
	uint16_t frames;
	uint16_t worst;
	uint8_t room;
	STPerf perf;
};
enum { kRepWinFrames = 64, kRepMaxWindows = 160, kRepBuckets = 41 };   // 5ms buckets; the last is 200ms+
static STRepWindow *g_repWin;           // only while replaying: no memory in play
static int g_repWins;
static STRepWindow g_repCur;
static uint16_t g_repHist[kRepBuckets];
static uint32_t g_repLast;              // when the previous frame ended
static uint32_t g_repFrames, g_repMs, g_repWorst;

static inline uint32_t rotHash(uint32_t h, uint32_t v) {
	return ST_rol5(h) ^ v;
}

// Added, not XORed: a layer is 8960 longs, so a frame's rotations come
// to a whole turn and two identical frames XORed in would cancel. Each
// frame's own hash is kept as well, and written to FHASH.BIN at the end,
// so two builds' replays can be compared to the frame; replay_dump=N
// writes frame N's layer and colours to DMP.BIN to look at.
static uint32_t *g_repFrameHash;
static uint32_t g_repFrameHashCap;

static void ST_replayHashFrame(const uint8_t *layer) {
	// the four planes, what is seen: the priority plane after them only
	// steers drawing, and two exact ways of drawing may leave it apart
	const uint32_t *p = (const uint32_t *)layer;
	uint32_t h = 0;
	for (int n = kSTPlaneBytes / 4; --n >= 0; ) {
		h = ST_rol5(h) + *p++;
	}
	g_repCur.hash = ST_rol5(g_repCur.hash) + h;
	if (g_repFrameHash && g_repFrames < g_repFrameHashCap) {
		g_repFrameHash[g_repFrames] = h;
	}
	if (g_options.replay_dump != 0 && g_repFrames == (uint32_t)g_options.replay_dump) {
		FILE *fp = fopen("DMP.BIN", "wb");
		if (fp) {
			fwrite((const void *)0xFFFF8240UL, 2, 16, fp);   // the colours on screen
			fwrite(layer, 1, kSTLayerSize, fp);
			fclose(fp);
		}
	}
}

static void ST_replayCloseWindow(uint8_t room) {
	g_repCur.perf = g_stPerf;
	g_repCur.room = room;
	memset(&g_stPerf, 0, sizeof(g_stPerf));
	if (g_repWin && g_repWins < kRepMaxWindows) {
		g_repWin[g_repWins++] = g_repCur;
	}
	memset(&g_repCur, 0, sizeof(g_repCur));
}

static void ST_replayReset(uint32_t now, uint32_t steps) {
	if (!g_repWin) {
		g_repWin = (STRepWindow *)malloc(kRepMaxWindows * sizeof(STRepWindow));
	}
	if (g_options.replay_hash) {
		free(g_repFrameHash);
		g_repFrameHash = (uint32_t *)malloc(steps * sizeof(uint32_t) + 4);
		g_repFrameHashCap = g_repFrameHash ? steps : 0;
	}
	g_repWins = 0;
	memset(&g_repCur, 0, sizeof(g_repCur));
	memset(g_repHist, 0, sizeof(g_repHist));
	memset(&g_stPerf, 0, sizeof(g_stPerf));
	g_repLast = now;
	g_repFrames = g_repMs = g_repWorst = 0;
}

// The saved games on disk for the title's Load line: RS<level>_<slot>.SAV
// as makeGameStateName writes them, level 1-7 and slot 00 (the level's
// save point) to 99, in level and slot order; newest is the index of
// the most recently written.
struct STSave {
	uint8_t level, slot;
	uint32_t stamp;                  // DOS date and time, for the newest
};
static int ST_scanSaves(STSave *out, int max, int *newest) {
	int n = 0;
	*newest = -1;
	_DTA dta;
	_DTA *saved = Fgetdta();
	Fsetdta(&dta);
	for (long r = Fsfirst("RS?_??.SAV", 0); r == 0; r = Fsnext()) {
		const char *s = dta.dta_name;
		if (s[2] < '1' || s[2] > '7' || s[4] < '0' || s[4] > '9' || s[5] < '0' || s[5] > '9') {
			continue;                    // the pattern's ? match anything
		}
		STSave e;
		e.level = (uint8_t)(s[2] - '1');
		e.slot = (uint8_t)((s[4] - '0') * 10 + (s[5] - '0'));
		e.stamp = ((uint32_t)dta.dta_date << 16) | dta.dta_time;
		if (n == max) {
			// full: the oldest gives way to a newer one, so the most
			// recent save is always listed however many there are
			int oldest = 0;
			for (int k = 1; k < n; ++k) {
				if (out[k].stamp < out[oldest].stamp) {
					oldest = k;
				}
			}
			if (e.stamp <= out[oldest].stamp) {
				continue;
			}
			for (int k = oldest; k < n - 1; ++k) {
				out[k] = out[k + 1];
			}
			--n;
		}
		int i = n++;
		while (i > 0 && (out[i - 1].level > e.level || (out[i - 1].level == e.level && out[i - 1].slot > e.slot))) {
			out[i] = out[i - 1];
			--i;
		}
		out[i] = e;
	}
	Fsetdta(saved);
	for (int i = 0; i < n; ++i) {
		if (*newest < 0 || out[i].stamp >= out[*newest].stamp) {
			*newest = i;
		}
	}
	return n;
}

// On the Amiga data a monster has no palette of its own: its slot is
// loaded with half of the object palette, entries (monster & 1) * 8
// on (loadMonsterSprites).
uint8_t ST_enemyHalves(int level) {
	uint8_t halves = 0;
	for (const uint8_t *m = Game::_monsterListLevels[level]; *m != 0xFF; m += 2) {
		halves |= 1 << (m[1] & 1);
	}
	return halves;
}

// Presentation splash: black screen, credit lines centred, shown as
// soon as the game font is loaded and held while the rest of the
// startup work happens behind it. Line one is drawn double-height
// (24 chars at double width would not fit 256px); line two uses the
// darker pale blue of the title artwork (0x478).
// The presentation splash: a 320x200 image compiled into the
// program (src/splash_data.cpp, generated by tools/png2splash.py)
// rather than loaded from DATA\\, because it goes up before any file
// is read and it is the port's own artwork rather than the game's.
//
// It is written straight to screen RAM instead of through the game's
// layer: the layer is 256 pixels wide and carries the engine's
// colour remap, while this is full width and wants its own three
// hardware colours. Anything the game draws afterwards goes through
// a full refresh, which puts the screen back.
extern const uint16_t g_splash_w;
extern const uint16_t g_splash_h;
extern const uint8_t g_splash_pal[][3];
extern const uint16_t g_splash_pal_count;
extern const uint8_t g_splash_rle[];
extern const uint32_t g_splash_rle_len;

static void ST_splash(Game *g) {
	STDL_Surface *screen = STDL_GetVideoSurface();
	if (screen == 0) {
		return;
	}
	// centre in whatever height the screen has: 200 normally, 227 or
	// more with a border open
	int top = (screen->h - (int)g_splash_h) / 2;
	if (top < 0) {
		top = 0;
	}
	memset(screen->pixels, 0, (uint32_t)screen->stride * screen->h);

	// two planes out of four: the RLE holds plane pairs in screen
	// order, so each group is two encoded words then two zero words
	const uint8_t *p = g_splash_rle;
	const uint8_t *end = g_splash_rle + g_splash_rle_len;
	// 16 pixels per group, two encoded planes = 4 bytes, so a row
	// of 320 pixels is 20 groups x 4 = 80 bytes of RLE output
	const int rowBytes = (int)g_splash_w / 4;
	int outRow = top, outCol = 0;
	uint8_t *row = (uint8_t *)screen->pixels + (uint32_t)top * screen->stride;
	while (p + 1 < end && outRow < screen->h) {
		int n = *p++;
		const uint8_t v = *p++;
		while (n-- > 0) {
			// column within the row's plane-pair stream
			const int grp = outCol >> 2;
			const int sub = outCol & 3;
			row[grp * 8 + sub] = v;
			if (++outCol == rowBytes) {
				outCol = 0;
				++outRow;
				row += screen->stride;
				if (outRow >= screen->h) {
					break;
				}
			}
		}
	}

	STDL_Colour cols[16];
	memset(cols, 0, sizeof(cols));
	for (int i = 0; i < (int)g_splash_pal_count && i < 16; ++i) {
		// the generator stores 3-bit channels, as the ST shows them
		cols[i].r = (uint8_t)(g_splash_pal[i][0] * 255 / 7);
		cols[i].g = (uint8_t)(g_splash_pal[i][1] * 255 / 7);
		cols[i].b = (uint8_t)(g_splash_pal[i][2] * 255 / 7);
	}
	STDL_SetColours(screen, cols, 0, 16);

	// The global sprites load behind the splash rather than after the
	// intro, where they were a black screen before the title: Conrad's
	// alone take three and a half seconds to unpack on an 8MHz ST. The
	// splash stays up for at least its three seconds.
	const uint32_t t0 = g->_stub->getTimeStamp();
	g->ST_loadGlobals();
	const int32_t left = 3000 - (int32_t)(g->_stub->getTimeStamp() - t0);
	if (left > 0) {
		g->_stub->sleep(left);
	}
	memset(g->_vid._frontLayer, 0, g->_vid._layerSize);
	g->_vid.fullRefresh();
	g->_vid.updateScreen();
}
#endif

Game::Game(SystemStub *stub, FileSystem *fs, const char *savePath, int level, ResourceType ver, Language lang, WidescreenMode widescreenMode, bool autoSave, const PrfMidiDriver *midiDriver, const char *midiSoundFont, uint32_t cheats)
	: _cut(&_res, stub, &_vid), _menu(&_res, stub, &_vid),
	_mix(fs, stub, midiDriver, midiSoundFont), _res(fs, ver, lang), _seq(stub, &_mix), _vid(&_res, stub, widescreenMode),
	_stub(stub), _fs(fs), _savePath(savePath) {
	_stateSlot = 1;
	_inp_demPos = 0;
	_skillLevel = _menu._skill = kSkillNormal;
	_currentLevel = _menu._level = level;
	_demoBin = -1;
	_widescreenMode = widescreenMode;
	_autoSave = autoSave;
	_rewindPtr = -1;
	_rewindLen = 0;
	_cheats = cheats;
#ifdef ATARIST
	_stGlobalsLoaded = false;
	_stLogMemRoom = false;
	_stPaletteMsgCounter = 0;
	_stPaletteReloaded = false;
	_stIconShown = -1;
	_stIconY = 0;
	_stIconGen = 0xFFFF;
	_stDeadline = 0;
	_stStepPhase = 0;
	_stLoadSlot = -1;
	memset(&_stRecHdr, 0, sizeof(_stRecHdr));
	_stRecBuf = 0;
	_stRecPos = 0;
	_stRecording = false;
	_stReplaying = false;
#endif
}


void Game::run() {
	// bench mode needs the same sequence every run: a clock seed is
	// what made enemy behaviour, and so the sprite count, differ from
	// one measurement to the next
	_randSeed = g_options.bench ? 0x4A5B6C7D : (uint32_t)time(0);

	_res.init();
	_res.load_TEXT();
#ifdef ATARIST
	ST_sprSetDecoder(stDecode, this, _res._scratchBuffer, Resource::kScratchBufferSize);
#endif

	switch (_res._type) {
	case kResourceTypeAmiga:
		_res.load("FONT8", Resource::OT_FNT, "SPR");
#ifdef ATARIST
		ST_splash(this);
#endif
		if (_res._isDemo) {
			_cut._patchedOffsetsTable = Cutscene::_amigaDemoOffsetsTable;
		}
		break;
	case kResourceTypeDOS:
		_res.load("FB_TXT", Resource::OT_FNT);
		if (g_options.use_seq_cutscenes) {
			_res._hasSeqData = _fs->exists("INTRO.SEQ");
		}
		if (_fs->exists("logosssi.cmd")) {
			_cut._patchedOffsetsTable = Cutscene::_ssiOffsetsTable;
		}
		break;
	case kResourceTypeMac:
		_res.MAC_loadClutData();
		_res.MAC_loadFontData();
		break;
	case kResourceTypePC98:
		// use _font8Jp
		break;
	case kResourceTypeSega:
		_res.load("FONT8", Resource::OT_FNT);
		break;
	}

	if (!g_options.bypass_protection && !g_options.use_words_protection && g_features->has_copy_protection) {
		while (!handleProtectionScreenShape()) {
			if (_stub->_pi.quit) {
				return;
			}
		}
	}

	_mix.init();
	_mix._mod._isAmiga = _res.isAmiga();

	if (_res.isMac()) {
		_menu.displayTitleScreenMac(Menu::kMacTitleScreen_MacPlay);
		if (!_stub->_pi.quit) {
			_menu.displayTitleScreenMac(Menu::kMacTitleScreen_Presage);
		}
	}
#ifdef ATARIST
	if (!g_options.skip_intro && g_options.replay < 0) {
#else
	if (!g_options.skip_intro) {
#endif
		playCutscene(0x40);
		playCutscene(0x0D);
	}

	// global resources
	switch (_res._type) {
	case kResourceTypeAmiga:
#ifdef ATARIST
		ST_loadGlobals();       // done already, behind the splash, unless it had no screen
#else
		_res.load("ICONE", Resource::OT_ICN, "SPR");
		_res.load("ICON", Resource::OT_ICN, "SPR");
		_res.load("PERSO", Resource::OT_SPM);
#endif
		break;
	case kResourceTypeDOS:
	case kResourceTypePC98:
		_res.load("GLOBAL", Resource::OT_ICN);
		_res.load("GLOBAL", Resource::OT_SPC);
		_res.load("PERSO", Resource::OT_SPR);
		_res.load_SPR_OFF("PERSO", _res._spr1);
		break;
	case kResourceTypeMac:
		_res.MAC_loadIconData();
		_res.MAC_loadPersoData();
		_res.MAC_loadSounds();
		break;
	case kResourceTypeSega:
		_res.load("GLOBAL", Resource::OT_ANI);
		_res.load("GLOBAL", Resource::OT_ICN);
		_res.load("GLOBAL", Resource::OT_OBJ);
		_res.load("GLOBAL", Resource::OT_SPC);
		_res.load("GLOBAL", Resource::OT_SPR);
		_res.load_SPR_OFF("GLOBAL", _res._spr1, "TAB");
		break;
	}

	// sound resources
	switch (_res._type) {
	case kResourceTypeAmiga:
		// sounds are loaded per level
		break;
	case kResourceTypeDOS:
		_res.load_FIB("GLOBAL");
		break;
	case kResourceTypePC98:
		_res.PC98_loadSounds();
		break;
	case kResourceTypeMac:
		_res.MAC_loadSounds();
		break;
	case kResourceTypeSega:
		break;
	}

	if (!g_options.bypass_protection && g_options.use_words_protection && _res.isDOS()) {
		while (!handleProtectionScreenWords()) {
			if (_stub->_pi.quit) {
				return;
			}
		}
	}

	while (!_stub->_pi.quit) {
		if (_stub->hasWidescreen()) {
			_stub->clearWidescreen();
		}
		_mix.playMusic(1);
		switch (_res._type) {
		case kResourceTypeDOS:
			if (!_res.fileExists("MENU1.MAP")) {
				// fbdemofr (no menus)
				break;
			}
			/* fall-through */
		case kResourceTypePC98:
		case kResourceTypeMac:
			_menu.handleTitleScreen();
			if (_menu._selectedOption == Menu::MENU_OPTION_ITEM_QUIT || _stub->_pi.quit) {
				_stub->_pi.quit = true;
				break;
			}
			if (_menu._selectedOption == Menu::MENU_OPTION_ITEM_DEMO) {
				_demoBin = (_demoBin + 1) % ARRAYSIZE(_demoInputs);
				const char *fn = _demoInputs[_demoBin].name;
				debug(DBG_DEMO, "Loading inputs from '%s'", fn);
				_res.load_DEM(fn);
				if (_res._demLen == 0) {
					continue;
				}
				_skillLevel = kSkillNormal;
				_currentLevel = _demoInputs[_demoBin].level;
				_randSeed = 0;
				if (_res.isMac()) {
					_demoRandCounter = 0;
					_demoRandRange = _demoInputsMac[_demoBin].rnd;
				}
			} else {
				_demoBin = -1;
				_skillLevel = _menu._skill;
				_currentLevel = _menu._level;
			}
			break;
		case kResourceTypeAmiga:
#ifdef ATARIST
			if (g_options.replay >= 0) {
				// straight to the recording's save: no title, and a
				// second pass round this loop is the end of the run
				if (_stReplaying || !ST_replayLoad()) {
					_stub->_pi.quit = true;
					break;
				}
				_menu._skill = _stRecHdr.skill;
				_cheats = _stRecHdr.cheats;
				_currentLevel = _stRecHdr.level;
				_stLoadSlot = _stRecHdr.slot;
			} else
#endif
			displayTitleScreenAmiga();
			_stub->useFillWindow(true);
			_skillLevel = _menu._skill;
			_stub->setScreenSize(Video::GAMESCREEN_W, Video::GAMESCREEN_H);
			break;
		case kResourceTypeSega:
			break;
		}
		_mix.stopMusic();
		if (_stub->_pi.quit) {
			break;
		}
		if (_stub->hasWidescreen()) {
			_stub->clearWidescreen();
		}
		if (_currentLevel == 7) {
			_vid.fadeOut();
			_vid.setTextPalette();
			playCutscene(0x3D);
		} else {
			_vid.setTextPalette();
			_vid.setPalette0xF();
			_stub->setOverscanColor(0xE0);
			_vid._unkPalSlot1 = 0;
			_vid._unkPalSlot2 = 0;
			_score = 0;
			clearStateRewind();
			loadLevelData();
			resetGameState();
#ifdef ATARIST
			if (_stLoadSlot >= 0) {
				// Load on the title: the saved state in place of the
				// level's start, and so without its opening cutscene
				if (loadGameState((uint8_t)_stLoadSlot)) {
					_cut._id = 0xFFFF;
					if (g_options.replay >= 0) {
						// the seed the recording ran on, which no save
						// holds; from here the game is input-driven
						_randSeed = _stRecHdr.seed;
						if (ST_stateHash() != _stRecHdr.startHash) {
							warning("Replay: the save is not the one recorded from");
						}
						_stReplaying = true;
						_stRecPos = 0;
						ST_replayReset(_stub->getTimeStamp(), _stRecHdr.steps);
					} else if (g_options.record >= 0) {
						ST_recStart();
					}
					if (_stLoadSlot != kIngameSaveSlot) {
						_stateSlot = (uint8_t)_stLoadSlot;      // Ctrl+S saves back to it
					} else {
						// a death continues from the save point, as
						// it would had it been reached in play
						_validSaveState = true;
					}
				}
				_stLoadSlot = -1;
			}
#endif
			_endLoop = false;
			_frameTimestamp = _stub->getTimeStamp();
			_logicDebt = 0;
			_logicCatchUps = 0;
			_stDeadline = _frameTimestamp;
			_saveTimestamp = _frameTimestamp;
			while (!_stub->_pi.quit && !_endLoop) {
				mainLoop();
				if (_demoBin != -1 && _inp_demPos >= _res._demLen) {
					debug(DBG_DEMO, "End of demo");
					// exit level
					_endLoop = true;
				}
			}
			_stub->setOverscanColor(0x00);
#ifdef ATARIST
			if (_stRecording) {
				ST_recFinish("the level loop ended");
			}
			ST_unloadLevel();
#endif
			// flush inputs
			_stub->_pi.dirMask = 0;
			_stub->_pi.enter = false;
			_stub->_pi.space = false;
			_stub->_pi.shift = false;
		}
	}

	_res.free_TEXT();
	_mix.free();
	_res.fini();
}

void Game::displayTitleScreenAmiga() {
	info("Title screen");
#ifdef ATARIST
	ST_logFreeMemory("title");
	ST_paletteForRoom(0, 0, -1);        // the title chooses its own colours
	ST_setColourWeights(0);
#endif
	_stub->useFillWindow(false);
	static const char *FILENAME = "present.cmp";
	_res.load_CMP_menu(FILENAME);
	static const int kW = 320;
	static const int kH = 224;
	uint8_t *buf = (uint8_t *)calloc(1, kW * kH);
	if (!buf) {
		error("Failed to allocate screen buffer w=%d h=%d", kW, kH);
	}
	static const uint16_t kAmigaColors[] = {
		0x000, 0x123, 0x012, 0x134, 0x433, 0x453, 0x046, 0x245,
		0x751, 0x455, 0x665, 0x268, 0x961, 0x478, 0x677, 0x786,
		0x17B, 0x788, 0xB84, 0xC92, 0x49C, 0xF00, 0x9A8, 0x9AA,
		0xCA7, 0xEA3, 0x8BD, 0xBBB, 0xEC7, 0xBCD, 0xDDB, 0xEED
	};
#ifdef ATARIST
	// The title only ever sets its own 32 entries. On the first visit
	// the rest of the logical palette is black and the 16 hardware
	// colours are all its own; after a level they hold that level's
	// colours, and the quantiser shares the 16 out between the two -
	// the eyes came back green and the text yellow after a game over.
	// Black out what is not the title's first. The cutscene banks
	// stay: game mode leaves 0xC0-0xDF out of the quantiser anyway.
	{
		const Color black = { 0, 0, 0 };
		for (int i = 32; i < 0xC0; ++i) {
			_stub->setPaletteEntry(i, &black);
		}
	}
#endif
	for (int i = 0; i < 32; ++i) {
		Color c = Video::AMIGA_convertColor(kAmigaColors[i]);
		_stub->setPaletteEntry(i, &c);
	}
	_vid.setTextPalette();
	_stub->setScreenSize(kW, kH);
	// fill with black
	_stub->copyRect(0, 0, kW, kH, buf, kW);
	_stub->updateScreen(0);
	_vid.AMIGA_decodeCmp(_res._scratchBuffer + 6, buf);
	int h = 0;
	// Three pages: Start, Load, Options and Quit; the levels, under
	// Start; and the settings, under Options. The rows are chosen so
	// the menu is whole in every mode it can select. Fill hides rows
	// 0-11, so the list starts below them. Fit keeps rows in runs of
	// ten from row 10, dropping every eleventh, so an 11-pixel pitch
	// from row 21 puts each line inside one run with its shadow -
	// eight rows of glyph and one below - and none of them loses a row.
	// The longest page, the levels and Back, ends at row 118, clear of
	// the FLASHBACK logo at about 150. _currentLevel only ever holds a
	// real level.
	enum { kPageMain, kPageLevels, kPageOptions };
	enum {
		kBackLevel = Menu::LEVELS_COUNT,  // the levels page's last line
		kLines                            // the longest page
	};
	enum {
		kStartItem,
#ifdef ATARIST
		kLoadItem,
#endif
		kOptionsItem,
		kQuitItem,
		kMainCount
	};
#ifdef ATARIST
	// Load: the saves on disk, one line that cycles through them,
	// starting at the most recent
	enum { kMaxSaves = 64 };
	STSave saves[kMaxSaves];
	int loadSel;
	const int saveCount = ST_scanSaves(saves, kMaxSaves, &loadSel);
#endif
	// the lines left and right change; Load's is on the main page
	enum { kSkillOpt, kScreenOpt, kMusicOpt, kVolumeOpt, kQualityOpt, kHzOpt, kBackOpt, kLoadOpt };
	int optItems[7];
	int optCount = 0;
	optItems[optCount++] = kSkillOpt;
	optItems[optCount++] = kScreenOpt;
#ifdef ATARIST
	optItems[optCount++] = kMusicOpt;
	optItems[optCount++] = kVolumeOpt;
	if (STDL_VoicesOpen()) {
		// the rate of the STE's sampled sound; nothing to choose on a
		// plain ST, or with ste_sound=false
		optItems[optCount++] = kQualityOpt;
	}
	optItems[optCount++] = kHzOpt;
#endif
	optItems[optCount++] = kBackOpt;
	static const int kTextY = 21;
	static const int kTextPitch = 11;
	static const int kLineH = Video::CHAR_H + 1;   // the glyphs and their shadow
	static const int kBand = kW * kLineH;
	static const char *const kScreenNames[kScreenModes] = { "Fit", "Fill", "Overscan Top", "Overscan Full" };
	static const char *const kSkillNames[3] = { "Easy", "Normal", "Expert" };
	// Lines change length as they cycle, and a change of page replaces
	// every one, so the strip of picture under each line is kept and
	// put back before the line is drawn again.
	uint8_t *bands = (uint8_t *)malloc(kLines * kBand);
	if (!bands) {
		error("Failed to allocate the title menu's text bands");
	}
	for (int i = 0; i < kLines; ++i) {
		memcpy(bands + i * kBand, buf + (kTextY + i * kTextPitch) * kW, kBand);
	}
#ifdef ATARIST
	// Music cycles Off, YM and MOD, offering only what is there to play:
	// the YM streams on any ST, the modules on an STE with the voices
	const bool ymInstalled = Mixer::ST_musicInstalled(false);
	const bool modInstalled = Mixer::ST_musicInstalled(true);
#endif
	int page = kPageMain;
	int selected = kStartItem;
	int shown = -1;           // the selection on screen; -1 to draw every line
	uint16_t changed = 0;     // lines whose text changed
	bool quitSelected = false;
	MenuConfirm confirm;
	uint32_t wipeDue = 0;     // when the wipe's next step is due
	while (1) {
		const bool wipeStep = (h <= kH / 2);
		if (wipeStep) {
			if (h == 0) {
				wipeDue = _stub->getTimeStamp();
			}
			// The picture opens from the middle and stays still as it
			// does, so each step puts up only the two rows it gains at
			// either end. Copying the whole open band every time put
			// 28 screens' worth of rows through the ST's conversion,
			// unchanged ones included, and stretched the wipe from
			// under two seconds to seven.
			if (h > 0) {
				_stub->copyRect(0, kH / 2 - h, kW, 2, buf, kW);
				_stub->copyRect(0, kH / 2 + h - 2, kW, 2, buf, kW);
			}
			_stub->updateScreen(0);
			h += 2;
		} else {
			// Only the lines that change are drawn, and only when they
			// do: a full 320x224 chunky-to-planar conversion costs
			// 100ms on a Mega STE and three times that on a plain ST.
			// Redrawing every pass left the loop polling input once
			// per frame, which is long enough to miss a joystick tap
			// or a short keypress outright: several presses could go
			// by before one happened to straddle a poll. Draw when
			// something moves and spend the rest of the time listening.
			const int count = (page == kPageMain) ? (int)kMainCount
				: (page == kPageLevels) ? (int)kLines : optCount;
			if (shown != selected || changed != 0) {
				static const uint8_t selectedColor = 0xE4;
				static const uint8_t defaultColor = 0xE8;
				uint16_t lines = changed;
				if (shown < 0) {
					lines = (1 << kLines) - 1;
				} else if (shown != selected) {
					lines |= (1 << shown) | (1 << selected);
				}
				for (int i = 0; i < kLines; ++i) {
					if (!(lines & (1 << i))) {
						continue;
					}
					const int y = kTextY + i * kTextPitch;
					memcpy(buf + y * kW, bands + i * kBand, kBand);
					if (i >= count) {
						continue;                   // a line this page does not use: picture only
					}
					char label[40];
					const char *str = label;
					if (page == kPageLevels) {
						str = (i == kBackLevel) ? "Back" : Menu::_levelNames[i];
					} else if (page == kPageMain) {
						// START, QUIT and LOAD GAME as the DOS menus have
						// them, in the data's language (QUITTER, END,
						// SALIR, ESCI; CHARGER, LADEN...), and OPTIONS in
						// capitals to match: no menu of the DOS game had
						// one, so there is no translation to take
						str = (i == kQuitItem) ? _res.getMenuString(LocaleData::LI_11_QUIT)
							: (i == kStartItem) ? _res.getMenuString(LocaleData::LI_07_START) : "OPTIONS";
#ifdef ATARIST
						if (i == kLoadItem) {
							// the level's number rather than its name: the
							// longest name and a slot run past the edge.
							// The longest word, Italian's CARICA IL GIOCO,
							// still ends at x=299.
							const char *load = _res.getMenuString(LocaleData::LI_20_LOAD_GAME);
							str = label;
							if (saveCount == 0) {
								snprintf(label, sizeof(label), "%s: No Saved Games", load);
							} else if (saves[loadSel].slot == kIngameSaveSlot) {
								snprintf(label, sizeof(label), "%s: Level %d, Save Point", load, saves[loadSel].level + 1);
							} else {
								snprintf(label, sizeof(label), "%s: Level %d, Slot %02d", load, saves[loadSel].level + 1, saves[loadSel].slot);
							}
						}
#endif
					} else {
						switch (optItems[i]) {
						case kSkillOpt:
							snprintf(label, sizeof(label), "Skill: %s", kSkillNames[_menu._skill]);
							break;
						case kScreenOpt:
							snprintf(label, sizeof(label), "Screen: %s", kScreenNames[g_options.screen]);
							break;
#ifdef ATARIST
						case kMusicOpt: {
								static const char *const kMusicNames[] = { "Music: Off", "Music: YM", "Music: MOD" };
								str = (ymInstalled || modInstalled) ? kMusicNames[Mixer::ST_musicKind()] : "Music: Not Installed";
							}
							break;
						case kVolumeOpt:
							snprintf(label, sizeof(label), "Music Volume: %d%%", g_options.music_volume);
							break;
						case kQualityOpt:
							str = (g_options.sound_quality == kSoundHigh) ? "Sound Quality: High" : "Sound Quality: Normal";
							break;
						case kHzOpt:
							// the rate asked for: an open border holds the
							// display at 50Hz regardless, and this comes in
							// when it closes
							snprintf(label, sizeof(label), "Refresh: %dHz", g_options.refresh_rate);
							break;
#endif
						default:
							str = "Back";
							break;
						}
					}
					const uint8_t color = (selected == i) ? selectedColor : defaultColor;
					// left-aligned with the F of the FLASHBACK logo
					// below: its ink starts at x=13, and the font
					// carries two blank columns before a glyph
					const int x = 11;
					// A one-pixel black shadow down and to the right, as
					// the Amiga and Archimedes menus draw theirs (entry 0
					// is the title palette's black). Every shadow goes
					// first, so none lands on a neighbouring letter's ink.
					for (int j = 0; str[j]; ++j) {
						_vid.AMIGA_drawStringChar(buf, kW, x + 1 + j * Video::CHAR_W, y + 1, _res._fnt, 0, str[j]);
					}
					for (int j = 0; str[j]; ++j) {
						_vid.AMIGA_drawStringChar(buf, kW, x + j * Video::CHAR_W, y, _res._fnt, color, str[j]);
					}
				}
				// the picture behind the names is untouched, so blit
				// the lines that changed rather than the screen
				if (shown < 0) {
					_stub->copyRect(0, kTextY, kW, (kLines - 1) * kTextPitch + kLineH, buf, kW);
				} else {
					for (int i = 0; i < kLines; ++i) {
						if (lines & (1 << i)) {
							_stub->copyRect(0, kTextY + i * kTextPitch, kW, kLineH, buf, kW);
						}
					}
				}
				_stub->updateScreen(0);
				shown = selected;
				changed = 0;
			}
			if (_stub->_pi.dirMask & PlayerInput::DIR_UP) {
				_stub->_pi.dirMask &= ~PlayerInput::DIR_UP;
				if (selected > 0) {
					--selected;
				}
			}
			if (_stub->_pi.dirMask & PlayerInput::DIR_DOWN) {
				_stub->_pi.dirMask &= ~PlayerInput::DIR_DOWN;
				if (selected < count - 1) {
					++selected;
				}
			}
			// Left, right or fire on a setting cycles it, and the change
			// is made there and then: the menu is a static screen, the
			// one place a mode change costs nothing but a repaint.
			int step = 0;
			int item = (page == kPageOptions) ? optItems[selected] : -1;
#ifdef ATARIST
			if (page == kPageMain && selected == kLoadItem) {
				item = kLoadOpt;
			}
#endif
			if (item >= 0 && item != kBackOpt) {
				if (_stub->_pi.dirMask & PlayerInput::DIR_LEFT) {
					step = -1;
				} else if (_stub->_pi.dirMask & PlayerInput::DIR_RIGHT) {
					step = 1;
				}
				_stub->_pi.dirMask &= ~(PlayerInput::DIR_LEFT | PlayerInput::DIR_RIGHT);
			}
			if (step != 0) {
				switch (item) {
				case kSkillOpt:
					// The Amiga screen never offered this: the port pinned
					// the skill at Normal, and Expert was unreachable.
					_menu._skill = (_menu._skill + 3 + step) % 3;
					break;
				case kScreenOpt: {
						// Each border transition reshapes the surface, so the
						// whole picture goes back up, not just the line.
						int want = (g_options.screen + kScreenModes + step) % kScreenModes;
#ifdef ATARIST
						// skipping modes this machine lacks the RAM for
						// (kOverscanMinK)
						while (!ST_screenModeAvailable(want)) {
							want = (want + kScreenModes + step) % kScreenModes;
						}
#endif
						g_options.screen = _stub->setScreenMode(want);
						shown = -1;
						_stub->copyRect(0, 0, kW, kH, buf, kW);
						_stub->updateScreen(0);
					}
					break;
#ifdef ATARIST
				case kMusicOpt:
					// Off, YM and MOD in turn, skipping a kind not
					// installed; the title plays the menu track, so the
					// change is heard
					if (ymInstalled || modInstalled) {
						const int now = Mixer::ST_musicKind();
						int want = now;
						do {
							want = (want + 3 + step) % 3;
						} while ((want == Mixer::kMusicYm && !ymInstalled) || (want == Mixer::kMusicMod && !modInstalled));
						if (want != now) {
							_mix.stopMusic();
							g_options.music = (want != Mixer::kMusicOff);
							if (g_options.music) {
								g_options.music_mod = (want == Mixer::kMusicMod);
								_mix.playMusic(1);
							}
						}
					}
					break;
				case kLoadOpt:
					// to the next save on disk, either way round
					if (saveCount > 1) {
						loadSel = (loadSel + saveCount + step) % saveCount;
					}
					break;
				case kVolumeOpt: {
						// tens, wrapping, so fire alone reaches every level;
						// a figure from RS.CFG between them rounds to the next
						int q = (step > 0) ? g_options.music_volume / 10 + 1 : (g_options.music_volume + 9) / 10 - 1;
						if (q > 10) {
							q = 0;
						} else if (q < 0) {
							q = 10;
						}
						_mix.ST_setMusicVolume(q * 10);
					}
					break;
				case kQualityOpt:
					// Normal and High in turn. The device reopens at the
					// new rate, which takes the music with it, so the
					// menu track starts again - at the new rate, heard.
					_mix.stopMusic();
					Mixer::ST_setSoundQuality((g_options.sound_quality == kSoundHigh) ? kSoundNormal : kSoundHigh);
					if (g_options.music) {
						_mix.playMusic(1);
					}
					break;
				case kHzOpt:
					g_options.refresh_rate = (g_options.refresh_rate == 60) ? 50 : 60;
					STDL_SetRefresh(g_options.refresh_rate);
					break;
#endif
				}
				changed |= 1 << selected;
			}
			if (page == kPageLevels && selected < Menu::LEVELS_COUNT) {
				_currentLevel = selected;
			}
		}
		_stub->processEvents();
		if (_stub->_pi.quit) {
			break;
		}
		const bool back = (page != kPageMain) && (_stub->_pi.escape || _stub->_pi.backspace);
		if (back) {
			_stub->_pi.escape = false;
			_stub->_pi.backspace = false;
		}
		if (back || confirm(_stub->_pi)) {
			if (page == kPageLevels) {
				if (back || selected == kBackLevel) {
					page = kPageMain;
					selected = kStartItem;
					shown = -1;
					continue;
				}
				break;                        // play _currentLevel
			}
			if (page == kPageOptions) {
				if (!back && optItems[selected] != kBackOpt) {
					_stub->_pi.dirMask |= PlayerInput::DIR_RIGHT;   // fire cycles it too
					continue;
				}
				page = kPageMain;
				selected = kOptionsItem;
				shown = -1;
				continue;
			}
			if (selected == kStartItem) {
				// on the level last played: the first on a fresh
				// start, so Start and fire begin a new game
				page = kPageLevels;
				selected = _currentLevel;
				shown = -1;
				continue;
			}
			if (selected == kOptionsItem) {
				page = kPageOptions;
				selected = 0;
				shown = -1;
				continue;
			}
#ifdef ATARIST
			if (selected == kLoadItem) {
				if (saveCount == 0) {
					continue;                 // nothing to load
				}
				_currentLevel = saves[loadSel].level;
				_stLoadSlot = saves[loadSel].slot;
				info("Loading level %d slot %d from the title", _currentLevel + 1, _stLoadSlot);
				break;
			}
#endif
			quitSelected = (selected == kQuitItem);
			break;
		}
		if (wipeStep) {
			// the steps 30ms apart whatever one takes to draw: a plain
			// ST's conversion added a third to the wipe on top of its
			// sleeps
			wipeDue += 30;
			const int32_t left = (int32_t)(wipeDue - _stub->getTimeStamp());
			if (left > 0) {
				_stub->sleep(left);
			}
		} else {
			_stub->sleep(30);
		}
	}
	free(bands);
	if (quitSelected) {
		_stub->_pi.quit = true;
	}
	free(buf);
}

void Game::resetGameState() {
	_animBuffers._states[0] = _animBuffer0State;
	_animBuffers._curPos[0] = 0xFF;
	_animBuffers._states[1] = _animBuffer1State;
	_animBuffers._curPos[1] = 0xFF;
	_animBuffers._states[2] = _animBuffer2State;
	_animBuffers._curPos[2] = 0xFF;
	_animBuffers._states[3] = _animBuffer3State;
	_animBuffers._curPos[3] = 0xFF;
	_currentRoom = _res._pgeInit[0].init_room;
	_cut._deathCutsceneId = 0xFFFF;
	_deathCutsceneCounter = 0;
	_saveStateCompleted = false;
	_loadMap = true;
	pge_resetMessages();
	_blinkingConradCounter = 0;
	_pge_deathAck = false;
	_pge_opGunVar = 0;
	_textToDisplay = 0xFFFF;
	_pge_zoomPiegeNum = 0;
	_pge_zoomCounter = 0;
	_pge_zoomX = _pge_zoomY = 0;
}

void Game::mainLoop() {
#ifdef ATARIST
	// a recording is gameplay only: what follows a cutscene, a death
	// or a level change depends on more than the keymask carries
	if (_stRecording && (_cut._id != 0xFFFF || _deathCutsceneCounter != 0 || _currentLevel != _stRecHdr.level)) {
		ST_recFinish("a cutscene, a death or a new level");
	}
	if (_stReplaying && _stRecPos >= _stRecHdr.steps) {
		ST_replayDone();       // where the recording stopped, to the step
		return;
	}
#endif
	playCutscene();
	if (_cut._id == 0x3D) {
		showFinalScore();
		_endLoop = true;
		return;
	}
	if (_deathCutsceneCounter) {
		--_deathCutsceneCounter;
		if (_deathCutsceneCounter == 0) {
			playCutscene(_cut._deathCutsceneId);
			if (!handleContinueAbort()) {
				playCutscene(0x41);
				_endLoop = true;
			} else {
				if (_autoSave && _rewindLen != 0 && loadGameState(kAutoSaveSlot)) {
					// autosave
				} else if (_validSaveState && loadGameState(kIngameSaveSlot)) {
					// ingame save
				} else {
					clearStateRewind();
					loadLevelData();
					resetGameState();
				}
			}
			return;
		}
	}
#ifdef ATARIST
	// The engine advances the game once per drawn frame, so a machine
	// that draws in 43ms plays at 76% speed: Conrad walked, fell and
	// shot in slow motion on an STE. Keep the game at 30 steps a second
	// instead: updateTiming measures how far a frame ends behind its
	// deadline, and once a whole step is owed the logic runs twice for
	// one draw. One catch-up per frame at most, so the shown rate
	// never drops below half the step rate; the cutscene player
	// holds its pace the same way (Cutscene::stDecideSkip).
	// Not while recording: a replay runs one step a frame, and the
	// recording has to have done the same to be replayed
	if (_logicDebt >= kLogicStepMs && !_stRecording) {
		_logicDebt -= kLogicStepMs;
		_stDeadline += stNextStep();      // the catch-up takes a step of the schedule
		++_logicCatchUps;
		if (!stepLogic()) {
			return;
		}
		if (_blinkingConradCounter != 0) {
			--_blinkingConradCounter;
		}
	}
	_vid.ST_frameBegin();
#else
	memcpy(_vid._frontLayer, _vid._backLayer, _vid._layerSize);
#endif
	if (!stepLogic()) {
#ifdef ATARIST
		_vid.ST_frameAbort();
#endif
		return;
	}
	if (_res.isDOS() && (_stub->_pi.dbgMask & PlayerInput::DF_AUTOZOOM) != 0) {
		pge_updateZoom();
	}
	prepareAnims();
#ifdef ATARIST
	ST_placeIcon();
#endif
	drawAnims();
	drawCurrentInventoryItem();
	drawLevelTexts();
	if (g_options.enable_password_menu) {
		printLevelCode();
	}
#ifdef ATARIST
	if (_stPaletteMsgCounter != 0) {
		// Ctrl+P's confirmation, drawn a frame at a time for a
		// couple of seconds like the level code
		--_stPaletteMsgCounter;
		_vid.drawString(_stPaletteMsg, (Video::GAMESCREEN_W - strlen(_stPaletteMsg) * Video::CHAR_W) / 2, TEXT_ROW(16), 0xE7);
	}
#endif
	if (_blinkingConradCounter != 0) {
		--_blinkingConradCounter;
	}
#ifdef ATARIST
	_vid.ST_frameEnd();
	if (_stReplaying && g_options.replay_hash) {
		ST_replayHashFrame(_vid._frontLayer);
	}
#endif
	_vid.updateScreen();
	updateTiming();
	drawStoryTexts();
	if (_stub->_pi.backspace) {
		_stub->_pi.backspace = false;
		handleInventory();
		return;
	}
	if (_stub->_pi.escape) {
		_stub->_pi.escape = false;
#ifdef ATARIST
		if (_stRecording) {
			ST_recFinish("Escape");
		}
#endif
		if (_demoBin != -1 || handleConfigPanel()) {
			_endLoop = true;
			return;
		}
	}
	inp_handleSpecialKeys();
	if (_autoSave && _stub->getTimeStamp() - _saveTimestamp >= kAutoSaveIntervalMs) {
		// do not save if we died or about to
		if (_pgeLive[0].life > 0 && _deathCutsceneCounter == 0) {
			saveGameState(kAutoSaveSlot);
			_saveTimestamp = _stub->getTimeStamp();
		}
	}
}

// One step of the game: input, collision setup, every live object's
// script. Returns false when the caller must return without drawing
// (a level change or a cutscene queued).
bool Game::stepLogic() {
	pge_getInput();
	pge_prepare();
	col_prepareRoomState();
	uint8_t oldLevel = _currentLevel;
#ifdef ATARIST
	{
		// only the slots in use, the byte read again after each object
		// (see pge_firstLive)
		const int pgeNum = _res._pgeNum;
		for (int b = 0; b < pgeNum; b += 8) {
			unsigned bits = _pge_liveBits[b >> 3];
			while (bits) {
				const int k = pge_firstLive(bits);
				if (b + k >= pgeNum) {
					break;
				}
				LivePGE *pge = _pge_liveTable2[b + k];
				if (pge) {
					_col_currentPiegeGridPosY = (pge->pos_y / 36) & ~1;
					_col_currentPiegeGridPosX = (pge->pos_x + 8) >> 4;
					pge_process(pge);
				}
				bits = _pge_liveBits[b >> 3] & (0x7F >> k);
			}
		}
	}
#else
	{
		// walking pointer and a counter in registers, as pge_prepare
		// does: indexed over a member count, gcc 4.6 reloads both the
		// table address and the count every iteration
		LivePGE **p = _pge_liveTable2;
		for (int n = _res._pgeNum; --n >= 0; ++p) {
			LivePGE *pge = *p;
			if (pge) {
				_col_currentPiegeGridPosY = (pge->pos_y / 36) & ~1;
				_col_currentPiegeGridPosX = (pge->pos_x + 8) >> 4;
				pge_process(pge);
			}
		}
	}
#endif
	if (oldLevel != _currentLevel) {
		if (_res._isDemo) {
			_currentLevel = oldLevel;
		}
		changeLevel();
		_pge_opGunVar = 0;
		return false;
	}
	if (_cut._id != 0xFFFF) {
		// do not draw room level background when switching between cutscenes
		return false;
	}
	if (_loadMap) {
		if (_currentRoom == 0xFF || !hasLevelRoom(_currentLevel, _pgeLive[0].room_location)) {
			_cut._id = 6;
			_deathCutsceneCounter = 1;
		} else {
			_currentRoom = _pgeLive[0].room_location;
#ifdef ATARIST
			// timed for the log: a room's first visit is where the
			// unpacking and tile decoding show
			const uint32_t roomT0 = _stub->getTimeStamp();
			loadLevelRoom();
			info("Room %d (%dms)", _currentRoom, (int)(_stub->getTimeStamp() - roomT0));
#else
			info("Room %d", _currentRoom);
			loadLevelRoom();
#endif
#ifdef ATARIST
			if (_stPaletteReloaded) {
				// Ctrl+Shift+P's confirmation, once the room is back
				_stPaletteReloaded = false;
				const char *name = ST_paletteInUse();
				snprintf(_stPaletteMsg, sizeof(_stPaletteMsg), "PALETTE %s", name[0] ? name : "AUTOMATIC");
				_stPaletteMsgCounter = 50;
			}
			if (_stLogMemRoom) {
				// after the level's first room: decoding it is what
				// allocates the tile pool, so this is the level at
				// its fullest
				_stLogMemRoom = false;
				ST_logFreeMemory("level loaded");
			}
#endif
			_loadMap = false;
			_vid.fullRefresh();
		}
	}
	return true;
}

void Game::updateTiming() {
#ifdef ATARIST
	if (_stReplaying) {
		ST_replayFrame();
		return;
	}
	// Benchmark: a fixed number of gameplay frames with no input, no
	// frame limiter and a fixed seed, timed on the ST's own 200Hz
	// counter so the figure is emulated time and fast-forward does
	// not distort it. Prints one line and quits.
	if (g_options.bench) {
		enum { kBenchFrames = 512 };
		static int frames;
		static uint32_t t0;
		if (frames == 0) {
			t0 = _stub->getTimeStamp();
		}
		if (++frames > kBenchFrames) {
			const uint32_t d = _stub->getTimeStamp() - t0;
			info("BENCH complete, quitting: %d frames %d ms, %d.%02d ms/frame, %d.%d fps",
				kBenchFrames, (int)d,
				(int)(d / kBenchFrames), (int)((d * 100 / kBenchFrames) % 100),
				(int)(kBenchFrames * 1000 / d), (int)((kBenchFrames * 10000 / d) % 10));
			_stub->_pi.quit = true;
		}
		return;
	}
#endif
#ifdef ATARIST
	// log_fps: average over 64 frames, to RS.LOG. The engine asks for
	// 30Hz and sleeps when it is ahead, so anything below 30 is what
	// the machine actually managed.
	if (g_options.log_fps) {
		static int frames;
		static uint32_t t0;
		// the longest frame of the window, from the end of the last
		// one's wait to here: an average hides the frame that hitched
		static uint32_t worst;
		static uint16_t gen0;
		const uint32_t now = _stub->getTimeStamp();
		if (now - _frameTimestamp > worst) {
			worst = now - _frameTimestamp;
		}
		if (++frames == 64) {
			const uint32_t d = now - t0;
			if (t0 != 0 && d != 0) {
				// the miss count rides along: a border that drops
				// under load shows here rather than only in the
				// per-cutscene line
				// steps = frames + catch-ups: the game's own pace
				char perf[160];
				ST_perfFormat(g_stPerf, perf, sizeof(perf));
				info("fps %d.%d (%d ms/frame) catch-ups %u misses %u worst %dms remap %u pge %d | %s",
					(int)(64000 / d), (int)((640000 / d) % 10), (int)(d / 64),
					(unsigned)_logicCatchUps, (unsigned)ST_overscanMisses(),
					(int)worst, (unsigned)(uint16_t)(ST_remapGen() - gen0), ST_livePges(), perf);
			}
			memset(&g_stPerf, 0, sizeof(g_stPerf));
			t0 = now;
			frames = 0;
			worst = 0;
			gen0 = ST_remapGen();
			_logicCatchUps = 0;
		}
	}
#endif
#ifdef ATARIST
	// Pace to a deadline rather than a pause. The clock is the 200Hz
	// counter, so time comes in 5ms ticks and 33.3ms is not one of
	// them: sleeping "33ms less this frame's work" rounded every pause
	// up to a whole tick and a machine with time to spare ran at
	// 28.5fps, the game 5% slow. The deadline moves on 30, 35 and 35ms
	// in turn - 100ms for three steps, exactly 30 a second - and a
	// frame done early sleeps until it, so the rate is exact over any
	// three frames and each wait ends on a tick.
	if (!(_stub->_pi.dbgMask & PlayerInput::DF_FASTMODE)) {
		const uint32_t now = _stub->getTimeStamp();
		_stDeadline += stNextStep();
		const int32_t behind = (int32_t)(now - _stDeadline);
		if (behind > kLogicStallMs) {
			// a load, a cutscene or a menu, not gameplay: the schedule
			// starts again from here and nothing is made up
			_stDeadline = now;
			_logicDebt = 0;
		} else if (behind > 0) {
			// Late: mainLoop runs a catch-up step once a whole step is
			// owed. Two at most, so a machine that cannot keep pace
			// even with catch-ups slows down instead of falling ever
			// further behind.
			if (behind > 2 * kLogicStepMs) {
				_stDeadline = now - 2 * kLogicStepMs;
			}
			_logicDebt = (int32_t)(now - _stDeadline);
		} else {
			_logicDebt = 0;
			_stub->sleep(-behind);
		}
		_frameTimestamp = _stub->getTimeStamp();
		return;
	}
#endif
	static const int frameHz = 30;
	int32_t delay = _stub->getTimeStamp() - _frameTimestamp;
	int32_t pause = (_stub->_pi.dbgMask & PlayerInput::DF_FASTMODE) ? 20 : (1000 / frameHz);
	pause -= delay;
	if (pause > 0) {
		_stub->sleep(pause);
	}
	_frameTimestamp = _stub->getTimeStamp();
}

#ifdef ATARIST
// the objects live now, for the log_fps line
int Game::ST_livePges() const {
	int n = 0;
	for (int i = 0; i < _res._pgeNum; ++i) {
		if (_pge_liveTable2[i]) {
			++n;
		}
	}
	return n;
}

// The game's own state, for telling whether a replay did what its
// recording did: every object's fields up to its pointers, where it
// is, the seed, the score and the level's collision data.
uint32_t Game::ST_stateHash() const {
	uint32_t h = 0x9E3779B9;
	enum { kPgeWords = offsetof(LivePGE, next_PGE_in_room) / 2 };
	for (int i = 0; i < _res._pgeNum; ++i) {
		const uint16_t *w = (const uint16_t *)&_pgeLive[i];
		for (int k = 0; k < kPgeWords; ++k) {
			h = rotHash(h, w[k]);
		}
	}
	h = rotHash(h, _currentRoom);
	h = rotHash(h, _currentLevel);
	h = rotHash(h, _randSeed);
	h = rotHash(h, _score);
	h = rotHash(h, _pge_opGunVar);
	const uint16_t *ct = (const uint16_t *)_res._ctData;
	for (int i = 0; i < (int)(sizeof(_res._ctData) / 2); ++i) {
		h = rotHash(h, ct[i]);
	}
	return h;
}

void Game::ST_recStart() {
	if (!_stRecBuf) {
		_stRecBuf = (uint8_t *)malloc(kRecMaxSteps);
		if (!_stRecBuf) {
			warning("Record: no memory for the recording");
			return;
		}
	}
	memset(&_stRecHdr, 0, sizeof(_stRecHdr));
	_stRecHdr.tag = TAG_RSRC;
	_stRecHdr.version = kRecVersion;
	_stRecHdr.level = _currentLevel;
	_stRecHdr.slot = (uint8_t)_stLoadSlot;
	_stRecHdr.skill = _skillLevel;
	_stRecHdr.cheats = (uint8_t)_cheats;
	_stRecHdr.seed = _randSeed;
	_stRecHdr.startHash = ST_stateHash();
	_stRecPos = 0;
	_stRecording = true;
	info("Record: from level %d slot %d into REC%02d.DEM", _currentLevel + 1, _stLoadSlot, g_options.record);
}

// One inp_update's worth: kept while recording, supplied while
// replaying - after processEvents either way, so a replay overrides
// whatever the keyboard said.
void Game::ST_recStep() {
	PlayerInput &pi = _stub->_pi;
	if (_stReplaying) {
		if (_stRecPos >= _stRecHdr.steps) {
			// ran dry inside a menu or the inventory: end it there
			ST_replayDone();
			return;
		}
		setKeymask(pi, _stRecBuf[_stRecPos++]);
		return;
	}
	if (_stRecPos >= kRecMaxSteps) {
		ST_recFinish("the recording is full");
		return;
	}
	uint8_t m = pi.dirMask & 0xF;
	if (pi.enter) {
		m |= 0x10;
	}
	if (pi.space) {
		m |= 0x20;
	}
	if (pi.shift) {
		m |= 0x40;
	}
	if (pi.backspace) {
		m |= 0x80;
	}
	_stRecBuf[_stRecPos++] = m;
}

void Game::ST_recFinish(const char *why) {
	_stRecording = false;
	_stRecHdr.steps = _stRecPos;
	_stRecHdr.endHash = ST_stateHash();
	char name[16];
	snprintf(name, sizeof(name), "REC%02d.DEM", g_options.record);
	FILE *fp = fopen(name, "wb");
	bool ok = fp != 0;
	if (fp) {
		ok = fwrite(&_stRecHdr, sizeof(_stRecHdr), 1, fp) == 1
		     && fwrite(_stRecBuf, 1, _stRecPos, fp) == _stRecPos;
		ok = (fclose(fp) == 0) && ok;
	}
	info("RECORD done at %s: %s, %u steps, hash %08lx%s", why, name,
		(unsigned)_stRecPos, (unsigned long)_stRecHdr.endHash, ok ? "" : ", NOT WRITTEN");
}

bool Game::ST_replayLoad() {
	char name[16];
	snprintf(name, sizeof(name), "REC%02d.DEM", g_options.replay);
	FILE *fp = fopen(name, "rb");
	if (!fp) {
		warning("Replay: %s not found", name);
		return false;
	}
	bool ok = fread(&_stRecHdr, sizeof(_stRecHdr), 1, fp) == 1
	          && _stRecHdr.tag == TAG_RSRC && _stRecHdr.version == kRecVersion
	          && _stRecHdr.steps <= kRecMaxSteps;
	if (ok) {
		_stRecBuf = (uint8_t *)malloc(_stRecHdr.steps + 1);
		ok = _stRecBuf && fread(_stRecBuf, 1, _stRecHdr.steps, fp) == _stRecHdr.steps;
	}
	fclose(fp);
	if (!ok) {
		warning("Replay: %s is not a recording this build can play", name);
		return false;
	}
	info("Replay: %s, level %d slot %d, %u steps", name, _stRecHdr.level + 1, _stRecHdr.slot, (unsigned)_stRecHdr.steps);
	return true;
}

// The replay's clock: one call a frame, from updateTiming, which does
// no pacing while a replay runs. 5ms resolution, like every ST time.
void Game::ST_replayFrame() {
	const uint32_t now = _stub->getTimeStamp();
	const uint32_t d = now - g_repLast;
	g_repLast = now;
	++g_repFrames;
	g_repMs += d;
	if (d > g_repWorst) {
		g_repWorst = d;
	}
	int b = (int)(d / 5);
	if (b >= kRepBuckets) {
		b = kRepBuckets - 1;
	}
	++g_repHist[b];
	++g_repCur.frames;
	g_repCur.ms += d;
	if (d > g_repCur.worst) {
		g_repCur.worst = (uint16_t)d;
	}
	if (g_repCur.frames == kRepWinFrames) {
		ST_replayCloseWindow(_currentRoom);
	}
}

void Game::ST_replayDone() {
	_stReplaying = false;
	PlayerInput &pi = _stub->_pi;
	pi.dirMask = 0;
	pi.enter = pi.space = pi.shift = pi.backspace = false;
	pi.quit = true;
	const uint32_t endHash = ST_stateHash();
	if (g_repCur.frames != 0) {
		ST_replayCloseWindow(_currentRoom);
	}
	uint32_t steadyMs = 0, steadyFrames = 0;
	for (int i = 0; i < g_repWins; ++i) {
		const STRepWindow &w = g_repWin[i];
		char perf[160];
		ST_perfFormat(w.perf, perf, sizeof(perf));
		info("REPLAY window %d room %d: %d frames %d.%02d ms/frame worst %dms hash %08lx | %s",
			i, w.room, w.frames, (int)(w.ms / w.frames), (int)((w.ms * 100 / w.frames) % 100),
			w.worst, (unsigned long)w.hash, perf);
		if (i != 0) {          // the first holds the room's load
			steadyMs += w.ms;
			steadyFrames += w.frames;
		}
	}
	char hist[kRepBuckets * 12 + 1];
	int len = 0;
	for (int b = 0; b < kRepBuckets; ++b) {
		if (g_repHist[b]) {
			len += snprintf(hist + len, sizeof(hist) - len, " %d:%u", b * 5, (unsigned)g_repHist[b]);
		}
	}
	hist[len] = 0;
	info("REPLAY frame times, 5ms buckets from:%s", hist);
	if (g_repFrameHash) {
		FILE *fp = fopen("FHASH.BIN", "wb");
		if (fp) {
			const uint32_t n = (g_repFrames < g_repFrameHashCap) ? g_repFrames : g_repFrameHashCap;
			fwrite(g_repFrameHash, sizeof(uint32_t), n, fp);
			fclose(fp);
		}
	}
	const uint32_t f = g_repFrames ? g_repFrames : 1;
	const uint32_t sf = steadyFrames ? steadyFrames : 1;
	info("REPLAY done: %u steps %u frames %u ms, %d.%02d ms/frame, steady %d.%02d, worst %ums, hash %08lx %s",
		(unsigned)_stRecHdr.steps, (unsigned)g_repFrames, (unsigned)g_repMs,
		(int)(g_repMs / f), (int)((g_repMs * 100 / f) % 100),
		(int)(steadyMs / sf), (int)((steadyMs * 100 / sf) % 100),
		(unsigned)g_repWorst, (unsigned long)endHash,
		endHash == _stRecHdr.endHash ? "match" : "DIVERGED");
}

// the next step of the schedule, 6 or 7 ticks of the 200Hz clock:
// 30, 35, 35ms, which is 20 ticks for three steps
int Game::stNextStep() {
	static const uint8_t kStepMs[3] = { 30, 35, 35 };
	const int ms = kStepMs[_stStepPhase];
	_stStepPhase = (uint8_t)((_stStepPhase == 2) ? 0 : _stStepPhase + 1);
	return ms;
}
#endif

void Game::playCutscene(int id) {
	if (id != -1) {
		_cut._id = id;
	}
	if (_cut._id != 0xFFFF) {
		info("Cutscene %02X start", _cut._id);
#ifdef ATARIST
		// A cutscene takes about 140K for its caches and pages. On a
		// 2.5MB machine the sprite bakes and kept rooms can leave less
		// than that: they go, and come back as play needs them.
		if (ST_freeK() < 200) {
			ST_releaseRamCaches();
		}
#endif
		ToggleWidescreenStack tws(_stub, false);
		_mix.stopMusic();
		if (_res._hasSeqData) {
			int num = 0;
			switch (_cut._id) {
			case 0x02: {
					static const uint8_t tab[] = { 1, 2, 1, 3, 3, 4, 4 };
					num = tab[_currentLevel];
				}
				break;
			case 0x05: {
					static const uint8_t tab[] = { 1, 2, 3, 5, 5, 4, 4 };
					num = tab[_currentLevel];
				}
				break;
			case 0x0A: {
					static const uint8_t tab[] = { 1, 2, 2, 2, 2, 2, 2 };
					num = tab[_currentLevel];
				}
				break;
			case 0x10: {
					static const uint8_t tab[] = { 1, 1, 1, 2, 2, 3, 3 };
					num = tab[_currentLevel];
				}
				break;
			case 0x3C: {
					static const uint8_t tab[] = { 1, 1, 1, 1, 1, 2, 2 };
					num = tab[_currentLevel];
				}
				break;
			case 0x40:
				return;
			case 0x4A:
				return;
			}
			if (SeqPlayer::_namesTable[_cut._id]) {
			        char name[16];
			        snprintf(name, sizeof(name), "%s.SEQ", SeqPlayer::_namesTable[_cut._id]);
				char *p = strchr(name, '0');
				if (p) {
					*p += num;
				}
			        if (playCutsceneSeq(name)) {
					if (_cut._id == 0x3D) {
						playCutsceneSeq("CREDITS.SEQ");
						_cut._interrupted = false;
					} else {
						_cut._id = 0xFFFF;
					}
					_mix.stopMusic();
					return;
				}
			}
		}
		if (_res.isAmiga()) {
			const int num = Cutscene::_musicTableAmiga[_cut._id * 2];
			if (num != 0xFF) {
				const int bpm = Cutscene::_musicTableAmiga[_cut._id * 2 + 1];
				_mix.playMusic(num, bpm);
			}
		} else {
			const int num = Cutscene::_musicTableDOS[_cut._id];
			if (num != 0xFF) {
				_mix.playMusic(num);
			}
		}
		_cut.play();
		if (id == 0xD && !_cut._interrupted) {
			if (g_features->has_extended_intro) {
				_cut._id = 0x4A; // second part of the introduction cutscene
				_cut.play();
			}
		}
		if (_res.isMac() && !(id == 0x48 || id == 0x49)) { // continue or score screens
			// restore palette entries modified by the cutscene player (0xC and 0xD)
			Color palette[32];
			_res.MAC_copyClut16(palette, 0, 0x37);
			_res.MAC_copyClut16(palette, 1, 0x38);
			for (int i = 0; i < 32; ++i) {
				_stub->setPaletteEntry(0xC0 + i, &palette[i]);
			}
		}
		if (_cut._id == 0x3D) {
			_mix.playMusic(Mixer::MUSIC_TRACK + 9);
			_cut.playCredits();
		}
		_mix.stopMusic();
#ifdef ATARIST
		uint32_t segHits, segMisses, shpDrawn, shpBaked;
		ST_segCacheStats(&segHits, &segMisses);
		ST_shpStats(&shpDrawn, &shpBaked);
		info("Cutscene done: %u frames, %u late, %u skipped, %u unshown, work %ums (copy %ums) of %ums budget (overscan misses %u), ready after %ums, polygons from the cache %u of %u, shapes as sprites %u (%u baked)",
			_cut._statFrames, _cut._statLate, _cut._statSkipped, _cut._statNoShow,
			(unsigned)_cut._statWork, (unsigned)_cut._statCopy,
			(unsigned)_cut._statBudget,
			(unsigned)ST_overscanMisses(), (unsigned)_cut._statLoad,
			(unsigned)segHits, (unsigned)(segHits + segMisses),
			(unsigned)shpDrawn, (unsigned)shpBaked);
#else
		info("Cutscene done");
#endif
	}
}

bool Game::playCutsceneSeq(const char *name) {
	File f;
	if (f.open(name, "rb", _fs)) {
		_seq.setBackBuffer(_res._scratchBuffer);
		_seq.play(&f);
		_vid.fullRefresh();
		return true;
	}
	return false;
}

void Game::inp_handleSpecialKeys() {
	if (_stub->_pi.dbgMask & PlayerInput::DF_SETLIFE) {
		_pgeLive[0].life = 0x7FFF;
	}
	if (_stub->_pi.load) {
		loadGameState(_stateSlot);
		_stub->_pi.load = false;
	}
#ifdef ATARIST
	// Ctrl+P (palette_custom): the room's colours out to a .hex
	if (_stub->_pi.dumpPalette) {
		_stub->_pi.dumpPalette = false;
		const char *name = ST_paletteDump(_currentLevel + 1, _res._levNum, _currentRoom);
		if (name) {
			snprintf(_stPaletteMsg, sizeof(_stPaletteMsg), "SAVED %s", name);
			_stPaletteMsgCounter = 50;
		}
	}
	// Ctrl+Up / Ctrl+Down: in Fill, slide the 200-row window over the
	// 224-row picture, to find where it best sits. Up shows more of the
	// top, Down more of the bottom; the rows shown go to RS.LOG.
	if (_stub->_pi.reloadPalette) {
		// Ctrl+Shift+P: the room is loaded again, which picks up
		// whatever PALETTE\ now holds for it
		_stub->_pi.reloadPalette = false;
		if (ST_paletteReload()) {
			_stPaletteReloaded = true;
			_loadMap = true;
		}
	}
	if (_stub->_pi.panUp || _stub->_pi.panDown) {
		const int top = _stub->panScreen(_stub->_pi.panUp ? -1 : 1);
		_stub->_pi.panUp = _stub->_pi.panDown = false;
		if (top >= 0) {
			info("Fill shows rows %d-%d", top, top + 199);
			_vid.fullRefresh();
		}
	}
#endif
	if (_stub->_pi.save) {
		saveGameState(_stateSlot);
		_stub->_pi.save = false;
	}
	if (_stub->_pi.stateSlot != 0) {
		int8_t slot = _stateSlot + _stub->_pi.stateSlot;
		if (slot >= 1 && slot < 100) {
			_stateSlot = slot;
			info("Current game state slot is %d", _stateSlot);
		}
		_stub->_pi.stateSlot = 0;
	}
	if (_stub->_pi.rewind) {
		if (_rewindLen != 0) {
			loadStateRewind();
		} else {
			info("Rewind buffer is empty");
		}
		_stub->_pi.rewind = false;
	}
}

#ifdef ATARIST
// The inventory icon lives in the room's background layer, put there
// only when it changes: from there every restore puts it back, and a
// frame where nothing changed costs nothing. Drawn afresh every frame
// it cost ~3ms on an STE - a priority-marking blit takes STDL's CPU
// route - plus the restore and the copy of its blocks. A new room
// rebuilds the background without it; the patch under it is kept so
// that a change of item, or none, puts the scenery back. This runs
// before the sprites, so copying the change onto the visible layer
// cannot wipe one.
void Game::ST_placeIcon() {
	// Placed for the mode. Fit drops rows 9 and 20, both inside the
	// engine's y=8; from 10 it spans 10-25 and loses only row 20, low
	// in the middle, where a missing line hides in the shading. Fill in
	// play starts at row 18, so there it sits just under that edge; the
	// overscan modes show every row and keep the engine's 8.
	const int iconY = (g_options.screen == kScreenFill) ? 19 : (g_options.screen == kScreenFit) ? 10 : 8;
	const uint16_t src = _pgeLive[0].current_inventory_PGE;
	const int want = (src != 0xFF) ? _res._pgeInit[src].icon_num : -1;
	if (_vid._stBackGen != _stIconGen) {
		_stIconGen = _vid._stBackGen;
		_stIconShown = -1;
	}
	if (want == _stIconShown && iconY == _stIconY) {
		return;
	}
	const int oldY = (_stIconShown >= 0) ? _stIconY : -1;
	if (oldY >= 0) {
		ST_layerLoadRect(_vid._backLayer, 232, oldY, 16, _stIconPatch);
	}
	if (want >= 0) {
		ST_layerSaveRect(_vid._backLayer, 232, iconY, 16, _stIconPatch);
		drawIcon(want, 232, iconY, 0xA, _vid._backLayer);
	}
	if (oldY >= 0) {
		ST_layerCopyRect(_vid._frontLayer, _vid._backLayer, 232, oldY, 16);
		_vid.ST_backChanged(232, oldY, 16, 16);
	}
	if (want >= 0 && iconY != oldY) {
		ST_layerCopyRect(_vid._frontLayer, _vid._backLayer, 232, iconY, 16);
		_vid.ST_backChanged(232, iconY, 16, 16);
	}
	_stIconShown = want;
	_stIconY = iconY;
}
#endif

void Game::drawCurrentInventoryItem() {
#ifdef ATARIST
	// The icon is already in place (ST_placeIcon). The engine draws it
	// over everything, so it goes back on top only where a sprite was
	// drawn across it this frame.
	// With the display list the icon is drawn every frame: unchanged, it
	// costs nothing, and when a sprite crosses it the list redraws it.
	if (_stIconShown >= 0 && (_vid.ST_dlOn() || _vid.ST_blocksDirty(232, _stIconY, 16, 16))) {
		drawIcon(_stIconShown, 232, _stIconY, 0xA);
	}
#else
	uint16_t src = _pgeLive[0].current_inventory_PGE;
	if (src != 0xFF) {
		_currentIcon = _res._pgeInit[src].icon_num;
		drawIcon(_currentIcon, 232, 8, 0xA);
	}
#endif
}

void Game::showFinalScore() {
	if (_stub->hasWidescreen()) {
		_stub->clearWidescreen();
	}
	playCutscene(0x49);
	char buf[50];
	snprintf(buf, sizeof(buf), "SCORE %08u", _score);
	_vid.drawString(buf, (Video::GAMESCREEN_W - strlen(buf) * Video::CHAR_W) / 2, TEXT_ROW(40), 0xE5);
	const char *str = _menu.getLevelPassword(7, _skillLevel);
	_vid.drawString(str, (Video::GAMESCREEN_W - strlen(str) * Video::CHAR_W) / 2, TEXT_ROW(16), 0xE7);
	MenuConfirm confirm;
	while (!_stub->_pi.quit) {
		GAME_COPYRECT_FRONT();
		_stub->updateScreen(0);
		_stub->processEvents();
		if (confirm(_stub->_pi)) {
			break;
		}
		_stub->sleep(100);
	}
}

bool Game::handleConfigPanel() {
	static const int x = 7;
	static const int y = 10;
	static const int w = 17;
	static const int h = 12;

	_vid._charShadowColor = 0xE2;
	_vid._charFrontColor = 0xEE;
	_vid._charTransparentColor = 0xFF;

	// the panel background is drawn using special characters from FB_TXT.FNT
	static const bool kUseDefaultFont = true;

	switch (_res._type) {
	case kResourceTypeAmiga:
	case kResourceTypeSega:
		for (int i = 0; i < h; ++i) {
			for (int j = 0; j < w; ++j) {
				_vid.fillRect(Video::CHAR_W * (x + j), Video::CHAR_H * (y + i), Video::CHAR_W, Video::CHAR_H, 0xE2);
			}
		}
		break;
	case kResourceTypeDOS:
	case kResourceTypePC98:
		// top-left rounded corner
		_vid.DOS_drawChar(0x81, y, x, kUseDefaultFont);
		// top-right rounded corner
		_vid.DOS_drawChar(0x82, y, x + w, kUseDefaultFont);
		// bottom-left rounded corner
		_vid.DOS_drawChar(0x83, y + h, x, kUseDefaultFont);
		// bottom-right rounded corner
		_vid.DOS_drawChar(0x84, y + h, x + w, kUseDefaultFont);
		// horizontal lines
		for (int i = 1; i < w; ++i) {
			_vid.DOS_drawChar(0x85, y, x + i, kUseDefaultFont);
			_vid.DOS_drawChar(0x88, y + h, x + i, kUseDefaultFont);
		}
		for (int j = 1; j < h; ++j) {
			_vid._charTransparentColor = 0xFF;
			// left vertical line
			_vid.DOS_drawChar(0x86, y + j, x, kUseDefaultFont);
			// right vertical line
			_vid.DOS_drawChar(0x87, y + j, x + w, kUseDefaultFont);
			_vid._charTransparentColor = 0xE2;
			for (int i = 1; i < w; ++i) {
				_vid.DOS_drawChar(0x20, y + j, x + i, kUseDefaultFont);
			}
		}
		break;
	case kResourceTypeMac:
		// top-left rounded corner
		_vid.MAC_drawStringChar(_vid._frontLayer, _vid._w, Video::CHAR_W * x,       Video::CHAR_H * y,       _res._fnt, _vid._charFrontColor, 0x81);
		// top-right rounded corner
		_vid.MAC_drawStringChar(_vid._frontLayer, _vid._w, Video::CHAR_W * (x + w), Video::CHAR_H * y,       _res._fnt, _vid._charFrontColor, 0x82);
		// bottom-left rounded corner
		_vid.MAC_drawStringChar(_vid._frontLayer, _vid._w, Video::CHAR_W * x,       Video::CHAR_H * (y + h), _res._fnt, _vid._charFrontColor, 0x83);
		// bottom-right rounded corner
		_vid.MAC_drawStringChar(_vid._frontLayer, _vid._w, Video::CHAR_W * (x + w), Video::CHAR_H * (y + h), _res._fnt, _vid._charFrontColor, 0x84);
		// horizontal lines
		for (int i = 1; i < w; ++i) {
			_vid.MAC_drawStringChar(_vid._frontLayer, _vid._w, Video::CHAR_W * (x + i), Video::CHAR_H * y,       _res._fnt, _vid._charFrontColor, 0x85);
			_vid.MAC_drawStringChar(_vid._frontLayer, _vid._w, Video::CHAR_W * (x + i), Video::CHAR_H * (y + h), _res._fnt, _vid._charFrontColor, 0x88);
		}
		// vertical lines
		for (int i = 1; i < h; ++i) {
			_vid.MAC_drawStringChar(_vid._frontLayer, _vid._w, Video::CHAR_W * x,       Video::CHAR_H * (y + i), _res._fnt, _vid._charFrontColor, 0x86);
			_vid.MAC_drawStringChar(_vid._frontLayer, _vid._w, Video::CHAR_W * (x + w), Video::CHAR_H * (y + i), _res._fnt, _vid._charFrontColor, 0x87);
			for (int j = 1; j < w; ++j) {
				_vid.fillRect(Video::CHAR_W * (x + j), Video::CHAR_H * (y + i), Video::CHAR_W, Video::CHAR_H, 0xE2);
			}
		}
		break;
	}

	_menu._charVar3 = 0xE4;
	_menu._charVar4 = 0xE5;
	_menu._charVar1 = 0xE2;
	_menu._charVar2 = 0xEE;

	_vid.fullRefresh();
	enum { MENU_ITEM_ABORT = 1, MENU_ITEM_LOAD = 2, MENU_ITEM_SAVE = 3 };
	uint8_t colors[] = { 2, 3, 3, 3 };
	int current = 0;
	MenuConfirm confirm;
#ifdef ATARIST
	// Rows in pixels rather than cells, placed for Fit: it keeps rows
	// in runs of ten from row 10 and drops every eleventh, so an
	// 11-pixel pitch from 100 puts each 8-pixel line inside one run,
	// and the five lines sit centred in the panel's 80-176 box.
	static const int16_t kRow[5] = { 100, 111, 122, 133, 144 };
#else
	static const int16_t kRow[5] = {
		(y + 2) * Video::CHAR_H, (y + 4) * Video::CHAR_H, (y + 6) * Video::CHAR_H,
		(y + 8) * Video::CHAR_H, (y + 10) * Video::CHAR_H
	};
#endif
	while (!_stub->_pi.quit) {
		_menu.drawStringPx(_res.getMenuString(LocaleData::LI_18_RESUME_GAME), kRow[0], 9, colors[0]);
		_menu.drawStringPx(_res.getMenuString(LocaleData::LI_19_ABORT_GAME), kRow[1], 9, colors[1]);
		_menu.drawStringPx(_res.getMenuString(LocaleData::LI_20_LOAD_GAME), kRow[2], 9, colors[2]);
		_menu.drawStringPx(_res.getMenuString(LocaleData::LI_21_SAVE_GAME), kRow[3], 9, colors[3]);
		_vid.fillRect(Video::CHAR_W * (x + 1), kRow[4], Video::CHAR_W * (w - 2), Video::CHAR_H, 0xE2);
		char buf[32];
		snprintf(buf, sizeof(buf), "%s < %02d >", _res.getMenuString(LocaleData::LI_22_SAVE_SLOT), _stateSlot);
		_menu.drawStringPx(buf, kRow[4], 9, 1);

		_vid.updateScreen();
		_stub->sleep(80);
		inp_update();

		int prev = current;
		if (_stub->_pi.dirMask & PlayerInput::DIR_UP) {
			_stub->_pi.dirMask &= ~PlayerInput::DIR_UP;
			current = (current + 3) % 4;
		}
		if (_stub->_pi.dirMask & PlayerInput::DIR_DOWN) {
			_stub->_pi.dirMask &= ~PlayerInput::DIR_DOWN;
			current = (current + 1) % 4;
		}
		if (_stub->_pi.dirMask & PlayerInput::DIR_LEFT) {
			_stub->_pi.dirMask &= ~PlayerInput::DIR_LEFT;
			--_stateSlot;
			if (_stateSlot < 1) {
				_stateSlot = 1;
			}
		}
		if (_stub->_pi.dirMask & PlayerInput::DIR_RIGHT) {
			_stub->_pi.dirMask &= ~PlayerInput::DIR_RIGHT;
			++_stateSlot;
			if (_stateSlot > 99) {
				_stateSlot = 99;
			}
		}
		if (prev != current) {
			SWAP(colors[prev], colors[current]);
		}
		if (confirm(_stub->_pi)) {
			switch (current) {
			case MENU_ITEM_LOAD:
				_stub->_pi.load = true;
				break;
			case MENU_ITEM_SAVE:
				_stub->_pi.save = true;
				break;
			}
			break;
		}
		if (_stub->_pi.escape) {
			_stub->_pi.escape = false;
			break;
		}
	}
	_vid.fullRefresh();
	return (current == MENU_ITEM_ABORT);
}

bool Game::handleContinueAbort() {
	if (_stub->hasWidescreen()) {
		_stub->clearWidescreen();
	}
	playCutscene(0x48);
	int timeout = 100;
	int current_color = 0;
	uint8_t colors[] = { 0xE4, 0xE5 };
	uint8_t color_inc = 0xFF;
	Color col;
	_stub->getPaletteEntry(0xE4, &col);
	memcpy(_vid._tempLayer, _vid._frontLayer, _vid._layerSize);
	MenuConfirm confirm;
	while (timeout >= 0 && !_stub->_pi.quit) {
		const char *str;
		str = _res.getMenuString(LocaleData::LI_01_CONTINUE_OR_ABORT);
		_vid.drawString(str, (Video::GAMESCREEN_W - strlen(str) * Video::CHAR_W) / 2, TEXT_ROW(64), 0xE3);
		str = _res.getMenuString(LocaleData::LI_02_TIME);
		char buf[50];
		snprintf(buf, sizeof(buf), "%s : %d", str, timeout / 10);
		_vid.drawString(buf, 96, TEXT_ROW(88), 0xE3);
		str = _res.getMenuString(LocaleData::LI_03_CONTINUE);
		_vid.drawString(str, (Video::GAMESCREEN_W - strlen(str) * Video::CHAR_W) / 2, TEXT_ROW(104), colors[0]);
		str = _res.getMenuString(LocaleData::LI_04_ABORT);
		_vid.drawString(str, (Video::GAMESCREEN_W - strlen(str) * Video::CHAR_W) / 2, TEXT_ROW(112), colors[1]);
		snprintf(buf, sizeof(buf), "SCORE  %08u", _score);
		_vid.drawString(buf, 64, TEXT_ROW(154), 0xE3);
		if (_stub->_pi.dirMask & PlayerInput::DIR_UP) {
			_stub->_pi.dirMask &= ~PlayerInput::DIR_UP;
			if (current_color > 0) {
				SWAP(colors[current_color], colors[current_color - 1]);
				--current_color;
			}
		}
		if (_stub->_pi.dirMask & PlayerInput::DIR_DOWN) {
			_stub->_pi.dirMask &= ~PlayerInput::DIR_DOWN;
			if (current_color < 1) {
				SWAP(colors[current_color], colors[current_color + 1]);
				++current_color;
			}
		}
		if (confirm(_stub->_pi)) {
			return (current_color == 0);
		}
		GAME_COPYRECT_FRONT();
		_stub->updateScreen(0);
		static const int COLOR_STEP = 8;
		static const int COLOR_MIN = 16;
		static const int COLOR_MAX = 256 - 16;
		if (col.b >= COLOR_MAX) {
			color_inc = 0;
		} else if (col.b < COLOR_MIN) {
			color_inc = 0xFF;
		}
		if (color_inc == 0xFF) {
			col.b += COLOR_STEP;
			col.g += COLOR_STEP;
		} else {
			col.b -= COLOR_STEP;
			col.g -= COLOR_STEP;
		}
		_stub->setPaletteEntry(0xE4, &col);
		_stub->processEvents();
		_stub->sleep(100);
		--timeout;
		memcpy(_vid._frontLayer, _vid._tempLayer, _vid._layerSize);
	}
	return false;
}

void Game::printLevelCode() {
	if (_printLevelCodeCounter != 0) {
		--_printLevelCodeCounter;
		if (_printLevelCodeCounter != 0) {
			char buf[32];
			snprintf(buf, sizeof(buf), "CODE: %s", _menu.getLevelPassword(_currentLevel, _skillLevel));
			_vid.drawString(buf, (Video::GAMESCREEN_W - strlen(buf) * Video::CHAR_W) / 2, TEXT_ROW(16), 0xE7);
		}
	}
}

void Game::printSaveStateCompleted() {
	if (_saveStateCompleted) {
		const char *str = _res.getMenuString(LocaleData::LI_05_COMPLETED);
		_vid.drawString(str, (176 - strlen(str) * Video::CHAR_W) / 2, TEXT_ROW(34), 0xE6);
	}
}

void Game::drawLevelTexts() {
	LivePGE *pge = &_pgeLive[0];
	int8_t obj = col_findCurrentCollidingObject(pge, 3, 0xFF, 0xFF, &pge);
	if (obj == 0) {
		obj = col_findCurrentCollidingObject(pge, 0xFF, 5, 9, &pge);
	}
	if (obj > 0) {
		_printLevelCodeCounter = 0;
		if (_textToDisplay == 0xFFFF) {
			uint8_t icon_num = obj - 1;
			drawIcon(icon_num, 80, 8, 0xA);
			uint8_t txt_num = pge->init_PGE->text_num;
			const uint8_t *str = _res.getTextString(_currentLevel, txt_num);
			drawString(str, 176, 26, 0xE6, true);
			if (icon_num == 2) {
				printSaveStateCompleted();
				return;
			}
		} else {
			_currentInventoryIconNum = obj - 1;
		}
	}
	_saveStateCompleted = false;
}

static int getLineLength(const uint8_t *str) {
	int len = 0;
	while (*str && *str != 0xB && *str != 0xA) {
		++str;
		++len;
	}
	return len;
}

void Game::drawStoryTexts() {
	if (_textToDisplay != 0xFFFF) {
		uint8_t textColor = 0xE8;
		const uint8_t *str = _res.getGameString(_textToDisplay);
		memcpy(_vid._tempLayer, _vid._frontLayer, _vid._layerSize);
		int textSpeechSegment = 0;
		int textSegmentsCount = 0;
		while (!_stub->_pi.quit) {
			drawIcon(_currentInventoryIconNum, 80, 8, 0xA);
			int yPos = 26;
			if (_res._type == kResourceTypeMac) {
				if (textSegmentsCount == 0) {
					textSegmentsCount = *str++;
				}
				int len = *str++;
				if (*str == '@' /* Floppy version */ || *str == '~' /* CD version */) {
					switch (str[1]) {
					case '1':
						textColor = 0xE9;
						break;
					case '2':
						textColor = 0xEB;
						break;
					default:
						warning("Unhandled MAC text color code 0x%x", str[1]);
						break;
					}
					str += 2;
					len -= 2;
				}
				for (; len > 0; yPos += 8) {
					const uint8_t *next = (const uint8_t *)memchr(str, '|', len);
					if (!next) {
						_vid.drawStringLen((const char *)str, len, (176 - len * Video::CHAR_W) / 2, yPos, textColor);
						// point 'str' to beginning of next text segment
						str += len;
						break;
					}
					const int lineLength = next - str;
					_vid.drawStringLen((const char *)str, lineLength, (176 - lineLength * Video::CHAR_W) / 2, yPos, textColor);
					str = next + 1;
					len -= lineLength + 1;
				}
			} else {
				if (*str == 0xFF) {
					if (_res._lang == LANG_JP) {
						switch (str[1]) {
						case 0:
							textColor = 0xE9;
							break;
						case 1:
							textColor = 0xEB;
							break;
						default:
							warning("Unhandled JP text color code 0x%x", str[1]);
							break;
						}
						str += 2;
					} else {
						textColor = str[1];
						// str[2] is an unused color (possibly the shadow)
						str += 3;
					}
				}
				while (1) {
					const int len = getLineLength(str);
					str = (const uint8_t *)_vid.drawString((const char *)str, (176 - len * Video::CHAR_W) / 2, TEXT_ROW(yPos), textColor);
					if (*str == 0 || *str == 0xB) {
						break;
					}
					++str;
					yPos += 8;
				}
			}
			uint8_t *voiceSegmentData = 0;
			uint32_t voiceSegmentLen = 0;
			_res.load_VCE(_textToDisplay, textSpeechSegment++, &voiceSegmentData, &voiceSegmentLen);
			if (voiceSegmentData) {
				_mix.play(voiceSegmentData, voiceSegmentLen, 32000, Mixer::MAX_VOLUME);
			}
			_vid.updateScreen();
			while (!_stub->_pi.backspace && !_stub->_pi.quit) {
				if (voiceSegmentData && !_mix.isPlaying(voiceSegmentData)) {
					break;
				}
				inp_update();
				_stub->sleep(80);
			}
			if (voiceSegmentData) {
				_mix.stopAll();
				free(voiceSegmentData);
			}
			_stub->_pi.backspace = false;
			if (_res._type == kResourceTypeMac) {
				if (textSpeechSegment == textSegmentsCount) {
					break;
				}
			} else {
				if (*str == 0) {
					break;
				}
				++str;
			}
			memcpy(_vid._frontLayer, _vid._tempLayer, _vid._layerSize);
		}
		_textToDisplay = 0xFFFF;
	}
}

void Game::drawString(const uint8_t *p, int x, int y, uint8_t color, bool hcenter) {
	const char *str = (const char *)p;
	int len = 0;
	if (_res._type == kResourceTypeMac) {
		len = *p;
		++str;
	} else {
		len = strlen(str);
	}
	if (hcenter) {
		x = (x - len * Video::CHAR_W) / 2;
	}
	_vid.drawStringLen(str, len, x, y, color);
}

void Game::prepareAnims() {
	if (_currentRoom < 0x40) {
		int8_t pge_room;
		LivePGE *pge = _pge_liveTable1[_currentRoom];
		while (pge) {
			prepareAnimsHelper(pge, 0, 0);
			pge = pge->next_PGE_in_room;
		}
		pge_room = _res._ctData[CT_UP_ROOM + _currentRoom];
		if (pge_room >= 0 && pge_room < 0x40) {
			pge = _pge_liveTable1[pge_room];
			while (pge) {
				if ((pge->init_PGE->object_type != kObjectTypeMonster && pge->pos_y > 176) || (pge->init_PGE->object_type == kObjectTypeMonster && pge->pos_y > 216)) {
					prepareAnimsHelper(pge, 0, -216);
				}
				pge = pge->next_PGE_in_room;
			}
		}
		pge_room = _res._ctData[CT_DOWN_ROOM + _currentRoom];
		if (pge_room >= 0 && pge_room < 0x40) {
			pge = _pge_liveTable1[pge_room];
			while (pge) {
				if (pge->pos_y < 48) {
					prepareAnimsHelper(pge, 0, 216);
				}
				pge = pge->next_PGE_in_room;
			}
		}
		pge_room = _res._ctData[CT_LEFT_ROOM + _currentRoom];
		if (pge_room >= 0 && pge_room < 0x40) {
			pge = _pge_liveTable1[pge_room];
			while (pge) {
				if (pge->pos_x > 224) {
					prepareAnimsHelper(pge, -256, 0);
				}
				pge = pge->next_PGE_in_room;
			}
		}
		pge_room = _res._ctData[CT_RIGHT_ROOM + _currentRoom];
		if (pge_room >= 0 && pge_room < 0x40) {
			pge = _pge_liveTable1[pge_room];
			while (pge) {
				if (pge->pos_x <= 32) {
					prepareAnimsHelper(pge, 256, 0);
				}
				pge = pge->next_PGE_in_room;
			}
		}
	}
}

void Game::prepareAnimsHelper(LivePGE *pge, int16_t dx, int16_t dy) {
	debug(DBG_GAME, "Game::prepareAnimsHelper() dx=0x%X dy=0x%X pge_num=%ld pge->flags=0x%X pge->anim_number=0x%X", dx, dy, pge - &_pgeLive[0], pge->flags, pge->anim_number);
	if (!(pge->flags & 8)) {
		if (pge->index != 0 && loadMonsterSprites(pge) == 0) {
			return;
		}
		const uint8_t *dataPtr = 0;
		int8_t dw = 0, dh = 0;
		switch (_res._type) {
		case kResourceTypeAmiga:
		case kResourceTypeDOS:
		case kResourceTypePC98:
		case kResourceTypeSega:
			assert(pge->anim_number < 1287);
			dataPtr = _res._sprData[pge->anim_number];
			if (dataPtr == 0) {
				return;
			}
			dw = (int8_t)dataPtr[0];
			dh = (int8_t)dataPtr[1];
			break;
		case kResourceTypeMac:
			break;
		}
		uint8_t w = 0, h = 0;
		switch (_res._type) {
		case kResourceTypeAmiga:
			w = ((dataPtr[2] >> 7) + 1) * 16;
			h = dataPtr[2] & 0x7F;
			break;
		case kResourceTypeDOS:
		case kResourceTypePC98:
			w = dataPtr[2];
			h = dataPtr[3];
			dataPtr += 4;
			break;
		case kResourceTypeMac:
			break;
		case kResourceTypeSega:
			w = 32;
			h = 24 * 2;
			break;
		}
		int16_t ypos = dy + pge->pos_y - dh + 2;
		int16_t xpos = dx + pge->pos_x - dw;
		if (pge->flags & 2) {
			xpos = dw + dx + pge->pos_x;
			uint8_t _cl = w;
			if (_cl & 0x40) {
				_cl = h;
			} else {
				_cl &= 0x3F;
			}
			xpos -= _cl;
		}
		if (xpos <= -32 || xpos >= 256 || ypos < -48 || ypos >= 224) {
			return;
		}
		xpos += 8;
		if (pge == &_pgeLive[0]) {
			_animBuffers.addState(1, xpos, ypos, dataPtr, pge, w, h);
		} else if (pge->flags & 0x10) {
			_animBuffers.addState(2, xpos, ypos, dataPtr, pge, w, h);
		} else {
			_animBuffers.addState(0, xpos, ypos, dataPtr, pge, w, h);
		}
	} else {
		const uint8_t *dataPtr = 0;
		switch (_res._type) {
		case kResourceTypeAmiga:
		case kResourceTypeDOS:
		case kResourceTypePC98:
		case kResourceTypeSega:
			assert(pge->anim_number < _res._numSpc);
			dataPtr = _res._spc + READ_BE_UINT16(_res._spc + pge->anim_number * 2);
			break;
		case kResourceTypeMac:
			break;
		}
		const int16_t xpos = dx + pge->pos_x + 8;
		const int16_t ypos = dy + pge->pos_y + 2;
		if (pge->init_PGE->object_type == kObjectTypeRenderAbove) {
			_animBuffers.addState(3, xpos, ypos, dataPtr, pge);
		} else if (pge->flags & 0x10) {
			_animBuffers.addState(2, xpos, ypos, dataPtr, pge);
		} else {
			_animBuffers.addState(0, xpos, ypos, dataPtr, pge);
		}
	}
}

void Game::drawAnims() {
	debug(DBG_GAME, "Game::drawAnims()");
	_eraseBackground = false;
	drawAnimBuffer(2, _animBuffer2State);
	drawAnimBuffer(1, _animBuffer1State);
	drawAnimBuffer(0, _animBuffer0State);
	_eraseBackground = true;
	drawAnimBuffer(3, _animBuffer3State);
}

void Game::drawAnimBuffer(uint8_t stateNum, AnimBufferState *state) {
	debug(DBG_GAME, "Game::drawAnimBuffer() state=%d", stateNum);
	assert(stateNum < 4);
	_animBuffers._states[stateNum] = state;
	uint8_t lastPos = _animBuffers._curPos[stateNum];
	if (lastPos != 0xFF) {
		uint8_t numAnims = lastPos + 1;
		state += lastPos;
		_animBuffers._curPos[stateNum] = 0xFF;
		do {
			LivePGE *pge = state->pge;
			if (!(pge->flags & 8)) {
				if (stateNum == 1 && (_blinkingConradCounter & 1)) {
					break;
				}
				switch (_res._type) {
				case kResourceTypeAmiga:
					{
#ifdef ATARIST
						ST_drawCharacterBaked(state->dataPtr, state->x, state->y, state->h, state->w, pge->flags);
#else
						_vid.AMIGA_decodeSpm(state->dataPtr, _res._scratchBuffer);
						drawCharacter(_res._scratchBuffer, state->x, state->y, state->h, state->w, pge->flags);
#endif
					}
					break;
				case kResourceTypeDOS:
				case kResourceTypePC98:
					if (!(state->dataPtr[-2] & 0x80)) {
						_vid.DOS_decodeSpm(state->dataPtr, _res._scratchBuffer);
						drawCharacter(_res._scratchBuffer, state->x, state->y, state->h, state->w, pge->flags);
					} else {
						drawCharacter(state->dataPtr, state->x, state->y, state->h, state->w, pge->flags);
					}
					break;
				case kResourceTypeMac:
					drawPiege(state);
					break;
				case kResourceTypeSega:
					_vid.SEGA_decodeSpm(state->dataPtr, _res._scratchBuffer);
					drawCharacter(_res._scratchBuffer, state->x, state->y, state->h, state->w, pge->flags);
					break;
				}
			} else {
				drawPiege(state);
			}
			--state;
		} while (--numAnims != 0);
	}
}

void Game::drawPiege(AnimBufferState *state) {
	LivePGE *pge = state->pge;
	switch (_res._type) {
	case kResourceTypeAmiga:
	case kResourceTypeDOS:
	case kResourceTypePC98:
	case kResourceTypeSega:
		drawObject(state->dataPtr, state->x, state->y, pge->flags);
		break;
	case kResourceTypeMac:
		if (pge->flags & 8) {
			_vid.MAC_drawSprite(state->x, state->y, _res._spc, pge->anim_number, (pge->flags & 2) != 0, _eraseBackground);
		} else if (pge->index == 0) {
			if (pge->anim_number == 0x386) {
				break;
			}
			const int frame = _res.MAC_getPersoFrame(pge->anim_number);
			_vid.MAC_drawSprite(state->x, state->y, _res._perso, frame, (pge->flags & 2) != 0, _eraseBackground);
		} else {
			const int frame = _res.MAC_getMonsterFrame(pge->anim_number);
			_vid.MAC_drawSprite(state->x, state->y, _res._monster, frame, (pge->flags & 2) != 0, _eraseBackground);
		}
		break;
	}
}

void Game::drawObject(const uint8_t *dataPtr, int16_t x, int16_t y, uint8_t flags) {
	debug(DBG_GAME, "Game::drawObject() dataPtr[]=0x%X dx=%d dy=%d",  dataPtr[0], (int8_t)dataPtr[1], (int8_t)dataPtr[2]);
	assert(dataPtr[0] < 0x4A);
	uint8_t slot = _res._rp[dataPtr[0]];
	uint8_t *data = _res.findBankData(slot);
	if (data == 0) {
		data = _res.loadBankData(slot);
	}
	int16_t posy = y - (int8_t)dataPtr[2];
	int16_t posx = x;
	if (flags & 2) {
		posx += (int8_t)dataPtr[1];
	} else {
		posx -= (int8_t)dataPtr[1];
	}
	int count = 0;
	switch (_res._type) {
	case kResourceTypeAmiga:
		count = dataPtr[8];
		dataPtr += 9;
		break;
	case kResourceTypeDOS:
	case kResourceTypePC98:
	case kResourceTypeSega:
		count = dataPtr[5];
		dataPtr += 6;
		break;
	case kResourceTypeMac:
		assert(0); // different graphics format
		break;
	}
	for (int i = 0; i < count; ++i) {
		drawObjectFrame(data, dataPtr, posx, posy, flags);
		dataPtr += 4;
	}
}

#ifdef ATARIST
INLINE_ALWAYS                        // drawObject's, once for every piece of every object drawn
#endif
void Game::drawObjectFrame(const uint8_t *bankDataPtr, const uint8_t *dataPtr, int16_t x, int16_t y, uint8_t flags) {
	debug(DBG_GAME, "Game::drawObjectFrame(%p, %d, %d, 0x%X)", dataPtr, x, y, flags);
	const uint8_t *src = bankDataPtr + dataPtr[0] * 32;

	int16_t sprite_y = y + dataPtr[2];
	int16_t sprite_x;
	if (flags & 2) {
		sprite_x = x - dataPtr[1] - (((dataPtr[3] & 0xC) + 4) * 2);
	} else {
		sprite_x = x + dataPtr[1];
	}

	uint8_t sprite_flags = dataPtr[3];
	if (flags & 2) {
		sprite_flags ^= 0x10;
	}

	const uint8_t sprite_h = (((sprite_flags >> 0) & 3) + 1) * 8;
	const uint8_t sprite_w = (((sprite_flags >> 2) & 3) + 1) * 8;

#ifdef ATARIST
	if (_res._type == kResourceTypeAmiga) {
		// the piece whole, at its unclipped place: the bake keys on the
		// bank bytes, and the layer clips it
		unsigned stFlags = (sprite_flags & 0x10) ? kSTSpriteXflip : 0;
		if (!_eraseBackground) {
			stFlags |= kSTSpriteRespectPrio;
		}
		ST_sprDraw(_vid._frontLayer, kSprKindSPC, src, kSprIdBank | (_res._stBankEpoch & kSprIdEpochMask),
		           sprite_w, sprite_h, sprite_x, sprite_y, (flags & 0x60) >> 1, stFlags, false);
		stMarkClipped(_vid, sprite_x, sprite_y, sprite_w, sprite_h);
		return;
	}
#endif
	switch (_res._type) {
	case kResourceTypeAmiga:
		_vid.AMIGA_decodeSpc(src, sprite_w, sprite_h, _res._scratchBuffer);
		break;
	case kResourceTypeDOS:
	case kResourceTypePC98:
		_vid.DOS_decodeSpc(src, sprite_w, sprite_h, _res._scratchBuffer);
		break;
	case kResourceTypeMac:
		assert(0); // different graphics format
		break;
	case kResourceTypeSega:
		_vid.SEGA_decodeSpc(src, sprite_w, sprite_h, _res._scratchBuffer);
		break;
	}

	src = _res._scratchBuffer;
	bool sprite_mirror_x = false;
	int16_t sprite_clipped_w;
	if (sprite_x >= 0) {
		sprite_clipped_w = sprite_x + sprite_w;
		if (sprite_clipped_w < Video::GAMESCREEN_W) {
			sprite_clipped_w = sprite_w;
		} else {
			sprite_clipped_w = Video::GAMESCREEN_W - sprite_x;
			if (sprite_flags & 0x10) {
				sprite_mirror_x = true;
				src += sprite_w - 1;
			}
		}
	} else {
		sprite_clipped_w = sprite_x + sprite_w;
		if (!(sprite_flags & 0x10)) {
			src -= sprite_x;
			sprite_x = 0;
		} else {
			sprite_mirror_x = true;
			src += sprite_x + sprite_w - 1;
			sprite_x = 0;
		}
	}
	if (sprite_clipped_w <= 0) {
		return;
	}

	int16_t sprite_clipped_h;
	if (sprite_y >= 0) {
		sprite_clipped_h = Video::GAMESCREEN_H - sprite_h;
		if (sprite_y < sprite_clipped_h) {
			sprite_clipped_h = sprite_h;
		} else {
			sprite_clipped_h = Video::GAMESCREEN_H - sprite_y;
		}
	} else {
		sprite_clipped_h = sprite_h + sprite_y;
		src -= sprite_w * sprite_y;
		sprite_y = 0;
	}
	if (sprite_clipped_h <= 0) {
		return;
	}

	if (!sprite_mirror_x && (sprite_flags & 0x10)) {
		src += sprite_w - 1;
	}

	const uint32_t dst_offset = Video::GAMESCREEN_W * sprite_y + sprite_x;
	const uint8_t sprite_col_mask = (flags & 0x60) >> 1;

#ifdef ATARIST
	(void)dst_offset;
	(void)sprite_col_mask;
#else
	if (_eraseBackground) {
		if (!(sprite_flags & 0x10)) {
			_vid.drawSpriteSub1(src, _vid._frontLayer + dst_offset, sprite_w, sprite_clipped_h, sprite_clipped_w, sprite_col_mask);
		} else {
			_vid.drawSpriteSub2(src, _vid._frontLayer + dst_offset, sprite_w, sprite_clipped_h, sprite_clipped_w, sprite_col_mask);
		}
	} else {
		if (!(sprite_flags & 0x10)) {
			_vid.drawSpriteSub3(src, _vid._frontLayer + dst_offset, sprite_w, sprite_clipped_h, sprite_clipped_w, sprite_col_mask);
		} else {
			_vid.drawSpriteSub4(src, _vid._frontLayer + dst_offset, sprite_w, sprite_clipped_h, sprite_clipped_w, sprite_col_mask);
		}
	}
#endif
	_vid.markBlockAsDirty(sprite_x, sprite_y, sprite_clipped_w, sprite_clipped_h, _vid._layerScale);
}

void Game::drawCharacter(const uint8_t *dataPtr, int16_t pos_x, int16_t pos_y, uint8_t a, uint8_t b, uint8_t flags) {
	debug(DBG_GAME, "Game::drawCharacter(%p, %d, %d, 0x%X, 0x%X, 0x%X)", dataPtr, pos_x, pos_y, a, b, flags);
	bool sprite_mirror_y = false;
	if (b & 0x40) {
		b &= ~0x40;
		SWAP(a, b);
		sprite_mirror_y = true;
	}
	uint16_t sprite_h = a;
	uint16_t sprite_w = b;

	const uint8_t *src = dataPtr;
	bool sprite_mirror_x = false;

	int16_t sprite_clipped_w;
	if (pos_x >= 0) {
		if (pos_x + sprite_w < 256) {
			sprite_clipped_w = sprite_w;
		} else {
			sprite_clipped_w = 256 - pos_x;
			if (flags & 2) {
				sprite_mirror_x = true;
				if (sprite_mirror_y) {
					src += (sprite_w - 1) * sprite_h;
				} else {
					src += sprite_w - 1;
				}
			}
		}
	} else {
		sprite_clipped_w = pos_x + sprite_w;
		if (!(flags & 2)) {
			if (sprite_mirror_y) {
				src -= sprite_h * pos_x;
				pos_x = 0;
			} else {
				src -= pos_x;
				pos_x = 0;
			}
		} else {
			sprite_mirror_x = true;
			if (sprite_mirror_y) {
				src += sprite_h * (pos_x + sprite_w - 1);
				pos_x = 0;
			} else {
				src += pos_x + sprite_w - 1;
				sprite_mirror_x = true;
				pos_x = 0;
			}
		}
	}
	if (sprite_clipped_w <= 0) {
		return;
	}

	int16_t sprite_clipped_h;
	if (pos_y >= 0) {
		if (pos_y < 224 - sprite_h) {
			sprite_clipped_h = sprite_h;
		} else {
			sprite_clipped_h = 224 - pos_y;
		}
	} else {
		sprite_clipped_h = sprite_h + pos_y;
		if (sprite_mirror_y) {
			src -= pos_y;
		} else {
			src -= sprite_w * pos_y;
		}
		pos_y = 0;
	}
	if (sprite_clipped_h <= 0) {
		return;
	}

	if (!sprite_mirror_x && (flags & 2)) {
		if (sprite_mirror_y) {
			src += sprite_h * (sprite_w - 1);
		} else {
			src += sprite_w - 1;
		}
	}

	const uint32_t dst_offset = 256 * pos_y + pos_x;
	const uint8_t sprite_col_mask = ((flags & 0x60) == 0x60) ? 0x50 : 0x40;

	debug(DBG_GAME, "dst_offset=0x%X src_offset=%ld", dst_offset, src - dataPtr);

#ifdef ATARIST
	(void)dst_offset;
	unsigned stFlags = kSTSpriteRespectPrio;
	if (flags & 2) {
		stFlags |= kSTSpriteXflip;
	}
	int stPitch = sprite_w;
	if (sprite_mirror_y) {
		stFlags |= kSTSpriteColMajor;
		stPitch = sprite_h;
	}
	// only non-Amiga data comes this way on the ST (never, in practice):
	// Amiga frames go through ST_drawCharacterBaked
	uint8_t map16[16];
	ST_buildMap16(sprite_col_mask, map16);
	ST_drawSprite(_vid._frontLayer, src, stPitch, pos_x, pos_y, sprite_clipped_w, sprite_clipped_h, map16, stFlags, false);
#else
	if (!(flags & 2)) {
		if (sprite_mirror_y) {
			_vid.drawSpriteSub5(src, _vid._frontLayer + dst_offset, sprite_h, sprite_clipped_h, sprite_clipped_w, sprite_col_mask);
		} else {
			_vid.drawSpriteSub3(src, _vid._frontLayer + dst_offset, sprite_w, sprite_clipped_h, sprite_clipped_w, sprite_col_mask);
		}
	} else {
		if (sprite_mirror_y) {
			_vid.drawSpriteSub6(src, _vid._frontLayer + dst_offset, sprite_h, sprite_clipped_h, sprite_clipped_w, sprite_col_mask);
		} else {
			_vid.drawSpriteSub4(src, _vid._frontLayer + dst_offset, sprite_w, sprite_clipped_h, sprite_clipped_w, sprite_col_mask);
		}
	}
#endif
	_vid.markBlockAsDirty(pos_x, pos_y, sprite_clipped_w, sprite_clipped_h, _vid._layerScale);
}

#ifdef ATARIST
// drawCharacter for Amiga frames, through the planar bakes: the frame is
// named by its bytes in the sprite data, drawn whole at its unclipped
// place, and only decoded on the bake's miss.
void Game::ST_drawCharacterBaked(const uint8_t *dataPtr, int16_t x, int16_t y, uint8_t a, uint8_t b, uint8_t flags) {
	unsigned stFlags = kSTSpriteRespectPrio;
	if (b & 0x40) {
		b &= ~0x40;
		SWAP(a, b);
		stFlags |= kSTSpriteColMajor;
	}
	if (flags & 2) {
		stFlags |= kSTSpriteXflip;
	}
	const uint8_t colMask = ((flags & 0x60) == 0x60) ? 0x50 : 0x40;
	const uint32_t ident = _res.ST_inPerso(dataPtr) ? kSprIdPerso
	                       : (kSprIdMonster | (_res._stMonsterEpoch & kSprIdEpochMask));
	ST_sprDraw(_vid._frontLayer, kSprKindSPM, dataPtr, ident, b, a, x, y, colMask, stFlags, false);
	stMarkClipped(_vid, x, y, b, a);
}
#endif

int Game::loadMonsterSprites(LivePGE *pge) {
	debug(DBG_GAME, "Game::loadMonsterSprites()");
	const InitPGE *init_pge = pge->init_PGE;
	if (init_pge->obj_node_number != 0x49 && init_pge->object_type != kObjectTypeMonster) {
		return 0xFFFF;
	}
	if (init_pge->obj_node_number == _curMonsterFrame) {
		return 0xFFFF;
	}
	if (pge->room_location != _currentRoom) {
		return 0;
	}

	const uint8_t *mList = _monsterListLevels[_currentLevel];
	while (*mList != init_pge->obj_node_number) {
		if (*mList == 0xFF) { // end of list
			return 0;
		}
		mList += 2;
	}
	_curMonsterFrame = mList[0];
	if (_curMonsterNum != mList[1]) {
		_curMonsterNum = mList[1];
		switch (_res._type) {
		case kResourceTypeAmiga:
		case kResourceTypeSega: {
#ifdef ATARIST
				// Once read, a monster's sprites stay for the level in a
				// buffer of their own. All four used to take turns in
				// one, so two kinds of monster in a room read a file
				// from disk every time the drawing passed from one to
				// the other, and threw away every sprite decoded and
				// baked with it.
				if (!_res.ST_monsterResident(_curMonsterNum)) {
					_res.load(_monsterNames[1][_curMonsterNum], Resource::OT_SPM);
					++g_stPerf.monsterLoads;
					if (!(g_options.ab & kAbNoMonsterKeep)) {
						_res.ST_keepMonster(_curMonsterNum);
					}
				}
				_res.ST_pointMonsters();
#else
				_res.load(_monsterNames[1][_curMonsterNum], Resource::OT_SPM);
#endif
				const int dst_offset = Video::PALETTE_INDEX_MONSTER * 16;
				const int src_offset = _vid._mapPalSlot3 * 16 + (_curMonsterNum & 1) * 8;
				for (int i = 0; i < 8; ++i) {
					_vid.setPaletteColorBE(dst_offset + i, src_offset + i);
				}
			}
			break;
		case kResourceTypeDOS:
		case kResourceTypePC98: {
				const char *name = _monsterNames[0][_curMonsterNum];
				_res.load(name, Resource::OT_SPRM);
				_res.load_SPR_OFF(name, _res._sprm);
				_vid.setPaletteSlotLE(Video::PALETTE_INDEX_MONSTER, _monsterPals[_curMonsterNum]);
			}
			break;
		case kResourceTypeMac: {
				Color palette[16];
				_res.MAC_loadMonsterData(_monsterNames[0][_curMonsterNum], palette, _currentLevel);
				const int offset = Video::PALETTE_INDEX_MONSTER * 16;
				for (int i = 0; i < 16; ++i) {
					_stub->setPaletteEntry(offset + i, &palette[i]);
				}
			}
			break;
		}
	}
	return 0xFFFF;
}

bool Game::hasLevelRoom(int level, int room) const {
	if (_res._type == kResourceTypeMac) {
		return _res.MAC_hasLevelMap(level, room);
	}
	if (_res._map) {
		return READ_LE_UINT32(_res._map + room * 6) != 0;
	} else if (_res._lev) {
		return READ_BE_UINT32(_res._lev + room * 4) > 0x100;
	}
	return false;
}

static bool isMetro(int level, int room) {
	return level == 1 && (room == 0 || room == 13 || room == 38 || room == 51);
}

void Game::loadLevelRoomHelper(int level, int room) {
	switch (_res._type) {
	case kResourceTypeAmiga:
		if (level == 1) {
			int num = 0;
			switch (room) {
			case 14:
			case 19:
			case 52:
			case 53:
				num = 1;
				break;
			case 11:
			case 24:
			case 27:
			case 56:
				num = 2;
				break;
			}
#ifdef ATARIST
			// The switches above fire on the way through a transition
			// room, so a saved game resumed in the level's second half
			// (Load on the title, Ctrl+L) drew its rooms from the first
			// half's file. A room that is not in the loaded half has no
			// data - it ends where the room before it ends - so take
			// the other half then.
			if (num == 0 && _res._lev) {
				const uint32_t end = READ_BE_UINT32(_res._lev + room * 4);
				const uint32_t prev = room ? READ_BE_UINT32(_res._lev + (room - 1) * 4) : 64 * 4;
				if (end == prev) {
					num = (_res._levNum == 1) ? 2 : 1;
				}
			}
#endif
			if (num != 0 && _res._levNum != num) {
				char name[9];
				snprintf(name, sizeof(name), "level2_%d", num);
				_res.load(name, Resource::OT_LEV);
				_res._levNum = num;
			}
		}
		_vid.AMIGA_decodeLev(level, room);
		break;
	case kResourceTypeDOS:
		if (_res._map) {
			_vid.DOS_decodeMap(level, room);
		} else {
			assert(_res._lev);
			_vid.DOS_decodeLev(level, room);
		}
		break;
	case kResourceTypeMac:
		_vid.MAC_decodeMap(level, room);
		break;
	case kResourceTypePC98:
		_vid.PC98_decodeMap(level, room);
		break;
	case kResourceTypeSega:
		_vid.AMIGA_decodeLev(level, room);
		break;
	}
}

void Game::loadLevelRoom() {
	debug(DBG_GAME, "Game::loadLevelRoom() room=%d", _currentRoom);
	bool widescreenUpdated = false;
	_currentIcon = 0xFF;
	if (_stub->hasWidescreen() && _widescreenMode == kWidescreenAdjacentRooms) {
		const int leftRoom = _res._ctData[CT_LEFT_ROOM + _currentRoom];
		if (leftRoom >= 0 && hasLevelRoom(_currentLevel, leftRoom) && !isMetro(_currentLevel, leftRoom)) {
			loadLevelRoomHelper(_currentLevel, leftRoom);
			_stub->copyWidescreenLeft(_vid._w, _vid._h, _vid._backLayer);
		} else {
			_stub->copyWidescreenLeft(_vid._w, _vid._h, 0);
		}
		const int rightRoom = _res._ctData[CT_RIGHT_ROOM + _currentRoom];
		if (rightRoom >= 0 && hasLevelRoom(_currentLevel, rightRoom) && !isMetro(_currentLevel, rightRoom)) {
			loadLevelRoomHelper(_currentLevel, rightRoom);
			_stub->copyWidescreenRight(_vid._w, _vid._h, _vid._backLayer);
		} else {
			_stub->copyWidescreenRight(_vid._w, _vid._h, 0);
		}
		widescreenUpdated = true;
	}
	loadLevelRoomHelper(_currentLevel, _currentRoom);
	if (!widescreenUpdated) {
		_vid.updateWidescreen();
	}
}

#ifdef ATARIST
// The Amiga data's global sprites, once: the icons and Conrad
void Game::ST_loadGlobals() {
	if (_stGlobalsLoaded) {
		return;
	}
	_stGlobalsLoaded = true;
	_res.load("ICONE", Resource::OT_ICN, "SPR");
	_res.load("ICON", Resource::OT_ICN, "SPR");
	_res.load("PERSO", Resource::OT_SPM);
}

// Leaving a level for the title: nothing the level loaded is needed
// until the next one loads it again, and the title needs a 70K block
// for its picture - which a 2MB machine did not have with level 5 or
// 7 still in memory and music or a border open. The effects are
// stopped first: a sample still playing would be read from freed
// memory.
void Game::ST_unloadLevel() {
	_mix.stopAll();
	clearStateRewind();
	_res.freeLevelData();
	_vid.ST_releaseLevelCaches();
	ST_logFreeMemory("level unloaded");
}
#endif

void Game::loadLevelData() {
#ifdef ATARIST
	// the bakes' memory back while the level loads; the first sprite
	// drawn in it sizes the ring to what is left. The rooms kept for
	// going back belong to the level that is going.
	ST_releaseRamCaches();
#endif
	info("Loading level %d", _currentLevel + 1);
#ifdef ATARIST
	_stLogMemRoom = true;
#endif
	_res.clearLevelRes();
	const Level *lvl = &_gameLevels[_currentLevel];
	switch (_res._type) {
	case kResourceTypeAmiga:
		if (_res._isDemo) {
			static const char *fname1 = "demo";
			static const char *fname2 = "demof";
			_res.load(fname1, Resource::OT_MBK);
			_res.load(fname1, Resource::OT_CT);
			_res.load(fname1, Resource::OT_PAL);
			_res.load(fname1, Resource::OT_RPC);
			_res.load(fname1, Resource::OT_SPC);
			_res.load(fname1, Resource::OT_LEV);
			_res.load(fname2, Resource::OT_PGE);
			_res.load(fname1, Resource::OT_OBJ);
			_res.load(fname1, Resource::OT_ANI);
			_res.load(fname2, Resource::OT_TBN);
			_res.load_SPL_demo();
			_res.load("level1", Resource::OT_SGD);
			break;
		}
		{
			const char *name = lvl->nameAmiga;
			if (_currentLevel == 4) {
				name = _gameLevels[3].nameAmiga;
			}
			_res.load(name, Resource::OT_MBK);
			if (_currentLevel == 6) {
				_res.load(_gameLevels[5].nameAmiga, Resource::OT_CT);
			} else {
				_res.load(name, Resource::OT_CT);
			}
			_res.load(name, Resource::OT_PAL);
			_res.load(name, Resource::OT_RPC);
			_res.load(name, Resource::OT_SPC);
			if (_currentLevel == 1) {
				_res.load("level2_1", Resource::OT_LEV);
				_res._levNum = 1;
			} else {
				_res.load(name, Resource::OT_LEV);
			}
		}
		_res.load(lvl->nameAmiga, Resource::OT_PGE);
		_res.load(lvl->nameAmiga, Resource::OT_OBC);
		_res.load(lvl->nameAmiga, Resource::OT_ANI);
		_res.load(lvl->nameAmiga, Resource::OT_TBN);
		{
			char name[32];
			snprintf(name, sizeof(name), "level%d", lvl->sound);
#ifdef ATARIST
			// up to 152K of samples a plain ST cannot play: it gets the
			// YM's versions instead, built here rather than mid-play
			extern bool ATARIST_samplesPlayable();
			if (!ATARIST_samplesPlayable()) {
				ST_prepareYmSfx();
			} else
#endif
			_res.load(name, Resource::OT_SPL);
		}
		if (_currentLevel == 0) {
			_res.load(lvl->nameAmiga, Resource::OT_SGD);
		}
		break;
	case kResourceTypeDOS:
	case kResourceTypePC98:
	case kResourceTypeSega:
		_res.load(lvl->name, Resource::OT_MBK);
		_res.load(lvl->name, Resource::OT_CT);
		_res.load(lvl->name, Resource::OT_PAL);
		_res.load(lvl->name, Resource::OT_RP);
		if (_res.isPC98()) {
			_res.PC98_loadLevelMap(_currentLevel);
		} else if (_res._isDemo || g_options.use_tile_data || _res._aba) { // use .BNQ/.LEV/(.SGD) instead of .MAP (PC demo)
			if (_currentLevel == 0) {
				_res.load(lvl->name, Resource::OT_SGD);
			}
			_res.load(lvl->name, Resource::OT_LEV);
			_res.load(lvl->name, Resource::OT_BNQ);
		} else if (_res.isSega()) {
			if (_currentLevel == 0) {
				_res.load(lvl->name, Resource::OT_SGD);
			}
			_res.load(lvl->name, Resource::OT_LEV);
		} else {
			_res.load(lvl->name, Resource::OT_MAP);
		}
		_res.load(lvl->name2, Resource::OT_PGE);
		if (!_res.isSega()) {
			_res.load(lvl->name2, Resource::OT_OBJ);
			_res.load(lvl->name2, Resource::OT_ANI);
		}
		_res.load(lvl->name2, Resource::OT_TBN);
		break;
	case kResourceTypeMac:
		_res.MAC_unloadLevelData();
		_res.MAC_loadLevelData(_currentLevel);
		break;
	}
	if (0) {
		for (int i = 0; i < 64; ++i) {
			if (hasLevelRoom(_currentLevel, i)) {
				_currentRoom = i;
				loadLevelRoom();
				uint8_t palette[256 * 3];
				_stub->getPalette(palette, 256);
				char name[64];
				snprintf(name, sizeof(name), "DUMP/level%d_room%02d.bmp", _currentLevel, i);
				saveBMP(name, _vid._backLayer, palette, _vid._w, _vid._h);

				memcpy(_vid._frontLayer, _vid._backLayer, _vid._layerSize);
				for (int y = 0; y < 7; ++y) {
					for (int x = 0; x < 16; ++x) {
						const uint8_t num = _res._ctData[0x100 + _currentRoom * 0x70 + y * 16 + x];
						char buf[16];
						snprintf(buf, sizeof(buf), "%d", num);
						_vid.drawString(buf, x * 16, y * 36 + 8, 0xE7);
					}
				}
				snprintf(name, sizeof(name), "DUMP/level%d_room%02d_grid.bmp", _currentLevel, i);
				saveBMP(name, _vid._frontLayer, palette, _vid._w, _vid._h);
			}
		}
	}

	_cut._id = (int8_t)lvl->cutscene_id;

	_curMonsterNum = 0xFFFF;
	_curMonsterFrame = 0;

	_res.clearBankData();
	_printLevelCodeCounter = 150;

	_col_slots2Cur = _col_slots2;
	_col_slots2Next = 0;

	pge_clearLive();
	memset(_pge_liveTable1, 0, sizeof(_pge_liveTable1));

	_currentRoom = _res._pgeInit[0].init_room;
	uint16_t n = _res._pgeNum;
	while (n--) {
		pge_loadForCurrentLevel(n);
	}

	if (_demoBin != -1) {
		_cut._id = 0xFFFF;
		if (_res.isMac()) {
			_pgeLive[0].room_location = _demoInputsMac[_demoBin].room;
			_pgeLive[0].pos_x = _demoInputsMac[_demoBin].x;
			_pgeLive[0].pos_y = _demoInputsMac[_demoBin].y;
			_inp_demPos = 0;
		} else if (_demoInputs[_demoBin].room != 255) {
			_pgeLive[0].room_location = _demoInputs[_demoBin].room;
			_pgeLive[0].pos_x = _demoInputs[_demoBin].x;
			_pgeLive[0].pos_y = _demoInputs[_demoBin].y;
			_inp_demPos = 0;
		} else {
			_inp_demPos = 1;
		}
		_printLevelCodeCounter = 0;
	}

	for (uint16_t i = 0; i < _res._pgeNum; ++i) {
		if (_res._pgeInit[i].skill <= _skillLevel) {
			LivePGE *pge = &_pgeLive[i];
			pge->next_PGE_in_room = _pge_liveTable1[pge->room_location];
			_pge_liveTable1[pge->room_location] = pge;
		}
	}
	pge_resetMessages();
	_validSaveState = false;

	_mix.playMusic(Mixer::MUSIC_TRACK + lvl->track);

	if (_widescreenMode == kWidescreenCDi) {
		char name[16];
		snprintf(name, sizeof(name), "flashp%d.bob", _currentLevel + 1);
		File f;
		if (f.open(name, "rb", _fs)) {
			const int w = f.readUint16LE();
			const int h = f.readUint16LE();
			if (w != kWidescreenBorderCDiW || h != kWidescreenBorderCDiH) {
				warning("Unsupported CDi border w:%d h:%d", w, h);
			} else {
				f.read(_res._scratchBuffer, 256 * 3 + w * h);
				_stub->copyWidescreenCDi(w, h, _res._scratchBuffer + 256 * 3, _res._scratchBuffer);
			}
		}
	}
}

void Game::drawIcon(uint8_t iconNum, int16_t x, int16_t y, uint8_t colMask, uint8_t *layer) {
#ifdef ATARIST
	if (_res._type == kResourceTypeAmiga) {
		// Into another layer - the icon's place in the background - it
		// leaves the priority plane alone, so sprites crossing it keep
		// the fast path; drawCurrentInventoryItem puts it back on top of
		// them. The bake keys on the icon number: icons are decoded once
		// (ST_decodeIcon), where drawIcon used to unpack four bitplanes
		// every frame - 18ms of a 93ms frame on a stock ST.
		const bool front = !layer || layer == _vid._frontLayer;
		ST_sprDraw(front ? _vid._frontLayer : layer, kSprKindIcon, (const uint8_t *)(uintptr_t)(iconNum + 1), kSprIdIcon,
		           16, 16, x, y, colMask << 4, 0, front && (colMask & 8) != 0);
		if (front) {
			_vid.markBlockAsDirty(x, y, 16, 16, _vid._layerScale);
		}
		return;
	}
#endif
	uint8_t scratch[16 * 16];
	uint8_t *buf = scratch;
	switch (_res._type) {
	case kResourceTypeAmiga:
		if (iconNum > 30) {
			// inventory icons
			switch (iconNum) {
			case 76: // cursor
				memset(buf, 0, 16 * 16);
				for (int i = 0; i < 3; ++i) {
					buf[i] = buf[15 * 16 + (15 - i)] = 1;
					buf[i * 16] = buf[(15 - i) * 16 + 15] = 1;
				}
				break;
			case 77: // up - icon.spr 4
				memset(buf, 0, 16 * 16);
				_vid.AMIGA_decodeIcn(_res._icn, 35, buf);
				break;
			case 78: // down - icon.spr 5
				memset(buf, 0, 16 * 16);
				_vid.AMIGA_decodeIcn(_res._icn, 36, buf);
				break;
			default:
				memset(buf, 5, 16 * 16);
				break;
			}
		} else {
			_vid.AMIGA_decodeIcn(_res._icn, iconNum, buf);
		}
		break;
	case kResourceTypeDOS:
	case kResourceTypePC98:
		_vid.DOS_decodeIcn(_res._icn, iconNum, buf);
		break;
	case kResourceTypeMac:
		switch (iconNum) {
		case 76: // cursor
			iconNum = 32;
			break;
		case 77: // up
			iconNum = 33;
			break;
		case 78: // down
			iconNum = 34;
			break;
		}
		_vid.MAC_drawSprite(x, y, _res._icn, iconNum, false, true);
		return;
	case kResourceTypeSega:
		_vid.SEGA_decodeIcn(_res._icn, iconNum, buf);
		break;
	}
#ifdef ATARIST
	{
		uint8_t map16[16];
		ST_buildMap16(colMask << 4, map16);
		ST_drawSprite(layer ? layer : _vid._frontLayer, buf, 16, x, y, 16, 16, map16, 0, false);
	}
#else
	_vid.drawSpriteSub1(buf, _vid._frontLayer + x + y * _vid._w, 16, 16, 16, colMask << 4);
#endif
	_vid.markBlockAsDirty(x, y, 16, 16, _vid._layerScale);
}

#ifdef ATARIST
// An Amiga icon as chunky 16x16, for its bake (drawIcon)
void Game::ST_decodeIcon(uint8_t iconNum, uint8_t *buf) {
	if (iconNum > 30) {
		// inventory icons
		switch (iconNum) {
		case 76: // cursor
			memset(buf, 0, 16 * 16);
			for (int i = 0; i < 3; ++i) {
				buf[i] = buf[15 * 16 + (15 - i)] = 1;
				buf[i * 16] = buf[(15 - i) * 16 + 15] = 1;
			}
			break;
		case 77: // up - icon.spr 4
			memset(buf, 0, 16 * 16);
			_vid.AMIGA_decodeIcn(_res._icn, 35, buf);
			break;
		case 78: // down - icon.spr 5
			memset(buf, 0, 16 * 16);
			_vid.AMIGA_decodeIcn(_res._icn, 36, buf);
			break;
		default:
			memset(buf, 5, 16 * 16);
			break;
		}
	} else {
		_vid.AMIGA_decodeIcn(_res._icn, iconNum, buf);
	}
}
#endif

void Game::playSound(uint8_t num, uint8_t softVol) {
	debug(DBG_GAME, "playSound num:%d volume:%d", num, softVol);
#ifdef ATARIST
	if (_res._numSfx == 0 && num < Resource::NUM_SFXS) {
		// no samples loaded: this machine cannot play them, so the
		// YM's simplified version where there is one (ym_sfx_st.cpp)
		ST_playYmSfx(num, softVol);
		return;
	}
#endif
	if (num < _res._numSfx) {
		SoundFx *sfx = &_res._sfxList[num];
		if (sfx->data) {
			const int volume = Mixer::MAX_VOLUME >> (2 * softVol);
			_mix.play(sfx->data, sfx->len, sfx->freq, volume);
		}
	} else if (num == 66) {
		// open/close inventory (DOS)
	} else if (num >= 68 && num <= 75) {
		// in-game music
		_mix.playMusic(num);
	} else if (num == 76) {
		// metro
	} else if (num == 77) {
		// triggered when Conrad draw his gun
	} else {
		warning("Unknown sound num %d", num);
	}
}

uint16_t Game::getRandomNumber() {
	uint32_t n = _randSeed * 2;
	if (((int32_t)_randSeed) >= 0) {
		n ^= 0x1D872B41;
	}
	_randSeed = n;
	return n & 0xFFFF;
}

void Game::changeLevel() {
	_vid.fadeOut();
	clearStateRewind();
	loadLevelData();
	loadLevelRoom();
	_vid.setPalette0xF();
	_vid.setTextPalette();
	_vid.fullRefresh();
}

void Game::handleInventory() {
	const int soundNum = _res.isMac() ? 52 : 66;
	LivePGE *selected_pge = 0;
	LivePGE *pge = &_pgeLive[0];
	if (pge->life > 0 && pge->current_inventory_PGE != 0xFF) {
		playSound(soundNum, 0);
		InventoryItem items[24];
		int num_items = 0;
		uint8_t inv_pge = pge->current_inventory_PGE;
		while (inv_pge != 0xFF) {
			items[num_items].icon_num = _res._pgeInit[inv_pge].icon_num;
			items[num_items].init_pge = &_res._pgeInit[inv_pge];
			items[num_items].live_pge = &_pgeLive[inv_pge];
			inv_pge = _pgeLive[inv_pge].next_inventory_PGE;
			++num_items;
		}
		items[num_items].icon_num = 0xFF;
		int current_item = 0;
		int num_lines = (num_items - 1) / 4 + 1;
		int current_line = 0;
		bool display_score = false;
		while (!_stub->_pi.backspace && !_stub->_pi.quit) {
			static const int icon_spr_w = 16;
			static const int icon_spr_h = 16;
			switch (_res._type) {
			case kResourceTypeAmiga:
			case kResourceTypeDOS:
			case kResourceTypePC98: {
					// draw inventory background
					int icon_num = 31;
					for (int y = 140; y < 140 + 5 * icon_spr_h; y += icon_spr_h) {
						for (int x = 56; x < 56 + 9 * icon_spr_w; x += icon_spr_w) {
							drawIcon(icon_num, x, y, 0xF);
							++icon_num;
						}
					}
				}
				if (_res._type == kResourceTypeAmiga) {
					// draw outline rectangle
					static const uint8_t outline_color = 0xE7;
#ifdef ATARIST
					ST_hspan(_vid._frontLayer, 57, 56 + 9 * icon_spr_w - 2, 140, outline_color);
					for (int y = 1; y < 5 * icon_spr_h - 1; ++y) {
						ST_drawPoint(_vid._frontLayer, 56, 140 + y, outline_color);
						ST_drawPoint(_vid._frontLayer, 56 + 9 * icon_spr_w - 1, 140 + y, outline_color);
					}
					ST_hspan(_vid._frontLayer, 57, 56 + 9 * icon_spr_w - 2, 140 + 5 * icon_spr_h - 1, outline_color);
					_vid.markBlockAsDirty(56, 140, 9 * icon_spr_w, 5 * icon_spr_h, 1);
#else
					uint8_t *p = _vid._frontLayer + 140 * Video::GAMESCREEN_W + 56;
					memset(p + 1, outline_color, 9 * icon_spr_w - 2);
					p += Video::GAMESCREEN_W;
					for (int y = 1; y < 5 * icon_spr_h - 1; ++y) {
						p[0] = p[9 * icon_spr_w - 1] = outline_color;
						p += Video::GAMESCREEN_W;
					}
					memset(p + 1, outline_color, 9 * icon_spr_w - 2);
#endif
				}
				break;
			case kResourceTypeMac:
				drawIcon(31, 56, 140, 0xF);
				break;
			case kResourceTypeSega:
				break;
			}
			if (!display_score) {
				int icon_x_pos = 72;
				for (int i = 0; i < 4; ++i) {
					int item_it = current_line * 4 + i;
					if (items[item_it].icon_num == 0xFF) {
						break;
					}
					drawIcon(items[item_it].icon_num, icon_x_pos, 157, 0xA);
					if (current_item == item_it) {
						drawIcon(76, icon_x_pos, 157, 0xA);
						selected_pge = items[item_it].live_pge;
						uint8_t txt_num = items[item_it].init_pge->text_num;
						const uint8_t *str = _res.getTextString(_currentLevel, txt_num);
						drawString(str, Video::GAMESCREEN_W, TEXT_ROW(189), 0xED, true);
						if (items[item_it].init_pge->init_flags & 4) {
							char buf[10];
							snprintf(buf, sizeof(buf), "%d", selected_pge->life);
							_vid.drawString(buf, (Video::GAMESCREEN_W - strlen(buf) * Video::CHAR_W) / 2, TEXT_ROW(197), 0xED);
						}
					}
					icon_x_pos += 32;
				}
				if (current_line != (g_options.order_inventory_original ? 0 : (num_lines - 1))) {
					drawIcon(77, 120, 143, 0xA); // up arrow
				}
				if (current_line != (g_options.order_inventory_original ? (num_lines - 1) : 0)) {
					drawIcon(78, 120, 176, 0xA); // down arrow
				}
			} else {
				char buf[50];
				snprintf(buf, sizeof(buf), "SCORE %08u", _score);
				_vid.drawString(buf, (114 - strlen(buf) * Video::CHAR_W) / 2 + 72, TEXT_ROW(158), 0xE5);
				snprintf(buf, sizeof(buf), "%s:%s", _res.getMenuString(LocaleData::LI_06_LEVEL), _res.getMenuString(LocaleData::LI_13_EASY + _skillLevel));
				_vid.drawString(buf, (114 - strlen(buf) * Video::CHAR_W) / 2 + 72, TEXT_ROW(166), 0xE5);
				if (0) { // if the protection screen code was not properly cracked...
					static const uint8_t kCrackerText[17] = {
						0x19, 0x08, 0x1B, 0x19, 0x11, 0x1F, 0x08, 0x67, 0x18,
						0x16, 0x1B, 0x13, 0x08, 0x1F, 0x1B, 0x0F, 0x5A
					};
					for (int i = 0; i < 17; ++i) {
						buf[i] = kCrackerText[i] ^ 0x5A;
					}
					_vid.drawString(buf, 65, 193, 0xE4);
				}
			}

			_vid.updateScreen();
			_stub->sleep(80);
			inp_update();

			if (g_options.order_inventory_original) {
				if (_stub->_pi.dirMask & PlayerInput::DIR_DOWN) {
					_stub->_pi.dirMask &= ~PlayerInput::DIR_DOWN;
					if (current_line < num_lines - 1) {
						++current_line;
						current_item = current_line * 4;
					}
				}
				if (_stub->_pi.dirMask & PlayerInput::DIR_UP) {
					_stub->_pi.dirMask &= ~PlayerInput::DIR_UP;
					if (current_line > 0) {
						--current_line;
						current_item = current_line * 4;
					}
				}
			} else {
				if (_stub->_pi.dirMask & PlayerInput::DIR_UP) {
					_stub->_pi.dirMask &= ~PlayerInput::DIR_UP;
					if (current_line < num_lines - 1) {
						++current_line;
						current_item = current_line * 4;
					}
				}
				if (_stub->_pi.dirMask & PlayerInput::DIR_DOWN) {
					_stub->_pi.dirMask &= ~PlayerInput::DIR_DOWN;
					if (current_line > 0) {
						--current_line;
						current_item = current_line * 4;
					}
				}
			}
			if (_stub->_pi.dirMask & PlayerInput::DIR_LEFT) {
				_stub->_pi.dirMask &= ~PlayerInput::DIR_LEFT;
				if (current_item > 0) {
					const int item_num = current_item % 4;
					if (item_num > 0) {
						--current_item;
					}
				}
			}
			if (_stub->_pi.dirMask & PlayerInput::DIR_RIGHT) {
				_stub->_pi.dirMask &= ~PlayerInput::DIR_RIGHT;
				if (current_item < num_items - 1) {
					const int item_num = current_item % 4;
					if (item_num < 3) {
						++current_item;
					}
				}
			}
			if (_stub->_pi.enter) {
				_stub->_pi.enter = false;
				display_score = !display_score;
			}
		}
		_vid.fullRefresh();
		_stub->_pi.backspace = false;
		if (selected_pge) {
			pge_setCurrentInventoryObject(selected_pge);
		}
		playSound(soundNum, 0);
	}
}

void Game::inp_update() {
	_stub->processEvents();
#ifdef ATARIST
	if (_stRecording || _stReplaying) {
		ST_recStep();
	}
#endif
	if (_demoBin != -1 && _inp_demPos < _res._demLen) {
		setKeymask(_stub->_pi, _res._dem[_inp_demPos++]);
	}
}

void Game::makeGameStateName(uint8_t slot, char *buf) {
#ifdef ATARIST
	// GEMDOS 8.3
	sprintf(buf, "RS%d_%02d.SAV", _currentLevel + 1, slot);
#else
	sprintf(buf, "rs-level%d-%02d.state", _currentLevel + 1, slot);
#endif
}

#ifdef USE_ZLIB
#define SAVE_MODE "zwb"
#define LOAD_MODE "zrb"
#else
#define SAVE_MODE "wb"
#define LOAD_MODE "rb"
#endif

// 3: persist _pge_opGunVar
static const int kSaveVersion = 3;

static const uint32_t TAG_FBSV = 0x46425356;

bool Game::saveGameState(uint8_t slot) {
	if (slot == kAutoSaveSlot) {
		return saveStateRewind();
	}
	bool success = false;
	char stateFile[32];
	makeGameStateName(slot, stateFile);
	File f;
	if (!f.open(stateFile, SAVE_MODE, _savePath)) {
		warning("Unable to save state file '%s'", stateFile);
	} else {
		// header
		f.writeUint32BE(TAG_FBSV);
		f.writeUint16BE(kSaveVersion);
		char buf[32];
		memset(buf, 0, sizeof(buf));
		snprintf(buf, sizeof(buf), "level=%d room=%d", _currentLevel + 1, _currentRoom);
		f.write(buf, sizeof(buf));
		// contents
		saveState(&f);
		if (f.ioErr()) {
			warning("I/O error when saving game state");
		} else {
			info("Saved state to slot %d", slot);
			success = true;
		}
	}
	return success;
}

bool Game::loadGameState(uint8_t slot) {
	if (slot == kAutoSaveSlot) {
		return loadStateRewind();
	}
	bool success = false;
	char stateFile[32];
	makeGameStateName(slot, stateFile);
	File f;
	if (!f.open(stateFile, LOAD_MODE, _savePath)) {
		warning("Unable to open state file '%s'", stateFile);
	} else {
		uint32_t id = f.readUint32BE();
		if (id != TAG_FBSV) {
			warning("Bad save state format");
		} else {
			const uint16_t version = f.readUint16BE();
			if (version < 2) {
				warning("Invalid save state version");
			} else {
				// header
				char buf[32];
				f.read(buf, sizeof(buf));
				// contents
				loadState(&f, version);
				if (f.ioErr()) {
					warning("I/O error when loading game state");
				} else {
					info("Loaded state from slot %d", slot);
					success = true;
				}
			}
		}
	}
	return success;
}

void Game::saveState(File *f) {
	f->writeByte(_skillLevel);
	f->writeUint32BE(_score);
	if (_col_slots2Cur == 0) {
		f->writeUint32BE(0xFFFFFFFF);
	} else {
		f->writeUint32BE(_col_slots2Cur - &_col_slots2[0]);
	}
	if (_col_slots2Next == 0) {
		f->writeUint32BE(0xFFFFFFFF);
	} else {
		f->writeUint32BE(_col_slots2Next - &_col_slots2[0]);
	}
	for (int i = 0; i < _res._pgeNum; ++i) {
		LivePGE *pge = &_pgeLive[i];
		f->writeUint16BE(pge->obj_type);
		f->writeUint16BE(pge->pos_x);
		f->writeUint16BE(pge->pos_y);
		f->writeByte(pge->anim_seq);
		f->writeByte(pge->room_location);
		f->writeUint16BE(pge->life);
		f->writeUint16BE(pge->counter_value);
		f->writeByte(pge->collision_slot);
		f->writeByte(pge->next_inventory_PGE);
		f->writeByte(pge->current_inventory_PGE);
		f->writeByte(pge->ref_inventory_PGE);
		f->writeUint16BE(pge->anim_number);
		f->writeByte(pge->flags);
		f->writeByte(pge->index);
		f->writeUint16BE(pge->first_obj_number);
		if (pge->next_PGE_in_room == 0) {
			f->writeUint32BE(0xFFFFFFFF);
		} else {
			f->writeUint32BE(pge->next_PGE_in_room - &_pgeLive[0]);
		}
		if (pge->init_PGE == 0) {
			f->writeUint32BE(0xFFFFFFFF);
		} else {
			f->writeUint32BE(pge->init_PGE - &_res._pgeInit[0]);
		}
	}
	f->write(&_res._ctData[0x100], 0x1C00);
	for (CollisionSlot2 *cs2 = &_col_slots2[0]; cs2 < _col_slots2Cur; ++cs2) {
		if (cs2->next_slot == 0) {
			f->writeUint32BE(0xFFFFFFFF);
		} else {
			f->writeUint32BE(cs2->next_slot - &_col_slots2[0]);
		}
		if (cs2->collision_grid_ptr == 0) {
			f->writeUint32BE(0xFFFFFFFF);
		} else {
			f->writeUint32BE(cs2->collision_grid_ptr - &_res._ctData[0x100]);
		}
		f->writeByte(cs2->data_size);
		f->write(cs2->data_buf, 0x10);
	}
	f->writeUint16BE(_pge_opGunVar);
}

void Game::loadState(File *f, int version) {
	uint16_t i;
	uint32_t off;
	_skillLevel = f->readByte();
	_score = f->readUint32BE();
	pge_clearLive();
	memset(_pge_liveTable1, 0, sizeof(_pge_liveTable1));
	off = f->readUint32BE();
	if (off == 0xFFFFFFFF) {
		_col_slots2Cur = 0;
	} else {
		_col_slots2Cur = &_col_slots2[0] + off;
	}
	off = f->readUint32BE();
	if (off == 0xFFFFFFFF) {
		_col_slots2Next = 0;
	} else {
		_col_slots2Next = &_col_slots2[0] + off;
	}
	for (i = 0; i < _res._pgeNum; ++i) {
		LivePGE *pge = &_pgeLive[i];
		pge->obj_type = f->readUint16BE();
		pge->pos_x = f->readUint16BE();
		pge->pos_y = f->readUint16BE();
		pge->anim_seq = f->readByte();
		pge->room_location = f->readByte();
		pge->life = f->readUint16BE();
		pge->counter_value = f->readUint16BE();
		pge->collision_slot = f->readByte();
		pge->next_inventory_PGE = f->readByte();
		pge->current_inventory_PGE = f->readByte();
		pge->ref_inventory_PGE = f->readByte();
		pge->anim_number = f->readUint16BE();
		pge->flags = f->readByte();
		pge->index = f->readByte();
		pge->first_obj_number = f->readUint16BE();
		off = f->readUint32BE();
		if (off == 0xFFFFFFFF) {
			pge->next_PGE_in_room = 0;
		} else {
			pge->next_PGE_in_room = &_pgeLive[0] + off;
		}
		off = f->readUint32BE();
		if (off == 0xFFFFFFFF) {
			pge->init_PGE = 0;
		} else {
			pge->init_PGE = &_res._pgeInit[0] + off;
		}
	}
	f->read(&_res._ctData[0x100], 0x1C00);
	for (CollisionSlot2 *cs2 = &_col_slots2[0]; cs2 < _col_slots2Cur; ++cs2) {
		off = f->readUint32BE();
		if (off == 0xFFFFFFFF) {
			cs2->next_slot = 0;
		} else {
			cs2->next_slot = &_col_slots2[0] + off;
		}
		off = f->readUint32BE();
		if (off == 0xFFFFFFFF) {
			cs2->collision_grid_ptr = 0;
		} else {
			cs2->collision_grid_ptr = &_res._ctData[0x100] + off;
		}
		cs2->data_size = f->readByte();
		f->read(cs2->data_buf, 0x10);
	}
	for (i = 0; i < _res._pgeNum; ++i) {
		if (_res._pgeInit[i].skill <= _skillLevel) {
			LivePGE *pge = &_pgeLive[i];
			if (pge->flags & 4) {
				pge_setLive(pge->index, pge);
			}
			pge->next_PGE_in_room = _pge_liveTable1[pge->room_location];
			_pge_liveTable1[pge->room_location] = pge;
		}
	}
	resetGameState();
	if (version >= 3) {
		_pge_opGunVar = f->readUint16BE();
	}
}

void Game::clearStateRewind() {
	// info("Clear rewind state (count %d)", _rewindLen);
	for (int i = 0; i < _rewindLen; ++i) {
		int ptr = _rewindPtr - i;
		if (ptr < 0) {
			ptr += kRewindSize;
		}
		_rewindBuffer[ptr].close();
	}
	_rewindPtr = -1;
	_rewindLen = 0;
}

bool Game::saveStateRewind() {
	if (_rewindPtr == kRewindSize - 1) {
		_rewindPtr = 0;
	} else {
		++_rewindPtr;
	}
	static const int kGameStateSize = 16384;
	File &f = _rewindBuffer[_rewindPtr];
	f.openMemoryBuffer(kGameStateSize);
	saveState(&f);
	if (_rewindLen < kRewindSize) {
		++_rewindLen;
	}
	// info("Save state for rewind (index %d, count %d, size %d)", _rewindPtr, _rewindLen, f.size());
	return !f.ioErr();
}

bool Game::loadStateRewind() {
	const int ptr = _rewindPtr;
	if (_rewindPtr == 0) {
		_rewindPtr = kRewindSize - 1;
	} else {
		--_rewindPtr;
	}
	File &f = _rewindBuffer[ptr];
	f.seek(0);
	loadState(&f, kSaveVersion);
	if (_rewindLen > 0) {
		--_rewindLen;
	}
	// info("Rewind state (index %d, count %d, size %d)", ptr, _rewindLen, f.size());
	return !f.ioErr();
}

void AnimBuffers::addState(uint8_t stateNum, int16_t x, int16_t y, const uint8_t *dataPtr, LivePGE *pge, uint8_t w, uint8_t h) {
	debug(DBG_GAME, "AnimBuffers::addState() stateNum=%d x=%d y=%d dataPtr=%p pge=%p", stateNum, x, y, dataPtr, pge);
	assert(stateNum < 4);
	AnimBufferState *state = _states[stateNum];
	state->x = x;
	state->y = y;
	state->w = w;
	state->h = h;
	state->dataPtr = dataPtr;
	state->pge = pge;
	++_curPos[stateNum];
	++_states[stateNum];
}
