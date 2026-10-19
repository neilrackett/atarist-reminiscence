
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 * Atari ST port changes Copyright (C) 2026 Neil Rackett
 */

#include "unpack.h"
#include "util.h"

/*
 * Bytekiller. The stream is read backwards a long at a time and
 * the output written backwards too; every long carries 32 bits, and
 * the very first one (the last in the file) has a marker bit above
 * its data instead of a full 32.
 *
 * Bits are taken from the bottom of the current long, most
 * significant first in the value they build, so an n-bit field is
 * the bit reverse of the low n bits: one table lookup, where the
 * reference decoder called a function per bit. Everything lives in
 * locals, as the Atari ST unpacks every room and sprite bank with
 * this and it was a visible part of each room change.
 */

static uint8_t g_rev8[256];
static bool g_rev8Ready;

static void initRev8() {
	for (int b = 0; b < 256; ++b) {
		uint8_t r = 0;
		for (int i = 0; i < 8; ++i) {
			if (b & (1 << i)) {
				r |= 0x80 >> i;
			}
		}
		g_rev8[b] = r;
	}
	g_rev8Ready = true;
}

// the low n (1..8) bits of x, reversed
#define BK_REV(x, n) ((uint32_t)g_rev8[(x) & 0xFF] >> (8 - (n)))

#define BK_REFILL() \
	do { \
		bits = READ_BE_UINT32(in); in -= 4; \
		crc ^= bits; \
		nbits = 32; \
	} while (0)

// v = the next n (1..8) bits
#define BK_GET(n, v) \
	do { \
		if (nbits >= (n)) { \
			v = BK_REV(bits, (n)); \
			bits >>= (n); \
			nbits -= (n); \
		} else { \
			const int k = nbits; \
			const uint32_t hi = k ? BK_REV(bits, k) : 0; \
			const int r = (n) - k; \
			BK_REFILL(); \
			v = (hi << r) | BK_REV(bits, r); \
			bits >>= r; \
			nbits -= r; \
		} \
	} while (0)

#if defined(ATARIST) && defined(__m68k__)
/*
 * The same decoder in 68000 code: gcc 4.6 spent about 100 cycles on a
 * two-bit field, 250 a byte overall, and Conrad's sprites alone took
 * 5.5s to unpack on an 8MHz ST. Here a field of n bits is four
 * instructions while the long holds n of them: the low byte indexes a
 * table of its low n bits reversed (g_bkTab: 2, 3, 4 and 8 bits, 256
 * bytes each), and the long shifts on. A field that runs off the end
 * of the long takes a call to bk_slow, which joins the two parts.
 *
 *   d0 bits, valid from the bottom   d7 how many (byte)
 *   d1-d3 code, length, offset       d4 table index (bits 8-15 clear)
 *   d5 crc                           d6 bytes left to write
 *   a0 in (read with -(a0))          a1 out (written with -(a1))
 *   a2 a reference's source          a3-a6 the 2, 3, 4 and 8-bit tables
 */
static uint8_t g_bkTab[4 * 256];

