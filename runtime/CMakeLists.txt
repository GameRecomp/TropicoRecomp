# SPDX-License-Identifier: MIT
add_library(rc_runtime STATIC
    src/mem.c
    src/log.c
    src/dispatch.c
    src/x87.c
)
target_include_directories(rc_runtime PUBLIC include)
if(NOT MSVC)
    target_link_libraries(rc_runtime PUBLIC m)
endif()

if(RC_BUILD_TESTS)
    add_executable(rc_runtime_tests tests/test_main.c tests/test_mem.c tests/test_cpu.c tests/test_x87.c)
    target_link_libraries(rc_runtime_tests PRIVATE rc_runtime)
    add_test(NAME runtime COMMAND rc_runtime_tests)
endif()
