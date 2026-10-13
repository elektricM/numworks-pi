// SPDX-License-Identifier: GPL-2.0
/*
 * NEON version of drm-spifb's exact 2:1 downscale: 2x2 box average, ordered
 * dither and big-endian RGB565 pack, bit for bit the same as the scalar
 * nw_build_rect_2x() and nw_pack(). Built with the kernel's FPU flags
 * (CC_FLAGS_FPU), so it must only run inside kernel_fpu_begin()/end().
 */
#include <arm_neon.h>
#include "drm-spifb-neon.h"

/*
 * Downscale @n8 groups of 8 output pixels from the XRGB8888 source rows
 * @r0 and @r1 (16 pixels each per group) into @out. @drb and @dg are the
 * dither added to red/blue and to green for pixels 0..7 of every group.
 */
void nw_neon_2x_row(const unsigned char *r0, const unsigned char *r1,
		    unsigned short *out, unsigned int n8,
		    const unsigned char *drb, const unsigned char *dg)
{
	uint8x8_t vdrb = vld1_u8(drb), vdg = vld1_u8(dg);

	while (n8--) {
		/* De-interleave 16 pixels per row: val[0] blue, [1] green, [2] red */
		uint8x16x4_t a = vld4q_u8(r0), b = vld4q_u8(r1);
		/* Add horizontal pairs, then the row below; (sum + 2) >> 2 */
		uint8x8_t bl = vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(a.val[0]), b.val[0]), 2);
		uint8x8_t gr = vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(a.val[1]), b.val[1]), 2);
		uint8x8_t rd = vrshrn_n_u16(vpadalq_u8(vpaddlq_u8(a.val[2]), b.val[2]), 2);
		uint16x8_t px;

		/* Scale by 31/32 (63/64 for green) and dither, as nw_pack() */
		rd = vadd_u8(vsub_u8(rd, vshr_n_u8(rd, 5)), vdrb);
		gr = vadd_u8(vsub_u8(gr, vshr_n_u8(gr, 6)), vdg);
		bl = vadd_u8(vsub_u8(bl, vshr_n_u8(bl, 5)), vdrb);

		/* RGB565: red's top 5 bits, then green's top 6, then blue's top 5 */
		px = vshll_n_u8(rd, 8);
		px = vsriq_n_u16(px, vshll_n_u8(gr, 8), 5);
		px = vsriq_n_u16(px, vshll_n_u8(bl, 8), 11);
		/* Big-endian on the wire */
		vst1q_u8((unsigned char *)out, vrev16q_u8(vreinterpretq_u8_u16(px)));

		r0 += 64;
		r1 += 64;
		out += 8;
	}
}
