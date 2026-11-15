# SPDX-License-Identifier: MIT
# The host platform: SDL3 when it is found, otherwise the null platform
# (no window, no input), so that everything else still builds.
option(RC_PLATFORM_NULL "Build the null platform even if SDL3 is available" OFF)

if(NOT RC_PLATFORM_NULL)
    find_package(SDL3 CONFIG QUIET)
endif()

if(SDL3_FOUND)
    find_package(SDL3_ttf CONFIG QUIET)
    find_package(SDL3_mixer CONFIG QUIET)
endif()

if(SDL3_FOUND AND SDL3_ttf_FOUND)
    add_library(rc_platform STATIC src/frame.c src/platform_sdl3.c src/text_ttf.c)
    target_link_libraries(rc_platform PUBLIC SDL3::SDL3 SDL3_ttf::SDL3_ttf)
    message(STATUS "platform: SDL3 ${SDL3_VERSION}, SDL3_ttf ${SDL3_ttf_VERSION}")
else()
    add_library(rc_platform STATIC src/frame.c src/platform_null.c src/text_null.c)
    message(STATUS "platform: null (SDL3 or SDL3_ttf not found)")
endif()

# Sound: SDL3_mixer when it is there, otherwise none.
if(SDL3_FOUND AND SDL3_ttf_FOUND AND SDL3_mixer_FOUND)
    target_sources(rc_platform PRIVATE src/audio_mixer.c)
    target_link_libraries(rc_platform PUBLIC SDL3_mixer::SDL3_mixer)
    message(STATUS "platform: SDL3_mixer ${SDL3_mixer_VERSION}")
else()
    target_sources(rc_platform PRIVATE src/audio_null.c)
endif()
target_include_directories(rc_platform PUBLIC include)
