# ==============================
# Project setup
# ==============================
cmake_minimum_required(VERSION 3.12)
project(rk3588_deploy)

if(NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE
        Release
        CACHE STRING "Build type"
    )
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

# ==============================
# C++ Version
# ==============================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CODING_STANDARD.md: no exceptions, no RTTI.
add_compile_options(-fno-exceptions -fno-rtti -Wall -Wextra)

find_package(Threads REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(RockchipSdk)

# ==============================
# Subdirectories
# ==============================
add_subdirectory(app)
add_subdirectory(library)
add_subdirectory(test)

option(BUILD_PROBES "Build board hardware probes (needs rknn_api.h, see probes/CMakeLists.txt)" OFF)
if(BUILD_PROBES)
    add_subdirectory(probes)
endif()
