cmake_minimum_required(VERSION 3.24)
project(itb_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

include(cmake/Dependencies.cmake)

# Where the game's shipped files live. Game files are never committed; the
# default points at the git-ignored local copy of the Linux build.
get_filename_component(ITB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(ITB_DEFAULT_GAME_DIR "${ITB_REPO_ROOT}/.local_decompile/builds/linux_21601364"
    CACHE PATH "Default game root (directory containing scripts/)")

add_library(itb_engine
  src/core.cpp
  src/symbols.cpp
  src/board.cpp
  src/lua_env.cpp
  src/lua_host_values.cpp
  src/lua_host_board.cpp
  src/lua_host.cpp
  src/weapon_ports.cpp
  src/weapon_ports_list.cpp
  src/weapon_diff.cpp
  src/game_data.cpp
  src/recording.cpp
  src/format.cpp
  src/movement.cpp
  src/tile_rules.cpp
  src/tile_status.cpp
  src/space_damage.cpp
  src/timing.cpp
  src/executor.cpp
  src/push_death.cpp
  src/pilot_xp.cpp
  src/engine.cpp
  src/enemy_phase.cpp
  src/ai_phase.cpp
  src/deploy.cpp
  src/lookahead.cpp
  src/environments.cpp
  src/score.cpp
  src/threat.cpp
  src/objectives.cpp
  src/board_hash.cpp
  src/solver.cpp
)
target_include_directories(itb_engine PUBLIC include PRIVATE src)
target_link_libraries(itb_engine PRIVATE itb_lua nlohmann_json::nlohmann_json)
target_compile_definitions(itb_engine PRIVATE
  ITB_DEFAULT_GAME_DIR="${ITB_DEFAULT_GAME_DIR}")
# No fused multiply-add: A* path choice depends on the game's exact float32
# rounding (movement.cpp).
target_compile_options(itb_engine PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)

add_executable(itb_inspect tools/itb_inspect.cpp tools/replay.cpp tools/turn_replay.cpp tools/score_tool.cpp
  tools/solve_tool.cpp tools/predict_tool.cpp tools/diff_weapons.cpp tools/threat_tool.cpp tools/ai_tool.cpp
  tools/lookahead_tool.cpp)
target_compile_options(itb_inspect PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(itb_inspect PRIVATE itb_engine nlohmann_json::nlohmann_json)

# The live driver's engine tool (scripts/live_play.py): JSON solve / predict /
# board, one-shot or as a long-running `serve` process.
add_library(itb_live_lib STATIC tools/live_tool.cpp)
target_include_directories(itb_live_lib PUBLIC tools)
target_compile_options(itb_live_lib PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(itb_live_lib PUBLIC itb_engine nlohmann_json::nlohmann_json)
add_executable(itb_live tools/itb_live.cpp)
target_compile_options(itb_live PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(itb_live PRIVATE itb_live_lib)

# The game's Lua (5.1.5) as a command-line interpreter: runs the bridge
# script harness (tests/bridge) outside the game.
add_executable(itb_lua51 "${lua51_SOURCE_DIR}/src/lua.c")
target_link_libraries(itb_lua51 PRIVATE itb_lua)
target_compile_options(itb_lua51 PRIVATE -w)
# luac -p -l lists a script's global reads and writes (GETGLOBAL/SETGLOBAL).
add_executable(itb_luac51 "${lua51_SOURCE_DIR}/src/luac.c" "${lua51_SOURCE_DIR}/src/print.c")
target_link_libraries(itb_luac51 PRIVATE itb_lua)
target_include_directories(itb_luac51 PRIVATE "${lua51_SOURCE_DIR}/src")
target_compile_options(itb_luac51 PRIVATE -w)

enable_testing()
add_executable(itb_tests
  tests/test_main.cpp
  tests/test_core.cpp
  tests/test_board.cpp
  tests/test_game_data.cpp
  tests/test_recording.cpp
  tests/test_movement.cpp
  tests/test_tile_rules.cpp
  tests/test_lua_host.cpp
  tests/test_executor.cpp
  tests/test_engine.cpp
  tests/test_enemy_phase.cpp
  tests/test_ai_phase.cpp
  tests/test_score.cpp
  tests/test_solver.cpp
  tests/test_threat.cpp
  tests/test_lookahead.cpp
  tests/test_bridge.cpp
  tests/test_live.cpp
  tests/test_weapon_ports.cpp
  tests/test_pilot_xp.cpp
)
target_link_libraries(itb_tests PRIVATE itb_live_lib itb_engine itb_lua doctest::doctest)
target_compile_definitions(itb_tests PRIVATE
  ITB_REPO_ROOT="${ITB_REPO_ROOT}"
  ITB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME itb_tests COMMAND itb_tests)

# Python bindings (module itb_engine), the live bot's solver. Off by default:
#   cmake -S engine -B engine/build -G Ninja -DITB_BUILD_PYTHON=ON \
#     -DPython_EXECUTABLE=$(command -v python3)
# builds engine/build/python/itb_engine.<abi>.so for that interpreter
# (README "Python bindings").
option(ITB_BUILD_PYTHON "Build the itb_engine Python module" OFF)
if(ITB_BUILD_PYTHON)
  set_target_properties(itb_engine itb_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
  find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)
  FetchContent_Declare(pybind11
    URL https://github.com/pybind/pybind11/archive/refs/tags/v2.13.6.tar.gz
    URL_HASH SHA256=e08cb87f4773da97fa7b5f035de8763abc656d87d5773e62f6da0587d1f0ec20
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(pybind11)
  execute_process(COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${ITB_REPO_ROOT}"
    OUTPUT_VARIABLE ITB_BUILD_GIT OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT ITB_BUILD_GIT)
    set(ITB_BUILD_GIT "unknown")
  endif()
  pybind11_add_module(itb_engine_py MODULE python/itb_engine_py.cpp)
  set_target_properties(itb_engine_py PROPERTIES
    OUTPUT_NAME itb_engine
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python")
  target_compile_options(itb_engine_py PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(itb_engine_py PRIVATE ITB_BUILD_GIT="${ITB_BUILD_GIT}")
  target_link_libraries(itb_engine_py PRIVATE itb_engine nlohmann_json::nlohmann_json)
endif()
