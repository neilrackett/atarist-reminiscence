
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The 030 build's chunky-to-planar conversion: 8-bit pixels into the
 * eight interleaved planes of TT Low and a Falcon's 8-bit modes, 16
 * pixels a group, each pixel XORed with 0x70 on the way (see
 * systemstub_030.cpp for why). It reads the engine's own layers, where
 * SDL's routine, which cannot do the XOR, needed a copy of them made
 * first: that copy was about 15% of a cutscene frame on a TT and a
 * Falcon (Hatari).
 *
 * Ten bit-group exchanges (merges) between the four longs that hold a
 * group's 16 pixels move every bit to its plane, the technique SDL's own
 * conversion uses, and the XOR is done on the planes it lands in, 4, 5
 * and 6, inverted once the bits are in place. On the 68030 the loop is
 * assembler, with the C below as its twin: the C is what runs on the
 * host, where tmp/tt/c2ptest.cpp checked it against a pixel-by-pixel
 * reference, and with logging on the stub compares the two on the
 * machine at start-up (systemstub_030.cpp). gcc's own build of the C was
 * slower than SDL's routine in cutscenes, which convert whole frames:
 * its loop wrote a group's four longs back to back, each a wait for
 * ST-RAM, where the assembler spreads them among the next group's
 * merges and the 68030 goes on working while each is written (the
 * intro: 136% of its budget on a Falcon, where SDL's routine took
 * 121%). Its loop also has to fit the 68030's 256-byte instruction
 * cache: a first C version of twelve merges with word stores did not,
 * fetched its code from ST-RAM on every group, and took twice SDL's
 * time.
 *
 * Included by systemstub_030.cpp alone, which the assembler's
 * definitions need.
 */

#ifndef C2P_030_H__
#define C2P_030_H__

#include <stdint.h>

// exchange a's bits under m with b's bits under m << s
#define C2P_MERGE(a, b, s, m) do { \
	const uint32_t t_ = (((b) >> (s)) ^ (a)) & (m); \
	(a) ^= t_; \
	(b) ^= t_ << (s); \
} while (0)

// h rows of `groups` groups, each row to one line or, with DBL, to two
template <int DBL>
static void c2p8rowsC(const uint8_t *src, int sp, uint8_t *dst, int dp, int groups, int h) {
	for (; --h >= 0; src += sp, dst += dp << DBL) {
		const uint32_t *s = (const uint32_t *)src;
		uint32_t *d = (uint32_t *)dst;
		uint32_t *e = (uint32_t *)(dst + dp);
		for (int g = groups; --g >= 0; ) {
			uint32_t a0 = *s++;
			uint32_t a1 = *s++;
			uint32_t a2 = *s++;
			uint32_t a3 = *s++;
			// A long holds four pixels; numbering the bits of a group by
			// long (k1 k0) and by place in the long from the top (u4..u0),
			// each merge swaps one k bit with one u bit, and five leave
			// the longs holding planes and their words pixels in order.
			C2P_MERGE(a0, a1, 4, 0x0F0F0F0FUL);      // k0 with u2
			C2P_MERGE(a2, a3, 4, 0x0F0F0F0FUL);
			C2P_MERGE(a0, a2, 8, 0x00FF00FFUL);      // k1 with u3
			C2P_MERGE(a1, a3, 8, 0x00FF00FFUL);
			C2P_MERGE(a0, a2, 1, 0x55555555UL);      // k1 with u0
			C2P_MERGE(a1, a3, 1, 0x55555555UL);
			C2P_MERGE(a0, a2, 16, 0x0000FFFFUL);     // k1 with u4
			C2P_MERGE(a1, a3, 16, 0x0000FFFFUL);
			C2P_MERGE(a0, a2, 2, 0x33333333UL);      // k1 with u1
			C2P_MERGE(a1, a3, 2, 0x33333333UL);
			// planes 0 and 1 are a3's low word and high word, 2 and 3
			// a1's, 4 and 5 a2's, 6 and 7 a0's: the XOR's planes
			// inverted, then a swap each and a long
			a2 = ~a2;
			a0 ^= 0xFFFFUL;
			a3 = (a3 << 16) | (a3 >> 16);
			a1 = (a1 << 16) | (a1 >> 16);
			a2 = (a2 << 16) | (a2 >> 16);
			a0 = (a0 << 16) | (a0 >> 16);
			*d++ = a3;
			*d++ = a1;
			*d++ = a2;
			*d++ = a0;
			if (DBL) {
				*e++ = a3;
				*e++ = a1;
				*e++ = a2;
				*e++ = a0;
			}
		}
	}
}

#undef C2P_MERGE

#ifdef __m68k__
// Narrower boxes than this go through the C: the assembler's saved
// registers and the pipeline's first group cost gameplay's sprite boxes,
// a group or two wide, more than the pipelining gains them.
enum { kC2pAsmGroups = 4 };

// h rows (at least one) of `groups` groups (at least one) from src (sp
// bytes a row) to dst (dp bytes a line); the 2 puts each row on two
// lines, dp apart.
extern "C" void c2p8Asm(const uint8_t *src, long sp, uint8_t *dst, long dp, long groups, long h);
extern "C" void c2p8Asm2(const uint8_t *src, long sp, uint8_t *dst, long dp, long groups, long h);

