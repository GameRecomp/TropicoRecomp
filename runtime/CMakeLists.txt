# SPDX-License-Identifier: MIT
add_library(rc_runtime STATIC
    src/mem.c
    src/log.c
)
target_include_directories(rc_runtime PUBLIC include)

if(RC_BUILD_TESTS)
    add_executable(rc_runtime_tests tests/test_main.c tests/test_mem.c tests/test_cpu.c)
    target_link_libraries(rc_runtime_tests PRIVATE rc_runtime)
    add_test(NAME runtime COMMAND rc_runtime_tests)
endif()
