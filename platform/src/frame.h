/* SPDX-License-Identifier: MIT */
#ifndef RC_FRAME_H
#define RC_FRAME_H

#include <stdint.h>

/* Keep a copy of a frame being presented. */
void rc_frame_keep(const uint32_t *pixels, int width, int height, int pitch);

#endif
