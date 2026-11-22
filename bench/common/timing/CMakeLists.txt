# Clocks and percentiles: plain C++.
add_library(bench_timing_lib STATIC timing.cpp)

target_include_directories(bench_timing_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
