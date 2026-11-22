# A small JSON writer and the text file write: plain C++.
add_library(bench_json_lib STATIC json_writer.cpp)

target_include_directories(bench_json_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_json_lib PUBLIC logger_lib)
