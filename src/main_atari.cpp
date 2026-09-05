
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Atari ST entry point (no SDL, no getopt - GEMDOS programs launched
 * from the desktop have no command line worth parsing).
 */

#include <ctype.h>
#include "file.h"
#include "fs.h"
#include "game.h"
#include "systemstub.h"
#include "util.h"

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
	g_options.overscan = true;
	g_options.music = false;
	g_options.log_fps = false;
	g_options.bench = false;
	g_options.logging = false;
	g_options.frame_skip = true;
	struct {
		const char *name;
		bool *value;
	} opts[] = {
		{ "bypass_protection", &g_options.bypass_protection },
		{ "enable_password_menu", &g_options.enable_password_menu },
		{ "enable_language_selection", &g_options.enable_language_selection },
		{ "use_tile_data", &g_options.use_tile_data },
		{ "use_seq_cutscenes", &g_options.use_seq_cutscenes },
		{ "use_words_protection", &g_options.use_words_protection },
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
		{ 0, 0 }
	};
	FILE *fp = fopen("RS.CFG", "rb");
	if (fp) {
		char buf[256];
		while (fgets(buf, sizeof(buf), fp)) {
			if (buf[0] == '#') {
				continue;
			}
			const char *p = strchr(buf, '=');
			if (p) {
				++p;
				while (*p && isspace(*p)) {
					++p;
				}
				if (*p) {
					const bool value = (*p == 't' || *p == 'T' || *p == '1');
					for (int i = 0; opts[i].name; ++i) {
						if (strncmp(buf, opts[i].name, strlen(opts[i].name)) == 0) {
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
}

extern SystemStub *SystemStub_STDL_create();

#undef main
int main(int argc, char *argv[]) {
	initOptions();
	FileSystem fs("DATA");
	const int version = detectVersion(&fs);
	if (version == -1) {
		printf("Unable to find Amiga data files in DATA\\ - press a key\n");
		getchar();
		return -1;
	}
	const Language language = detectLanguage(&fs);
	ScalerParameters scalerParameters = ScalerParameters::defaults();
	SystemStub *stub = SystemStub_STDL_create();
	Game *g = new Game(stub, &fs, ".", 0, (ResourceType)version, language, kWidescreenNone, false, 0, 0, 0);
	stub->init(g_caption, g->_vid._w, g->_vid._h, true, kWidescreenNone, false, &scalerParameters, 0);
	g->run();
	delete g;
	stub->destroy();
	delete stub;
	return 0;
}
