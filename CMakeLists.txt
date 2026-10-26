# Builds out/bin/wc from the runtime (runtime/) and the recompiled program (out/recomp/, written by
# `npm run recomp`). `npm run build` configures out/build and builds it; by hand:
#   cmake -S . -B out/build -G Ninja && cmake --build out/build
# With Emscripten (`npm run web`: emcmake, out/build-web), it builds the web page instead: wc.js and
# wc.wasm next to web/index.html in out/web.
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

if(EMSCRIPTEN)
  # The emulation runs on a pthread: shared memory, so the whole program is built for it
  add_compile_options(-pthread)
else()
  # The window: SDL3 (brew install sdl3)
  find_package(SDL3 REQUIRED CONFIG)
endif()

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
list(FILTER WC_RUNTIME EXCLUDE REGEX "/host_(sdl|web)\\.c$")
add_executable(wc ${WC_RUNTIME} $<TARGET_OBJECTS:wc_generated>)
target_include_directories(wc PRIVATE runtime "${WC_RECOMP_DIR}")
target_compile_options(wc PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(wc PRIVATE opl3)
set_target_properties(wc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${WC_BIN_DIR}")

if(EMSCRIPTEN)
  target_sources(wc PRIVATE runtime/host_web.c)
  set(WC_IMAGE "${WC_RECOMP_DIR}/image.bin" "${WC_RECOMP_DIR}/relocs.bin")
  target_link_options(wc PRIVATE -pthread -O1
    -sPTHREAD_POOL_SIZE=1 -sINITIAL_MEMORY=128MB -sEXIT_RUNTIME=1
    -sMODULARIZE=1 -sEXPORT_NAME=WingCommander -sENVIRONMENT=web,worker
    -sEXPORTED_RUNTIME_METHODS=FS,ENV,HEAPU8
    "--embed-file=${WC_RECOMP_DIR}/image.bin@/wc/image.bin"
    "--embed-file=${WC_RECOMP_DIR}/relocs.bin@/wc/relocs.bin")
  set_target_properties(wc PROPERTIES SUFFIX ".js" LINK_DEPENDS "${WC_IMAGE}")
  # The page and its service worker, next to wc.js and wc.wasm
  set(WC_DATA)
  foreach(f index.html sw.js)
    add_custom_command(OUTPUT "${WC_BIN_DIR}/${f}"
      COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/web/${f}" "${WC_BIN_DIR}/${f}"
      DEPENDS "${CMAKE_SOURCE_DIR}/web/${f}")
    list(APPEND WC_DATA "${WC_BIN_DIR}/${f}")
  endforeach()
  add_custom_target(wc_page ALL DEPENDS ${WC_DATA})
else()
  target_sources(wc PRIVATE runtime/host_sdl.c)
  target_link_libraries(wc PRIVATE SDL3::SDL3)
  # The program image the runtime loads, found next to the executable
  set(WC_DATA)
  foreach(f image.bin relocs.bin)
    add_custom_command(OUTPUT "${WC_BIN_DIR}/${f}"
      COMMAND "${CMAKE_COMMAND}" -E copy "${WC_RECOMP_DIR}/${f}" "${WC_BIN_DIR}/${f}"
      DEPENDS "${WC_RECOMP_DIR}/${f}")
    list(APPEND WC_DATA "${WC_BIN_DIR}/${f}")
  endforeach()
  add_custom_target(wc_data ALL DEPENDS ${WC_DATA})
endif()
