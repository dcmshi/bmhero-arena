cmake_minimum_required(VERSION 3.15)
project(bmhero_arena C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The sim must build warning-clean; determinism bugs love to hide in warnings.
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wconversion -Wno-sign-conversion)
endif()

add_library(arena_sim STATIC src/arena/arena_sim.c)
target_include_directories(arena_sim PUBLIC src)

enable_testing()
add_executable(test_determinism tests/test_determinism.c)
target_link_libraries(test_determinism arena_sim)
add_test(NAME determinism COMMAND test_determinism)

# --- viewer pure-logic modules (no SDL; always built and tested) ---
add_executable(test_viewer_clock tests/test_viewer_clock.c tools/viewer/viewer_clock.c)
add_test(NAME viewer_clock COMMAND test_viewer_clock)

add_executable(test_viewer_cam tests/test_viewer_cam.c tools/viewer/viewer_cam.c)
if(NOT MSVC)
  target_link_libraries(test_viewer_cam m)
endif()
add_test(NAME viewer_cam COMMAND test_viewer_cam)
