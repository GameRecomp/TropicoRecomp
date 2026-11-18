# SPDX-License-Identifier: MIT
# Lifting as part of the build: a game's lifted C (generated/<game>/c) is
# made again by `recomp lift` whenever it is older than what it comes from,
# so that building the game (from an IDE's debug action too) never runs
# code lifted for patches that have changed since.
#
#   rc_lift(GAME GAME_DIR GENERATED)
#
# - recomp is built by cargo on every build (nothing to do when nothing
#   changed);
# - the game is lifted again when games/<GAME>/game.toml (its hooks and
#   points) or recomp (the lifter) is newer than the last lift;
# - without lifted code but with the game in GAME_DIR, it is lifted while
#   configuring, so that the sources are there to be found.
#
# `recomp lift` rewrites only the files whose content changed, so only those
# are compiled again. The lifted files are the lift's byproducts (that is
# how the build sees them change while it runs), so cleaning the build
# removes them too, and the next build lifts again. Off with
# -DRC_AUTO_LIFT=OFF, and without cargo or the game (CI builds without it):
# then `recomp lift` is run by hand.

option(RC_AUTO_LIFT "Lift a game's code again when its patches or the lifter changed" ON)

set(RC_RECOMP_DIR "${PROJECT_SOURCE_DIR}/recomp")
set(RC_RECOMP "${RC_RECOMP_DIR}/target/release/recomp${CMAKE_EXECUTABLE_SUFFIX}")
find_program(RC_CARGO cargo)

# Whether games can be lifted here; sets RC_CAN_LIFT in the caller.
macro(rc_can_lift game_dir)
    set(RC_CAN_LIFT OFF)
    if(RC_AUTO_LIFT AND RC_CARGO AND NOT CMAKE_CROSSCOMPILING AND EXISTS "${game_dir}")
        set(RC_CAN_LIFT ON)
    endif()
endmacro()

# recomp, built by cargo: a target every lift depends on.
function(rc_recomp_target)
    if(TARGET recomp_cli)
        return()
    endif()
    add_custom_target(recomp_cli
        COMMAND "${RC_CARGO}" build --release --quiet -p rc-cli --manifest-path "${RC_RECOMP_DIR}/Cargo.toml"
        BYPRODUCTS "${RC_RECOMP}"
        COMMENT "Building recomp (cargo)"
        USES_TERMINAL)
endfunction()

# Lift while configuring, when there is no lifted code yet.
function(rc_lift_now game game_dir generated)
    rc_can_lift("${game_dir}")
    if(NOT RC_CAN_LIFT OR EXISTS "${generated}/dispatch.c")
        return()
    endif()
    message(STATUS "${game}: lifting ${game_dir} (no lifted code yet)")
    execute_process(
        COMMAND "${RC_CARGO}" build --release --quiet -p rc-cli --manifest-path "${RC_RECOMP_DIR}/Cargo.toml"
        RESULT_VARIABLE failed)
    if(NOT failed)
        execute_process(
            COMMAND "${RC_RECOMP}" lift "${game_dir}" --game "${game}" --out "${generated}"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            RESULT_VARIABLE failed)
    endif()
    if(failed)
        message(WARNING "${game}: lifting failed; run recomp lift by hand")
    endif()
endfunction()

# Lift again during the build when the patches or the lifter changed, before
# the lifted code (the target `lifted`, of the files `sources`) is compiled.
function(rc_lift game game_dir generated lifted sources)
    rc_can_lift("${game_dir}")
    if(NOT RC_CAN_LIFT)
        return()
    endif()
    rc_recomp_target()
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${game}-lift.stamp")
    file(GLOB headers "${generated}/*.h")
    add_custom_command(
        OUTPUT "${stamp}"
        BYPRODUCTS ${sources} ${headers}
        COMMAND "${RC_RECOMP}" lift "${game_dir}" --game "${game}" --out "${generated}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${PROJECT_SOURCE_DIR}/games/${game}/game.toml" "${RC_RECOMP}" recomp_cli
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Lifting ${game} again (its patches or the lifter changed)"
        USES_TERMINAL)
    add_custom_target(${game}_lift DEPENDS "${stamp}")
    add_dependencies(${lifted} ${game}_lift)
endfunction()
