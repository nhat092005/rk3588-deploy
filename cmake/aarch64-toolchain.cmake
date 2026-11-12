# Cross-compile for the Orange Pi 5 (RK3588, Ubuntu 22.04 Jammy, glibc 2.35, gcc 11.4).
# Use: cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
# Needs: sudo apt install g++-11-aarch64-linux-gnu
# Sysroot: copy of the board's /usr/include and /usr/lib, default ~/sysroots/orangepi5
#          (override with -DRK3588_SYSROOT=<path> or env RK3588_SYSROOT).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT RK3588_SYSROOT)
    if(DEFINED ENV{RK3588_SYSROOT})
        set(RK3588_SYSROOT "$ENV{RK3588_SYSROOT}")
    else()
        set(RK3588_SYSROOT "$ENV{HOME}/sysroots/orangepi5")
    endif()
endif()

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-11)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-11)
set(CMAKE_SYSROOT "${RK3588_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH "${RK3588_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
