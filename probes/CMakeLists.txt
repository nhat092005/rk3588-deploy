# Board hardware probes. Switches: cmake/build_config.cmake.
#
# * Cross-compile only: they link the board libraries.
# * SDK targets rockchip::rknn, rockchip::rga, rockchip::mpp: cmake/RockchipSdk.cmake.
# * Optional: -DRKNN_API_INCLUDE_DIR=<dir containing rknn_api.h, version 2.3.2>.

# add_probe(<dir> <name> [LIBS <target>...]): build <dir>/<name>.cpp as an executable linked with
# the logger and LIBS. The executable lands in <build>/probes/<dir>/.
function(add_probe dir name)
    cmake_parse_arguments(
        PROBE
        ""
        ""
        "LIBS"
        ${ARGN}
    )

    # probe_link -> BUILD_PROBE_LINK
    string(REGEX REPLACE "^probe_" "" short ${name})
    string(TOUPPER ${short} switch)
    set(switch BUILD_PROBE_${switch})
    if(NOT DEFINED ${switch})
        message(FATAL_ERROR "${name}: add set(${switch} ON) to cmake/build_config.cmake")
    endif()
    if(NOT ${switch})
        message(STATUS "skip ${name}: ${switch} is OFF")
        return()
    endif()

    add_executable(${name} ${dir}/${name}.cpp)
    target_link_libraries(${name} PRIVATE logger_lib ${PROBE_LIBS})
    set_target_properties(
        ${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${dir}"
    )
endfunction()

add_probe(
    device
    probe_link
    LIBS
    rockchip::rknn
    rockchip::rga
    rockchip::mpp
)
add_probe(
    device
    probe_infer
    LIBS
    rockchip::rknn
    process_lib
)
add_probe(
    device
    probe_fill_heap
    LIBS
    dmabuf_lib
    rga_lib
)
add_probe(
    device
    probe_camera
    LIBS
    camera_lib
    process_lib
)
add_probe(
    stage
    probe_letterbox
    LIBS
    camera_lib
    rga_lib
    process_lib
)
add_probe(
    stage
    probe_jpeg
    LIBS
    camera_lib
    rga_lib
    jpeg_lib
    store_lib
    process_lib
)
add_probe(
    stage
    probe_overlay
    LIBS
    camera_lib
    rga_lib
    jpeg_lib
    process_lib
)
add_probe(
    stage
    probe_h265
    LIBS
    camera_lib
    rga_lib
    video_lib
    process_lib
)
add_probe(
    device
    probe_dmabuf
    LIBS
    rockchip::rknn
    rockchip::rga
    rockchip::mpp
    dmabuf_lib
    process_lib
)
add_probe(
    pipeline
    probe_threads
    LIBS
    camera_lib
    rga_lib
    npu_lib
    video_lib
    jpeg_lib
    stats_lib
    process_lib
)
add_probe(
    pipeline
    probe_output
    LIBS
    camera_lib
    rga_lib
    pipeline_lib
    options_lib
    process_lib
)
add_probe(
    pipeline
    probe_disk_write
    LIBS
    process_lib
    os_lib
)
add_probe(
    stage
    probe_tracker
    LIBS
    tracker_lib
    process_lib
)
