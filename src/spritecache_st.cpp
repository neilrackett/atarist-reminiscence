/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Atari ST gameplay sprites, baked to planar once (see video_st.h).
 */

#ifdef ATARIST

extern "C" {
#include <stdl/stdl.h>
}
#include <mint/osbind.h>
#include "util.h"
#include "video_st.h"

// Drawing from chunky costs a per-pixel gather every frame: the source
// byte is mapped, then scattered a bit at a time into four plane words.
// The same frame, flip and colours always give the same plane words, so
// they are baked once and afterwards just moved into the layer.
//
// What a bake is keyed on is the bytes it came from - the sprite frame
// in the level's data, a piece of an object bank, an icon number - with
// an identity saying which load of those bytes it was, and a token for
// the colours. It used to key on the decode caches' buffers, so a decode
// evicted from a 64-entry FIFO took its bake with it, and a fight that
// cycled more frames than that drew nearly every sprite the slow way.
// Keyed on the source, a hit needs no decode at all: the frame is only
// decoded, into one scratch buffer, when it has to be baked.
//
// The bakes live in one block from GEMDOS, sized to what the machine has
// free once a level is in (up to 1MB with 4MB of RAM), used as a ring of
// four segments: new bakes go at the head, and when the head moves into
// a segment everything still in it is dropped. A bake drawn from a
// segment two behind the head is copied forward, so what stays on screen
// stays in the ring. Pre-shifted copies are bakes like any other, keyed
// with their phase. No 64-entry ceiling, and nothing to collide on.

enum {
	kMaxW = 128, kMaxH = 160, kMaxG = kMaxW / 16,
	kSegs = 4,
	kTagPhaseShift = 2,             // tag: token << 8 | phase << 2 | colmajor | xflip
	kTagPhaseMask = 15 << kTagPhaseShift
};

struct Rec {
	Rec *next, *prev;           // hash chain
	const void *key;            // the bytes it was baked from; 0 = dead
	uint32_t ident;             // which kind of bytes, and which load of them
	uint16_t tag;
	uint8_t w, h;               // the whole sprite, as asked for
	uint8_t offG, useG;         // groups skipped at the left, groups kept
	uint8_t offY, useH;         // rows skipped at the top, rows kept
	uint16_t sizeL;             // the record in longs, this header included
	uint16_t bucket;
	uint8_t seg;                // ring segment it sits in
	uint8_t lastPhase;          // phase a base bake was last drawn at
	uint16_t hits;
	// then the planes, useG * 4 words a row, and the mask, useG words a row
};
enum { kRecBytes = (sizeof(Rec) + 3) & ~3 };

static inline uint16_t *recPlanes(const Rec *r) {
	return (uint16_t *)((uint8_t *)r + kRecBytes);
}

// --- the ring ---------------------------------------------------------

static uint8_t *g_block;            // the GEMDOS block, 0 = none
static bool g_tried;                // asked for this level already
static Rec **g_bucket;
static uint16_t g_bucketMask;
static uint8_t *g_segBase[kSegs];
static uint8_t *g_segFill[kSegs];   // the end of what each segment holds
static uint32_t g_segSize;
static int g_seg;                   // the segment new bakes go in

static STSprDecodeFn g_decode;
static void *g_decodeCtx;

// A miss decodes into the game's own scratch buffer: a chunky frame
// first, the whole bake after it. Nothing else holds the buffer while
// sprites are drawn (the room decode that also uses it runs before), and
// on a 2.5MB machine the 33K of static buffers this used to be was
// memory the level loads had to do without.
enum { kChunkyBytes = kMaxW * kMaxH, kBakeWords = 4 + kMaxG * 5 * kMaxH + 4 };
static uint8_t *g_chunky;
static uint16_t *g_bakeBuf;

void ST_sprSetDecoder(STSprDecodeFn fn, void *ctx, uint8_t *scratch, uint32_t scratchSize) {
	g_decode = fn;
	g_decodeCtx = ctx;
	if (scratchSize < kChunkyBytes + kBakeWords * sizeof(uint16_t)) {
		error("Sprite bakes: scratch buffer too small");
	}
	g_chunky = scratch;
	g_bakeBuf = (uint16_t *)(scratch + kChunkyBytes);
}

static void unlinkRec(Rec *r) {
	if (r->prev) {
		r->prev->next = r->next;
	} else {
		g_bucket[r->bucket] = r->next;
	}
	if (r->next) {
		r->next->prev = r->prev;
	}
	r->key = 0;
}

static void linkRec(Rec *r, uint16_t b) {
	r->bucket = b;
	r->prev = 0;
	r->next = g_bucket[b];
	if (r->next) {
		r->next->prev = r;
	}
	g_bucket[b] = r;
}

static void flushSeg(int s) {
	for (uint8_t *p = g_segBase[s]; p < g_segFill[s]; ) {
		Rec *r = (Rec *)p;
		if (r->key) {
			if (r->hits == 0) {
				++g_stPerf.unused;
			}
			unlinkRec(r);
		}
		p += (uint32_t)r->sizeL << 2;
	}
	g_segFill[s] = g_segBase[s];
	++g_stPerf.segFlush;
}

// A record of `bytes` (a multiple of 4) at the head. pinSeg is the
// segment of a record the caller is still using - the base of a shifted
// copy, the original of a promotion - which must not be emptied to make
// the room: 0 then, and the caller does without.
static Rec *allocRec(uint32_t bytes, int pinSeg) {
	if (g_segFill[g_seg] + bytes > g_segBase[g_seg] + g_segSize) {
		const int next = (g_seg + 1) & (kSegs - 1);
		if (next == pinSeg || bytes > g_segSize) {
			return 0;
		}
		g_seg = next;
		flushSeg(next);
	}
	Rec *r = (Rec *)g_segFill[g_seg];
	g_segFill[g_seg] += bytes;
	r->sizeL = (uint16_t)(bytes >> 2);
	r->seg = (uint8_t)g_seg;
	r->hits = 0;
	return r;
}

