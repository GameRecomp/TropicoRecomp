/* SPDX-License-Identifier: MIT */
/* The reimplemented DLLs. A game project registers the ones it needs
 * before its executable is loaded (rc_config.setup). */
#ifndef RC_HLE_MODULES_H
#define RC_HLE_MODULES_H

#ifdef __cplusplus
extern "C" {
#endif

void rc_hle_kernel32(void);
void rc_hle_user32(void);
void rc_hle_gdi32(void);
void rc_hle_ddraw(void);
void rc_hle_mss32(void);
void rc_hle_binkw32(void);

/* Everything above. */
void rc_hle_all(void);

#ifdef __cplusplus
}
#endif

#endif
