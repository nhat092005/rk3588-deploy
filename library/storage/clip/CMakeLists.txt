# Pre-roll ring and clips of the H.265 stream: plain C++, builds on the PC.
add_library(clip_lib STATIC clip_recorder.cpp)

target_include_directories(clip_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(clip_lib PUBLIC logger_lib store_lib)