// Sized the first time a sprite is drawn after a level loads: by then
// the level's data, its first room and its music are in, so what is
// free is what play has to work with. What is left must cover what
// play still asks for - a module per track (size plus 128K, or the
// track is skipped), a cutscene's arenas, a room's data - so the ring
// takes what is over that, 96K at least while 128K stays free, and 1MB
// at most, which is plenty for any room.
static bool ringOpen() {
	if (g_block) {
		return true;
	}
	if (g_tried) {
		return false;
	}
	g_tried = true;
	const long largestK = (long)Malloc(-1) >> 10;
	enum { kReserveK = 320, kMinK = 96, kMaxK = 1024, kKeepK = 128 };
	long ringK = largestK - kReserveK;
	if (ringK < kMinK) {
		ringK = (largestK - kKeepK >= kMinK) ? kMinK : largestK - kKeepK;
	}
	if (ringK > kMaxK) {
		ringK = kMaxK;
	}
	if (ringK < 32) {
		info("Sprite bakes: no room (%ldK free)", largestK);
		return false;
	}
	// a bucket for every 512 bytes of ring, which is about a bake
	int buckets = 256;
	while (buckets < 2048 && (long)buckets * 512 < ringK * 1024) {
		buckets <<= 1;
	}
	const uint32_t segSize = ((uint32_t)ringK << 10) / kSegs & ~3UL;
	const uint32_t total = buckets * sizeof(Rec *) + segSize * kSegs + 16;
	long p = Mxalloc(total, 3);       // either RAM: only the CPU and the BLiTTER read it, and STDL checks the latter
	if (p == -32) {                   // no Mxalloc on TOS 1.0x
		p = Malloc(total);
	}
	if (p <= 0) {
		info("Sprite bakes: no block of %luK", (unsigned long)(total >> 10));
		return false;
	}
	g_block = (uint8_t *)p;
	g_bucket = (Rec **)g_block;
	g_bucketMask = (uint16_t)(buckets - 1);
	memset(g_bucket, 0, buckets * sizeof(Rec *));
	uint8_t *seg = g_block + buckets * sizeof(Rec *);
	for (int s = 0; s < kSegs; ++s, seg += segSize) {
		g_segBase[s] = g_segFill[s] = seg;
	}
	g_segSize = segSize;
	g_seg = 0;
	info("Sprite bakes: %ldK ring, %d buckets, %ldK was free", ringK, buckets, largestK);
	return true;
}

void ST_sprRelease() {
	if (g_block) {
		Mfree(g_block);
		g_block = 0;
	}
	g_tried = false;
}

// --- colours ----------------------------------------------------------
//
// A bake holds hardware colours, so it is only good while the colour
// bank it was drawn with maps the same way. The map for a bank is 16
// four-bit slots: packed into 64 bits, it names itself. Each distinct
// map gets a small token, and a bake keys on that rather than on the
// remap's generation: a palette change that leaves a bank's map alone -
// a monster's half of the palette, a cutscene, the next room - leaves
// its bakes good, and two maps that alternate keep two sets of bakes.

enum { kTokens = 255 };
static uint32_t g_tokHi[kTokens], g_tokLo[kTokens];
static int g_tokCount;
static bool g_tokReset;            // the numbers were reused during a frame's draws
static uint16_t g_tokGen;
static bool g_tokGenValid;
static uint8_t g_tokOfBank[16];
static uint16_t g_tokBankValid;     // a bit per bank

static void flushAll() {
	if (g_block) {
		for (int s = 0; s < kSegs; ++s) {
			flushSeg(s);
		}
	}
}

// Once a frame's drawing starts nothing should move the remap, so the
// generation is read at its first entry rather than at every one - and
// read again at its end, and a frame where it moved anyway is redrawn
// whole (ST_dlEnd).
static bool g_tokFrozen;

static void tokCheckGen() {
	ST_getRemap();                    // may build a new remap, which moves the generation
	const uint16_t gen = ST_remapGen();
	if (!g_tokGenValid || gen != g_tokGen) {
		g_tokGen = gen;
		g_tokGenValid = true;
		g_tokBankValid = 0;
	}
}

void ST_sprTokensBegin() {
	tokCheckGen();
	g_tokFrozen = true;
}

bool ST_sprTokensEnd() {
	if (!g_tokFrozen) {
		return true;
	}
	g_tokFrozen = false;
	const uint16_t gen = g_tokGen;
	tokCheckGen();
	const bool reset = g_tokReset;
	g_tokReset = false;
	return gen == g_tokGen && !reset;
}

static void sprPairInvalidate(int i);

static uint8_t palToken(uint8_t colMask) {
	if (!g_tokFrozen) {
		tokCheckGen();
	}
	const int bank = colMask >> 4;
	if (g_tokBankValid & (1 << bank)) {
		return g_tokOfBank[bank];
	}
	uint8_t map16[16];
	ST_buildMap16(colMask, map16);
	uint32_t hi = 0, lo = 0;
	for (int i = 0; i < 8; ++i) {
		hi = (hi << 4) | (map16[i] & 15);
		lo = (lo << 4) | (map16[i + 8] & 15);
	}
	int t = 0;
	while (t < g_tokCount && (g_tokHi[t] != hi || g_tokLo[t] != lo)) {
		++t;
	}
	if (t == g_tokCount) {
		if (g_tokCount == kTokens) {
			// Every token taken: start again, and with them everything
			// keyed on a token's number - the bakes, the pair tables, the
			// banks' tokens, the display list's last frame - as the same
			// numbers now name other colours
			flushAll();
			g_tokCount = 0;
			t = 0;
			g_tokBankValid = 0;
			for (int i = 0; i < 8; ++i) {
				sprPairInvalidate(i);
			}
			ST_dlInvalidate();
			if (g_tokFrozen) {
				g_tokReset = true;
			}
		}
		g_tokHi[t] = hi;
		g_tokLo[t] = lo;
		++g_tokCount;
	}
	g_tokOfBank[bank] = (uint8_t)t;
	g_tokBankValid |= (uint16_t)(1 << bank);
	return (uint8_t)t;
}

// --- finding and baking -----------------------------------------------

static inline uint16_t hashOf(const void *key, uint32_t ident, uint16_t tag) {
	const uint32_t k = (uint32_t)key;
	return (uint16_t)((uint16_t)(k >> 2) ^ (uint16_t)(k >> 11) ^ (uint16_t)ident
	                  ^ (uint16_t)(ident >> 13) ^ tag ^ (uint16_t)(tag >> 7)) & g_bucketMask;
}

