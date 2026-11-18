# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)
project(tropicorecomp LANGUAGES C)

# The builds' version (the VERSION file, the one place it is set) and the
# git revision they are built from, in the window's title and the programs'
# metadata; tools/version says the same.
file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" RC_VERSION LIMIT_COUNT 1)
string(STRIP "${RC_VERSION}" RC_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/VERSION")
# Windows' version resources: four numbers, 0.1.0 as 0,1,0,0.
string(REPLACE "." ";" rc_version_parts "${RC_VERSION}")
list(LENGTH rc_version_parts n)
while(n LESS 4)
    list(APPEND rc_version_parts 0)
    math(EXPR n "${n} + 1")
endwhile()
list(JOIN rc_version_parts "," RC_VERSION_COMMAS)
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                OUTPUT_VARIABLE RC_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT RC_REVISION)
    set(RC_REVISION unknown)
endif()

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
add_subdirectory(platform)
add_subdirectory(hle)
add_subdirectory(games/tropico1)
add_subdirectory(games/tropico2)
