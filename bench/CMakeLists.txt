# dvbench: the engine on decoded pictures (timing, throughput, real-time
# playback, output hashes, --probe for the dmabuf allocation and import).
pkg_check_modules(BENCH REQUIRED IMPORTED_TARGET libdrm)
add_executable(dvbench dvbench.c source.c alloc.c)
set_target_properties(dvbench PROPERTIES C_STANDARD 11 BUILD_RPATH_USE_ORIGIN ON INSTALL_RPATH "$ORIGIN/../lib")
target_compile_options(dvbench PRIVATE -O2 -Wall -Wno-unused-parameter)
target_link_libraries(dvbench PRIVATE pi_engine dv_planner PkgConfig::AV PkgConfig::BENCH Threads::Threads m)
install(TARGETS dvbench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
