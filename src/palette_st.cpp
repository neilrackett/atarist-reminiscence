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
 * .hex palettes; comments, blank lines and a # before a colour are
 * allowed. Colours are rounded to the STE's 4 bits per channel. After
 * a colour, "= 1,2,17" optionally lists which of the Amiga's 32
 * colours (0-15 background, 16-31 Conrad and objects - see the map in
 * video.cpp) it replaces; anything not listed takes the nearest.
 *
 * Ctrl+P writes the room's palette to the game folder under the
 * room's name, in the same format: the room's 32 Amiga colours
 * numbered in comments, and each of the 16 lines listing the ones it
 * is used for now. An author starts from what the game chose (or from
 * the custom palette in use), changes colours and moves numbers, and
 * puts the file in PALETTE\. Ctrl+Shift+P rescans the folder and
 * redraws the room with whatever now applies, to see an edit at once.
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

void ST_setCustomPalette(const Color *cols, const uint8_t *map32);
void ST_getHwColours(Color *cols);
void ST_getAmigaColours(Color *cols, uint8_t *slots, bool *have);

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

// the six hex digits of a colour at p, or -1
static long parseColour(const char *p) {
	long c = 0;
	for (int i = 0; i < 6; ++i) {
		const int d = hexDigit(p[i]);
		if (d < 0) {
			return -1;
		}
		c = (c << 4) | d;
	}
	return c;
}

// One line: RRGGBB, optionally "= n,n,..." naming the Amiga colours
// (0-31) it replaces, optionally a # comment. A line that is only a
// comment, or blank, is skipped; so is a leading # before the colour,
// the form some tools write. Returns 1 for a colour, 0 for nothing,
// -1 for a line that is neither.
static int parseLine(const char *p, const char *end, Color *col, uint8_t *map32, int slot, const char *path) {
	while (p < end && (*p == ' ' || *p == '\t')) {
		++p;
	}
	if (p >= end) {
		return 0;
	}
	if (*p == '#') {
		if (end - p < 7 || parseColour(p + 1) < 0) {
			return 0;                          // a comment
		}
		++p;
	}
	if (end - p < 6) {
		return -1;
	}
	const long rgb = parseColour(p);
	if (rgb < 0) {
		return -1;
	}
	p += 6;
	// to the STE's 4 bits, stored as n * 17 like the quantiser's colours
	const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
	col->r = (uint8_t)(((r * 15 + 127) / 255) * 17);
	col->g = (uint8_t)(((g * 15 + 127) / 255) * 17);
	col->b = (uint8_t)(((b * 15 + 127) / 255) * 17);
	while (p < end && (*p == ' ' || *p == '\t')) {
		++p;
	}
	if (p < end && *p == '=') {
		++p;
		for (;;) {
			while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) {
				++p;
			}
			int n;
			if (p >= end || !parseNumber(p, &n)) {
				break;
			}
			if (n > 31) {
				warning("%s: no Amiga colour %d (0-31)", path, n);
				continue;
			}
			if (slot < 16 && map32[n] != 0xFF && map32[n] != slot) {
				warning("%s: Amiga colour %d listed twice, the later line wins", path, n);
			}
			if (slot < 16) {
				map32[n] = (uint8_t)slot;
			}
		}
	}
	return 1;
}

