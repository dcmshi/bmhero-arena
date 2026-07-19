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

add_executable(test_bomb_mechanics tests/test_bomb_mechanics.c)
target_link_libraries(test_bomb_mechanics arena_sim)
if(NOT MSVC)
  target_compile_options(test_bomb_mechanics PRIVATE -Wconversion -Wno-sign-conversion)
endif()
add_test(NAME bomb_mechanics COMMAND test_bomb_mechanics)

# --- netplay: GekkoNet (BSD-2, C API, C++ internals) via FetchContent ---
include(FetchContent)
FetchContent_Declare(gekkonet
  GIT_REPOSITORY https://github.com/HeatXD/GekkoNet.git
  GIT_TAG        v20260629200724-02c447c
  SOURCE_SUBDIR  GekkoLib)          # library only; skips SDL3 examples
FetchContent_MakeAvailable(gekkonet)

add_library(arena_netplay STATIC src/netplay/sync_session.c)
target_include_directories(arena_netplay PUBLIC
  src src/netplay ${gekkonet_SOURCE_DIR}/GekkoLib/include)
target_compile_definitions(arena_netplay PUBLIC GEKKONET_STATIC)
target_link_libraries(arena_netplay PUBLIC arena_sim GekkoNet)
if(WIN32)
  target_link_libraries(arena_netplay PUBLIC ws2_32)
endif()

add_executable(test_netplay_link tests/test_netplay_link.c)
target_link_libraries(test_netplay_link arena_netplay)
add_test(NAME netplay_link COMMAND test_netplay_link)

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
  target_link_libraries(arena_viewer arena_sim SDL3::SDL3)
  if(NOT MSVC)
    target_link_libraries(arena_viewer m)
    target_link_options(arena_viewer PRIVATE -static-libgcc)
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
