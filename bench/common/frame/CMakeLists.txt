# Synthetic NV12 frames: plain C++ and OpenCV.
add_library(bench_frame_lib STATIC synthetic_frame.cpp)

target_include_directories(bench_frame_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_frame_lib PUBLIC letterbox_lib opencv_core)
