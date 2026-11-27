# The CPU side of the JPEG stage: libjpeg-turbo (board sysroot) from an NV12 image.
find_package(JPEG REQUIRED)

add_library(bench_jpeg_lib STATIC turbo_encoder.cpp)

target_include_directories(bench_jpeg_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_jpeg_lib PUBLIC logger_lib bench_timing_lib opencv_core JPEG::JPEG)
