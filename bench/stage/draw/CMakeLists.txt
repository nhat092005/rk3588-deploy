# The methods of the draw stage (OpenCV on the CPU, the RGA2) and the result of a run.
add_library(bench_draw_lib STATIC draw_method.cpp draw_result.cpp)

target_include_directories(bench_draw_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    bench_draw_lib
    PUBLIC logger_lib
           dmabuf_lib
           rga_lib
           bench_preprocess_lib
           bench_frame_lib
           bench_json_lib
           bench_platform_lib
           bench_timing_lib
           opencv_core
)
