/* SPDX-License-Identifier: MIT */
#include "ddraw/ddraw.h"
#include "mss32/mss32.h"
#include "gdi32/gdi32.h"
#include "kernel32/k32.h"
#include "rc/hle_modules.h"
#include "user32/user32.h"
#include "winmm/winmm.h"

void rc_hle_kernel32(void)
{
    rc_register_imports(rc_k32_process, rc_k32_process_count);
    rc_register_imports(rc_k32_memory, rc_k32_memory_count);
    rc_register_imports(rc_k32_locale, rc_k32_locale_count);
    rc_register_imports(rc_k32_file, rc_k32_file_count);
}

void rc_hle_user32(void)
{
    rc_register_imports(rc_user32_setup, rc_user32_setup_count);
    rc_register_imports(rc_user32_windows, rc_user32_windows_count);
}

void rc_hle_gdi32(void)
{
    rc_register_imports(rc_gdi32_setup, rc_gdi32_setup_count);
    rc_register_imports(rc_gdi32_text, rc_gdi32_text_count);
}

void rc_hle_ddraw(void) { rc_register_imports(rc_ddraw_defs, rc_ddraw_count); }

void rc_hle_mss32(void)
{
    rc_register_imports(rc_mss32_defs, rc_mss32_count);
    rc_mss32_init();
}

void rc_hle_winmm(void) { rc_register_imports(rc_winmm_defs, rc_winmm_count); }

void rc_hle_all(void)
{
    rc_hle_winmm();
    rc_hle_mss32();
    rc_hle_ddraw();
    rc_hle_kernel32();
    rc_hle_user32();
    rc_hle_gdi32();
}
