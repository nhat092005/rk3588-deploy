# The methods of the preprocess stage (OpenCV on the CPU, RGA on each core).
add_library(bench_preprocess_lib STATIC preprocess_method.cpp rga_forced.cpp)

target_include_directories(bench_preprocess_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    bench_preprocess_lib
    PUBLIC logger_lib
           dmabuf_lib
           rga_lib
           letterbox_lib
           bench_frame_lib
           opencv_core
           opencv_imgproc
)