static Rec *findRec(const void *key, uint32_t ident, uint16_t tag, uint8_t w, uint8_t h, uint16_t *bucket) {
	const uint16_t b = hashOf(key, ident, tag);
	*bucket = b;
	for (Rec *r = g_bucket[b]; r; r = r->next) {
		if (r->key == key && r->ident == ident && r->tag == tag && r->w == w && r->h == h) {
			return r;
		}
	}
	return 0;
}

static STDL_Surface *g_scratch;

// the source pointer and pitch STDL_BlitIndexed8 wants for a whole
// sprite: a flipped one is read from its last column backwards
static const uint8_t *chunkySource(const uint8_t *buf, int w, int h, unsigned flags, int *pitch) {
	*pitch = (flags & kSTSpriteColMajor) ? h : w;
	if (flags & kSTSpriteXflip) {
		buf += (flags & kSTSpriteColMajor) ? h * (w - 1) : (w - 1);
	}
	return buf;
}

static unsigned i8Flags(unsigned flags) {
	unsigned f = 0;
	if (flags & kSTSpriteXflip) {
		f |= STDL_I8_XFLIP;
	}
	if (flags & kSTSpriteColMajor) {
		f |= STDL_I8_COLMAJOR;
	}
	return f;
}

static Rec *bakeChunky(const void *key, uint32_t ident, uint16_t tag, int kind, int w, int h, uint8_t colMask, unsigned flags, uint16_t bucket) {
	g_decode(g_decodeCtx, kind, (const uint8_t *)key, w, h, g_chunky, false);
	const int groups = (w + 15) >> 4;
	const int planeWords = groups * 4 * h;
	uint16_t *planes = g_bakeBuf + 4;
	uint16_t *mask = planes + planeWords;
	// STDL's source mask has a bit set where the destination is kept:
	// opaque everywhere to start, and the blit clears a bit under each
	// pixel it draws
	memset(mask, 0xFF, groups * h * sizeof(uint16_t));
	if (!g_scratch) {
		g_scratch = STDL_CreateSurfaceFrom((uint8_t *)planes, groups * 16, h, groups * 8, (uint8_t *)mask, groups * 2);
		if (!g_scratch) {
			return 0;
		}
	}
	g_scratch->pixels = (uint8_t *)planes;
	g_scratch->w = (int16_t)(groups * 16);
	g_scratch->h = (int16_t)h;
	g_scratch->stride = (uint16_t)(groups * 8);
	g_scratch->mask = (uint8_t *)mask;
	g_scratch->maskstride = (uint16_t)(groups * 2);
	g_scratch->clip.x = 0;
	g_scratch->clip.y = 0;
	g_scratch->clip.w = (uint16_t)(groups * 16);
	g_scratch->clip.h = (uint16_t)h;
	uint8_t map16[16];
	ST_buildMap16(colMask, map16);
	int pitch;
	const uint8_t *src = chunkySource(g_chunky, w, h, flags, &pitch);
	STDL_BlitIndexed8(g_scratch, src, pitch, 0, 0, w, h, map16, i8Flags(flags));
	// Trim: a mask word of 0xFFFF is sixteen pixels that draw nothing,
	// and rows and columns of them sit around most frames. Only what is
	// drawn is kept, which costs the blit nothing for them afterwards
	// and the ring nothing to hold.
	int y0 = h, y1 = -1, g0 = groups, g1 = -1;
	{
		const uint16_t *m = mask;
		for (int r = 0; r < h; ++r, m += groups) {
			for (int g = 0; g < groups; ++g) {
				if (m[g] != 0xFFFF) {
					if (r < y0) {
						y0 = r;
					}
					y1 = r;
					if (g < g0) {
						g0 = g;
					}
					if (g > g1) {
						g1 = g;
					}
				}
			}
		}
	}
	if (y1 < 0) {                    // nothing drawn at all
		y0 = y1 = 0;
		g0 = g1 = 0;
	}
	const int useG = g1 - g0 + 1;
	const int useH = y1 - y0 + 1;
	const uint32_t bytes = kRecBytes + (uint32_t)(useG * 5) * (uint32_t)useH * 2;
	Rec *rec = allocRec((bytes + 3) & ~3UL, -1);
	if (!rec) {
		return 0;
	}
	uint16_t *dp = recPlanes(rec);
	uint16_t *dm = dp + useG * 4 * useH;
	const uint16_t *sp = planes + (y0 * groups + g0) * 4;
	const uint16_t *sm = mask + y0 * groups + g0;
	for (int r = useH; --r >= 0; ) {
		memcpy(dp, sp, useG * 8);
		memcpy(dm, sm, useG * 2);
		dp += useG * 4;
		dm += useG;
		sp += groups * 4;
		sm += groups;
	}
	rec->key = key;
	rec->ident = ident;
	rec->tag = tag;
	rec->w = (uint8_t)w;
	rec->h = (uint8_t)h;
	rec->offG = (uint8_t)g0;
	rec->useG = (uint8_t)useG;
	rec->offY = (uint8_t)y0;
	rec->useH = (uint8_t)useH;
	rec->lastPhase = 0;
	linkRec(rec, bucket);
	++g_stPerf.bakeMakes;
	return rec;
}

// Pair tables for sprites, as the room decoder's (video_st.cpp): index
// a pair of pixels' logical colours, left | right << 4, and get their
// remapped bits in the top two bits of each plane byte. One a colour
// bank, rebuilt when the bank's palette token changes.
struct SprPair {
	uint8_t colMask, token;
	bool valid;
	uint32_t t[256];
};
static SprPair g_sprPair[8];

static void sprPairInvalidate(int i) {
	g_sprPair[i].valid = false;
}

static const uint32_t *sprPair(uint8_t colMask, uint8_t token) {
	SprPair &p = g_sprPair[(colMask >> 4) & 7];
	if (p.valid && p.colMask == colMask && p.token == token) {
		return p.t;
	}
	uint8_t map16[16];
	ST_buildMap16(colMask, map16);
	ST_buildPairTable(map16, p.t);
	p.colMask = colMask;
	p.token = token;
	p.valid = true;
	return p.t;
}

