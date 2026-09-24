/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Custom room palettes (palette_custom=true in RS.CFG).
 *
 * The game's 16 hardware colours are normally chosen per room by the
 * quantiser in systemstub_stdl.cpp. A file in PALETTE\ replaces that
 * choice with 16 hand-picked colours: every logical colour then takes
 * the nearest of them. For each room the game looks for
 *
 *   PALETTE\L1R45.HEX     that room (level 1, room 45)
 *   PALETTE\L1.HEX        any room of the level without its own file
 *
 * and uses the automatic palette when neither exists. Level 2 is two
 * level files whose room numbers overlap, so its rooms are named by
 * part as well: L2_1R17.HEX and L2_2R17.HEX (L2.HEX covers both).
 *
 * A file is 16 lines of RRGGBB in hex, as Lospec and Aseprite export
 * .hex palettes; a leading # and blank lines are allowed. Colours are
 * rounded to the STE's 4 bits per channel.
 *
 * Ctrl+P writes the room's current 16 colours to the game folder
 * under the room's name, in the same format, so an author starts from
 * what the game chose (or from the custom palette in use) and moves
 * the edited file into PALETTE\.
 *
 * GEMDOS calls throughout: the C library's fopen converts the file's
 * date through mktime, over 100ms a call on a 68000, and that would
 * be paid on every room change.
 */

#ifdef ATARIST

#include <mint/osbind.h>
#include "intern.h"
#include "util.h"
#include "video_st.h"

void ST_setCustomPalette(const Color *cols);
void ST_getHwColours(Color *cols);

// what PALETTE\ holds, from one scan at startup: a bit per level file
// (L1..L7) and per room of each level part
enum { kLevels = 8, kParts = 3 };
static uint8_t g_levelFile;                 // bit n: Ln.HEX
static uint32_t g_roomFile[kLevels][kParts][2];
static bool g_scanned;
static char g_current[16];                  // file in use, "" = none

static bool parseNumber(const char *&p, int *out) {
	if (*p < '0' || *p > '9') {
		return false;
	}
	int n = 0;
	while (*p >= '0' && *p <= '9') {
		n = n * 10 + (*p++ - '0');
	}
	*out = n;
	return true;
}

// L<level>[_<part>][R<room>].HEX
static void noteFile(const char *name) {
	const char *p = name;
	int level, part = 0, room = -1;
	if (*p != 'L' || !(++p, parseNumber(p, &level)) || level < 1 || level >= kLevels) {
		return;
	}
	if (*p == '_') {
		++p;
		if (!parseNumber(p, &part) || part < 1 || part >= kParts) {
			return;
		}
	}
	if (*p == 'R') {
		++p;
		if (!parseNumber(p, &room) || room > 63) {
			return;
		}
	}
	if (strcmp(p, ".HEX") != 0) {
		return;
	}
	if (room < 0) {
		if (part == 0) {
			g_levelFile |= 1 << level;
		}
		return;
	}
	g_roomFile[level][part][room >> 5] |= 1UL << (room & 31);
}

static void scan() {
	g_scanned = true;
	_DTA dta;
	_DTA *saved = Fgetdta();
	Fsetdta(&dta);
	int count = 0;
	for (long r = Fsfirst("PALETTE\\*.HEX", 0); r == 0; r = Fsnext()) {
		noteFile(dta.dta_name);
		++count;
	}
	Fsetdta(saved);
	info("PALETTE\\: %d palette files", count);
}

