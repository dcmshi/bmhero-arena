cmake_minimum_required(VERSION 3.15)
project(bmhero_arena C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(arena_sim STATIC src/arena/arena_sim.c)
target_include_directories(arena_sim PUBLIC src)
if(NOT MSVC)
  # determinism bugs love to hide in conversion warnings — sim stays strictest
  target_compile_options(arena_sim PRIVATE -Wconversion -Wno-sign-conversion)
endif()

enable_testing()
add_executable(test_determinism tests/test_determinism.c)
target_link_libraries(test_determinism arena_sim)
if(NOT MSVC)
  target_compile_options(test_determinism PRIVATE -Wconversion -Wno-sign-conversion)
endif()
add_test(NAME determinism COMMAND test_determinism)

# Movement model tests. These previously ran ONLY in determinism.yml and were
# absent from ctest entirely - so a local `ctest` run silently skipped them.
add_executable(test_movement tests/test_movement.c)
target_link_libraries(test_movement arena_sim)
if(NOT MSVC)
  target_compile_options(test_movement PRIVATE -Wconversion -Wno-sign-conversion)
endif()
add_test(NAME movement COMMAND test_movement)

add_executable(test_bomb_mechanics tests/test_bomb_mechanics.c)
target_link_libraries(test_bomb_mechanics arena_sim)
if(NOT MSVC)
  target_compile_options(test_bomb_mechanics PRIVATE -Wconversion -Wno-sign-conversion)
endif()
add_test(NAME bomb_mechanics COMMAND test_bomb_mechanics)

# Round lifecycle + turn snap. Guards the 2026-07-27 feel-test bugs: a
# terminal PHASE_ROUND_END froze the player for good, and the turn swept from
# a standstill where the real walker snaps.
add_executable(test_round tests/test_round.c)
target_link_libraries(test_round arena_sim)
if(NOT MSVC)
  target_compile_options(test_round PRIVATE -Wconversion -Wno-sign-conversion)
endif()
add_test(NAME round_turn COMMAND test_round)

# Feel-metrics probes. tune_probes.c is compiled INTO the test target rather
# than linked from arena_sim: it is tools-side code that uses floats, and
# arena_sim carries -Wconversion + the no-floats invariant.
add_executable(test_tune_report tests/test_tune_report.c tools/tune_probes.c)
target_link_libraries(test_tune_report arena_sim)
add_test(NAME tune_report COMMAND test_tune_report)

# The tuning report itself (dev tool, not a test): one tune in, one table out.
add_executable(tune_report tools/tune_report.c tools/tune_probes.c)
target_link_libraries(tune_report arena_sim)

# The scripted-match hash generator, pinned by tools/pinned_hash.txt.
add_executable(arena_hash tools/arena_hash.c)
target_link_libraries(arena_hash arena_sim)

# Per-tick trace of that same scripted match - turns an opaque hash change into
# "diverges at tick N, in this field" (see tools/trace-diff.ps1).
add_executable(arena_trace tools/arena_trace.c)
target_link_libraries(arena_trace arena_sim)

# --- A3 lobby/rendezvous protocol (pure C, no sockets in this lib) ---
add_library(arena_lobby STATIC src/lobby/lobby_proto.c src/lobby/rendezvous.c)
target_include_directories(arena_lobby PUBLIC src src/lobby)
target_link_libraries(arena_lobby PUBLIC arena_sim)   # net_version replays the scripted match

add_executable(test_lobby_proto tests/test_lobby_proto.c)
target_link_libraries(test_lobby_proto arena_lobby)
add_test(NAME lobby_proto COMMAND test_lobby_proto)

add_executable(test_rendezvous tests/test_rendezvous.c)
target_link_libraries(test_rendezvous arena_lobby)
add_test(NAME rendezvous COMMAND test_rendezvous)

# --- A3 sockets + rendezvous binary ---
add_library(arena_udp STATIC src/netplay/udp_socket.c)
target_include_directories(arena_udp PUBLIC src src/netplay src/lobby)
if(WIN32)
  target_link_libraries(arena_udp PUBLIC ws2_32)
endif()

add_executable(test_udp_socket tests/test_udp_socket.c)
target_link_libraries(test_udp_socket arena_udp)
add_test(NAME udp_socket COMMAND test_udp_socket)

add_executable(arena_rendezvous src/lobby/rendezvous_main.c)
target_link_libraries(arena_rendezvous arena_lobby arena_udp)

# --- netplay: GekkoNet (BSD-2, C API, C++ internals) via FetchContent ---
include(FetchContent)
FetchContent_Declare(gekkonet
  GIT_REPOSITORY https://github.com/HeatXD/GekkoNet.git
  GIT_TAG        v20260629200724-02c447c
  SOURCE_SUBDIR  GekkoLib)          # library only; skips SDL3 examples
FetchContent_MakeAvailable(gekkonet)

add_library(arena_netplay STATIC src/netplay/sync_session.c src/netplay/net_adapter.c
                                src/netplay/lobby_client.c)
target_include_directories(arena_netplay PUBLIC
  src src/netplay ${gekkonet_SOURCE_DIR}/GekkoLib/include)
target_compile_definitions(arena_netplay PUBLIC GEKKONET_STATIC)
target_link_libraries(arena_netplay PUBLIC arena_sim arena_lobby arena_udp GekkoNet)
if(WIN32)
  target_link_libraries(arena_netplay PUBLIC ws2_32)
endif()

add_executable(test_net_adapter tests/test_net_adapter.c)
target_link_libraries(test_net_adapter arena_netplay)
add_test(NAME net_adapter COMMAND test_net_adapter)

add_executable(test_netplay_link tests/test_netplay_link.c)
target_link_libraries(test_netplay_link arena_netplay)
add_test(NAME netplay_link COMMAND test_netplay_link)

add_executable(test_netplay_stress tests/test_netplay_stress.c)
target_link_libraries(test_netplay_stress arena_netplay)
add_test(NAME netplay_stress COMMAND test_netplay_stress)

add_executable(test_netplay_p2p tests/test_netplay_p2p.c)
target_link_libraries(test_netplay_p2p arena_netplay)

add_executable(test_netplay_mesh tests/test_netplay_mesh.c)
target_link_libraries(test_netplay_mesh arena_netplay)

find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM)
  add_test(NAME netplay_p2p
           COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_p2p_test.sh
                   $<TARGET_FILE:test_netplay_p2p>)
  set_tests_properties(netplay_p2p PROPERTIES TIMEOUT 120)

  # The milestone gate: four processes, a real rendezvous, real punching.
  add_test(NAME netplay_mesh_direct
           COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mesh_test.sh
                   $<TARGET_FILE:test_netplay_mesh> $<TARGET_FILE:arena_rendezvous> direct)
  set_tests_properties(netplay_mesh_direct PROPERTIES TIMEOUT 180)