// A bake straight from the Amiga bitplanes, eight pixels (a byte of each
// plane) at a time: the four plane bytes spread into eight nibbles, and
// each pair of pixels indexes a table holding their remapped bits for all
// four ST planes - the room decoder's method (packCellsAsm), done here
// for a whole sprite in one call, straight into the bake's plane words
// (movep puts a unit's four plane bytes two apart, left unit on the high
// bytes, right on the low) and its mask. A flipped sprite's units are
// read from the end of each row, every byte through a bit reversal. The
// chunky route unpacked every pixel into a byte only for
// STDL_BlitIndexed8 to pack it back, and the C wrapping this kernel first
// had cost twice what the kernel did.
//
// bakeUnitsAsm(planes out, mask out, first source unit, plane size,
//              units a row, rows, source step at a row's end,
//              spread, pair, rep, bitrev or 0)
// units may be odd: the last group's right half is then empty.
extern "C" void bakeUnitsAsm(uint8_t *dp, uint8_t *dm, const uint8_t *src, long planeSize,
	long units, long rows, long step, const uint32_t *spread, const uint32_t *pair,
	const uint32_t *rep, const uint8_t *bitrev);
__asm__(
"    .text\n"
"    .even\n"
"    .globl _bakeUnitsAsm\n"
"_bakeUnitsAsm:\n"
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    subq.l #8,%sp\n"
"    move.l 56(%sp),%a0\n"
"    move.l 60(%sp),%a1\n"
"    move.l 64(%sp),%a2\n"
"    move.l 68(%sp),%d7\n"
"    move.l %d7,%d6\n"
"    add.l %d6,%d6\n"
"    move.l %d6,%d5\n"
"    add.l %d7,%d5\n"
"    move.l 84(%sp),%a3\n"
"    move.l 88(%sp),%a4\n"
"    move.l 92(%sp),%a5\n"
"    move.l 96(%sp),%a6\n"
"    move.l 76(%sp),(%sp)\n"
"    jbeq budone\n"
"    cmpa.w #0,%a6\n"
"    jbne frow\n"
"nrow:\n"
"    move.l 72(%sp),%d0\n"
"    lsr.l #1,%d0\n"
"    move.l %d0,4(%sp)\n"
"    jbeq ntail\n"
"npair:\n"
"    moveq #0,%d0\n"
"    move.b (%a2),%d0\n"
"    moveq #0,%d1\n"
"    move.b (%a2,%d7.l),%d1\n"
"    moveq #0,%d2\n"
"    move.b (%a2,%d6.l),%d2\n"
"    moveq #0,%d3\n"
"    move.b (%a2,%d5.l),%d3\n"
"    addq.l #1,%a2\n"
"    move.b %d0,%d4\n"
"    or.b %d1,%d4\n"
"    or.b %d2,%d4\n"
"    or.b %d3,%d4\n"
"    jbeq bu1\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a3,%d0.w),%d0\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    add.w #1024,%d1\n"
"    or.l 0(%a3,%d1.w),%d0\n"
"    add.w %d2,%d2\n"
"    add.w %d2,%d2\n"
"    add.w #2048,%d2\n"
"    or.l 0(%a3,%d2.w),%d0\n"
"    add.w %d3,%d3\n"
"    add.w %d3,%d3\n"
"    add.w #3072,%d3\n"
"    or.l 0(%a3,%d3.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #2,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #4,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a4,%d0.w),%d1\n"
"    lsr.l #6,%d1\n"
"    or.l %d1,%d2\n"
"    moveq #0,%d0\n"
"    move.b %d4,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    and.l 0(%a5,%d0.w),%d2\n"
"    movep.l %d2,0(%a0)\n"
"    not.b %d4\n"
"    move.b %d4,(%a1)+\n"
"    jbra bu2\n"
"bu1:\n"
"    moveq #0,%d2\n"
"    movep.l %d2,0(%a0)\n"
"    st (%a1)+\n"
"bu2:\n"
"    moveq #0,%d0\n"
"    move.b (%a2),%d0\n"
"    moveq #0,%d1\n"
"    move.b (%a2,%d7.l),%d1\n"
"    moveq #0,%d2\n"
"    move.b (%a2,%d6.l),%d2\n"
"    moveq #0,%d3\n"
"    move.b (%a2,%d5.l),%d3\n"
"    addq.l #1,%a2\n"
"    move.b %d0,%d4\n"
"    or.b %d1,%d4\n"
"    or.b %d2,%d4\n"
"    or.b %d3,%d4\n"
"    jbeq bu3\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a3,%d0.w),%d0\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    add.w #1024,%d1\n"
"    or.l 0(%a3,%d1.w),%d0\n"
"    add.w %d2,%d2\n"
"    add.w %d2,%d2\n"
"    add.w #2048,%d2\n"
"    or.l 0(%a3,%d2.w),%d0\n"
"    add.w %d3,%d3\n"
"    add.w %d3,%d3\n"
"    add.w #3072,%d3\n"
"    or.l 0(%a3,%d3.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #2,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #4,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a4,%d0.w),%d1\n"
"    lsr.l #6,%d1\n"
"    or.l %d1,%d2\n"
"    moveq #0,%d0\n"
"    move.b %d4,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    and.l 0(%a5,%d0.w),%d2\n"
"    movep.l %d2,1(%a0)\n"
"    not.b %d4\n"
"    move.b %d4,(%a1)+\n"
"    jbra bu4\n"
"bu3:\n"
"    moveq #0,%d2\n"
"    movep.l %d2,1(%a0)\n"
"    st (%a1)+\n"
"bu4:\n"
"    addq.l #8,%a0\n"
"    subq.l #1,4(%sp)\n"
"    jbne npair\n"
"ntail:\n"
"    btst #0,75(%sp)\n"
"    jbeq nend\n"
"    moveq #0,%d0\n"
"    move.b (%a2),%d0\n"
"    moveq #0,%d1\n"
"    move.b (%a2,%d7.l),%d1\n"
"    moveq #0,%d2\n"
"    move.b (%a2,%d6.l),%d2\n"
"    moveq #0,%d3\n"
"    move.b (%a2,%d5.l),%d3\n"
"    addq.l #1,%a2\n"
"    move.b %d0,%d4\n"
"    or.b %d1,%d4\n"
"    or.b %d2,%d4\n"
"    or.b %d3,%d4\n"
"    jbeq bu5\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a3,%d0.w),%d0\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    add.w #1024,%d1\n"
"    or.l 0(%a3,%d1.w),%d0\n"
"    add.w %d2,%d2\n"
"    add.w %d2,%d2\n"
"    add.w #2048,%d2\n"
"    or.l 0(%a3,%d2.w),%d0\n"
"    add.w %d3,%d3\n"
"    add.w %d3,%d3\n"
"    add.w #3072,%d3\n"
"    or.l 0(%a3,%d3.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #2,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #4,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a4,%d0.w),%d1\n"
"    lsr.l #6,%d1\n"
"    or.l %d1,%d2\n"
"    moveq #0,%d0\n"
"    move.b %d4,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    and.l 0(%a5,%d0.w),%d2\n"
"    movep.l %d2,0(%a0)\n"
"    not.b %d4\n"
"    move.b %d4,(%a1)+\n"
"    jbra bu6\n"
"bu5:\n"
"    moveq #0,%d2\n"
"    movep.l %d2,0(%a0)\n"
"    st (%a1)+\n"
"bu6:\n"
"    moveq #0,%d2\n"
"    movep.l %d2,1(%a0)\n"
"    st (%a1)+\n"
"    addq.l #8,%a0\n"
"nend:\n"
"    add.l 80(%sp),%a2\n"
"    subq.l #1,(%sp)\n"
"    jbne nrow\n"
"    jbra budone\n"
"frow:\n"
"    move.l 72(%sp),%d0\n"
"    lsr.l #1,%d0\n"
"    move.l %d0,4(%sp)\n"
"    jbeq ftail\n"
"fpair:\n"
"    moveq #0,%d0\n"
"    move.b (%a2),%d0\n"
"    move.b 0(%a6,%d0.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b (%a2,%d7.l),%d1\n"
"    move.b 0(%a6,%d1.w),%d1\n"
"    moveq #0,%d2\n"
"    move.b (%a2,%d6.l),%d2\n"
"    move.b 0(%a6,%d2.w),%d2\n"
"    moveq #0,%d3\n"
"    move.b (%a2,%d5.l),%d3\n"
"    move.b 0(%a6,%d3.w),%d3\n"
"    subq.l #1,%a2\n"
"    move.b %d0,%d4\n"
"    or.b %d1,%d4\n"
"    or.b %d2,%d4\n"
"    or.b %d3,%d4\n"
"    jbeq bu7\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a3,%d0.w),%d0\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    add.w #1024,%d1\n"
"    or.l 0(%a3,%d1.w),%d0\n"
"    add.w %d2,%d2\n"
"    add.w %d2,%d2\n"
"    add.w #2048,%d2\n"
"    or.l 0(%a3,%d2.w),%d0\n"
"    add.w %d3,%d3\n"
"    add.w %d3,%d3\n"
"    add.w #3072,%d3\n"
"    or.l 0(%a3,%d3.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #2,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #4,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a4,%d0.w),%d1\n"
"    lsr.l #6,%d1\n"
"    or.l %d1,%d2\n"
"    moveq #0,%d0\n"
"    move.b %d4,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    and.l 0(%a5,%d0.w),%d2\n"
"    movep.l %d2,0(%a0)\n"
"    not.b %d4\n"
"    move.b %d4,(%a1)+\n"
"    jbra bu8\n"
"bu7:\n"
"    moveq #0,%d2\n"
"    movep.l %d2,0(%a0)\n"
"    st (%a1)+\n"
"bu8:\n"
"    moveq #0,%d0\n"
"    move.b (%a2),%d0\n"
"    move.b 0(%a6,%d0.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b (%a2,%d7.l),%d1\n"
"    move.b 0(%a6,%d1.w),%d1\n"
"    moveq #0,%d2\n"
"    move.b (%a2,%d6.l),%d2\n"
"    move.b 0(%a6,%d2.w),%d2\n"
"    moveq #0,%d3\n"
"    move.b (%a2,%d5.l),%d3\n"
"    move.b 0(%a6,%d3.w),%d3\n"
"    subq.l #1,%a2\n"
"    move.b %d0,%d4\n"
"    or.b %d1,%d4\n"
"    or.b %d2,%d4\n"
"    or.b %d3,%d4\n"
"    jbeq bu9\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a3,%d0.w),%d0\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    add.w #1024,%d1\n"
"    or.l 0(%a3,%d1.w),%d0\n"
"    add.w %d2,%d2\n"
"    add.w %d2,%d2\n"
"    add.w #2048,%d2\n"
"    or.l 0(%a3,%d2.w),%d0\n"
"    add.w %d3,%d3\n"
"    add.w %d3,%d3\n"
"    add.w #3072,%d3\n"
"    or.l 0(%a3,%d3.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #2,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #4,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a4,%d0.w),%d1\n"
"    lsr.l #6,%d1\n"
"    or.l %d1,%d2\n"
"    moveq #0,%d0\n"
"    move.b %d4,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    and.l 0(%a5,%d0.w),%d2\n"
"    movep.l %d2,1(%a0)\n"
"    not.b %d4\n"
"    move.b %d4,(%a1)+\n"
"    jbra bu10\n"
"bu9:\n"
"    moveq #0,%d2\n"
"    movep.l %d2,1(%a0)\n"
"    st (%a1)+\n"
"bu10:\n"
"    addq.l #8,%a0\n"
"    subq.l #1,4(%sp)\n"
"    jbne fpair\n"
"ftail:\n"
"    btst #0,75(%sp)\n"
"    jbeq fend\n"
"    moveq #0,%d0\n"
"    move.b (%a2),%d0\n"
"    move.b 0(%a6,%d0.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b (%a2,%d7.l),%d1\n"
"    move.b 0(%a6,%d1.w),%d1\n"
"    moveq #0,%d2\n"
"    move.b (%a2,%d6.l),%d2\n"
"    move.b 0(%a6,%d2.w),%d2\n"
"    moveq #0,%d3\n"
"    move.b (%a2,%d5.l),%d3\n"
"    move.b 0(%a6,%d3.w),%d3\n"
"    subq.l #1,%a2\n"
"    move.b %d0,%d4\n"
"    or.b %d1,%d4\n"
"    or.b %d2,%d4\n"
"    or.b %d3,%d4\n"
"    jbeq bu11\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a3,%d0.w),%d0\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    add.w #1024,%d1\n"
"    or.l 0(%a3,%d1.w),%d0\n"
"    add.w %d2,%d2\n"
"    add.w %d2,%d2\n"
"    add.w #2048,%d2\n"
"    or.l 0(%a3,%d2.w),%d0\n"
"    add.w %d3,%d3\n"
"    add.w %d3,%d3\n"
"    add.w #3072,%d3\n"
"    or.l 0(%a3,%d3.w),%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #2,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    moveq #0,%d1\n"
"    move.b %d0,%d1\n"
"    add.w %d1,%d1\n"
"    add.w %d1,%d1\n"
"    move.l 0(%a4,%d1.w),%d1\n"
"    lsr.l #4,%d1\n"
"    or.l %d1,%d2\n"
"    lsr.l #8,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    move.l 0(%a4,%d0.w),%d1\n"
"    lsr.l #6,%d1\n"
"    or.l %d1,%d2\n"
"    moveq #0,%d0\n"
"    move.b %d4,%d0\n"
"    add.w %d0,%d0\n"
"    add.w %d0,%d0\n"
"    and.l 0(%a5,%d0.w),%d2\n"
"    movep.l %d2,0(%a0)\n"
"    not.b %d4\n"
"    move.b %d4,(%a1)+\n"
"    jbra bu12\n"
"bu11:\n"
"    moveq #0,%d2\n"
"    movep.l %d2,0(%a0)\n"
"    st (%a1)+\n"
"bu12:\n"
"    moveq #0,%d2\n"
"    movep.l %d2,1(%a0)\n"
"    st (%a1)+\n"
"    addq.l #8,%a0\n"
"fend:\n"
"    add.l 80(%sp),%a2\n"
"    subq.l #1,(%sp)\n"
"    jbne frow\n"
"    jbra budone\n"
"budone:\n"
"    addq.l #8,%sp\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"

);

