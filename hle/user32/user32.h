/* SPDX-License-Identifier: MIT */
/* Shared by the user32 sources. */
#ifndef RC_USER32_H
#define RC_USER32_H

#include <stdint.h>

#include "rc/hle.h"

typedef struct rc_wclass_info rc_wclass_info;

/* A registered window class: its window procedure and style. */
const rc_wclass_info *rc_find_class(const char *name, uint32_t *wndproc, uint32_t *style);

/* Deliver host events (input, window changes) into the message queue. */
void rc_pump_events(void);
/* The window procedure of a window, or 0. */
uint32_t rc_window_proc(uint32_t hwnd);
void rc_post_message(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam);

struct rc_event;
/* Deliver an input event as if the host had sent it (scripted input). */
void rc_inject_event(const struct rc_event *e);
/* Where the mouse is, in frame coordinates. */
void rc_input_mouse(int *x, int *y);

/* The device context of a window (0: the screen). */
uint32_t rc_window_dc(uint32_t hwnd);
/* The first top-level window, the one the platform shows. */
uint32_t rc_main_window(void);
/* Make a window cover a screen of this size (a display mode change). */
void rc_window_resize(uint32_t hwnd, int32_t width, int32_t height);

extern const rc_import_def rc_user32_windows[];
extern const uint32_t rc_user32_windows_count;
extern const rc_import_def rc_user32_setup[];
extern const uint32_t rc_user32_setup_count;

#endif
