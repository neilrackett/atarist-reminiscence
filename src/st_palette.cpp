/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The ST's 16 colours from the engine's 256 (st_palette.h): moved
 * here from the ST build's screen (systemstub_stdl.cpp) as it was, so
 * that the experimental build can have the same colours.
 */

extern "C" {
#include <stdl/stdl.h>
#include <stdl/stdl_math.h>
}

#include "st_palette.h"
#include "util.h"
#include "video_st.h"
#include "palette_tables.h"

STPalette *g_stpal;

// The experimental build on a TT or a Falcon shows the 256 colours
// themselves, so the game's calls feeding the 16 have nothing to feed
static inline bool noColours() {
#ifdef ATARIST_030_ST
	return g_stpal == 0;
#else
	return false;
#endif
}

void STPalette::resetColours() {
	memset(_pal, 0, sizeof(_pal));
	memset(_basePal, 0, sizeof(_basePal));
	memset(_hwPal, 0, sizeof(_hwPal));
	memset(_remap, 0, sizeof(_remap));
	_palDirty = true;
	_remapStale = true;
	_palLocked = false;
	_cutPending = false;
	_hwDirty = true;
	_fade = 256;
	_customPal = false;
	memset(_customMap, 0xFF, sizeof(_customMap));
	memset(_amigaIdx, 0xFF, sizeof(_amigaIdx));
	_haveColW = false;
}

// Draw-time access to the logical->hardware colour map for the
// planar layer primitives (video_st.cpp).
const uint8_t *ST_getRemap() {
	if (g_stpal->_palDirty) {
		g_stpal->buildRemap();
	}
	return g_stpal->_remap;
}

// Cutscene palette mode: shapes are authored against logical
// entries 0xC0-0xCF (8 base + 8 shadow colours), so map those to
// hardware slots 0-15 directly. Baked page content then follows
// palette changes exactly like the chunky original, and the shadow
// effect (index |= 8) becomes a plane-3 OR.
static bool g_cutscenePal;

// Bumped whenever _remap changes. What bakes the mapping into planar
// pixels keys on the colours themselves (palette tokens, the pair
// tables' versions, the room cache's remap bytes); this only says when
// to look at them again.
static uint16_t g_remapGen = 1;

uint16_t ST_remapGen() {
	return g_remapGen;
}

// The game's mapping is put aside while a cutscene has the slots and
// restored afterwards: cutscenes only write logical entries 0xC0 and
// up, so the room's mapping is still right when they finish, and
// restoring it means the room in the back layer is still correct
// as decoded (no requantise, no re-bake - the quantiser alone is
// two and a half seconds on an 8MHz ST).
struct RemapSnapshot {
	bool valid;
	uint8_t remap[256];
	Color hwPal[16];
	Color basePal[256];
	Color pal[256];
};
static RemapSnapshot g_gameRemap;

void ST_setCutscenePalMode(bool enable) {
	if (noColours()) {
		return;
	}
	if (g_cutscenePal == enable) {
		return;
	}
	g_cutscenePal = enable;
	STPalette *stub = g_stpal;
	if (enable) {
		if (stub->_palDirty) {
			stub->buildRemap();
		}
		memcpy(g_gameRemap.remap, stub->_remap, sizeof(stub->_remap));
		memcpy(g_gameRemap.hwPal, stub->_hwPal, sizeof(stub->_hwPal));
		memcpy(g_gameRemap.basePal, stub->_basePal, sizeof(stub->_basePal));
		memcpy(g_gameRemap.pal, stub->_pal, sizeof(stub->_pal));
		g_gameRemap.valid = true;
		stub->_palDirty = true;
		stub->_remapStale = true;
	} else if (g_gameRemap.valid) {
		memcpy(stub->_remap, g_gameRemap.remap, sizeof(stub->_remap));
		memcpy(stub->_hwPal, g_gameRemap.hwPal, sizeof(stub->_hwPal));
		memcpy(stub->_basePal, g_gameRemap.basePal, sizeof(stub->_basePal));
		// The logical palette comes back too. It used to be left
		// holding the cutscene's entries with the remap marked dirty,
		// to be reconciled at the next flip - which was fine until two
		// cutscenes ran back to back. Dying plays the death scene and
		// then the continue screen's own scene straight after, and the
		// second one's snapshot was taken from that unreconciled state:
		// a remap rebuilt around the first scene's colours, restored
		// as if it were the room's. Every colour was wrong until the
		// level reloaded. With the palette itself put back there is
		// nothing to reconcile; only the registers need the room's
		// colours written again.
		memcpy(stub->_pal, g_gameRemap.pal, sizeof(stub->_pal));
		++g_remapGen;
		stub->_palDirty = false;
		stub->_remapStale = false;
		stub->_hwDirty = true;
	} else {
		stub->_palDirty = true;
		stub->_remapStale = true;
	}
}

bool ST_cutscenePalMode() {
	return g_cutscenePal;
}