// a byte in all four bytes of a long: what is drawn of a unit, per plane
static uint32_t g_rep[256];

static Rec *bakePlanar(const void *key, uint32_t ident, uint16_t tag, int kind, int w, int h, uint8_t colMask, unsigned flags, uint16_t bucket) {
	uint8_t *planes = g_chunky;                       // four planes fit where a chunky frame would
	if (!g_decode(g_decodeCtx, kind, (const uint8_t *)key, w, h, planes, true)) {
		return 0;
	}
	if (g_rep[1] == 0) {
		for (int i = 0; i < 256; ++i) {
			g_rep[i] = (uint32_t)i * 0x01010101UL;
		}
	}
	const int n = w >> 3;                             // units a row
	const int planeSize = n * h;
	const bool flip = (flags & kSTSpriteXflip) != 0;
	// What is drawn at all: the rows with anything in them, and an OR of
	// every row per unit for the columns. One pass, no compares per unit.
	int y0 = -1, y1 = -1;
	uint8_t colOr[kMaxG * 2];
	memset(colOr, 0, sizeof(colOr));
	{
		const uint8_t *q0 = planes, *q1 = q0 + planeSize, *q2 = q1 + planeSize, *q3 = q2 + planeSize;
		for (int r = 0; r < h; ++r) {
			uint8_t rowOr = 0;
			for (int u = 0; u < n; ++u) {
				const uint8_t v = (uint8_t)(*q0++ | *q1++ | *q2++ | *q3++);
				colOr[u] |= v;
				rowOr |= v;
			}
			if (rowOr) {
				if (y0 < 0) {
					y0 = r;
				}
				y1 = r;
			}
		}
	}
	int u0 = n, u1 = -1;                              // drawing order
	for (int u = 0; u < n; ++u) {
		if (colOr[u]) {
			const int du = flip ? n - 1 - u : u;
			if (du < u0) {
				u0 = du;
			}
			if (du > u1) {
				u1 = du;
			}
		}
	}
	if (y1 < 0 || u1 < 0) {          // nothing drawn at all
		y0 = y1 = 0;
		u0 = u1 = 0;
	}
	const int g0 = u0 >> 1;
	const int useG = (u1 >> 1) - g0 + 1;
	const int useH = y1 - y0 + 1;
	const uint32_t bytes = kRecBytes + (uint32_t)(useG * 5) * (uint32_t)useH * 2;
	Rec *rec = allocRec((bytes + 3) & ~3UL, -1);
	if (!rec) {
		return 0;
	}
	uint16_t *dp = recPlanes(rec);
	uint16_t *dm = dp + useG * 4 * useH;
	const int ua = g0 * 2;                            // first unit kept, drawing order
	int units = n - ua;                               // the kept units the sprite has: one short
	if (units > useG * 2) {                           // of the groups when its width is odd
		units = useG * 2;
	}
	const uint8_t *src = planes + y0 * n + (flip ? n - 1 - ua : ua);
	bakeUnitsAsm((uint8_t *)dp, (uint8_t *)dm, src, planeSize, units, useH,
		flip ? n + units : n - units, ST_spreadTable(), sprPair(colMask, (uint8_t)(tag >> 8)),
		g_rep, flip ? ST_bitrevTable() : 0);
	rec->key = key;
	rec->ident = ident;
	rec->tag = tag;
	rec->w = (uint8_t)w;
	rec->h = (uint8_t)h;
	rec->offG = (uint8_t)g0;
	rec->useG = (uint8_t)useG;
	rec->offY = (uint8_t)y0;
	rec->useH = (uint8_t)useH;
	rec->lastPhase = 0;
	linkRec(rec, bucket);
	++g_stPerf.bakeMakes;
	++g_stPerf.bakePlanar;
	return rec;
}