// The C twin's steps, d0-d3 its a0-a3 and d7 the merges' t; masks
// 0x0F0F0F0F in d4 and 0x00FF00FF in d5, the other two immediate. Each
// stage takes S, a store of the previous group's planes, parked in
// a3-a6 so the writes to ST-RAM go out among the arithmetic. The row
// loop is here too: one call a box, where a call a row cost gameplay's
// short boxes more than the pipelining saved them (16.7ms a frame on a
// TT against the C's 16.0).
#define C2P_M(sh, back, ra, rb, mask) \
	"	move.l	" rb ",%d7\n" \
	"	lsr.l	#" #sh ",%d7\n" \
	"	eor.l	" ra ",%d7\n" \
	"	and.l	" mask ",%d7\n" \
	"	eor.l	%d7," ra "\n" \
	"	" back "\n" \
	"	eor.l	%d7," rb "\n"
#define C2P_W(ra, rb) \
	"	swap	" rb "\n" \
	"	move.w	" rb ",%d7\n" \
	"	move.w	" ra "," rb "\n" \
	"	move.w	%d7," ra "\n" \
	"	swap	" rb "\n"
#define C2P_GROUP(S1, S2, S3, S4) \
	"	move.l	(%a0)+,%d0\n" \
	"	move.l	(%a0)+,%d1\n" \
	"	move.l	(%a0)+,%d2\n" \
	"	move.l	(%a0)+,%d3\n" \
	C2P_M(4, "lsl.l	#4,%d7", "%d0", "%d1", "%d4") \
	C2P_M(4, "lsl.l	#4,%d7", "%d2", "%d3", "%d4") \
	S1 \
	C2P_M(8, "lsl.l	#8,%d7", "%d0", "%d2", "%d5") \
	C2P_M(8, "lsl.l	#8,%d7", "%d1", "%d3", "%d5") \
	S2 \
	C2P_M(1, "add.l	%d7,%d7", "%d0", "%d2", "#0x55555555") \
	C2P_M(1, "add.l	%d7,%d7", "%d1", "%d3", "#0x55555555") \
	S3 \
	C2P_W("%d0", "%d2") \
	C2P_W("%d1", "%d3") \
	S4 \
	C2P_M(2, "lsl.l	#2,%d7", "%d0", "%d2", "#0x33333333") \
	C2P_M(2, "lsl.l	#2,%d7", "%d1", "%d3", "#0x33333333") \
	"	not.l	%d2\n" \
	"	not.w	%d0\n" \
	"	swap	%d3\n" \
	"	swap	%d1\n" \
	"	swap	%d2\n" \
	"	swap	%d0\n" \
	"	move.l	%d3,%a3\n" \
	"	move.l	%d1,%a4\n" \
	"	move.l	%d2,%a5\n" \
	"	move.l	%d0,%a6\n"
#define C2P_S1(r) "	move.l	" r ",(%a1)+\n"
#define C2P_S2(r) "	move.l	" r ",(%a1)+\n	move.l	" r ",(%a2)+\n"
// the arguments, past the eleven registers saved: src 48(sp), sp 52,
// dst 56, dp 60, groups 64, h 68; src, dst and h move on a row at a time
#define C2P_FUNC(name, S, DST2, DSTSTEP) \
"	.even\n" \
"	.globl	_" name "\n" \
"_" name ":\n" \
"	movem.l	%d2-%d7/%a2-%a6,-(%sp)\n" \
"	move.l	#0x0F0F0F0F,%d4\n" \
"	move.l	#0x00FF00FF,%d5\n" \
"3:	move.l	48(%sp),%a0\n" \
"	move.l	56(%sp),%a1\n" \
	DST2 \
"	move.l	64(%sp),%d6\n" \
"	subq.l	#1,%d6\n" \
	C2P_GROUP("", "", "", "") \
"	bra.w	2f\n" \
"1:\n" \
	C2P_GROUP(S("%a3"), S("%a4"), S("%a5"), S("%a6")) \
"2:	dbra	%d6,1b\n" \
	S("%a3") S("%a4") S("%a5") S("%a6") \
"	move.l	52(%sp),%d7\n" \
"	add.l	%d7,48(%sp)\n" \
"	move.l	60(%sp),%d7\n" \
	DSTSTEP \
"	add.l	%d7,56(%sp)\n" \
"	subq.l	#1,68(%sp)\n" \
"	bne.w	3b\n" \
"	movem.l	(%sp)+,%d2-%d7/%a2-%a6\n" \
"	rts\n"

__asm__(
"	.text\n"
C2P_FUNC("c2p8Asm", C2P_S1, "", "")
C2P_FUNC("c2p8Asm2", C2P_S2, "	move.l	60(%sp),%d7\n	lea	0(%a1,%d7.l),%a2\n", "	add.l	%d7,%d7\n")
);

#undef C2P_M
#undef C2P_W
#undef C2P_GROUP
#undef C2P_S1
#undef C2P_S2
#undef C2P_FUNC

// groups x h rows from src (pitch sp bytes) to dst (pitch dp bytes); with
// dbl each row goes to two lines, dp apart
static inline void c2p8(const uint8_t *src, int sp, uint8_t *dst, int dp, int groups, int h, int dbl) {
	if (groups < kC2pAsmGroups) {
		if (dbl) {
			c2p8rowsC<1>(src, sp, dst, dp, groups, h);
		} else {
			c2p8rowsC<0>(src, sp, dst, dp, groups, h);
		}
	} else if (dbl) {
		c2p8Asm2(src, sp, dst, dp, groups, h);
	} else {
		c2p8Asm(src, sp, dst, dp, groups, h);
	}
}
#else
static inline void c2p8(const uint8_t *src, int sp, uint8_t *dst, int dp, int groups, int h, int dbl) {
	if (dbl) {
		c2p8rowsC<1>(src, sp, dst, dp, groups, h);
	} else {
		c2p8rowsC<0>(src, sp, dst, dp, groups, h);
	}
}
#endif

#endif // C2P_030_H__
