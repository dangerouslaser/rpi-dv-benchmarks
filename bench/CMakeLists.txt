# dvbench: the engine on decoded pictures (timing, throughput, real-time
# playback, output hashes). vk-micro: one synthetic 4K pass as Vulkan compute,
# Vulkan fragment, GLES fragment and GLES compute. probe-dmabuf: Vulkan's
# dmabuf import, checked.
pkg_check_modules(BENCH REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GL REQUIRED IMPORTED_TARGET egl glesv2)
add_executable(dvbench dvbench.c source.c alloc.c)
set_target_properties(dvbench PROPERTIES C_STANDARD 11 BUILD_RPATH_USE_ORIGIN ON INSTALL_RPATH "$ORIGIN/../lib")
target_compile_options(dvbench PRIVATE -O2 -Wall -Wno-unused-parameter)
target_link_libraries(dvbench PRIVATE pi_engine PkgConfig::AV PkgConfig::BENCH Threads::Threads m)

add_executable(probe-dmabuf probe-dmabuf.c)
target_link_libraries(probe-dmabuf PRIVATE PkgConfig::AV PkgConfig::BENCH PkgConfig::GL vulkan)

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
target_link_libraries(vk-micro PRIVATE vulkan PkgConfig::GL)

install(TARGETS dvbench probe-dmabuf vk-micro RUNTIME DESTINATION bin)
