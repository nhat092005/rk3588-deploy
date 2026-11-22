# The state of the board that a result must record(clock governors) : plain C++.
add_library(bench_platform_lib STATIC freq_state.cpp)

target_include_directories(bench_platform_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
