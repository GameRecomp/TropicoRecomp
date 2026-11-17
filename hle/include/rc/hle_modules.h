/* SPDX-License-Identifier: MIT */
/* The reimplemented DLLs. A game project registers the ones it needs
 * before its executable is loaded (rc_config.setup). */
#ifndef RC_HLE_MODULES_H
#define RC_HLE_MODULES_H

#include <stdbool.h>
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

/* Work for a game project's patches that needs the guest CPU and must
 * happen regularly: `fn` runs whenever the program looks for messages. */
struct rc_cpu;
void rc_add_pump_hook(void (*fn)(struct rc_cpu *c));

/* A filter for the host's input before the program gets it (one): true
 * when it takes the event, to hold it back or act on it itself. An event
 * held back is given to the program later with rc_input_deliver. */
struct rc_event;
void rc_set_input_filter(bool (*fn)(const struct rc_event *e));
void rc_input_deliver(const struct rc_event *e);
/* What the last pointer event came from (rc_event_source). */
int rc_input_source(void);

/* DirectDraw's display mode (0 x 0 before one is set). */
void rc_ddraw_mode(uint32_t *w, uint32_t *h);

/* A display mode DirectDraw offers besides its own list (a game project's
 * resolution patch); 0 x 0 for none. */
void rc_ddraw_extra_mode(uint32_t w, uint32_t h);
/* Further display modes DirectDraw lists beside its own, kept (up to 8);
 * for a game that enumerates the modes once and looks its size up there. */
void rc_ddraw_add_mode(uint32_t w, uint32_t h);
/* The next Blt from a source one column wide stretches it to `width`
 * columns; the column it takes is, within `range` columns of its own, the
 * one whose colours are closest to theirs on average (neither a shadow nor
 * a highlight). For a game project's patch that widens a picture; 0 cancels. */
void rc_ddraw_stretch_next(uint32_t width, uint32_t range);

#ifdef __cplusplus
}
#endif

#endif