static int hexDigit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// 16 RRGGBB lines into STE colours; false if the file is not that
static bool loadFile(const char *name, Color *cols) {
	char path[32];
	snprintf(path, sizeof(path), "PALETTE\\%s", name);
	const long fh = Fopen(path, 0);
	if (fh < 0) {
		return false;
	}
	char buf[512];
	const long len = Fread((short)fh, sizeof(buf) - 1, buf);
	Fclose((short)fh);
	if (len <= 0) {
		return false;
	}
	buf[len] = 0;
	int n = 0;
	const char *p = buf;
	while (*p && n <= 16) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '#') {
			++p;
		}
		if (!*p) {
			break;
		}
		int v[6];
		for (int i = 0; i < 6; ++i) {
			v[i] = hexDigit(p[i]);
			if (v[i] < 0) {
				warning("%s: not a line of RRGGBB", path);
				return false;
			}
		}
		p += 6;
		while (*p && *p != '\n') {
			++p;                       // anything after the colour
		}
		if (n < 16) {
			// to the STE's 4 bits, stored as n * 17 like the
			// quantiser's colours
			const int r = v[0] * 16 + v[1], g = v[2] * 16 + v[3], b = v[4] * 16 + v[5];
			cols[n].r = (uint8_t)(((r * 15 + 127) / 255) * 17);
			cols[n].g = (uint8_t)(((g * 15 + 127) / 255) * 17);
			cols[n].b = (uint8_t)(((b * 15 + 127) / 255) * 17);
		}
		++n;
	}
	if (n != 16) {
		warning("%s: %d colours, not 16", path, n);
		return false;
	}
	return true;
}

// the part of a level whose rooms need telling apart, else 0
static int levelPart(int level, int levNum) {
	return (level == 2 && levNum >= 1 && levNum < kParts) ? levNum : 0;
}

static void roomName(char *name, size_t size, int level, int part, int room) {
	if (part != 0) {
		snprintf(name, size, "L%d_%dR%d.HEX", level, part, room);
	} else {
		snprintf(name, size, "L%dR%d.HEX", level, room);
	}
}

// Called for every room before its palettes go in: level counts from
// 1, levNum is the level file in use (level 2 has two), and a room
// below 0 means no room - the title - so the automatic palette.
void ST_paletteForRoom(int level, int levNum, int room) {
	if (!g_options.palette_custom) {
		return;
	}
	if (!g_scanned) {
		scan();
	}
	char name[16];
	name[0] = 0;
	if (room >= 0 && level >= 1 && level < kLevels) {
		const int part = levelPart(level, levNum);
		if (g_roomFile[level][part][room >> 5] & (1UL << (room & 31))) {
			roomName(name, sizeof(name), level, part, room);
		} else if (g_levelFile & (1 << level)) {
			snprintf(name, sizeof(name), "L%d.HEX", level);
		}
	}
	if (strcmp(name, g_current) == 0) {
		return;                        // already in use (or none)
	}
	Color cols[16];
	if (name[0] && loadFile(name, cols)) {
		ST_setCustomPalette(cols);
		info("Palette %s", name);
	} else {
		ST_setCustomPalette(0);
		name[0] = 0;
	}
	strcpy(g_current, name);
}

// Ctrl+P: the room's current colours as a .hex in the game folder.
// Returns the name written, or 0.
const char *ST_paletteDump(int level, int levNum, int room) {
	if (!g_options.palette_custom) {
		return 0;
	}
	static char name[16];
	roomName(name, sizeof(name), level, levelPart(level, levNum), room);
	Color cols[16];
	ST_getHwColours(cols);
	char buf[16 * 7 + 1];
	char *q = buf;
	for (int i = 0; i < 16; ++i) {
		q += sprintf(q, "%02x%02x%02x\n", (cols[i].r >> 4) * 17, (cols[i].g >> 4) * 17, (cols[i].b >> 4) * 17);
	}
	const long fh = Fcreate(name, 0);
	if (fh < 0) {
		warning("Unable to write %s", name);
		return 0;
	}
	const long len = q - buf;
	const long written = Fwrite((short)fh, len, buf);
	Fclose((short)fh);
	if (written != len) {
		warning("Unable to write %s", name);
		return 0;
	}
	info("Wrote palette %s", name);
	return name;
}

#endif // ATARIST
