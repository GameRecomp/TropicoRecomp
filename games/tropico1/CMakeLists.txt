# SPDX-License-Identifier: MIT
# The game executable: lifted code (generated locally by `recomp lift` from
# the user's own copy), the runtime and the HLE modules.
#
# tropico1-shell is the same program without lifted code, built always (CI
# builds it on every system): it runs the native code and game files it
# finds in data/ beside it, which the converter puts there.
# The art generator of the Tropico resolution patch (patch_art.c), built
# with a shim that gives it what it needs on every platform.
set(TROPICO1_ARTGEN "${PROJECT_SOURCE_DIR}/third_party/tropico-resolution-patch/artgen.c")
add_library(tropico1_artgen STATIC "${TROPICO1_ARTGEN}")
target_link_libraries(tropico1_artgen PRIVATE rc_runtime)
if(MSVC)
    target_compile_options(tropico1_artgen PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/artgen_shim.h" /w)
else()
    target_compile_options(tropico1_artgen PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/artgen_shim.h" -w)
    target_compile_definitions(tropico1_artgen PRIVATE _DEFAULT_SOURCE)
    if(NOT EMSCRIPTEN)
        target_link_libraries(tropico1_artgen PRIVATE m)
    endif()
endif()

if(ANDROID)
    # On Android the shell is libmain.so, which SDL's activity loads and
    # starts at SDL_main.
    add_library(main SHARED main.c patches.c patch_options.c patch_resolution.c patch_art.c patch_vtext.c patch_fullscreen.c patch_textedit.c patch_preview.c patch_scroll.c lang.c shell.c)
    set_source_files_properties(main.c PROPERTIES COMPILE_DEFINITIONS "RC_VERSION=\"${RC_VERSION}\";RC_REVISION=\"${RC_REVISION}\"")
    target_compile_definitions(main PRIVATE main=SDL_main)
    target_link_libraries(main PRIVATE rc_hle rc_runtime tropico1_artgen)
    return()
endif()
add_executable(tropico1-shell main.c patches.c patch_options.c patch_resolution.c patch_art.c patch_vtext.c patch_fullscreen.c patch_textedit.c patch_preview.c patch_scroll.c lang.c shell.c)
target_link_libraries(tropico1-shell PRIVATE rc_hle rc_runtime tropico1_artgen)
set_source_files_properties(main.c PROPERTIES COMPILE_DEFINITIONS "RC_VERSION=\"${RC_VERSION}\";RC_REVISION=\"${RC_REVISION}\"")

# The Windows programs' icon (tools/make-icons; CI makes it) and version
# information.
if(WIN32)
    enable_language(RC)
    set(TROPICO1_ICON "${PROJECT_SOURCE_DIR}/build/icons/tropico.ico" CACHE FILEPATH
        "The Windows icon (tools/make-icons); none if the file is missing")
    if(EXISTS "${TROPICO1_ICON}")
        file(TO_CMAKE_PATH "${TROPICO1_ICON}" icon)
        set(TROPICO1_ICON_LINE "1 ICON \"${icon}\"")
    else()
        set(TROPICO1_ICON_LINE "")
    endif()
    configure_file(tropico1.rc.in "${CMAKE_CURRENT_BINARY_DIR}/tropico1.rc" @ONLY)
    target_sources(tropico1-shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tropico1.rc")
endif()
if(EMSCRIPTEN)
    # The web shell: an ES module (createTropico) that web/play.js starts
    # after it has given it the game's module and files. The game owns its
    # loop, so main runs on JSPI and gives the browser its turn (rc/web.h).
    target_link_options(tropico1-shell PRIVATE
        -sJSPI=1
        "-sJSPI_EXPORTS=['main','rc_web_thread_entry']"
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=4GB
        -sALLOW_TABLE_GROWTH=1
        -sSTACK_SIZE=8MB
        -sINITIAL_MEMORY=64MB
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createTropico
        -sENVIRONMENT=web
        -sINVOKE_RUN=0
        -sEXIT_RUNTIME=0
        -sFORCE_FILESYSTEM=1
        "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_rc_web_thread_entry','_rc_wasm_helpers','_rc_wasm_register']"
        "-sEXPORTED_RUNTIME_METHODS=['FS','IDBFS','ENV','callMain','HEAPU8','HEAPU32','addRunDependency','removeRunDependency']"
        "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$rcLoadCode','$rcGameFS']"
        -lidbfs.js)
    set_target_properties(tropico1-shell PROPERTIES OUTPUT_NAME tropico)
elseif(MSVC)
    # A windowed program (no console window) that still starts at main().
    target_link_options(tropico1-shell PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

set(TROPICO1_GENERATED "${PROJECT_SOURCE_DIR}/generated/tropico1/c" CACHE PATH
    "Directory with the C that recomp lift generated for Tropico")

# Configure again as soon as the lifted code appears (or goes), so that the
# tropico1 target below follows `recomp lift` without a manual step. A glob,
# because CMake re-checks it on every build even while dispatch.c is
# missing; a missing file in CMAKE_CONFIGURE_DEPENDS is never checked.
file(GLOB TROPICO1_DISPATCH CONFIGURE_DEPENDS "${TROPICO1_GENERATED}/dispatch.c")

# tropico1 always exists, so an IDE can always select it. Without lifted code
# it is main.c alone, which cannot link until `recomp lift` has been run, so
# it is then left out of the default build (CI builds without game code).
if(NOT EXISTS "${TROPICO1_GENERATED}/dispatch.c")
    message(STATUS "tropico1: no generated code in ${TROPICO1_GENERATED}; run recomp lift first (tropico1 will not link)")
    add_executable(tropico1 EXCLUDE_FROM_ALL main.c patches.c patch_options.c patch_resolution.c patch_art.c patch_vtext.c patch_fullscreen.c patch_textedit.c patch_preview.c patch_scroll.c lang.c)
    target_link_libraries(tropico1 PRIVATE rc_hle rc_runtime tropico1_artgen)
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

add_executable(tropico1 main.c patches.c patch_options.c patch_resolution.c patch_art.c patch_vtext.c patch_fullscreen.c patch_textedit.c patch_preview.c patch_scroll.c lang.c $<TARGET_OBJECTS:tropico1_lifted>)
target_link_libraries(tropico1 PRIVATE rc_hle rc_runtime tropico1_artgen)
if(WIN32)
    target_sources(tropico1 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tropico1.rc")
endif()