// The cutscene shadow effect in hardware-colour space: the chunky op
// is L |= colour8 & 0xF8 in logical space, so for each hw slot take
// a representative logical entry, apply the OR, and see where the
// result lands. Built per shadow shape - 16 lookups.
void ST_getOrMap(uint8_t colour8, uint8_t *orMap) {
	if (g_stpal->_palDirty) {
		g_stpal->buildRemap();
	}
	for (int s = 0; s < 16; ++s) {
		const uint8_t L = g_stpal->_cutsceneRep[s];
		orMap[s] = g_stpal->_remap[L | (colour8 & 0xF8)];
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

// the same as the quantiser in play judges it: Manhattan in CIELAB
static inline int labDist(const Color &a, const Color &b) {
	const uint8_t *la = kSTLab[((a.r >> 4) << 8) | ((a.g >> 4) << 4) | (a.b >> 4)];
	const uint8_t *lb = kSTLab[((b.r >> 4) << 8) | ((b.g >> 4) << 4) | (b.b >> 4)];
	int dl = la[0] - lb[0]; if (dl < 0) dl = -dl;
	int da = la[1] - lb[1]; if (da < 0) da = -da;
	int db = la[2] - lb[2]; if (db < 0) db = -db;
	return dl + da + db;
}

// distance of two packed 4-bit colours: Manhattan, green-weighted
static inline int dist4(uint16_t a, uint16_t b) {
	int dr = ((a >> 8) & 15) - ((b >> 8) & 15); if (dr < 0) dr = -dr;
	int dg = ((a >> 4) & 15) - ((b >> 4) & 15); if (dg < 0) dg = -dg;
	int db = (a & 15) - (b & 15); if (db < 0) db = -db;
	return dr + 2 * dg + db;
}

// The merge distance of colours i and j: dist4 weighted by the
// smaller usage count, so rare colours give way to common ones.
// The components are kept in byte arrays (r, g and b, and each
// again shifted up four bits for the row side) so the distance is
// three lookups in a 16x16 table of absolute differences; the
// weight is a 16-bit multiply (a mulu.w, not a __mulsi3 library
// call).
//
// In play (perceptual), the colours are compared as the eye sees them
// - Manhattan in CIELAB, from a table of all 4096 - and cnt is not a
// count of entries but each colour's weight: how much of the level's
// rooms it covers, plus the importance of the sprite colours (see
// Video::AMIGA_setLevelPalettes). Measured over every room of every
// level against the original (tools/gen-palette-tables.py), entry
// counts and RGB distance put the jungle's quarter-screen teal in the
// same slot as a grey and Conrad's trousers, and the room came out
// purple; weighted, the rooms match the Amiga to within a barely
// visible difference.
struct MergeCols {
	uint8_t r[256], g[256], b[256];      // components of col[], RGB only
	uint8_t rh[256], gh[256], bh[256];   // the same, times 16
	uint8_t L[256], A[256], B[256];      // CIELAB, perceptual only
	uint16_t w[256];                     // 1 + MIN(cnt, 63), or cnt
	uint8_t absd[256];                   // |a - b| at a * 16 + b, built once
	bool perceptual;
	void set(int i, uint16_t c, uint16_t cnt) {
		if (perceptual) {
			L[i] = kSTLab[c & 0xFFF][0];
			A[i] = kSTLab[c & 0xFFF][1];
			B[i] = kSTLab[c & 0xFFF][2];
			w[i] = cnt;
			return;
		}
		r[i] = (c >> 8) & 15;
		g[i] = (c >> 4) & 15;
		b[i] = c & 15;
		rh[i] = r[i] << 4;
		gh[i] = g[i] << 4;
		bh[i] = b[i] << 4;
		w[i] = (uint16_t)(1 + MIN(cnt, (uint16_t)63));
	}
	uint32_t dist(int i, int j) const {
		const uint16_t wt = MIN(w[i], w[j]);
		if (perceptual) {
			int dl = L[i] - L[j]; if (dl < 0) dl = -dl;
			int da = A[i] - A[j]; if (da < 0) da = -da;
			int db = B[i] - B[j]; if (db < 0) db = -db;
			return (uint32_t)(uint16_t)(dl + da + db) * wt;
		}
		const int d = absd[rh[i] | r[j]] + 2 * absd[gh[i] | g[j]] + absd[bh[i] | b[j]];
		return (uint32_t)(uint16_t)d * wt;
	}
};

// nearest partner of k among the m candidates, the lowest index on
// ties; dm is the tabled merge distances (n a row), or null to
// compute them
static void nearestPartner(const MergeCols &mc, const uint32_t *dm, int n,
		const uint8_t *cand, int m, int k, uint32_t *nnd, uint8_t *nnj) {
	uint32_t best = 0xFFFFFFFF;
	int bj = 0xFF;
	if (dm) {
		// the table's walk on its own: the test for it, left in the
		// loop, was a third of the merge on an 8MHz ST
		const uint32_t *row = dm + (uint16_t)k * (uint16_t)n;
		const uint8_t *c = cand;
		for (int a = m; --a >= 0; ) {
			const int j = *c++;
			const uint32_t d = row[j];
			if (d < best && j != k) {
				best = d;
				bj = j;
			}
		}
	} else {
		for (int a = 0; a < m; ++a) {
			const int j = cand[a];
			if (j == k) {
				continue;
			}
			const uint32_t d = mc.dist(k, j);
			if (d < best) {
				best = d;
				bj = j;
			}
		}
	}
	nnd[k] = best;
	nnj[k] = (uint8_t)bj;
}

// Merge n distinct 4-bit colours down to 16 clusters and give each
// surviving cluster a hardware slot, preferring the slot whose current
// colour is nearest: planar pages bake slots into pixels, so content
// that outlives a palette change only stays correct if unchanged
// colours keep their slots. Fills alias[] (cluster union-find) and
// slotOf[] (cluster -> slot), and updates _hwPal.
void STPalette::quantiseClusters(uint16_t *col, uint16_t *cnt, int n, uint8_t *alias, uint8_t *slotOf, bool perceptual) {
	// Greedy merge to 16 clusters, closest pair first (lowest
	// indices on ties). Scanning
	// every pair for each merge is n cubed, seconds on an 8MHz
	// 68000 for a level's fifty-odd colours, so the merge distances
	// are tabled and each colour remembers its nearest partner: a
	// merge redoes the survivor's row and the partners of whoever
	// pointed at either half, nothing else. Without the table (no
	// memory) the same walk computes the distances as it goes.
	for (int i = 0; i < n; ++i) {
		alias[i] = i;
	}
	int live = n;
	if (live <= 16) {
		goto assign;
	}
	{
	// The same colours as one of the last few times - a room seen
	// before, back from a cutscene or a death - merge the same way: a
	// fifth of a second or more of the merge below on an 8MHz ST, every
	// time. The slots are still assigned afresh, from the hardware
	// palette as it stands now.
	enum { kMemo = 4 };
	struct Memo {
		bool valid, perceptual;
		int16_t n;
		uint32_t hash;
		uint16_t col[256], cnt[256];        // what came in
		uint16_t mcol[256], mcnt[256];      // ... and what the merge left
		uint8_t alias[256];
	};
	static Memo memo[kMemo];
	static int memoNext;
	uint32_t hash = (uint32_t)n * 2 + (perceptual ? 1 : 0);
	for (int i = 0; i < n; ++i) {
		hash = ST_rol5(hash) + col[i];
		hash = ST_rol5(hash) + cnt[i];
	}
	for (int k = 0; k < kMemo; ++k) {
		const Memo &e = memo[k];
		if (e.valid && e.hash == hash && e.n == n && e.perceptual == perceptual
		    && memcmp(e.col, col, n * sizeof(*col)) == 0 && memcmp(e.cnt, cnt, n * sizeof(*cnt)) == 0) {
			memcpy(col, e.mcol, n * sizeof(*col));
			memcpy(cnt, e.mcnt, n * sizeof(*cnt));
			memcpy(alias, e.alias, n);
			goto assign;
		}
	}
	Memo &entry = memo[memoNext];
	memoNext = (memoNext + 1) & (kMemo - 1);
	entry.valid = false;
	entry.hash = hash;
	entry.n = (int16_t)n;
	entry.perceptual = perceptual;
	memcpy(entry.col, col, n * sizeof(*col));
	memcpy(entry.cnt, cnt, n * sizeof(*cnt));
	{
	static MergeCols mc;
	mc.perceptual = perceptual;
	// perceptual: each cluster stays at its heaviest colour (repW its
	// weight) rather than drifting to an average - measured, an
	// average smears the few colours that matter most, Conrad's, into
	// the scenery, and the heaviest member is exact
	uint16_t repW[256];
	for (int i = 0; i < n; ++i) {
		repW[i] = cnt[i];
	}
	if (mc.absd[1] == 0) {           // not yet built (|0 - 1| is 1)
		for (int a = 0; a < 16; ++a) {
			for (int b = 0; b < 16; ++b) {
				mc.absd[a * 16 + b] = (uint8_t)(a > b ? a - b : b - a);
			}
		}
	}
	// the candidates, the clusters still live, in index order
	uint8_t cand[256];
	int m = 0;
	for (int i = 0; i < n; ++i) {
		mc.set(i, col[i], cnt[i]);
		cand[m++] = i;
	}
	uint32_t *dm = (uint32_t *)malloc((long)n * n * 4);
	if (dm) {
		for (int a = 0; a < m; ++a) {
			const int i = cand[a];
			uint32_t *row = dm + (uint16_t)i * (uint16_t)n;
			uint32_t *mirror = dm + i;
			for (int b = a + 1; b < m; ++b) {
				const int j = cand[b];
				row[j] = mirror[(uint16_t)j * (uint16_t)n] = mc.dist(i, j);
			}
		}
	}
	uint32_t nnd[256];
	uint8_t nnj[256];
	for (int a = 0; a < m; ++a) {
		nearestPartner(mc, dm, n, cand, m, cand[a], nnd, nnj);
	}
	while (live > 16 && m >= 2) {
		int bi = -1;
		uint32_t best = 0xFFFFFFFF;
		for (int a = 0; a < m; ++a) {
			const int i = cand[a];
			if (nnd[i] < best) {
				best = nnd[i];
				bi = i;
			}
		}
		if (bi < 0) {
			break;
		}
		const int bj = nnj[bi];
		// merge bj into bi: the heavier colour, or the weighted average
		const uint16_t oldW = cnt[bi];
		bool moved = true;              // the survivor's colour changed
		if (perceptual) {
			moved = repW[bj] > repW[bi];
			if (moved) {
				col[bi] = col[bj];
				repW[bi] = repW[bj];
			}
		} else {
			const long w1 = cnt[bi], w2 = cnt[bj];
			const int r = (((col[bi] >> 8) & 15) * w1 + ((col[bj] >> 8) & 15) * w2) / (w1 + w2);
			const int g = (((col[bi] >> 4) & 15) * w1 + ((col[bj] >> 4) & 15) * w2) / (w1 + w2);
			const int b = ((col[bi] & 15) * w1 + (col[bj] & 15) * w2) / (w1 + w2);
			col[bi] = (r << 8) | (g << 4) | b;
		}
		cnt[bi] += cnt[bj];
		alias[bj] = bi;
		--live;
		mc.set(bi, col[bi], cnt[bi]);
		int a = 0;
		while (cand[a] != bj) {
			++a;
		}
		--m;
		memmove(cand + a, cand + a + 1, m - a);
		// the survivor moved: its distances, its partner, and the
		// partner of anyone who was pointing at either half
		uint32_t *biRow = dm ? dm + (uint16_t)bi * (uint16_t)n : 0;
		if (dm) {
			uint32_t *mirror = dm + bi;
			for (int a = 0; a < m; ++a) {
				const int j = cand[a];
				if (j != bi) {
					biRow[j] = mirror[(uint16_t)j * (uint16_t)n] = mc.dist(bi, j);
				}
			}
		}
		nearestPartner(mc, dm, n, cand, m, bi, nnd, nnj);
		for (int a = 0; a < m; ++a) {
			const int k = cand[a];
			if (k == bi) {
				continue;
			}
			// Pointing at the survivor: its distance to k is only
			// different if its colour moved, or if k is heavier than
			// the survivor was (the smaller weight is what counts, so
			// a lighter k sees the same distance). Recomputing all of
			// them, as the old path does, made the perceptual merge
			// twice as slow: pixel weights point most colours at the
			// same few heavy ones.
			if (nnj[k] == bj || (nnj[k] == bi && (moved || cnt[k] > oldW))) {
				nearestPartner(mc, dm, n, cand, m, k, nnd, nnj);
				continue;
			}
			if (nnj[k] == bi) {
				continue;
			}
			const uint32_t d = biRow ? biRow[k] : mc.dist(k, bi);   // symmetric
			if (d < nnd[k] || (d == nnd[k] && bi < nnj[k])) {
				nnd[k] = d;
				nnj[k] = (uint8_t)bi;
			}
		}
	}
	free(dm);
	}
	memcpy(entry.mcol, col, n * sizeof(*col));
	memcpy(entry.mcnt, cnt, n * sizeof(*cnt));
	memcpy(entry.alias, alias, n);
	entry.valid = true;
	}
assign:

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
static uint16_t g_partLast[kCutParts][kCutEntries]; // last palette collected

static inline uint16_t pack4(const Color &c) {
	return (uint16_t)(((c.r >> 4) << 8) | ((c.g >> 4) << 4) | (c.b >> 4));
}

void ST_cutscenePalReset() {
	memset(g_partUsed, 0, sizeof(g_partUsed));
	g_collectPart = 0;
}

void ST_cutscenePalPart(int part) {
	g_collectPart = (part < kCutParts) ? part : (kCutParts - 1);
}

// |a - b| for nibbles a and b, at a * 16 + b
static uint8_t g_absd[256];

void ST_cutscenePalCollect(const uint16_t *packed) {
	if (noColours()) {
		return;
	}
	const int part = g_collectPart;
	// Only pairs with an entry that differs from the part's previous
	// palette can raise a distance; a cutscene palette step usually
	// touches a few entries (and often none), so each changed entry is
	// walked against the rest and nothing else. Scanning all 496 pairs
	// to find them, and building the table below every time, made
	// each palette 18ms on an 8MHz ST: three seconds of the intro's
	// wait before its first frame.
	uint8_t list[kCutEntries];
	int nChanged = 0;
	uint16_t *last = g_partLast[part];
	if (!g_partUsed[part]) {
		g_partUsed[part] = true;
		memset(g_partDist[part], 0, sizeof(g_partDist[part]));
		memset(g_partRep[part], 0, sizeof(g_partRep[part]));
		for (int i = 0; i < kCutEntries; ++i) {
			list[nChanged++] = (uint8_t)i;
		}
	} else {
		for (int i = 0; i < kCutEntries; ++i) {
			if (packed[i] != last[i]) {
				list[nChanged++] = (uint8_t)i;
			}
		}
	}
	if (nChanged == 0) {
		return;
	}
	if (g_absd[0x10] == 0) {
		for (int i = 0; i < 16; ++i) {
			for (int j = 0; j < 16; ++j) {
				g_absd[i * 16 + j] = (uint8_t)(i > j ? i - j : j - i);
			}
		}
	}
	for (int i = 0; i < kCutEntries; ++i) {
		last[i] = packed[i];
	}
	uint8_t (*dm)[kCutEntries] = g_partDist[part];
	uint16_t *rep = g_partRep[part];
	// component nibbles, the changed entry's side shifted up for the
	// |a-b| table
	uint8_t r[kCutEntries], g[kCutEntries], b[kCutEntries];
	for (int i = 0; i < kCutEntries; ++i) {
		r[i] = (packed[i] >> 8) & 15;
		g[i] = (packed[i] >> 4) & 15;
		b[i] = packed[i] & 15;
	}
	for (int k = 0; k < nChanged; ++k) {
		const int i = list[k];
		if (dist4(packed[i], 0) > dist4(rep[i], 0)) {
			rep[i] = packed[i];
		}
		const int ri = r[i] << 4, gi = g[i] << 4, bi = b[i] << 4;
		uint8_t *row = dm[i];
		// every j: the diagonal gives 0 and can never raise it, and a
		// pair of two changed entries comes round twice to the same
		// answer, both cheaper than testing for them
		for (int j = 0; j < kCutEntries; ++j) {
			// max component sum is 15 + 30 + 15, so no clamp needed
			const int d = g_absd[ri | r[j]] + 2 * g_absd[gi | g[j]] + g_absd[bi | b[j]];
			if (d > row[j]) {
				row[j] = dm[j][i] = (uint8_t)d;
			}
		}
	}
}

// row i's nearest live j > i: the first at the least distance (-1 none)
static void rowNearest(const uint8_t (*dm)[kCutEntries], const bool *liveE, int i, int16_t *dMin, int8_t *arg) {
	int m = 0x7FFF, a = -1;
	const uint8_t *row = dm[i];
	for (int j = i + 1; j < kCutEntries; ++j) {
		if (liveE[j] && row[j] < m) {
			m = row[j];
			a = j;
		}
	}
	*dMin = (int16_t)m;
	*arg = (int8_t)a;
}

void ST_cutscenePalLock(int part) {
	if (noColours()) {
		return;
	}
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
	STPalette *stub = g_stpal;
	const uint16_t *rep = g_partRep[part];
	static uint8_t dm[kCutEntries][kCutEntries];
	memcpy(dm, g_partDist[part], sizeof(dm));

	uint8_t alias[kCutEntries];
	for (int i = 0; i < kCutEntries; ++i) {
		alias[i] = (uint8_t)i;
	}
	// Merge the nearest pair until 16 are left: the first nearest, in
	// the order of a walk of every live pair (i, then j > i). Kept as
	// each live row's nearest partner (the first j at the least
	// distance), so a merge rescans only the rows it can change - the
	// merged row, and those whose nearest was either of the two -
	// rather than every pair again. That walk and the slot search
	// below were ~200ms at each part start on an 8MHz ST, a visible
	// hitch and a late frame or two.
	bool liveE[kCutEntries];
	int16_t rowMin[kCutEntries];
	int8_t rowArg[kCutEntries];
	for (int i = 0; i < kCutEntries; ++i) {
		liveE[i] = true;
	}
	for (int i = 0; i < kCutEntries; ++i) {
		rowNearest(dm, liveE, i, &rowMin[i], &rowArg[i]);
	}
	int live = kCutEntries;
	while (live > 16) {
		int bi = -1, best = 0x7FFF;
		for (int i = 0; i < kCutEntries; ++i) {
			if (liveE[i] && rowArg[i] >= 0 && rowMin[i] < best) {
				best = rowMin[i];
				bi = i;
			}
		}
		if (bi < 0) {
			break;
		}
		const int bj = rowArg[bi];
		for (int k = 0; k < kCutEntries; ++k) {
			if (dm[bj][k] > dm[bi][k]) {
				dm[bi][k] = dm[k][bi] = dm[bj][k];
			}
		}
		alias[bj] = (uint8_t)bi;
		liveE[bj] = false;
		--live;
		for (int i = 0; i < kCutEntries; ++i) {
			if (liveE[i] && (i == bi || rowArg[i] == bi || rowArg[i] == bj)) {
				rowNearest(dm, liveE, i, &rowMin[i], &rowArg[i]);
			}
		}
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
	// every cluster's distance to every slot's colour, once, rather
	// than again in each of the sixteen passes
	uint16_t slotPacked[16];
	for (int sl = 0; sl < 16; ++sl) {
		slotPacked[sl] = pack4(stub->_hwPal[sl]);
	}
	uint8_t dcs[16][16];
	for (int c = 0; c < nc; ++c) {
		const uint16_t rc = rep[clusters[c]];
		for (int sl = 0; sl < 16; ++sl) {
			dcs[c][sl] = (uint8_t)dist4(rc, slotPacked[sl]);
		}
	}
	// Sixteen passes each took the nearest free cluster and slot, the
	// first in cluster-then-slot order on a tie. The pairs sorted by
	// distance, that order kept within a distance (a counting sort:
	// a distance is at most 60), give the same picks in one walk.
	enum { kMaxD = 61 };
	uint16_t at[kMaxD];
	memset(at, 0, sizeof(at));
	for (int c = 0; c < nc; ++c) {
		for (int sl = 0; sl < 16; ++sl) {
			++at[dcs[c][sl]];
		}
	}
	for (int d = 0, sum = 0; d < kMaxD; ++d) {
		const int n = at[d];
		at[d] = (uint16_t)sum;
		sum += n;
	}
	uint8_t order[16 * 16];
	for (int c = 0; c < nc; ++c) {
		for (int sl = 0; sl < 16; ++sl) {
			order[at[dcs[c][sl]]++] = (uint8_t)((c << 4) | sl);
		}
	}
	for (int k = 0, left = nc; k < nc * 16 && left > 0; ++k) {
		const int c = order[k] >> 4, sl = order[k] & 15;
		if (!clusterDone[c] && !slotUsed[sl]) {
			slotOfCluster[c] = (uint8_t)sl;
			slotUsed[sl] = true;
			clusterDone[c] = true;
			--left;
		}
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
	stub->_cutPending = true;
	stub->_remapStale = false;
	stub->_palDirty = true;
}

void ST_cutscenePalUnlock() {
	if (noColours()) {
		return;
	}
	g_stpal->_palLocked = false;
	g_stpal->_cutPending = false;
	g_stpal->_remapStale = true;
	g_stpal->_palDirty = true;
}

// Closeness for a custom palette with palette_hue=true. The automatic
// palette is built from
// the room's own colours, so a plain RGB distance finds a near twin
// for every entry; a hand-picked 16 usually has no twin, and plain RGB
// then sends the jungle's muted olives to grey, which is nearer in
// brightness than any saturated green. Here the hue counts for as
// much as the saturation both colours share - a grey has no hue to
// match - then saturation, then brightness. HSV in 0..255 steps,
// hue as 0..1535 around the wheel; 16-bit products throughout.
struct HueColour {
	int16_t h, s, v;
	HueColour() {}
	explicit HueColour(const Color &c) {
		const int r = c.r >> 4, g = c.g >> 4, b = c.b >> 4;
		const int mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b));
		v = (int16_t)(mx * 17);
		const int d = mx - mn;
		s = (int16_t)(mx ? d * 255 / mx : 0);
		if (d == 0) {
			h = 0;
		} else if (mx == r) {
			h = (int16_t)(((g - b) * 256 / d + 1536) % 1536);
		} else if (mx == g) {
			h = (int16_t)((b - r) * 256 / d + 512);
		} else {
			h = (int16_t)((r - g) * 256 / d + 1024);
		}
	}
	int dist(const HueColour &o) const {
		int dh = h - o.h;
		if (dh < 0) dh = -dh;
		if (dh > 768) dh = 1536 - dh;          // 0..768, the short way
		const int dh255 = dh / 3;              // 0..256
		const int16_t w = MIN(s, o.s);
		const int ds = s > o.s ? s - o.s : o.s - s;
		const int dv = v > o.v ? v - o.v : o.v - v;
		return (int)((int16_t)w * (int16_t)dh255 / 64)            // 4 w dh
			+ (int)((int16_t)ds * (int16_t)MAX(s, o.s) * 3 / 510)  // 1.5 ds s
			+ dv;
	}
};

// the custom colours as HueColour, worked out once when they are set:
// a divide or two each, too many to repeat for every entry
static HueColour g_customHue[16];

// Where logical entry i goes in a custom palette: the slot the file
// lists for its Amiga colour, else the nearest of the 16 - by hue with
// palette_hue, by colour otherwise.
int STPalette::customSlotFor(int i) {
	const uint8_t a = _amigaIdx[i];
	if (a < 32 && _customMap[a] < 16) {
		return _customMap[a];
	}
	long best = 0x7FFFFFFF;
	int bs = 0;
	if (g_options.palette_hue) {
		const HueColour c(_pal[i]);
		for (int s = 0; s < 16; ++s) {
			const long d = c.dist(g_customHue[s]);
			if (d < best) {
				best = d;
				bs = s;
			}
		}
	} else {
		for (int s = 0; s < 16; ++s) {
			const long d = colDist(_pal[i], _customCols[s]);
			if (d < best) {
				best = d;
				bs = s;
			}
		}
	}
	return bs;
}

// Quantise the 256-entry logical palette to 16 hardware colours.
// Colours are reduced to STE 4-bit per channel first (the hardware
// cannot do better), deduplicated, then greedily merged by nearest
// pair until 16 remain.
void STPalette::buildRemap() {
	_palDirty = false;

	// Locked cutscene mapping: the logical->slot assignment is fixed
	// for the scene, so a palette change is just a refresh of the 16
	// registers from each slot's representative entry. Fades cost
	// nothing and drawn pixels never need re-baking.
	if (_palLocked) {
		// only a slot's representative shows, so a change elsewhere
		// (most of a cutscene's 32 entries) leaves the registers alone
		bool changed = _fade != 256 || (g_options.ab & kAbCutOldPalette);
		for (int s = 0; s < 16; ++s) {
			const Color &c = _pal[_cutsceneRep[s]];
			if (_hwPal[s].r != c.r || _hwPal[s].g != c.g || _hwPal[s].b != c.b) {
				_hwPal[s] = c;
				changed = true;
			}
		}
		_fade = 256;
		if (changed) {
			_hwDirty = true;
		}
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
			bool isChg[256];
			memset(isChg, 0, sizeof(isChg));
			for (int c = 0; c < nchg; ++c) {
				isChg[changed[c]] = true;
			}
			const bool perceptual = _haveColW && !g_cutscenePal;
			// Only a slot that actually moved makes the remap new: the
			// caches that bake colours in look again at a new generation,
			// and a palette poke that lands each entry where it already
			// was (a monster's half of the palette set again, unchanged)
			// used to throw all of their work away.
			bool moved = false;
			for (int c = 0; c < nchg; ++c) {
				const int i = changed[c];
				if (_customPal && !g_cutscenePal) {
					const uint8_t cs = (uint8_t)customSlotFor(i);
					moved |= (_remap[i] != cs);
					_remap[i] = cs;
					continue;
				}
				// In play a changed entry is usually a copy - an enemy's
				// half of the object palette - so where an unchanged entry
				// has the same colour, share its slot: the copy draws as
				// the original does, whichever cluster that went to.
				int bs = -1;
				if (perceptual) {
					for (int j = 0; j < 256; ++j) {
						if (!isChg[j] && !excludedEntry(j) && _pal[j].r == _pal[i].r
						    && _pal[j].g == _pal[i].g && _pal[j].b == _pal[i].b) {
							bs = _remap[j];
							break;
						}
					}
				}
				if (bs < 0) {
					long best = 0x7FFFFFFF;
					for (int s = 0; s < 16; ++s) {
						const long d = perceptual ? labDist(_pal[i], _hwPal[s]) : colDist(_pal[i], _hwPal[s]);
						if (d < best) {
							best = d;
							bs = s;
						}
					}
				}
				moved |= (_remap[i] != (uint8_t)bs);
				_remap[i] = (uint8_t)bs;
			}
			if (moved || (g_options.ab & kAbRemapBumpAlways)) {
				++g_remapGen;
			}
			_fade = (fade > 1023) ? 1023 : fade;
			_hwDirty = true;
			return;
		}
	}
	_fade = 256;

	if (_customPal && !g_cutscenePal) {
		// A custom palette is the 16 colours, as given: every entry
		// takes the slot its file lists for it, or the nearest (see
		// customSlotFor). Cutscenes keep their own mapping.
		memcpy(_hwPal, _customCols, sizeof(_hwPal));
		uint8_t newRemap[256];
		for (int i = 0; i < 256; ++i) {
			newRemap[i] = (uint8_t)customSlotFor(i);
		}
		applyRemap(newRemap);
		return;
	}

	// Gather distinct 4-bit colours with usage counts. Logical
	// entries 0xC0-0xDF belong to cutscenes only (the game's slots
	// are 0x0-0xB plus the text slots): in game mode they are
	// excluded so a finished cutscene's palette cannot crowd the
	// game's colours out of the 16 hardware slots, and in cutscene
	// mode the quantisation runs over ONLY the cutscene banks and
	// the text slot (scenes use up to 32 colours - DEBUT draws
	// Conrad from the upper bank). Excluded entries are mapped to
	// the nearest surviving colour below.
	// in play, colours are weighed by what they cover (see MergeCols)
	const bool perceptual = _haveColW && !g_cutscenePal;
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
		// perceptual: at least 1, or a colour no room shows (the text
		// colours, say) would merge at no cost with the first colour in
		// the list rather than the nearest
		cnt[j] += perceptual ? (_colW[i] ? _colW[i] : 1) : 1;
		entryCluster[i] = j;
	}

	uint8_t alias[256];
	uint8_t slotOf[256];
	// (Conrad's jacket was once pinned to a slot of its own here. In
	// play his weight keeps it now, and on the title - the only place
	// left without weights - entry 0x17 is a title colour, which the
	// pin held exact at the cost of the flare's highlights: measured,
	// the title comes out nearer the original without it.)
	quantiseClusters(col, cnt, n, alias, slotOf, perceptual);
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
	applyRemap(newRemap);
}

// The end of every full rebuild, quantised or custom: the palette the
// mapping now describes, and a new generation for anything holding
// pixels baked with the old one.
void STPalette::applyRemap(const uint8_t *newRemap) {
	memcpy(_basePal, _pal, sizeof(_pal));
	_hwDirty = true;
	_remapStale = false;
	if (memcmp(newRemap, _remap, sizeof(_remap)) != 0) {
		memcpy(_remap, newRemap, sizeof(_remap));
		++g_remapGen;
#ifdef ST_PLANAR
		// stale planar data on screen uses the old mapping:
		// re-run the conversion from the shadow frame
		ST_reconvertScreen();
#endif
	}
}

void ST_setCustomPalette(const Color *cols, const uint8_t *map32) {
	STPalette *s = g_stpal;
	s->_customPal = (cols != 0);
	if (cols) {
		memcpy(s->_customCols, cols, sizeof(s->_customCols));
		for (int i = 0; i < 16; ++i) {
			g_customHue[i] = HueColour(cols[i]);
		}
	}
	if (map32) {
		memcpy(s->_customMap, map32, sizeof(s->_customMap));
	} else {
		memset(s->_customMap, 0xFF, sizeof(s->_customMap));
	}
	// a full rebuild, not the few-entries patch: every entry moves
	s->_remapStale = true;
	s->_palDirty = true;
}

void ST_setAmigaColourMap(const uint8_t *map256) {
	if (noColours()) {
		return;
	}
	STPalette *s = g_stpal;
	if (memcmp(s->_amigaIdx, map256, sizeof(s->_amigaIdx)) != 0) {
		memcpy(s->_amigaIdx, map256, sizeof(s->_amigaIdx));
		if (s->_customPal) {
			s->_remapStale = true;
			s->_palDirty = true;
		}
	}
}

// For Ctrl+P: the room's Amiga colours 0-31 and the slot each is drawn
// with now; have[n] is false for one the room does not load.
void ST_getAmigaColours(Color *cols, uint8_t *slots, bool *have) {
	ST_getRemap();
	STPalette *s = g_stpal;
	for (int n = 0; n < 32; ++n) {
		have[n] = false;
		for (int i = 0; i < 256; ++i) {
			if (s->_amigaIdx[i] == n) {
				cols[n] = s->_pal[i];
				slots[n] = s->_remap[i];
				have[n] = true;
				break;
			}
		}
	}
}

void ST_setColourWeights(const uint16_t *w) {
	if (noColours()) {
		return;
	}
	// Turning weights on or off (play against the title) rebuilds; new
	// weights alone do not, they steer the next rebuild the colours
	// call for. A room that loads a slot with another palette number,
	// or the same colours under one, would otherwise pause ~620ms on
	// its way in rather than take the patch path.
	STPalette *s = g_stpal;
	if ((w != 0) != s->_haveColW) {
		s->_remapStale = true;
		s->_palDirty = true;
	}
	s->_haveColW = (w != 0);
	if (w) {
		memcpy(s->_colW, w, sizeof(s->_colW));
	}
}

void ST_getHwColours(Color *cols) {
	ST_getRemap();
	memcpy(cols, g_stpal->_hwPal, sizeof(g_stpal->_hwPal));
}

// the 16 hardware colours at the current fade level; _fade can
// exceed 256 (brightening fades), so every channel is clamped
void STPalette::fadedColours(STDL_Colour *c) {
	if (_fade == 256) {
		// no fade - always so in a cutscene (see buildRemap)
		for (int i = 0; i < 16; ++i) {
			c[i].r = _hwPal[i].r;
			c[i].g = _hwPal[i].g;
			c[i].b = _hwPal[i].b;
			c[i].unused = 0;
		}
		return;
	}
	// muls.w: the int16_t casts were not enough for gcc 4.6, which
	// still called __mulsi3 48 times a write (6,000 calls in the
	// intro, all of them multiplying by 256)
	const int16_t fade = (int16_t)_fade;
	for (int i = 0; i < 16; ++i) {
		int r = STDL_MulS16(_hwPal[i].r, fade) >> 8;
		int g = STDL_MulS16(_hwPal[i].g, fade) >> 8;
		int b = STDL_MulS16(_hwPal[i].b, fade) >> 8;
		c[i].r = (uint8_t)(r > 255 ? 255 : r);
		c[i].g = (uint8_t)(g > 255 ? 255 : g);
		c[i].b = (uint8_t)(b > 255 ? 255 : b);
		c[i].unused = 0;
	}
}
