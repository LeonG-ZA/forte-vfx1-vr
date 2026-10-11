# CMake build, offered alongside the plain Makefile. Both produce the same
# artefacts; neither pulls in a dependency beyond libm.
cmake_minimum_required(VERSION 3.13)
project(vfxbridge C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
                      -Wmissing-prototypes -Wconversion -Wno-sign-conversion)
endif()

# --- the transport-neutral bridge -------------------------------------------
add_library(vfxbridge STATIC
  src/bridge/vfxbridge.c
  src/bridge/stereo.c
  src/bridge/reproject.c
  src/bridge/vfxmath.c
  src/bridge/mousemap.c)
target_include_directories(vfxbridge PUBLIC include PRIVATE src)
if(NOT MSVC)
  target_link_libraries(vfxbridge PUBLIC m)
endif()

# --- the browser wire, kept out of the bridge on purpose --------------------
add_library(wsproto STATIC src/wsproto/wsproto.c)
target_include_directories(wsproto PUBLIC src)

# --- the stub emulator core --------------------------------------------------
add_library(vfxstubcore STATIC src/core_stub/stub_core.c)
target_include_directories(vfxstubcore PUBLIC include src)
target_link_libraries(vfxstubcore PUBLIC vfxbridge)

# --- the headless reference adapter ------------------------------------------
add_executable(vfx-headless
  src/adapters/headless/main.c
  src/adapters/headless/png_write.c)
target_link_libraries(vfx-headless PRIVATE vfxstubcore vfxbridge)

# --- tests -------------------------------------------------------------------
enable_testing()
foreach(t math stereo reproject mousemap wsproto contract)
  add_executable(test_${t} tests/test_${t}.c)
  target_include_directories(test_${t} PRIVATE tests)
  target_link_libraries(test_${t} PRIVATE vfxstubcore vfxbridge wsproto)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

# The layering rule is a test, not a review convention.
find_program(SH_EXECUTABLE sh)
if(SH_EXECUTABLE)
  add_test(NAME layering
           COMMAND ${SH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_layering.sh)
endif()
