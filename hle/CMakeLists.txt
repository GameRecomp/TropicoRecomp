# SPDX-License-Identifier: MIT
add_library(rc_hle STATIC
    modules.c
    kernel32/process.c
    kernel32/memory.c
    kernel32/locale.c
    kernel32/file.c
    kernel32/profile.c
    kernel32/sync.c
    kernel32/version.c
    kernel32/ole32.c
    user32/user32.c
    user32/window.c
    gdi32/gdi32.c
    gdi32/dc.c
    gdi32/text.c
    ddraw/ddraw.c
    frames.c
    mss32/mss32.c
    winmm/winmm.c
)
target_include_directories(rc_hle PUBLIC include PRIVATE .)
target_link_libraries(rc_hle PUBLIC rc_runtime rc_platform)
