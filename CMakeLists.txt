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

find_package(Threads REQUIRED)

# ==============================
# Subdirectories
# ==============================
add_subdirectory(app)
add_subdirectory(library)
add_subdirectory(test)
