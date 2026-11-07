# The Raspberry Pi 5 Dolby Vision engine and its bench.
#   engine/   libpi_engine: the planner (CPU) + two Vulkan fragment passes (GPU)
#   bench/    dvbench: decode + the engine, timed (PI_BUILD_BENCH)
# Needs FFmpeg with patches/ffmpeg (its Dolby Vision metadata, the Pi's V4L2
# request decoder) and Vulkan. glslc is optional: without it the committed
# SPIR-V (engine/shaders/spirv/) is used.
# Licence: GPL-3.0-or-later (LICENSE), the planner's terms: a program linking
# the engine is GPLv3 as a whole (Kodi, GPL-2.0-or-later, allows that).
cmake_minimum_required(VERSION 3.22)
project(rpi5-dv C)
option(PI_BUILD_BENCH "Build dvbench (needs libdrm)" OFF)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_program(GLSLC glslc)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
add_subdirectory(engine)
if(PI_BUILD_BENCH)
  add_subdirectory(bench)
endif()