__asm__(
"    .text\n"
"    .even\n"
/* reg = the next \cnt bits, MSB first; a stub at the end of the
 * function (BK_SLOW) handles the end of a long. */
".macro BK_GET cnt,tab,reg,id\n"
"    subq.b  #\\cnt,%d7\n"
"    bcs     bks_\\id\n"
"    move.b  %d0,%d4\n"
"    move.b  (\\tab,%d4.w),\\reg\n"
"    lsr.l   #\\cnt,%d0\n"
"bkr_\\id:\n"
".endm\n"
/* reg (zero beforehand) = the next bit */
".macro BK_BIT reg,id\n"
"    subq.b  #1,%d7\n"
"    bcs     bks_\\id\n"
"    lsr.l   #1,%d0\n"
"    addx.b  \\reg,\\reg\n"
"bkr_\\id:\n"
".endm\n"
".macro BK_SLOW cnt,reg,id\n"
"bks_\\id:\n"
"    moveq   #\\cnt,%d4\n"
"    bsr     bk_slow\n"
"    move.b  %d4,\\reg\n"
"    bra     bkr_\\id\n"
".endm\n"
/* uint32_t bk_core(uint8_t *out, const uint8_t *in, uint32_t bits,
 *                  uint32_t nbits, uint32_t crc, int32_t size,
 *                  const uint8_t *tabs): the crc, 0 if it checks */
"_bk_core:\n"
"    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
"    move.l  48(%sp),%a1\n"
"    move.l  52(%sp),%a0\n"
"    move.l  56(%sp),%d0\n"
"    move.l  60(%sp),%d7\n"
"    move.l  64(%sp),%d5\n"
"    move.l  68(%sp),%d6\n"
"    move.l  72(%sp),%a3\n"
"    lea     256(%a3),%a4\n"
"    lea     256(%a4),%a5\n"
"    lea     256(%a5),%a6\n"
"    moveq   #0,%d4\n"
"    tst.l   %d6\n"
"    ble     bk_done\n"
"bk_loop:\n"
"    BK_GET  2,%a3,%d1,code\n"
"    cmp.b   #2,%d1\n"
"    bcc     bk_long\n"
"    tst.b   %d1\n"
"    bne     bk_m2\n"
/* 00: a run of 1-8 bytes */
"    moveq   #0,%d2\n"
"    BK_GET  3,%a4,%d2,l3\n"
"    addq.w  #1,%d2\n"
"    bra     bk_lit\n"
/* 01: two bytes from up to 255 back */
"bk_m2:\n"
"    moveq   #0,%d3\n"
"    BK_GET  8,%a6,%d3,o2\n"
"    moveq   #2,%d2\n"
"    bra     bk_ref\n"
"bk_long:\n"
"    moveq   #0,%d2\n"
"    BK_BIT  %d2,b\n"
"    cmp.b   #3,%d1\n"
"    beq     bk_c3\n"
/* 100: three bytes, 9-bit offset; 101: four bytes, 10-bit offset */
"    moveq   #0,%d3\n"
"    BK_GET  8,%a6,%d3,o9\n"
"    moveq   #0,%d1\n"
"    tst.b   %d2\n"
"    bne     bk_m4\n"
"    BK_BIT  %d1,o9b\n"
"    add.w   %d3,%d3\n"
"    or.b    %d1,%d3\n"
"    moveq   #3,%d2\n"
"    bra     bk_ref\n"
"bk_m4:\n"
"    BK_GET  2,%a3,%d1,o10\n"
"    lsl.w   #2,%d3\n"
"    or.b    %d1,%d3\n"
"    moveq   #4,%d2\n"
"    bra     bk_ref\n"
"bk_c3:\n"
"    tst.b   %d2\n"
"    bne     bk_lit8\n"
/* 110: 1-256 bytes, 12-bit offset */
"    BK_GET  8,%a6,%d2,ln\n"
"    addq.w  #1,%d2\n"
"    moveq   #0,%d3\n"
"    BK_GET  8,%a6,%d3,o12\n"
"    moveq   #0,%d1\n"
"    BK_GET  4,%a5,%d1,o12b\n"
"    lsl.w   #4,%d3\n"
"    or.b    %d1,%d3\n"
"    bra     bk_ref\n"
/* 111: a run of 9-264 bytes */
"bk_lit8:\n"
"    moveq   #0,%d2\n"
"    BK_GET  8,%a6,%d2,l8\n"
"    add.w   #9,%d2\n"
/* d2 bytes, clipped to what is left, read from the stream */
"bk_lit:\n"
"    sub.l   %d2,%d6\n"
"    bpl     1f\n"
"    add.l   %d6,%d2\n"
"    moveq   #0,%d6\n"
"1:  subq.w  #1,%d2\n"
"bk_litb:\n"
"    BK_GET  8,%a6,%d1,lb\n"
"    move.b  %d1,-(%a1)\n"
"    dbra    %d2,bk_litb\n"
"    tst.l   %d6\n"
"    bgt     bk_loop\n"
"    bra     bk_done\n"
/* d2 bytes, clipped, from d3 back; as in the C, a zero offset
 * reads the bytes from the stream instead */
"bk_ref:\n"
"    tst.w   %d3\n"
"    beq     bk_lit\n"
"    sub.l   %d2,%d6\n"
"    bpl     2f\n"
"    add.l   %d6,%d2\n"
"    moveq   #0,%d6\n"
"2:  lea     0(%a1,%d3.w),%a2\n"
"    subq.w  #1,%d2\n"
"bk_cp:\n"
"    move.b  -(%a2),-(%a1)\n"
"    dbra    %d2,bk_cp\n"
"    tst.l   %d6\n"
"    bgt     bk_loop\n"
"bk_done:\n"
"    move.l  %d5,%d0\n"
"    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
"    rts\n"
"    BK_SLOW 2,%d1,code\n"
"    BK_SLOW 3,%d2,l3\n"
"    BK_SLOW 8,%d3,o2\n"
"    BK_SLOW 1,%d2,b\n"
"    BK_SLOW 8,%d3,o9\n"
"    BK_SLOW 1,%d1,o9b\n"
"    BK_SLOW 2,%d1,o10\n"
"    BK_SLOW 8,%d2,ln\n"
"    BK_SLOW 8,%d3,o12\n"
"    BK_SLOW 4,%d1,o12b\n"
"    BK_SLOW 8,%d2,l8\n"
"    BK_SLOW 8,%d1,lb\n"
/* d4 = the next d4 (1-8) bits when the long holds k < d4 of them
 * (d7 = k - d4): the k it has, then the rest from a new long */
"bk_slow:\n"
"    movem.l %d1-%d3,-(%sp)\n"
"    move.w  %d4,%d3\n"              /* n */
"    add.b   %d3,%d7\n"              /* k */
"    moveq   #0,%d1\n"
"    move.b  %d7,%d2\n"
"    beq     3f\n"
"    move.b  %d0,%d4\n"
"    move.b  (%a6,%d4.w),%d1\n"
"    moveq   #8,%d4\n"
"    sub.b   %d2,%d4\n"
"    lsr.b   %d4,%d1\n"              /* the k bits, reversed */
"3:  sub.b   %d2,%d3\n"              /* r = n - k still to take */
"    move.l  -(%a0),%d0\n"
"    eor.l   %d0,%d5\n"
"    lsl.w   %d3,%d1\n"
"    moveq   #0,%d4\n"
"    move.b  %d0,%d4\n"
"    move.b  (%a6,%d4.w),%d4\n"
"    moveq   #8,%d2\n"
"    sub.b   %d3,%d2\n"
"    lsr.b   %d2,%d4\n"              /* the r bits, reversed */
"    or.b    %d1,%d4\n"
"    lsr.l   %d3,%d0\n"
"    moveq   #32,%d7\n"
"    sub.b   %d3,%d7\n"
"    movem.l (%sp)+,%d1-%d3\n"
"    rts\n"
);
extern "C" uint32_t bk_core(uint8_t *out, const uint8_t *in, uint32_t bits, uint32_t nbits,
                            uint32_t crc, int32_t size, const uint8_t *tabs);

