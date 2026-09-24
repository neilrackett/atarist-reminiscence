
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Atari ST entry point (no SDL, no getopt - GEMDOS programs launched
 * from the desktop have no command line worth parsing).
 */

#include <ctype.h>
#include <strings.h>
#include "file.h"
#include "fs.h"
#include "game.h"
#include "systemstub.h"
#include "util.h"
#include "version.h"

// mintlib: stack reserved by crt0 before the heap takes the rest of
// the TPA. Video::AMIGA_decodeSpm alone puts 8K on the stack.
extern "C" long _stksize = 64 * 1024;

static const Features kFeaturesAmiga = { false, true, 1, true };

const Features *g_features;
Options g_options;
const char *g_caption = "REminiscence";

ScalerParameters ScalerParameters::defaults() {
	ScalerParameters sp;
	memset(&sp, 0, sizeof(sp));
	sp.factor = 1;
	return sp;
}

static int detectVersion(FileSystem *fs) {
	static const struct {
		const char *filename;
		int type;
		const char *name;
	} table[] = {
		{ "LEVEL1.LEV", kResourceTypeAmiga, "Amiga" },
		{ "DEMO.LEV", kResourceTypeAmiga, "Amiga (Demo)" },
		{ 0, -1, 0 }
	};
	for (int i = 0; table[i].filename; ++i) {
		File f;
		if (f.open(table[i].filename, "rb", fs)) {
			info("Found %s data files", table[i].name);
			g_features = &kFeaturesAmiga;
			return table[i].type;
		}
	}
	return -1;
}

static Language detectLanguage(FileSystem *fs) {
	static const struct {
		const char *filename;
		Language language;
	} table[] = {
		{ "ENGCINE.TXT", LANG_EN },
		{ "FRCINE.TXT", LANG_FR },
		{ "GERCINE.TXT", LANG_DE },
		{ "SPACINE.TXT", LANG_SP },
		{ "ITACINE.TXT", LANG_IT },
		{ 0, LANG_EN }
	};
	for (int i = 0; table[i].filename; ++i) {
		File f;
		if (f.open(table[i].filename, "rb", fs)) {
			return table[i].language;
		}
	}
	return LANG_EN;
}