// 16 colours, and any Amiga colours they are listed against; false if
// the file is not that
static bool loadFile(const char *name, Color *cols, uint8_t *map32) {
	char path[32];
	snprintf(path, sizeof(path), "PALETTE\\%s", name);
	const long fh = Fopen(path, 0);
	if (fh < 0) {
		return false;
	}
	static char buf[4096];
	const long len = Fread((short)fh, sizeof(buf) - 1, buf);
	Fclose((short)fh);
	if (len <= 0) {
		return false;
	}
	buf[len] = 0;
	memset(map32, 0xFF, 32);
	int n = 0;
	const char *p = buf;
	while (*p) {
		const char *eol = p;
		while (*eol && *eol != '\n' && *eol != '\r') {
			++eol;
		}
		Color c;
		const int r = parseLine(p, eol, &c, map32, n, path);
		if (r < 0) {
			warning("%s: not a line of RRGGBB", path);
			return false;
		}
		if (r > 0) {
			if (n < 16) {
				cols[n] = c;
			}
			++n;
		}
		p = eol;
		while (*p == '\n' || *p == '\r') {
			++p;
		}
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
	uint8_t map32[32];
	if (name[0] && loadFile(name, cols, map32)) {
		ST_setCustomPalette(cols, map32);
		info("Palette %s", name);
	} else {
		ST_setCustomPalette(0, 0);
		name[0] = 0;
	}
	strcpy(g_current, name);
}

static char *put(char *q, const char *fmt, int a = 0, int b = 0, int c = 0, int d = 0) {
	return q + sprintf(q, fmt, a, b, c, d);
}

static int hex8(uint8_t v) {
	return (v >> 4) * 17;                      // the STE's 4 bits as 8
}

// Ctrl+Shift+P: forget what was scanned and what is in use, so the
// next room load - which the caller forces - reads PALETTE\ afresh:
// new files are found and an edited one is read again. Returns false
// when palette_custom is off.
bool ST_paletteReload() {
	if (!g_options.palette_custom) {
		return false;
	}
	g_scanned = false;
	g_levelFile = 0;
	memset(g_roomFile, 0, sizeof(g_roomFile));
	g_current[0] = 0;
	return true;
}

// what ST_paletteForRoom chose last, "" for the automatic palette
const char *ST_paletteInUse() {
	return g_current;
}

// Ctrl+P: the room's current colours as a .hex in the game folder,
// with the room's Amiga colours numbered in comments and each of the
// 16 lines listing the ones it is used for now - so an author can see
// which originals share a colour and move them. Returns the name
// written, or 0.
const char *ST_paletteDump(int level, int levNum, int room) {
	if (!g_options.palette_custom) {
		return 0;
	}
	static char name[16];
	const int part = levelPart(level, levNum);
	roomName(name, sizeof(name), level, part, room);
	Color hw[16];
	ST_getHwColours(hw);
	Color amiga[32];
	uint8_t slots[32];
	bool have[32];
	ST_getAmigaColours(amiga, slots, have);
	static char buf[4096];
	char *q = buf;
	q += sprintf(q, "# Flashback room palette: level %d%s, room %d\n", level, part ? (part == 1 ? " part 1" : " part 2") : "", room);
	q += sprintf(q, "# Save as PALETTE\\%s for this room, or PALETTE\\L%d.HEX\n", name, level);
	q += sprintf(q, "# for every room of the level without its own file.\n#\n");
	q += sprintf(q, "# 16 lines of RRGGBB: the colours the room is drawn with.\n");
	q += sprintf(q, "# After a colour, \"= 1,2,17\" lists which of the Amiga's 32\n");
	q += sprintf(q, "# colours below it replaces. The lists are optional: an Amiga\n");
	q += sprintf(q, "# colour not listed uses whichever of the 16 is nearest.\n#\n");
	q += sprintf(q, "# The Amiga's 32 colours in this room:\n");
	for (int row = 0; row < 8; ++row) {
		q += sprintf(q, row == 0 ? "# background" : row == 4 ? "# objects   " : "#           ");
		for (int k = 0; k < 4; ++k) {
			const int n = row * 4 + k;
			if (have[n]) {
				q += sprintf(q, "  %2d %02x%02x%02x", n, hex8(amiga[n].r), hex8(amiga[n].g), hex8(amiga[n].b));
			} else {
				q += sprintf(q, "  %2d ------", n);
			}
		}
		q = put(q, "\n");
	}
	q = put(q, "#\n");
	for (int s = 0; s < 16; ++s) {
		q += sprintf(q, "%02x%02x%02x", hex8(hw[s].r), hex8(hw[s].g), hex8(hw[s].b));
		bool first = true;
		for (int n = 0; n < 32; ++n) {
			if (have[n] && slots[n] == s) {
				q += sprintf(q, first ? " = %d" : ",%d", n);
				first = false;
			}
		}
		q = put(q, "\n");
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
