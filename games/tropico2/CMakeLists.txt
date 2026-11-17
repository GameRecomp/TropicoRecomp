# SPDX-License-Identifier: MIT
# Tropico 2: the game executable, lifted code (generated locally by
# `recomp lift` from the user's own copy), the runtime and the HLE modules.
# No patches yet, and no shell for the converter.
set(TROPICO2_GENERATED "${PROJECT_SOURCE_DIR}/generated/tropico2/c" CACHE PATH
    "Directory with the C that recomp lift generated for Tropico 2")

# Configure again as soon as the lifted code appears (or changes).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${TROPICO2_GENERATED}/dispatch.c")

set_source_files_properties(main.c PROPERTIES COMPILE_DEFINITIONS "RC_REVISION=\"${RC_REVISION}\"")

# Without lifted code tropico2 cannot link, so it is left out of the default
# build (CI builds without game code).
if(NOT EXISTS "${TROPICO2_GENERATED}/dispatch.c")
    message(STATUS "tropico2: no generated code in ${TROPICO2_GENERATED}; run recomp lift first (tropico2 will not link)")
    add_executable(tropico2 EXCLUDE_FROM_ALL main.c)
    target_link_libraries(tropico2 PRIVATE rc_hle rc_runtime)
    return()
endif()

file(GLOB TROPICO2_LIFTED CONFIGURE_DEPENDS "${TROPICO2_GENERATED}/*.c")
add_library(tropico2_lifted OBJECT ${TROPICO2_LIFTED})
target_include_directories(tropico2_lifted PRIVATE "${TROPICO2_GENERATED}")
target_link_libraries(tropico2_lifted PRIVATE rc_runtime)
# Lifted code is not debugged at the C level and is slow unoptimised.
if(NOT MSVC)
    target_compile_options(tropico2_lifted PRIVATE -O1 -w)
endif()

add_executable(tropico2 main.c $<TARGET_OBJECTS:tropico2_lifted>)
target_link_libraries(tropico2 PRIVATE rc_hle rc_runtime)
if(MSVC)
    target_link_options(tropico2 PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()
