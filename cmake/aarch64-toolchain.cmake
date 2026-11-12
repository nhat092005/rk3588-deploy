# Cross-compile for the Orange Pi 5 (RK3588, Ubuntu 22.04 Jammy, glibc 2.35, gcc 11.4).
# Use: cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
# Needs: sudo apt install g++-<n>-aarch64-linux-gnu, n = GCC major of the board (11 on Jammy).
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

if(NOT EXISTS "${RK3588_SYSROOT}/usr/include/c++")
    message(
        FATAL_ERROR
            "Sysroot not found at ${RK3588_SYSROOT}. Copy /usr/include and /usr/lib from the board (rsync) or pass -DRK3588_SYSROOT=<path>."
    )
endif()

# GCC major version of the board's libstdc++ headers (11 on Ubuntu 22.04). The cross compiler must match it.
# Override with -DRK3588_GCC_VERSION=<n>.
if(NOT RK3588_GCC_VERSION)
    file(
        GLOB RK3588_GCC_DIRS
        RELATIVE "${RK3588_SYSROOT}/usr/include/c++"
        "${RK3588_SYSROOT}/usr/include/c++/*"
    )
    list(SORT RK3588_GCC_DIRS COMPARE NATURAL)
    list(GET RK3588_GCC_DIRS -1 RK3588_GCC_VERSION)
endif()
find_program(RK3588_CROSS_GXX aarch64-linux-gnu-g++-${RK3588_GCC_VERSION})
if(NOT RK3588_CROSS_GXX)
    message(
        FATAL_ERROR
            "aarch64-linux-gnu-g++-${RK3588_GCC_VERSION} not found. Run: sudo apt install g++-${RK3588_GCC_VERSION}-aarch64-linux-gnu"
    )
endif()

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-${RK3588_GCC_VERSION})
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-${RK3588_GCC_VERSION})
set(CMAKE_SYSROOT "${RK3588_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH "${RK3588_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The cross package ships glibc 2.39 headers and libs that win over the sysroot by default, so binaries can
# need symbols the board lacks (for example GLIBC_2.38 via __isoc23_strtol). Force headers and libs of the
# board: drop the default include dirs and list them explicitly, and put the sysroot lib dirs first.
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
    OUTPUT_VARIABLE RK3588_GCC_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(RK3588_C_INCLUDES
    -nostdinc
    -isystem ${RK3588_GCC_INCLUDE}
    -isystem ${RK3588_SYSROOT}/usr/include/aarch64-linux-gnu
    -isystem ${RK3588_SYSROOT}/usr/include
)
set(RK3588_CXX_INCLUDES
    -nostdinc
    -isystem ${RK3588_SYSROOT}/usr/include/c++/${RK3588_GCC_VERSION}
    -isystem ${RK3588_SYSROOT}/usr/include/aarch64-linux-gnu/c++/${RK3588_GCC_VERSION}
    -isystem ${RK3588_SYSROOT}/usr/include/c++/${RK3588_GCC_VERSION}/backward
    -isystem ${RK3588_GCC_INCLUDE}
    -isystem ${RK3588_SYSROOT}/usr/include/aarch64-linux-gnu
    -isystem ${RK3588_SYSROOT}/usr/include
)
string(REPLACE ";" " " RK3588_C_INCLUDES_STR "${RK3588_C_INCLUDES}")
string(REPLACE ";" " " RK3588_CXX_INCLUDES_STR "${RK3588_CXX_INCLUDES}")
set(CMAKE_C_FLAGS_INIT "${RK3588_C_INCLUDES_STR}")
set(CMAKE_CXX_FLAGS_INIT "${RK3588_CXX_INCLUDES_STR}")

set(RK3588_LINK_DIRS
    "-L${RK3588_SYSROOT}/usr/lib/gcc/aarch64-linux-gnu/${RK3588_GCC_VERSION} -L${RK3588_SYSROOT}/usr/lib/aarch64-linux-gnu -L${RK3588_SYSROOT}/usr/lib"
)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${RK3588_LINK_DIRS} -Wl,-rpath-link,${RK3588_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${RK3588_LINK_DIRS}")