static Rec *bake(const void *key, uint32_t ident, uint16_t tag, int kind, int w, int h, uint8_t colMask, unsigned flags, uint16_t bucket) {
	if (!(flags & kSTSpriteColMajor) && kind != kSprKindIcon && !(g_options.ab & kAbChunkyBake)) {
		Rec *r = bakePlanar(key, ident, tag, kind, w, h, colMask, flags, bucket);
		if (r) {
			return r;
		}
	}
	return bakeChunky(key, ident, tag, kind, w, h, colMask, flags, bucket);
}

// The base bake shifted right by `phase` into a record of its own, one
// group wider so the pixels pushed out of the last group have somewhere
// to land: drawn from that, a sprite that stays put lands on a group
// boundary and the blit skips the shift chain (~120 cycles a group-row).
// Output pixel i of group g is source pixel 16g + i - phase, the top of
// a 32-bit window over the two neighbouring source words.
static Rec *buildShifted(Rec *base, int phase, uint16_t tag, uint16_t bucket) {
	const int sg = base->useG + 1;
	const int rows = base->useH;
	const uint32_t bytes = kRecBytes + (uint32_t)(sg * 5) * (uint32_t)rows * 2;
	Rec *s = allocRec((bytes + 3) & ~3UL, base->seg);
	if (!s) {
		++g_stPerf.shRefused;
		return 0;
	}
	const int last = base->useG;
	const uint16_t *sp = recPlanes(base);
	const uint16_t *sm = sp + last * 4 * rows;
	uint16_t *dp = recPlanes(s);
	uint16_t *dm = dp + sg * 4 * rows;
	// The first group has nothing on its left and the last nothing on its
	// right, so they are done apart and the groups between test nothing:
	// a test for each neighbour on every plane word cost more than the
	// shift.
	for (int row = rows; --row >= 0; ) {
		const uint16_t *a = sp;
		const uint16_t *am = sm;
		*dp++ = (uint16_t)(a[0] >> phase);
		*dp++ = (uint16_t)(a[1] >> phase);
		*dp++ = (uint16_t)(a[2] >> phase);
		*dp++ = (uint16_t)(a[3] >> phase);
		*dm++ = (uint16_t)((0xFFFF0000UL | am[0]) >> phase);
		for (int g = last - 1; --g >= 0; a += 4, ++am) {
			*dp++ = (uint16_t)((((uint32_t)a[0] << 16) | a[4]) >> phase);
			*dp++ = (uint16_t)((((uint32_t)a[1] << 16) | a[5]) >> phase);
			*dp++ = (uint16_t)((((uint32_t)a[2] << 16) | a[6]) >> phase);
			*dp++ = (uint16_t)((((uint32_t)a[3] << 16) | a[7]) >> phase);
			*dm++ = (uint16_t)((((uint32_t)am[0] << 16) | am[1]) >> phase);
		}
		*dp++ = (uint16_t)(((uint32_t)a[0] << 16) >> phase);
		*dp++ = (uint16_t)(((uint32_t)a[1] << 16) >> phase);
		*dp++ = (uint16_t)(((uint32_t)a[2] << 16) >> phase);
		*dp++ = (uint16_t)(((uint32_t)a[3] << 16) >> phase);
		*dm++ = (uint16_t)((((uint32_t)am[0] << 16) | 0xFFFF) >> phase);
		sp += last * 4;
		sm += last;
	}
	s->key = base->key;
	s->ident = base->ident;
	s->tag = tag;
	s->w = base->w;
	s->h = base->h;
	s->offG = base->offG;
	s->useG = (uint8_t)sg;
	s->offY = base->offY;
	s->useH = base->useH;
	s->lastPhase = 0;
	linkRec(s, bucket);
	++g_stPerf.shBuilds;
	return s;
}

