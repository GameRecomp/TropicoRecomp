# SPDX-License-Identifier: MIT
# Tropico 2: the game executable, lifted code (generated locally by
# `recomp lift` from the user's own copy), the runtime and the HLE modules.
# No patches yet.
#
# tropico2-shell is the same program without lifted code, built always (CI
# builds it on every system): it runs the native code and game files it
# finds in data/ beside it, which the converter puts there.
set_source_files_properties(main.c PROPERTIES COMPILE_DEFINITIONS "RC_REVISION=\"${RC_REVISION}\"")
set(TROPICO2_PATCHES patches.c patch_resolution.c)

if(ANDROID)
    # On Android the shell is libmain.so, which SDL's activity loads and
    # starts at SDL_main; the target has a name of its own beside Tropico's.
    add_library(tropico2_main SHARED main.c shell.c ${TROPICO2_PATCHES})
    set_target_properties(tropico2_main PROPERTIES OUTPUT_NAME main)
    target_compile_definitions(tropico2_main PRIVATE main=SDL_main)
    target_link_libraries(tropico2_main PRIVATE rc_hle rc_runtime)
    return()
endif()
add_executable(tropico2-shell main.c shell.c ${TROPICO2_PATCHES})
target_link_libraries(tropico2-shell PRIVATE rc_hle rc_runtime)

# The Windows programs' icon (tools/make-icons; CI makes it) and version
# information.
if(WIN32)
    enable_language(RC)
    set(TROPICO2_ICON "${PROJECT_SOURCE_DIR}/build/icons/tropico.ico" CACHE FILEPATH
        "The Windows icon (tools/make-icons); none if the file is missing")
    if(EXISTS "${TROPICO2_ICON}")
        file(TO_CMAKE_PATH "${TROPICO2_ICON}" icon)
        set(TROPICO2_ICON_LINE "1 ICON \"${icon}\"")
    else()
        set(TROPICO2_ICON_LINE "")
    endif()
    string(REPLACE "." "," RC_VERSION_COMMAS "${RC_VERSION}")
    configure_file(tropico2.rc.in "${CMAKE_CURRENT_BINARY_DIR}/tropico2.rc" @ONLY)
    target_sources(tropico2-shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tropico2.rc")
endif()
if(EMSCRIPTEN)
    # The web shell: an ES module (createTropico2) that web/play.js starts
    # after it has given it the game's module and files (as Tropico's).
    target_link_options(tropico2-shell PRIVATE
        -sJSPI=1
        "-sJSPI_EXPORTS=['main','rc_web_thread_entry']"
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=4GB
        -sALLOW_TABLE_GROWTH=1
        -sSTACK_SIZE=8MB
        -sINITIAL_MEMORY=64MB
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createTropico2
        -sENVIRONMENT=web
        -sINVOKE_RUN=0
        -sEXIT_RUNTIME=0
        -sFORCE_FILESYSTEM=1
        "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_rc_web_thread_entry','_rc_wasm_helpers','_rc_wasm_register']"
        "-sEXPORTED_RUNTIME_METHODS=['FS','IDBFS','ENV','callMain','HEAPU8','HEAPU32','addRunDependency','removeRunDependency']"
        "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$rcLoadCode','$rcGameFS']"
        -lidbfs.js)
    set_target_properties(tropico2-shell PROPERTIES OUTPUT_NAME tropico2)
    # The web build is the shell only (its output name is the game's).
    return()
elseif(MSVC)
    # A windowed program (no console window) that still starts at main().
    target_link_options(tropico2-shell PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

set(TROPICO2_GENERATED "${PROJECT_SOURCE_DIR}/generated/tropico2/c" CACHE PATH
    "Directory with the C that recomp lift generated for Tropico 2")

# Configure again as soon as the lifted code appears (or changes).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${TROPICO2_GENERATED}/dispatch.c")

# Without lifted code tropico2 cannot link, so it is left out of the default
# build (CI builds without game code).
if(NOT EXISTS "${TROPICO2_GENERATED}/dispatch.c")
    message(STATUS "tropico2: no generated code in ${TROPICO2_GENERATED}; run recomp lift first (tropico2 will not link)")
    add_executable(tropico2 EXCLUDE_FROM_ALL main.c ${TROPICO2_PATCHES})
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

add_executable(tropico2 main.c ${TROPICO2_PATCHES} $<TARGET_OBJECTS:tropico2_lifted>)
target_link_libraries(tropico2 PRIVATE rc_hle rc_runtime)
if(WIN32)
    target_sources(tropico2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tropico2.rc")
endif()
if(MSVC)
    target_link_options(tropico2 PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()