static void initOptions() {
	g_options.bypass_protection = true;
	g_options.enable_password_menu = false;
	g_options.enable_language_selection = false;
	g_options.use_tile_data = false;
	// palette fades are free on ST hardware: SystemStub::fadeScreen
	// scales the 16 palette registers instead of re-blitting 17 times
	g_options.fade_out_palette = false;
	g_options.use_text_cutscenes = false;
	g_options.use_seq_cutscenes = false;
	g_options.use_words_protection = false;
	g_options.use_white_tshirt = false;
	g_options.use_prf_music = false;
	g_options.play_asc_cutscene = false;
	g_options.play_caillou_cutscene = false;
	g_options.play_metro_cutscene = false;
	g_options.play_serrure_cutscene = false;
	g_options.play_carte_cutscene = false;
	g_options.play_gamesaved_sound = false;
	g_options.restore_memo_cutscene = true;
	g_options.order_inventory_original = false;
	g_options.fix_fmopl_e0_reg = false;
	g_options.skip_intro = false;
	g_options.crop_screen = false;
	// on by default: it shows the whole picture, and since the line
	// mapping is then one-to-one it is also the fastest of the three
	// ways of fitting 224 lines on the screen
	g_options.overscan = false;
	g_options.screen = kScreenFit;
	g_options.music = false;
	// 70% puts the chip music level with the sampled effects, which
	// a tester once called crazy loud against full YM output. The
	// figure was 60 while the STE voice mixer reserved a quarter of
	// DMA scale for four voices; STDL v1.8.2 halved that reserve, so
	// effects gained about 6dB (walking peaks -26.0 -> -20.3 dBFS)
	// and the music had to follow. Measured in Hatari on one binary:
	// effects peak -20.3, music at 70 peaks -21.4, at 80 -17.9.
	// Hardware may want a different figure, which is why it is an
	// RS.CFG option.
	g_options.music_volume = 70;
	g_options.cheats = 0;
	g_options.log_fps = false;
	g_options.bench = false;
	g_options.logging = false;
	g_options.frame_skip = true;
	g_options.blitter = true;
	// Off: the top border alone is the steady one. Both borders open
	// cleanly in the emulator - 80,000 traced frames across all four
	// wakeup states lost the bottom on three - but on hardware the
	// picture still jumps every few seconds and the bottom stripes,
	// and nothing measured so far has caught it in the act. The top
	// border shows every line either way; it just sits the picture on
	// the bottom edge instead of centring it.
	g_options.overscan_bottom = false;
	// A Mega STE is switched to 16MHz with its cache on. Off leaves
	// it at the speed set before the game ran, so it can stand in
	// for an STE when testing.
	g_options.megaste_speedup = true;
	// Draw each frame on a hidden page and flip to it at the VBL:
	// no tearing, but every frame waits for the beam.
	g_options.double_buffer = false;
	// Room palettes from PALETTE\ and Ctrl+P to write the current
	// one out (see palette_st.cpp).
	g_options.palette_custom = false;
	// The STE's sample device. Off, nothing opens the sound DMA at
	// all: no sampled effects, and the chip music (music=true) is
	// unaffected because that drives the YM instead. Exists because
	// the DMA ring loops every 82ms whether or not a sound is
	// playing, so this is the way to tell a fault in the ring from
	// a fault in something else.
	g_options.ste_sound = true;
	struct {
		const char *name;
		bool *value;
	} opts[] = {
		{ "bypass_protection", &g_options.bypass_protection },
		{ "enable_password_menu", &g_options.enable_password_menu },
		{ "enable_language_selection", &g_options.enable_language_selection },
		{ "use_prf_music", &g_options.use_prf_music },
		{ "play_gamesaved_sound", &g_options.play_gamesaved_sound },
		{ "fade_out_palette", &g_options.fade_out_palette },
		{ "use_text_cutscenes", &g_options.use_text_cutscenes },
		{ "use_white_tshirt", &g_options.use_white_tshirt },
		{ "play_asc_cutscene", &g_options.play_asc_cutscene },
		{ "play_caillou_cutscene", &g_options.play_caillou_cutscene },
		{ "play_metro_cutscene", &g_options.play_metro_cutscene },
		{ "play_serrure_cutscene", &g_options.play_serrure_cutscene },
		{ "play_carte_cutscene", &g_options.play_carte_cutscene },
		{ "restore_memo_cutscene", &g_options.restore_memo_cutscene },
		{ "order_inventory_original", &g_options.order_inventory_original },
		{ "skip_intro", &g_options.skip_intro },
		{ "crop_screen", &g_options.crop_screen },
		{ "overscan", &g_options.overscan },
		{ "music", &g_options.music },
		{ "log_fps", &g_options.log_fps },
		{ "bench", &g_options.bench },
		{ "logging", &g_options.logging },
		{ "frame_skip", &g_options.frame_skip },
		{ "blitter", &g_options.blitter },
		{ "overscan_bottom", &g_options.overscan_bottom },
		{ "megaste_speedup", &g_options.megaste_speedup },
		{ "double_buffer", &g_options.double_buffer },
		{ "palette_custom", &g_options.palette_custom },
		{ "ste_sound", &g_options.ste_sound },
		{ 0, 0 }
	};
	// options that take a number rather than true/false
	struct {
		const char *name;
		int *value;
		int min, max;
	} ints[] = {
		{ "music_volume", &g_options.music_volume, 0, 100 },
		// The engine has carried these since the desktop build's
		// --cheats: 1 monsters die in one hit, 2 Conrad is never
		// hit, 4 his life never goes down. Add them up, or say
		// true for all three. Nothing is on unless asked for.
		{ "cheats", &g_options.cheats, 0, 7 },
		{ 0, 0, 0, 0 }
	};
	bool screenSet = false;
	FILE *fp = fopen("RS.CFG", "rb");
	if (fp) {
		char buf[256];
		while (fgets(buf, sizeof(buf), fp)) {
			// a comment may be indented: the shipped RS.CFG groups
			// its options and comments the optional ones out
			const char *c = buf;
			while (*c && isspace(*c)) {
				++c;
			}
			if (*c == '#' || *c == ';' || *c == 0) {
				continue;
			}
			const char *eq = strchr(buf, '=');
			if (eq) {
				// the name is what precedes '=', matched whole:
				// a prefix match would let "overscan" claim
				// "overscan_bottom" and set the wrong option
				const char *name = buf;
				while (*name && isspace(*name)) {
					++name;
				}
				const char *nameEnd = eq;
				while (nameEnd > name && isspace(nameEnd[-1])) {
					--nameEnd;
				}
				const size_t nameLen = nameEnd - name;
				const char *p = eq + 1;
				while (*p && isspace(*p)) {
					++p;
				}
				if (*p && nameLen != 0) {
					bool found = false;
					// screen=fill|fit|top|full - the one option that takes a
					// word. Matched on the whole word, since fill and fit
					// share a first letter; anything else is left as it was.
					if (nameLen == 6 && strncmp(name, "screen", 6) == 0) {
						static const char *const words[kScreenModes] = { "fit", "fill", "top", "full" };
						for (int i = 0; i < kScreenModes; ++i) {
							const size_t n = strlen(words[i]);
							if (strncasecmp(p, words[i], n) == 0 && !isalpha((unsigned char)p[n])) {
								g_options.screen = i;
								screenSet = true;
							}
						}
						found = true;
					}
					for (int i = 0; ints[i].name; ++i) {
						if (strlen(ints[i].name) == nameLen
						    && strncmp(name, ints[i].name, nameLen) == 0) {
							// A number also answers to true and
							// false, meaning all of it and none of
							// it: cheats=true is easier to hand a
							// tester than cheats=7, and the numbers
							// stay there for anyone wanting one bit
							// rather than the lot. Digits are read
							// as digits, so cheats=1 is still the
							// first bit and not "true".
							int v;
							if (*p == 't' || *p == 'T') {
								v = ints[i].max;
							} else if (*p == 'f' || *p == 'F') {
								v = ints[i].min;
							} else {
								v = atoi(p);
							}
							if (v < ints[i].min) {
								v = ints[i].min;
							} else if (v > ints[i].max) {
								v = ints[i].max;
							}
							*ints[i].value = v;
							found = true;
							break;
						}
					}
					const bool value = (*p == 't' || *p == 'T' || *p == '1');
					for (int i = 0; !found && opts[i].name; ++i) {
						if (strlen(opts[i].name) == nameLen
						    && strncmp(name, opts[i].name, nameLen) == 0) {
							*opts[i].value = value;
							break;
						}
					}
				}
			}
		}
		fclose(fp);
	}
	// the two options whose whole output is the log
	if (g_options.log_fps || g_options.bench) {
		g_options.logging = true;
	}
	if (g_options.bench) {
		// A benchmark run ends by quitting, which looks exactly like
		// a crash to anyone who forgot the option was set: say so up
		// front, so the log explains the exit that follows.
		info("bench=true in RS.CFG: this is a benchmark run and the game will quit after 512 gameplay frames");
	}
	// The old spellings still work: overscan=true is top, with
	// overscan_bottom=true full, and crop_screen=true is fill. An old
	// RS.CFG keeps the picture it had; only the default has moved.
	if (!screenSet) {
		if (g_options.overscan) {
			g_options.screen = g_options.overscan_bottom ? kScreenFull : kScreenTop;
		} else if (g_options.crop_screen) {
			g_options.screen = kScreenFill;
		} else {
			g_options.screen = kScreenFit;
		}
	}
}

