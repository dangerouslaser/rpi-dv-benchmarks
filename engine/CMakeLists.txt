# The planner (vendored from yblod: planner/README.md) and the Pi engine:
# libpi_engine.a, its fragment shaders compiled in as SPIR-V.
add_library(dv_planner STATIC
  planner/plan.c planner/cb1_overlay.c planner/dm_cm4.c planner/dv_ffmpeg_metadata.c planner/dv_cm4.c
  planner/single_reshape.c planner/single_reshape_ffmpeg.c planner/mmr_prepare.c planner/pixel_luma.c
  planner/pixel_chroma_final.c planner/pixel_spatial.c planner/source_geometry.c planner/source_overlay.c
  planner/pixel_colour.c planner/colour_lut.c)
set_target_properties(dv_planner PROPERTIES C_STANDARD 11 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(dv_planner PRIVATE DV_FFMPEG_RAW_EXTENSIONS DV_PREPARED_CHROMA DV_ENCODE_STEPS=256 DV_COLOUR_LUT)
target_compile_options(dv_planner PRIVATE -O2 -ffp-contract=off -w)
target_include_directories(dv_planner PUBLIC planner)
target_link_libraries(dv_planner PRIVATE PkgConfig::AV m)

set(SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADERS_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADERS_OUT})
set(SPIRV)
foreach(shader fullscreen.vert p1_chroma.frag p2_pack.frag)
  add_custom_command(OUTPUT ${SHADERS_OUT}/${shader}.h
    COMMAND ${GLSLC} --target-env=vulkan1.2 -O -Werror -mfmt=c -I${SHADERS} ${SHADERS}/${shader} -o ${SHADERS_OUT}/${shader}.h
    DEPENDS ${SHADERS}/${shader} ${SHADERS}/common.glsl ${SHADERS}/piecewise.glsl VERBATIM)
  list(APPEND SPIRV ${SHADERS_OUT}/${shader}.h)
endforeach()

add_library(pi_engine STATIC pi_engine.c ${SPIRV})
set_target_properties(pi_engine PROPERTIES C_STANDARD 11 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
target_compile_options(pi_engine PRIVATE -O2 -Wall -Wno-format-truncation)
target_include_directories(pi_engine PUBLIC . PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(pi_engine PUBLIC dv_planner vulkan PkgConfig::AV m)