// A hit in a segment the head will reach within two moves is copied to
// the head, so a sprite that keeps being drawn keeps its bake.
static Rec *promote(Rec *r) {
	if (((g_seg - r->seg) & (kSegs - 1)) < 2) {
		return r;
	}
	const uint32_t bytes = (uint32_t)r->sizeL << 2;
	Rec *n = allocRec(bytes, r->seg);
	if (!n) {
		return r;
	}
	const uint8_t seg = n->seg;
	memcpy(n, r, bytes);
	n->seg = seg;
	unlinkRec(r);
	linkRec(n, n->bucket);
	++g_stPerf.promotes;
	return n;
}

// --- drawing ----------------------------------------------------------

// OR of N priority words a row over h rows, unrolled: the looped form
// spent more on its counters than on the reads
template <int N> static uint16_t orPrio(const uint16_t *row, int h) {
	uint16_t any = 0;
	for (int y = h; --y >= 0; row += kSTPrioRowBytes / 2) {
		any |= row[0];
		if (N > 1) any |= row[1];
		if (N > 2) any |= row[2];
		if (N > 3) any |= row[3];
		if (N > 4) any |= row[4];
		if (N > 5) any |= row[5];
	}
	return any;
}

// Is any priority bit set under groups [g0, g1] of rows [y0, y1)? A
// sprite drawn "under" the foreground only differs from a plain draw
// where there is foreground, and most of the time there is none under
// Conrad or an enemy: the check is a few hundred word reads, where the
// UNDER blit is STDL's per-pixel CPU route - 4.9ms a character on an
// STE, measured, against the BLiTTER or the aligned copy without it.
static bool prioUnder(const uint8_t *layer, int g0, int g1, int y0, int y1) {
	if (y0 < 0) y0 = 0;
	if (y1 > kSTLayerH) y1 = kSTLayerH;
	if (g0 < 0) g0 = 0;
	if (g1 > kSTLayerW / 16 - 1) g1 = kSTLayerW / 16 - 1;
	if (y0 >= y1 || g0 > g1) {
		return false;
	}
	const uint16_t *row = (const uint16_t *)(layer + kSTPlaneBytes + y0 * kSTPrioRowBytes) + g0;
	const int n = g1 - g0 + 1;
	if (n <= 6) {
		// eight rows at a time: foreground under a sprite's head need
		// not wait for a scan of its feet
		for (int y = y0; y < y1; y += 8, row += 8 * kSTPrioRowBytes / 2) {
			const int h = (y1 - y < 8) ? y1 - y : 8;
			uint16_t any;
			switch (n) {
			case 1: any = orPrio<1>(row, h); break;
			case 2: any = orPrio<2>(row, h); break;
			case 3: any = orPrio<3>(row, h); break;
			case 4: any = orPrio<4>(row, h); break;
			case 5: any = orPrio<5>(row, h); break;
			default: any = orPrio<6>(row, h); break;
			}
			if (any) {
				return true;
			}
		}
		return false;
	}
	for (int y = y1 - y0; --y >= 0; row += kSTPrioRowBytes / 2) {
		for (int i = 0; i < n; ++i) {
			if (row[i]) {
				return true;
			}
		}
	}
	return false;
}

