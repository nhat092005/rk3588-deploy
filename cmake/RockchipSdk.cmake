# Rockchip SDK targets for the board: rockchip::rknn, rockchip::rga, rockchip::mpp.
#
# Each target carries its include directory and its library, so a user writes only
# target_link_libraries(<target> PRIVATE rockchip::rga).
#
# * Cross-compile only. Headers and libraries come from the board sysroot.
# * Exception: rknn_api.h comes from the pinned rknn-toolkit2 submodule (meta-repo `make init`).
# * The INTERFACE targets are named sdk_*, so they do not clash with the library name rockchip_mpp.
# * New SDK: add one block like the ones below, nothing else changes.
if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

set(RKNN_API_INCLUDE_DIR
    "${PROJECT_SOURCE_DIR}/../3rdparty/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/include"
    CACHE PATH "Directory containing rknn_api.h (2.3.2)"
)
if(NOT EXISTS "${RKNN_API_INCLUDE_DIR}/rknn_api.h")
    message(
        FATAL_ERROR
            "rknn_api.h not found in ${RKNN_API_INCLUDE_DIR}. Run `make init` in the rk3588 meta-repo or pass -DRKNN_API_INCLUDE_DIR=."
    )
endif()

# NPU: librknnrt
add_library(sdk_rknn INTERFACE)
target_include_directories(sdk_rknn SYSTEM INTERFACE "${RKNN_API_INCLUDE_DIR}")
target_link_libraries(sdk_rknn INTERFACE rknnrt)
add_library(rockchip::rknn ALIAS sdk_rknn)

# 2D accelerator: librga (im2d)
add_library(sdk_rga INTERFACE)
target_include_directories(sdk_rga SYSTEM INTERFACE "${CMAKE_SYSROOT}/usr/include/rga")
target_link_libraries(sdk_rga INTERFACE rga)
add_library(rockchip::rga ALIAS sdk_rga)

# Video and JPEG codec: librockchip_mpp
add_library(sdk_mpp INTERFACE)
target_include_directories(sdk_mpp SYSTEM INTERFACE "${CMAKE_SYSROOT}/usr/include/rockchip")
target_link_libraries(sdk_mpp INTERFACE rockchip_mpp)
add_library(rockchip::mpp ALIAS sdk_mpp)

# Container writer (MP4): libavformat of the board's ffmpeg 4.x, with its codec and util libraries
add_library(sdk_av INTERFACE)
target_link_libraries(sdk_av INTERFACE avformat avcodec avutil)
