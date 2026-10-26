# Builds out/bin/wc from the runtime (runtime/) and the recompiled program (out/recomp/, written by
# `npm run recomp`). `npm run build` configures out/build and builds it; by hand:
#   cmake -S . -B out/build -G Ninja && cmake --build out/build
cmake_minimum_required(VERSION 3.24)
project(wc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(WC_RECOMP_DIR "${CMAKE_SOURCE_DIR}/out/recomp" CACHE PATH "The generated C (npm run recomp)")
set(WC_BIN_DIR "${CMAKE_SOURCE_DIR}/out/bin" CACHE PATH "Where wc goes, next to image.bin and relocs.bin")

# The window: SDL3 (brew install sdl3)
find_package(SDL3 REQUIRED CONFIG)

# AdLib: Nuked-OPL3 (git submodule)
add_library(opl3 STATIC third_party/Nuked-OPL3/opl3.c)
target_include_directories(opl3 PUBLIC third_party/Nuked-OPL3)
target_compile_options(opl3 PRIVATE -w)

# The recompiled program and drivers. The generated code has unused labels and variables by
# design; -O1 keeps its huge functions quick to compile.
file(GLOB WC_GENERATED CONFIGURE_DEPENDS "${WC_RECOMP_DIR}/*.c")
if(NOT WC_GENERATED)
  message(FATAL_ERROR "no generated C in ${WC_RECOMP_DIR}: run npm run recomp first")
endif()
add_library(wc_generated OBJECT ${WC_GENERATED})
target_include_directories(wc_generated PRIVATE runtime "${WC_RECOMP_DIR}")
target_compile_options(wc_generated PRIVATE -w -O1)

file(GLOB WC_RUNTIME CONFIGURE_DEPENDS runtime/*.c)
add_executable(wc ${WC_RUNTIME} $<TARGET_OBJECTS:wc_generated>)
target_include_directories(wc PRIVATE runtime "${WC_RECOMP_DIR}")
target_compile_options(wc PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(wc PRIVATE opl3 SDL3::SDL3)
set_target_properties(wc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${WC_BIN_DIR}")

# The program image the runtime loads, found next to the executable
set(WC_DATA)
foreach(f image.bin relocs.bin)
  add_custom_command(OUTPUT "${WC_BIN_DIR}/${f}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${WC_RECOMP_DIR}/${f}" "${WC_BIN_DIR}/${f}"
    DEPENDS "${WC_RECOMP_DIR}/${f}")
  list(APPEND WC_DATA "${WC_BIN_DIR}/${f}")
endforeach()
add_custom_target(wc_data ALL DEPENDS ${WC_DATA})
