// SPDX-License-Identifier: GPL-2.0-only
/*
 * NEON-accelerated pixel scaling for drm-spifb.
 *
 * Compiled separately with -mfpu=neon so NEON instructions are available.
 * Called from drm-spifb-core.c inside kernel_neon_begin()/end() sections.
 */

#include <linux/types.h>

/*
 * NEON 2:1 downscale: XRGB8888 → RGB565 big-endian, one row.
 *
 * VLD2.32 deinterleaves consecutive pixel pairs — even indices go to
 * one register, odd to another. This gives us the 2:1 decimation for free.
 *
 * Previous attempt had a color bug: VSHRN.U32 narrows u32→u16 but
 * keeps the upper byte. For pixel 0xXXRRGGBB:
 *   vshrn d, q, #16  → 0xXXRR (X leaks into R position)
 *   vshrn d, q, #8   → 0xRRGG (R leaks into G position)
 *   vmovn d, q        → 0xGGBB (G leaks into B position)
 * Fix: VAND with 0x00FF mask after each extraction.
 *
 * @src:   source row, vwidth (= 2*width) XRGB8888 pixels
 * @dst:   destination row, width RGB565 big-endian pixels
 * @width: number of output pixels (must be multiple of 8)
 */
void nw_neon_scale_row_2to1_xrgb8888(const u32 *src, u16 *dst, u32 width)
{
	__asm__ volatile (
		/* q15 = 0x00FF mask in every u16 lane (set once) */
		"vmov.i16	q15, #0xFF\n\t"

	"1:\n\t"
		/*
		 * Load 16 input pixels, deinterleave even/odd:
		 *   q0 = pixels 0, 2, 4, 6   (the ones we keep)
		 *   q1 = pixels 1, 3, 5, 7   (discarded)
		 *   q2 = pixels 8, 10, 12, 14
		 *   q3 = pixels 9, 11, 13, 15
		 */
		"vld2.32	{d0-d3}, [%[src]]!\n\t"
		"vld2.32	{d4-d7}, [%[src]]!\n\t"

		/* Extract R: (pixel >> 16) & 0xFF */
		"vshrn.u32	d8, q0, #16\n\t"
		"vshrn.u32	d9, q2, #16\n\t"
		"vand		q4, q4, q15\n\t"

		/* Extract G: (pixel >> 8) & 0xFF */
		"vshrn.u32	d10, q0, #8\n\t"
		"vshrn.u32	d11, q2, #8\n\t"
		"vand		q5, q5, q15\n\t"

		/* Extract B: pixel & 0xFF */
		"vmovn.u32	d12, q0\n\t"
		"vmovn.u32	d13, q2\n\t"
		"vand		q6, q6, q15\n\t"

		/* Pack RGB565: R[15:11] | G[10:5] | B[4:0] */
		"vshr.u16	q4, q4, #3\n\t"
		"vshl.u16	q4, q4, #11\n\t"
		"vshr.u16	q5, q5, #2\n\t"
		"vshl.u16	q5, q5, #5\n\t"
		"vshr.u16	q6, q6, #3\n\t"
		"vorr		q7, q4, q5\n\t"
		"vorr		q7, q7, q6\n\t"

		/* Byte-swap to big-endian */
		"vrev16.8	q7, q7\n\t"

		/* Store 8 output pixels */
		"vst1.16	{d14-d15}, [%[dst]]!\n\t"

		"subs		%[width], %[width], #8\n\t"
		"bne		1b\n\t"

		: [src] "+r" (src),
		  [dst] "+r" (dst),
		  [width] "+r" (width)
		:
		: "memory", "cc",
		  "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7", "q15"
	);
}
