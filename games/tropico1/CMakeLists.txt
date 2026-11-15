# SPDX-License-Identifier: MIT
# The game executable: lifted code (generated locally by `recomp lift` from
# the user's own copy), the runtime and the HLE modules.
set(TROPICO1_GENERATED "${PROJECT_SOURCE_DIR}/generated/tropico1/c" CACHE PATH
    "Directory with the C that recomp lift generated for Tropico")

if(NOT EXISTS "${TROPICO1_GENERATED}/dispatch.c")
    message(STATUS "tropico1: no generated code in ${TROPICO1_GENERATED}; run recomp lift first")
    return()
endif()

file(GLOB TROPICO1_LIFTED CONFIGURE_DEPENDS "${TROPICO1_GENERATED}/*.c")
add_library(tropico1_lifted OBJECT ${TROPICO1_LIFTED})
target_include_directories(tropico1_lifted PRIVATE "${TROPICO1_GENERATED}")
target_link_libraries(tropico1_lifted PRIVATE rc_runtime)
# Lifted code is not debugged at the C level and is slow unoptimised.
if(NOT MSVC)
    target_compile_options(tropico1_lifted PRIVATE -O1 -w)
endif()

add_executable(tropico1 main.c $<TARGET_OBJECTS:tropico1_lifted>)
target_link_libraries(tropico1 PRIVATE rc_hle rc_runtime)
