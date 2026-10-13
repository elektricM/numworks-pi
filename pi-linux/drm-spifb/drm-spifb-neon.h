/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm-spifb NEON helpers (drm-spifb-neon.c). Plain C types only: that file
 * cannot include kernel headers, whose stdint types clash with <arm_neon.h>.
 * Call between kernel_fpu_begin() and kernel_fpu_end().
 */
#ifndef DRM_SPIFB_NEON_H
#define DRM_SPIFB_NEON_H

void nw_neon_2x_row(const unsigned char *r0, const unsigned char *r1,
		    unsigned short *out, unsigned int n8,
		    const unsigned char *drb, const unsigned char *dg);

#endif
