# Board hardware probes. Off by default because they need headers that are not in this repo yet.
# Build: cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DBUILD_PROBES=ON
#        -DRKNN_API_INCLUDE_DIR=<dir containing rknn_api.h, version 2.3.2>
# RGA and MPP headers come from the sysroot (usr/include/rga, usr/include/rockchip).
if(NOT RKNN_API_INCLUDE_DIR OR NOT EXISTS "${RKNN_API_INCLUDE_DIR}/rknn_api.h")
    message(FATAL_ERROR "Set -DRKNN_API_INCLUDE_DIR to the directory containing rknn_api.h")
endif()

set(SYSROOT_INCLUDE "${CMAKE_SYSROOT}/usr/include")

add_executable(probe_link probe_link.cpp)
target_include_directories(
    probe_link PRIVATE "${SYSROOT_INCLUDE}/rga" "${SYSROOT_INCLUDE}/rockchip" "${RKNN_API_INCLUDE_DIR}"
)
target_link_libraries(probe_link PRIVATE rga rockchip_mpp rknnrt)

add_executable(probe_infer probe_infer.cpp)
target_include_directories(probe_infer PRIVATE "${RKNN_API_INCLUDE_DIR}")
target_link_libraries(probe_infer PRIVATE rknnrt)
