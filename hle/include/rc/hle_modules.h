/* SPDX-License-Identifier: MIT */
/* The reimplemented DLLs. A game project registers the ones it needs
 * before its executable is loaded (rc_config.setup). */
#ifndef RC_HLE_MODULES_H
#define RC_HLE_MODULES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rc_hle_kernel32(void);
void rc_hle_user32(void);
void rc_hle_gdi32(void);
void rc_hle_ddraw(void);
void rc_hle_mss32(void);
void rc_hle_winmm(void);

/* Everything above. */
void rc_hle_all(void);

/* DirectDraw's display mode (0 x 0 before one is set). */
void rc_ddraw_mode(uint32_t *w, uint32_t *h);

/* A display mode DirectDraw offers besides its own list (a game project's
 * resolution patch); 0 x 0 for none. */
void rc_ddraw_extra_mode(uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif
