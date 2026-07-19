cmake_minimum_required(VERSION 3.15)
project(bmhero_arena C)

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
    tools/viewer/viewer_clock.c)
  target_link_libraries(arena_viewer arena_sim SDL3::SDL3)
  if(NOT MSVC)
    target_link_libraries(arena_viewer m)
  endif()
  message(STATUS "arena_viewer: SDL3 found - building viewer")
else()
  message(STATUS "arena_viewer: SDL3 not found - viewer skipped (sim + tests unaffected)")
endif()
