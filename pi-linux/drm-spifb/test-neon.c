/*
 * Userspace test harness for NEON 2:1 XRGB8888 → RGB565 BE scaling.
 * Validates NEON assembly against the scalar reference from drm-spifb.c.
 *
 * Build on Pi:
 *   gcc -mfpu=neon -mfloat-abi=hard -O2 -o test-neon test-neon.c
 *   ./test-neon
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/*
 * Scalar reference — identical math to nw_spifb_scale_xrgb8888() in
 * the kernel driver, but for a single row doing 2:1 downscale.
 *
 * Takes 'count' input pairs (2*count u32 pixels), picks even pixels,
 * converts XRGB8888 → RGB565 big-endian, writes 'count' u16 outputs.
 */
static void scale_row_scalar(const uint32_t *src, uint16_t *dst, int count)
{
	for (int i = 0; i < count; i++) {
		uint32_t pix = src[i * 2]; /* 2:1 downscale: pick even pixel */
		uint16_t r = (pix >> 16) & 0xff;
		uint16_t g = (pix >> 8) & 0xff;
		uint16_t b = pix & 0xff;
		uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		/* Big-endian byte swap */
		dst[i] = (rgb565 >> 8) | (rgb565 << 8);
	}
}

/*
 * NEON implementation — processes 8 output pixels per call.
 * Takes 16 u32 input pixels (VLD2 deinterleaves even/odd),
 * converts even pixels to RGB565 BE, writes 8 u16 outputs.
 *
 * 'count' must be a multiple of 8.
 */
static void scale_row_neon(const uint32_t *src, uint16_t *dst, int count)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	__asm__ volatile (
		/* q15 = 0x00FF mask for each u16 lane */
		"vmov.i16	q15, #0xFF\n\t"

	"1:\n\t"
		/*
		 * VLD2.32 deinterleaves: even indices → q0, odd → q1.
		 * From 8 consecutive u32 pixels, we get pixels 0,2,4,6 in q0.
		 * Two loads cover 16 input pixels → 8 even (output) pixels.
		 */
		"vld2.32	{d0-d3}, [%[src]]!\n\t"   /* q0=pix[0,2,4,6] q1=pix[1,3,5,7] */
		"vld2.32	{d4-d7}, [%[src]]!\n\t"   /* q2=pix[8,10,12,14] q3=pix[9,11,13,15] */

		/* Extract R channel: shift right 16, narrow u32→u16, mask */
		"vshrn.u32	d8, q0, #16\n\t"          /* d8 = (pix >> 16) as u16 = 0xXXRR */
		"vshrn.u32	d9, q2, #16\n\t"          /* d9 = same for pixels 4-7 */
		"vand		q4, q4, q15\n\t"          /* q4 = 0x00RR (mask off XX byte) */

		/* Extract G channel: shift right 8, narrow, mask */
		"vshrn.u32	d10, q0, #8\n\t"          /* d10 = (pix >> 8) as u16 = 0xRRGG */
		"vshrn.u32	d11, q2, #8\n\t"
		"vand		q5, q5, q15\n\t"          /* q5 = 0x00GG (mask off RR byte) */

		/* Extract B channel: narrow (no shift), mask */
		"vmovn.u32	d12, q0\n\t"              /* d12 = pix as u16 = 0xGGBB */
		"vmovn.u32	d13, q2\n\t"
		"vand		q6, q6, q15\n\t"          /* q6 = 0x00BB (mask off GG byte) */

		/* Pack into RGB565: R[15:11] G[10:5] B[4:0] */
		"vshr.u16	q4, q4, #3\n\t"           /* R >> 3 (5 bits) */
		"vshl.u16	q4, q4, #11\n\t"          /* R << 11 → bits [15:11] */
		"vshr.u16	q5, q5, #2\n\t"           /* G >> 2 (6 bits) */
		"vshl.u16	q5, q5, #5\n\t"           /* G << 5 → bits [10:5] */
		"vshr.u16	q6, q6, #3\n\t"           /* B >> 3 (5 bits) → bits [4:0] */
		"vorr		q7, q4, q5\n\t"
		"vorr		q7, q7, q6\n\t"

		/* Byte-swap to big-endian */
		"vrev16.8	q7, q7\n\t"

		/* Store 8 output pixels */
		"vst1.16	{d14-d15}, [%[dst]]!\n\t"

		"subs		%[count], %[count], #8\n\t"
		"bne		1b\n\t"

		: [src] "+r" (src),
		  [dst] "+r" (dst),
		  [count] "+r" (count)
		:
		: "memory",
		  "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7", "q15"
	);
#else
	(void)src; (void)dst; (void)count;
	fprintf(stderr, "NEON not available — compile with -mfpu=neon\n");
	exit(1);
#endif
}

/* --- Test infrastructure --- */

static int failures = 0;

