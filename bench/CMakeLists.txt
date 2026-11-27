# Benches of the hardware accelerators against the CPU. Switches: cmake/build_config.cmake.
#
# * Cross-compile only: they link the board libraries and run on the board.
# * OpenCV comes from the board sysroot (4.5.4 on Ubuntu 22.04), the CPU side of the comparison.
# * Layout like probes/ and library/: common/ is shared, stage/ has one bench per pipeline stage,
#   stage/<name>/ holds the library of that bench.

# The bench reads OpenCV headers that throw: the project flags -fno-exceptions -fno-rtti compile
# them (checked), and a throw inside the library aborts.
find_package(OpenCV REQUIRED COMPONENTS core imgproc)

# add_bench(<dir> <name> [LIBS <target>...]): build <dir>/<name>.cpp as an executable linked with
# the logger, the command line reader and LIBS. The executable lands in <build>/bench/<dir>/.
function(add_bench dir name)
    cmake_parse_arguments(
        BENCH
        ""
        ""
        "LIBS"
        ${ARGN}
    )

    # bench_preprocess -> BUILD_BENCH_PREPROCESS
    string(REGEX REPLACE "^bench_" "" short ${name})
    string(TOUPPER ${short} switch)
    set(switch BUILD_BENCH_${switch})
    if(NOT DEFINED ${switch})
        message(FATAL_ERROR "${name}: add set(${switch} ON) to cmake/build_config.cmake")
    endif()
    if(NOT ${switch})
        message(STATUS "skip ${name}: ${switch} is OFF")
        return()
    endif()

    add_executable(${name} ${dir}/${name}.cpp)
    target_compile_features(${name} PUBLIC cxx_std_17)
    target_link_libraries(${name} PRIVATE logger_lib process_lib ${BENCH_LIBS})
    set_target_properties(
        ${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${dir}"
    )

    # bin/bench/<dir>/<name> on the board.
    install(TARGETS ${name} RUNTIME DESTINATION bin/bench/${dir})
endfunction()

add_subdirectory(common)
add_subdirectory(stage/preprocess)
add_subdirectory(stage/draw)
add_subdirectory(stage/jpeg)

add_bench(
    stage
    bench_preprocess
    LIBS
    bench_timing_lib
    bench_frame_lib
    bench_dump_lib
    bench_json_lib
    bench_platform_lib
    bench_npu_load_lib
    bench_preprocess_lib
)

add_bench(
    stage
    bench_draw
    LIBS
    bench_timing_lib
    bench_frame_lib
    bench_dump_lib
    bench_json_lib
    bench_platform_lib
    bench_draw_lib
)

add_bench(
    stage
    bench_encode
    LIBS
    bench_timing_lib
    bench_json_lib
    bench_platform_lib
    video_lib
    mpp_lib
    dmabuf_lib
)

add_bench(
    stage
    bench_jpeg
    LIBS
    bench_timing_lib
    bench_json_lib
    bench_platform_lib
    bench_jpeg_lib
    jpeg_lib
    mpp_lib
    dmabuf_lib
)

add_bench(
    stage
    bench_infer
    LIBS
    bench_timing_lib
    bench_json_lib
    bench_platform_lib
    npu_lib
    dmabuf_lib
)