static void initBkTab() {
	static const int kBits[4] = { 2, 3, 4, 8 };
	for (int t = 0; t < 4; ++t) {
		const int n = kBits[t];
		for (int x = 0; x < 256; ++x) {
			g_bkTab[t * 256 + x] = (uint8_t)BK_REV(x, n);
		}
	}
}
#endif

bool bytekiller_unpack(uint8_t *dst, int dstSize, const uint8_t *src, int srcSize) {
	if (!g_rev8Ready) {
		initRev8();
#if defined(ATARIST) && defined(__m68k__)
		initBkTab();
#endif
	}
	const uint8_t *in = src + srcSize - 4;
	int size = READ_BE_UINT32(in); in -= 4;
	if (size > dstSize) {
		warning("Unexpected unpack size %d, buffer size %d", size, dstSize);
		return false;
	}
	uint8_t *out = dst + size;
	uint32_t crc = READ_BE_UINT32(in); in -= 4;
	uint32_t bits = READ_BE_UINT32(in); in -= 4;
	crc ^= bits;
	// the first long's data is whatever sits below its top set bit
	int nbits = 0;
	for (uint32_t t = bits; t > 1; t >>= 1) {
		++nbits;
	}
#if defined(ATARIST) && defined(__m68k__)
	// in + 4: the asm reads its next long with -(a0)
	return bk_core(out, in + 4, bits, nbits, crc, size, g_bkTab) == 0;
#else
	while (size > 0) {
		uint32_t code, len, offset;
		BK_GET(2, code);
		if (code == 0) {
			BK_GET(3, len);
			len += 1;
			offset = 0;
		} else if (code == 1) {
			len = 2;
			BK_GET(8, offset);
		} else {
			uint32_t b;
			BK_GET(1, b);
			code = ((code & 1) << 1) | b;
			if (code == 3) {
				BK_GET(8, len);
				len += 9;
				offset = 0;
			} else if (code == 2) {
				BK_GET(8, len);
				len += 1;
				BK_GET(8, offset);
				BK_GET(4, b);
				offset = (offset << 4) | b;
			} else {
				len = code + 3;
				BK_GET(8, offset);
				BK_GET(code + 1, b);
				offset = (offset << (code + 1)) | b;
			}
		}
		size -= len;
		if (size < 0) {
			len += size;
			size = 0;
		}
		if (offset != 0) {
			const uint8_t *from = out + offset;
			while (len-- > 0) {
				*--out = *--from;
			}
		} else {
			while (len-- > 0) {
				uint32_t b;
				BK_GET(8, b);
				*--out = (uint8_t)b;
			}
		}
	}
	assert(size == 0);
	return crc == 0;
#endif
}

