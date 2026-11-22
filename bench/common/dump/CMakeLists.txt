# Raw image files for the checks on the PC: plain C++ and OpenCV.
add_library(bench_dump_lib STATIC dump_file.cpp)

target_include_directories(bench_dump_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_dump_lib PUBLIC logger_lib opencv_core)
