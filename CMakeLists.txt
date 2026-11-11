# The Raspberry Pi 5 Dolby Vision engine and its bench.
#   engine/   libpi_engine: the planner (CPU) + two Vulkan fragment passes (GPU)
#   bench/    dvbench: decode + the engine, timed (PI_BUILD_BENCH)
# The engine needs Vulkan and FFmpeg's libavutil (upstream: the frames' Dolby
# Vision side data). The bench needs libavformat and libavcodec too, built
# with patches/ffmpeg for the Pi's V4L2 request decoder. glslc is optional:
# without it the committed SPIR-V (engine/shaders/spirv/) is used.
# Licence: GPL-2.0-or-later (LICENSE), as Kodi; the planner's notices are in
# engine/planner/LICENSE. FFmpeg here is built --enable-gpl --enable-version3,
# so binaries linking it (dvbench, the bundle) are GPLv3 as a whole.
cmake_minimum_required(VERSION 3.22)
project(pi-dv-engine VERSION 0.4.1 LANGUAGES C)
option(PI_BUILD_BENCH "Build dvbench (needs libdrm)" OFF)
option(PI_SHARED "Build libpi_engine as a shared library (else static, with libdv_planner)" ON)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_program(GLSLC glslc)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
add_subdirectory(engine)
if(PI_BUILD_BENCH)
  pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
  add_subdirectory(bench)
endif()
