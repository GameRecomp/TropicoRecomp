# SPDX-License-Identifier: MIT
add_library(rc_hle STATIC
    modules.c
    kernel32/process.c
    kernel32/memory.c
    kernel32/locale.c
    kernel32/file.c
    user32/user32.c
    gdi32/gdi32.c
)
target_include_directories(rc_hle PUBLIC include)
target_link_libraries(rc_hle PUBLIC rc_runtime)