else()
  message(STATUS "bash not found - netplay_p2p/netplay_mesh tests skipped")
endif()

# --- viewer pure-logic modules (no SDL; always built and tested) ---
add_executable(test_viewer_clock tests/test_viewer_clock.c tools/viewer/viewer_clock.c)
add_test(NAME viewer_clock COMMAND test_viewer_clock)

add_executable(test_viewer_cam tests/test_viewer_cam.c tools/viewer/viewer_cam.c)
if(NOT MSVC)
  target_link_libraries(test_viewer_cam m)
endif()
add_test(NAME viewer_cam COMMAND test_viewer_cam)

# --- debug viewer (dev tool; requires SDL3, skipped when absent) ---
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_executable(arena_viewer
    tools/viewer/viewer_main.c
    tools/viewer/viewer_cam.c
    tools/viewer/viewer_clock.c
    tools/viewer/viewer_draw.c)
  target_link_libraries(arena_viewer arena_netplay SDL3::SDL3)
  if(NOT MSVC)
    target_link_libraries(arena_viewer m)
    # -static: MinGW runtimes (libgcc/libstdc++/winpthread) go static so the
    # exe runs outside the MSYS2 shell; SDL3 stays a DLL (import lib) and is
    # copied beside the exe below.
    target_link_options(arena_viewer PRIVATE -static)
  endif()
  if(WIN32)
    # copy SDL3.dll (and friends) beside the exe so it runs outside the
    # MSYS2 shell / by double-click
    add_custom_command(TARGET arena_viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:arena_viewer> $<TARGET_FILE_DIR:arena_viewer>
      COMMAND_EXPAND_LISTS)
  endif()
  message(STATUS "arena_viewer: SDL3 found - building viewer")
else()
  message(STATUS "arena_viewer: SDL3 not found - viewer skipped (sim + tests unaffected)")
endif()
