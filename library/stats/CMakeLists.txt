# Timing statistics: plain C++, builds on the PC.
add_library(stats_lib STATIC stage_stats.cpp)

target_include_directories(stats_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
