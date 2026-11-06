# dvbench: CB1 (static, from CroqueMr's tree) and yblod's Vulkan engine
# (dlopened) on the same DRM PRIME pictures.
cmake_minimum_required(VERSION 3.22)
project(dvbench C)
set(CB1_SOURCE_DIR "" CACHE PATH "CB1/src of CroqueMr's libreelec-x86-DV")
set(DVVK_INCLUDE_DIR "" CACHE PATH "yblod-engine/vk/include")
add_subdirectory(${CB1_SOURCE_DIR} cb1)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil egl glesv2 libdrm libplacebo)
# The Pi FEL engine (pi_engine.c): its fragment shaders as SPIR-V headers,
# and yblod's planner (the CPU half) built as dv-engine-vk's build.rs builds it.
find_program(GLSLC glslc REQUIRED)
set(PI_SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/pi)
set(PI_OUT ${CMAKE_CURRENT_BINARY_DIR}/pi)
file(MAKE_DIRECTORY ${PI_OUT})
set(PI_HEADERS)
foreach(shader fullscreen.vert p1_chroma.frag p3_pack.frag)
  add_custom_command(OUTPUT ${PI_OUT}/${shader}.h
    COMMAND ${GLSLC} --target-env=vulkan1.2 -O -Werror -mfmt=c -I${PI_SHADERS} ${PI_SHADERS}/${shader} -o ${PI_OUT}/${shader}.h
    DEPENDS ${PI_SHADERS}/${shader} ${PI_SHADERS}/common.glsl VERBATIM)
  list(APPEND PI_HEADERS ${PI_OUT}/${shader}.h)
endforeach()
set(YBLOD_ENGINE_DIR ${DVVK_INCLUDE_DIR}/../..)
set(YBLOD_SRC ${YBLOD_ENGINE_DIR}/src)
add_library(yblod_plan STATIC
  ${YBLOD_ENGINE_DIR}/vk/yblod/plan.c ${YBLOD_ENGINE_DIR}/vk/yblod/cb1_overlay.c ${YBLOD_ENGINE_DIR}/vk/yblod/dm_cm4.c
  ${YBLOD_SRC}/dv_ffmpeg_metadata.c ${YBLOD_SRC}/dv_cm4.c ${YBLOD_SRC}/single_reshape.c
  ${YBLOD_SRC}/single_reshape_ffmpeg.c ${YBLOD_SRC}/mmr_prepare.c ${YBLOD_SRC}/pixel_luma.c
  ${YBLOD_SRC}/pixel_chroma_final.c ${YBLOD_SRC}/pixel_spatial.c ${YBLOD_SRC}/source_geometry.c
  ${YBLOD_SRC}/source_overlay.c ${YBLOD_SRC}/pixel_colour.c ${YBLOD_SRC}/colour_lut.c)
set_target_properties(yblod_plan PROPERTIES C_STANDARD 11 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(yblod_plan PRIVATE DV_FFMPEG_RAW_EXTENSIONS DV_PREPARED_CHROMA DV_ENCODE_STEPS=256 DV_COLOUR_LUT)
target_compile_options(yblod_plan PRIVATE -O2 -ffp-contract=off -w)
target_include_directories(yblod_plan PRIVATE ${YBLOD_SRC} ${DVVK_INCLUDE_DIR})
target_link_libraries(yblod_plan PRIVATE PkgConfig::DEPS)

add_executable(dvbench dvbench.c source.c alloc.c pi_engine.c ${PI_HEADERS})
set_target_properties(dvbench PROPERTIES C_STANDARD 11 BUILD_RPATH_USE_ORIGIN ON INSTALL_RPATH "$ORIGIN/../lib")
target_compile_options(dvbench PRIVATE -O2 -Wall -Wno-unused-parameter)
target_include_directories(dvbench PRIVATE ${DVVK_INCLUDE_DIR} ${CB1_SOURCE_DIR})
find_package(Threads REQUIRED)
target_include_directories(dvbench PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(dvbench PRIVATE dvbridge_core yblod_plan vulkan PkgConfig::DEPS Threads::Threads ${CMAKE_DL_LIBS} m)
add_executable(probe-dmabuf probe-dmabuf.c)
target_link_libraries(probe-dmabuf PRIVATE PkgConfig::DEPS vulkan)
add_executable(vk-compile vk-compile.c)
target_include_directories(vk-compile PRIVATE ${DVVK_INCLUDE_DIR})
target_link_libraries(vk-compile PRIVATE ${CMAKE_DL_LIBS})
install(TARGETS dvbench probe-dmabuf vk-compile RUNTIME DESTINATION bin)

# vk-micro: one synthetic 4K pass as Vulkan compute (buffers / images),
# Vulkan fragment, GLES fragment and GLES compute (is it Vulkan, or the design?)
find_program(GLSLC glslc REQUIRED)
set(MICRO ${CMAKE_CURRENT_SOURCE_DIR}/micro)
set(MICRO_OUT ${CMAKE_CURRENT_BINARY_DIR}/micro)
file(MAKE_DIRECTORY ${MICRO_OUT})
set(MICRO_HEADERS)
foreach(spec "comp_buf_buf micro.comp -DIN_BUF -DOUT_BUF" "comp_tex_buf micro.comp -DIN_TEX -DOUT_BUF"
             "comp_tex_img micro.comp -DIN_TEX -DOUT_IMG" "frag_buf micro.frag -DIN_BUF"
             "frag_tex micro.frag -DIN_TEX" "vert micro.vert")
  separate_arguments(args UNIX_COMMAND "${spec}")
  list(POP_FRONT args name src)
  add_custom_command(OUTPUT ${MICRO_OUT}/${name}.h
    COMMAND ${GLSLC} --target-env=vulkan1.1 -O -mfmt=c -I${MICRO} ${args} ${MICRO}/${src} -o ${MICRO_OUT}/${name}.h
    DEPENDS ${MICRO}/${src} ${MICRO}/core.glsl ${MICRO}/fetch.glsl VERBATIM)
  list(APPEND MICRO_HEADERS ${MICRO_OUT}/${name}.h)
endforeach()
add_custom_command(OUTPUT ${MICRO_OUT}/core_glsl.h
  COMMAND ${CMAKE_COMMAND} -DIN=${MICRO}/core.glsl -DOUT=${MICRO_OUT}/core_glsl.h -DNAME=core_glsl -P ${MICRO}/embed.cmake
  DEPENDS ${MICRO}/core.glsl ${MICRO}/embed.cmake VERBATIM)
add_executable(vk-micro vk-micro.c ${MICRO_HEADERS} ${MICRO_OUT}/core_glsl.h)
target_include_directories(vk-micro PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(vk-micro PRIVATE -O2 -Wall)
target_link_libraries(vk-micro PRIVATE vulkan PkgConfig::DEPS)
install(TARGETS vk-micro RUNTIME DESTINATION bin)
