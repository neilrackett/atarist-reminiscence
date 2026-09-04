
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
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

bool bytekiller_unpack(uint8_t *dst, int dstSize, const uint8_t *src, int srcSize) {
	if (!g_rev8Ready) {
		initRev8();
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