struct bitstream_t {
	uint16_t mask;
	int size;
	const uint8_t *src;
};

static uint8_t read_byte(struct bitstream_t *bs) {
	const uint8_t b = *(bs->src);
	++bs->src;
	return b;
}

static void fill(struct bitstream_t *bs) {
	if (bs->size < 9) {
		const uint8_t b = read_byte(bs);
		bs->mask |= b << (8 - bs->size);
		bs->size += 8;
	}
}

static int get_bit(struct bitstream_t *bs) {
	fill(bs);
	assert(bs->size > 0);
	const int val = (bs->mask & 0x8000) != 0;
	bs->mask <<= 1;
	--bs->size;
	return val;
}

static int get_bits(struct bitstream_t *bs, int count) {
	if (count > bs->size) {
		const int val = bs->mask >> (16 - bs->size);
		count -= bs->size;
		bs->size = 0;
		bs->mask = 0;
		fill(bs);
		return (val << count) | get_bits(bs, count);
	} else {
		assert(count <= bs->size);
		const int val = bs->mask >> (16 - count);
		bs->mask <<= count;
		bs->size -= count;
		return val;
	}
}

static uint8_t rol1(uint8_t r) {
	return (r << 1) | (r >> 7);
}

struct UnpackCtx {
	struct bitstream_t bs;
	uint8_t *dst;
	uint32_t dstSize, dstOffset;
};

static void outputb(UnpackCtx *ctx, uint8_t b) {
	assert(ctx->dstOffset < ctx->dstSize);
	ctx->dst[ctx->dstOffset++] = b;
}

static void outputs(UnpackCtx *ctx, int offset, int count) {
	assert(ctx->dstOffset + count <= ctx->dstSize);
	for (int i = 0; i < count; ++i) {
		ctx->dst[ctx->dstOffset + i] = ctx->dst[ctx->dstOffset + offset + i];
	}
	ctx->dstOffset += count;
}

static int decode_bs(UnpackCtx *ctx, const uint8_t *src, int size) {
	struct bitstream_t *bs = &ctx->bs;
	bs->mask = 0;
	bs->size = 0;
	bs->src = src;

	uint8_t key = read_byte(bs);
	while (size != 0) {
		if (get_bit(bs) == 0) {
			const uint8_t val = get_bits(bs, 8);
			outputb(ctx, val ^ key);
			--size;
			if (size == 0) {
				break;
			}
			key = rol1(val);
			continue;
		}
		int count = 1;
		while (count <= 8 && get_bit(bs)) {
			++count;
		}
		const int count2 = get_bits(bs, count) + 3;
		const int offset = get_bits(bs, 13);
		outputs(ctx, -offset - 1, count2);
		size -= count2;
	}
	const int read = bs->src - src;
	debug(DBG_UNPACK, "end %d bytes, out %d", read, ctx->dstOffset);
	return read;
}

bool pc98_unpack(uint8_t *dst, int dstSize, const uint8_t *src, int srcSize) {
	UnpackCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.dst = dst;
	ctx.dstSize = dstSize;
	ctx.dstOffset = 0;
	return decode_bs(&ctx, src, dstSize);
}
