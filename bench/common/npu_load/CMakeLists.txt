# The background load of the "NPU running" measurements: a thread that runs a model back to back.
# Links the board libraries, so it is cross-compile only (the bench is too).
find_package(Threads REQUIRED)

add_library(bench_npu_load_lib STATIC npu_load.cpp)

target_include_directories(bench_npu_load_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    bench_npu_load_lib
    PUBLIC logger_lib
           dmabuf_lib
           npu_lib
           bench_timing_lib
           Threads::Threads
)
