# The Raspberry Pi 5 Dolby Vision engine and its tools.
#   engine/   the engine: planner (CPU) + two Vulkan fragment passes (GPU)
#   bench/    dvbench (decode + the engine, timed), vk-micro, probe-dmabuf
# Needs FFmpeg with patches/ffmpeg (its Dolby Vision metadata, the Pi's V4L2
# request decoder), Vulkan, glslc; vk-micro also EGL / GLES.
cmake_minimum_required(VERSION 3.22)
project(rpi5-dv C)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_program(GLSLC glslc REQUIRED)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
add_subdirectory(engine)
add_subdirectory(bench)
