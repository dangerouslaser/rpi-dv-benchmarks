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
target_include_directories(dv_planner PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/planner>)
target_link_libraries(dv_planner PRIVATE PkgConfig::AV m)

# The shaders as SPIR-V, built in as C arrays (shaders/<name>.h). With glslc
# compiled here; without, the committed binaries (shaders/spirv/*.spv,
# refreshed by the `update-spirv` target). The build warns when those are
# stale.
set(SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(PI_GLSLC_FLAGS "-O" CACHE STRING "glslc's optimisation flags for the shaders")
separate_arguments(GLSLC_OPT UNIX_COMMAND "${PI_GLSLC_FLAGS}")
set(SPIRV_DIR ${CMAKE_CURRENT_BINARY_DIR}/spirv)
file(MAKE_DIRECTORY ${SPIRV_DIR}/shaders)
set(SPIRV)
set(UPDATE)
if(NOT GLSLC)
  message(STATUS "glslc not found: using the committed SPIR-V (engine/shaders/spirv)")
endif()
foreach(shader fullscreen.vert p1_chroma.frag p2_pack.frag)
  set(committed ${SHADERS}/spirv/${shader}.spv)
  set(header ${SPIRV_DIR}/shaders/${shader}.h)
  if(GLSLC)
    set(spv ${SPIRV_DIR}/${shader}.spv)
    add_custom_command(OUTPUT ${spv}
      COMMAND ${GLSLC} --target-env=vulkan1.2 ${GLSLC_OPT} -Werror -I${SHADERS} ${SHADERS}/${shader} -o ${spv}
      COMMAND ${CMAKE_COMMAND} -E compare_files ${spv} ${committed} ||
              ${CMAKE_COMMAND} -E echo "warning: engine/shaders/spirv/${shader}.spv is stale (target update-spirv)"
      DEPENDS ${SHADERS}/${shader} ${SHADERS}/common.glsl ${SHADERS}/piecewise.glsl ${SHADERS}/pack.h VERBATIM)
    list(APPEND UPDATE COMMAND ${CMAKE_COMMAND} -E copy ${spv} ${committed})
  else()
    set(spv ${committed})
  endif()
  add_custom_command(OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${header} -P ${SHADERS}/embed.cmake
    DEPENDS ${spv} ${SHADERS}/embed.cmake VERBATIM)
  list(APPEND SPIRV ${header})
endforeach()
if(GLSLC)
  add_custom_target(update-spirv ${UPDATE} DEPENDS ${SPIRV} VERBATIM)
endif()

add_library(pi_engine STATIC pi_engine.c pi_input.c ${SPIRV})
set_target_properties(pi_engine PROPERTIES C_STANDARD 11 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON
                                           PUBLIC_HEADER pi_engine.h)
target_compile_options(pi_engine PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
                                         -Wno-format-truncation)
target_include_directories(pi_engine PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
                                     PRIVATE ${SPIRV_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pi_engine PUBLIC dv_planner vulkan PkgConfig::AV Threads::Threads m)

install(TARGETS pi_engine dv_planner EXPORT pi_engine
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/pi_engine)
