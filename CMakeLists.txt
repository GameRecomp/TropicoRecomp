# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)
project(tropicorecomp LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RC_BUILD_TESTS "Build the runtime's unit tests" ON)
option(RC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(MSVC)
    add_compile_options(/W4)
    if(RC_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)
    if(RC_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

if(RC_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(runtime)
add_subdirectory(hle)
add_subdirectory(games/tropico1)