extern SystemStub *SystemStub_STDL_create();

#undef main
int main(int argc, char *argv[]) {
	// On the TOS console, where it sits through the cursor's pause
	// before the splash takes the screen: the one moment a tester can
	// read which build this is without opening a log. Just the release
	// number - r13, or r13-6992ccc+ between releases - because it is
	// there briefly and has to be readable; the log carries the full
	// form.
	printf("%s\n", PORT_RELEASE);
	initOptions();
	info("REminiscence %s", PORT_VERSION);
	FileSystem fs("DATA");
	const int version = detectVersion(&fs);
	if (version == -1) {
		// The ST console is 40 columns, so every line here is
		// written to fit one - a wrapped error reads as a mess on
		// the machine it is meant to help.
		printf("Game data not found!\n\n");
		printf("Please add the Amiga data files to a\n");
		printf("DATA folder beside FLASHBAK.TOS\n\n");
		printf("Visit neilrackett.com/atarist\n");
		printf("for more information.\n\n");
		printf("Press a key\n");
		getchar();
		return -1;
	}
	const Language language = detectLanguage(&fs);
	ScalerParameters scalerParameters = ScalerParameters::defaults();
	SystemStub *stub = SystemStub_STDL_create();
	Game *g = new Game(stub, &fs, ".", 0, (ResourceType)version, language, kWidescreenNone, false, 0, 0,
	                   (uint32_t)g_options.cheats);
	stub->init(g_caption, g->_vid._w, g->_vid._h, true, kWidescreenNone, false, &scalerParameters, 0);
	g->run();
	delete g;
	stub->destroy();
	delete stub;
	return 0;
}
