# Board hardware probes. Off by default because they need headers that are not in this repo yet.
# Build: cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DBUILD_PROBES=ON
#        (optional) -DRKNN_API_INCLUDE_DIR=<dir containing rknn_api.h, version 2.3.2>
# RGA and MPP headers come from the sysroot (usr/include/rga, usr/include/rockchip).
# Default: the pinned rknn-toolkit2 submodule (v2.3.2) of the rk3588 meta-repo, see its Makefile target `init`.
if(NOT RKNN_API_INCLUDE_DIR)
    set(RKNN_API_INCLUDE_DIR
        "${CMAKE_SOURCE_DIR}/../3rdparty/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/include"
    )
endif()
if(NOT EXISTS "${RKNN_API_INCLUDE_DIR}/rknn_api.h")
    message(
        FATAL_ERROR
            "rknn_api.h not found in ${RKNN_API_INCLUDE_DIR}. Run `make init` in the rk3588 meta-repo or pass -DRKNN_API_INCLUDE_DIR=."
    )
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