static void test_pattern(const char *name, const uint32_t *src, int n_input,
			 int n_output)
{
	uint16_t *ref = calloc(n_output, sizeof(uint16_t));
	uint16_t *neon = calloc(n_output, sizeof(uint16_t));

	scale_row_scalar(src, ref, n_output);
	scale_row_neon(src, neon, n_output);

	int mismatch = 0;
	for (int i = 0; i < n_output; i++) {
		if (ref[i] != neon[i]) {
			if (mismatch < 8) {
				uint32_t pix = src[i * 2];
				printf("  MISMATCH [%d]: input=0x%08X  ref=0x%04X  neon=0x%04X\n",
				       i, pix, ref[i], neon[i]);
			}
			mismatch++;
		}
	}

	if (mismatch) {
		printf("FAIL %s: %d/%d mismatches\n", name, mismatch, n_output);
		failures++;
	} else {
		printf("PASS %s (%d pixels)\n", name, n_output);
	}

	free(ref);
	free(neon);
}

/* Fill an input buffer with a solid color (16 pixels = 8 output) */
static void fill_solid(uint32_t *buf, int count, uint32_t color)
{
	for (int i = 0; i < count; i++)
		buf[i] = color;
}

/* Fill with a gradient: each pixel gets a unique value */
static void fill_gradient(uint32_t *buf, int count)
{
	for (int i = 0; i < count; i++) {
		uint8_t r = (i * 7) & 0xFF;
		uint8_t g = (i * 13 + 50) & 0xFF;
		uint8_t b = (i * 23 + 100) & 0xFF;
		buf[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
	}
}

int main(void)
{
	/* 16 input pixels → 8 output pixels (minimum NEON iteration) */
	const int N_IN = 16;
	const int N_OUT = 8;
	uint32_t src[N_IN];

	printf("=== NEON RGB565 scaling test ===\n\n");

	/* Solid colors */
	fill_solid(src, N_IN, 0x00FF0000); /* Pure red */
	test_pattern("Pure red (0x00FF0000)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0x0000FF00); /* Pure green */
	test_pattern("Pure green (0x0000FF00)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0x000000FF); /* Pure blue */
	test_pattern("Pure blue (0x000000FF)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0x00FFFFFF); /* White */
	test_pattern("White (0x00FFFFFF)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0x00000000); /* Black */
	test_pattern("Black (0x00000000)", src, N_IN, N_OUT);

	/* Non-zero X byte — the bug that caused previous NEON failure */
	fill_solid(src, N_IN, 0xFFFF0000); /* Red with X=0xFF */
	test_pattern("Red with X=FF (0xFFFF0000)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0xFF00FF00); /* Green with X=0xFF */
	test_pattern("Green with X=FF (0xFF00FF00)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0xFF0000FF); /* Blue with X=0xFF */
	test_pattern("Blue with X=FF (0xFF0000FF)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0xFFFFFFFF); /* White with X=0xFF */
	test_pattern("White with X=FF (0xFFFFFFFF)", src, N_IN, N_OUT);

	/* Values that stress channel boundaries */
	fill_solid(src, N_IN, 0x00070307); /* Low bits only */
	test_pattern("Low bits (0x00070307)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0x00F8FCF8); /* Max RGB565 values */
	test_pattern("Max 565 (0x00F8FCF8)", src, N_IN, N_OUT);

	fill_solid(src, N_IN, 0x008040C0); /* Mid values */
	test_pattern("Mid (0x008040C0)", src, N_IN, N_OUT);

	/* Gradient — each pixel different, stresses all channels */
	fill_gradient(src, N_IN);
	test_pattern("Gradient (unique pixels)", src, N_IN, N_OUT);

	/* Larger test: 320 output pixels (full display row) */
	const int BIG_OUT = 320;
	const int BIG_IN = BIG_OUT * 2;
	uint32_t *big_src = malloc(BIG_IN * sizeof(uint32_t));
	fill_gradient(big_src, BIG_IN);
	test_pattern("Full row gradient (320px)", big_src, BIG_IN, BIG_OUT);
	free(big_src);

	/* Stress test: many rows */
	const int ROWS = 240;
	const int ROW_OUT = 320;
	const int ROW_IN = ROW_OUT * 2;
	uint32_t *row_src = malloc(ROW_IN * sizeof(uint32_t));
	uint16_t *ref_out = malloc(ROW_OUT * sizeof(uint16_t));
	uint16_t *neon_out = malloc(ROW_OUT * sizeof(uint16_t));
	int total_mismatch = 0;

	for (int y = 0; y < ROWS; y++) {
		for (int i = 0; i < ROW_IN; i++) {
			uint8_t r = (i + y * 3) & 0xFF;
			uint8_t g = (i * 2 + y * 7) & 0xFF;
			uint8_t b = (i * 3 + y * 11) & 0xFF;
			uint8_t x = (y & 1) ? 0xFF : 0x00;
			row_src[i] = (x << 24) | (r << 16) | (g << 8) | b;
		}
		scale_row_scalar(row_src, ref_out, ROW_OUT);
		scale_row_neon(row_src, neon_out, ROW_OUT);
		for (int i = 0; i < ROW_OUT; i++) {
			if (ref_out[i] != neon_out[i])
				total_mismatch++;
		}
	}

	if (total_mismatch) {
		printf("FAIL Full frame (320x240): %d mismatches\n", total_mismatch);
		failures++;
	} else {
		printf("PASS Full frame (320x240, %d pixels)\n", ROWS * ROW_OUT);
	}

	free(row_src);
	free(ref_out);
	free(neon_out);

	/* Summary */
	printf("\n=== %s: %d test(s) failed ===\n",
	       failures ? "FAILED" : "ALL PASSED", failures);

	return failures ? 1 : 0;
}