// Present one bake through STDL. The layer's mask is the game's priority
// plane, so UNDER passes the sprite behind marked foreground and MARK
// claims it - the semantics the chunky path gets from STDL_BlitIndexed8.
// The sprite is whole and may hang off the layer: STDL clips it.
static void blitRec(uint8_t *layer, Rec *r, int x, int y, bool respectPrio, bool setPrio) {
	const int dx = x + r->offG * 16;
	const int dy = y + r->offY;
	if (respectPrio && !setPrio) {
		respectPrio = prioUnder(layer, dx >> 4, (dx + r->useG * 16 - 1) >> 4, dy, dy + r->useH);
	}
	// A draw that neither reads nor sets the priority plane goes to the
	// maskless view: on the masked one STDL's default upkeep cleared the
	// plane's bits under every opaque pixel, and nothing restored them.
	STDL_Surface *dst = ST_layerView(layer, respectPrio || setPrio);
	if (!dst) {
		return;
	}
	static STDL_Surface *view;
	if (!view) {
		view = STDL_CreateSurfaceFrom((uint8_t *)recPlanes(r), r->useG * 16, r->useH, r->useG * 8,
		                              (uint8_t *)(recPlanes(r) + r->useG * 4 * r->useH), r->useG * 2);
		if (!view) {
			return;
		}
		// STDL_TRANSPARENT: the bake's own mask as it is. A numeric key
		// would rebuild it by scanning the plane words, and the words
		// under transparent pixels are whatever the bake left there.
		STDL_SetColourKey(view, 1, STDL_TRANSPARENT);
	}
	// Where a copy shifted to this phase is held the blit runs from that,
	// landing on a group boundary: the same pixels, no shift chain. It is
	// made the second time running a sprite is drawn at one phase.
	const int phase = dx & 15;
	Rec *src = r;
	int dstX = dx;
	if (phase != 0 && !(g_options.ab & kAbNoPreshift)) {
		const uint16_t stag = (uint16_t)((r->tag & ~kTagPhaseMask) | (phase << kTagPhaseShift));
		uint16_t b;
		Rec *s = findRec(r->key, r->ident, stag, r->w, r->h, &b);
		if (!s && r->lastPhase == phase) {
			s = buildShifted(r, phase, stag, b);
		}
		if (s) {
			++s->hits;
			src = s;
			dstX = dx & ~15;
		}
	}
	r->lastPhase = (uint8_t)phase;
	uint16_t *planes = recPlanes(src);
	view->pixels = (uint8_t *)planes;
	view->w = (int16_t)(src->useG * 16);
	view->h = (int16_t)src->useH;
	view->stride = (uint16_t)(src->useG * 8);
	view->mask = (uint8_t *)(planes + src->useG * 4 * src->useH);
	view->maskstride = (uint16_t)(src->useG * 2);
	STDL_Rect dr;
	dr.x = (int16_t)dstX;
	dr.y = (int16_t)dy;
	unsigned f = 0;
	if (respectPrio) {
		f |= STDL_BLIT_UNDER;
	}
	if (setPrio) {
		f |= STDL_BLIT_MARK;
		ST_prioTouchedRect(dstX, dy, src->useG * 16, src->useH);
	}
	if (respectPrio) {
		if (dstX & 15) {
			++g_stPerf.blitUnderSh;
		} else {
			++g_stPerf.blitUnderAl;
		}
	} else if (dstX & 15) {
		++g_stPerf.blitPlainSh;
	} else {
		++g_stPerf.blitPlainAl;
	}
	STDL_BlitSurfaceEx(view, NULL, dst, &dr, f);
}

static void drawChunky(uint8_t *layer, int kind, const uint8_t *data, int w, int h, int x, int y, uint8_t colMask, unsigned flags, bool setPrio) {
	if (w > kMaxW || h > kMaxH) {
		return;                       // no frame in the game is this big
	}
	g_decode(g_decodeCtx, kind, data, w, h, g_chunky, false);
	uint8_t map16[16];
	ST_buildMap16(colMask, map16);
	int pitch;
	const uint8_t *src = chunkySource(g_chunky, w, h, flags, &pitch);
	++g_stPerf.chunky;
	ST_drawSprite(layer, src, pitch, x, y, w, h, map16, flags, setPrio);
}

uint8_t ST_sprToken(uint8_t colMask) {
	return palToken(colMask);
}

void ST_sprDraw(uint8_t *layer, int kind, const uint8_t *data, uint32_t ident, int w, int h,
                int x, int y, uint8_t colMask, unsigned flags, bool setPrio) {
	if (w <= 0 || h <= 0 || x >= kSTLayerW || y >= kSTLayerH || x + w <= 0 || y + h <= 0) {
		return;
	}
	if (ST_dlRecording(layer) && ST_dlAddSprite(kind, data, ident, w, h, x, y, colMask, flags, setPrio)) {
		return;
	}
	if (w > kMaxW || h > kMaxH || (g_options.ab & kAbNoBakes) || !ringOpen()) {
		drawChunky(layer, kind, data, w, h, x, y, colMask, flags, setPrio);
		return;
	}
	const uint16_t tag = (uint16_t)((palToken(colMask) << 8) | (flags & (kSTSpriteXflip | kSTSpriteColMajor)));
	uint16_t b;
	Rec *r = findRec(data, ident, tag, (uint8_t)w, (uint8_t)h, &b);
	if (r) {
		++g_stPerf.bakeHits;
		++r->hits;
		r = promote(r);
	} else {
		r = bake(data, ident, tag, kind, w, h, colMask, flags, b);
		if (!r) {
			drawChunky(layer, kind, data, w, h, x, y, colMask, flags, setPrio);
			return;
		}
	}
	blitRec(layer, r, x, y, (flags & kSTSpriteRespectPrio) != 0, setPrio);
}

#endif
