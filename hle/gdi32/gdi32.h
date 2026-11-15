/* SPDX-License-Identifier: MIT */
/* Shared by the gdi32 sources. */
#ifndef RC_GDI32_H
#define RC_GDI32_H

#include "rc/hle.h"

/* Fill a rectangle of a DC with a COLORREF (FillRect). */
void rc_gdi_fill(uint32_t hdc, int32_t l, int32_t t, int32_t r, int32_t b, uint32_t colorref);

extern const rc_import_def rc_gdi32_text[];
extern const uint32_t rc_gdi32_text_count;
extern const rc_import_def rc_gdi32_setup[];
extern const uint32_t rc_gdi32_setup_count;

#endif
