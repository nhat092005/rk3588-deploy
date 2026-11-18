# Continuous recording: the packet queue, the segment files and their retention. Plain C++ and POSIX
# file calls, builds on the PC.
add_library(record_lib STATIC packet_queue.cpp segment_writer.cpp recording_retention.cpp)

target_include_directories(record_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(record_lib PUBLIC logger_lib os_lib store_lib Threads::Threads)
