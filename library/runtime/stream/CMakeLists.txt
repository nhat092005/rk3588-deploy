# Live stream of the video: a child process (ffmpeg) fed through a pipe. Plain C++ and POSIX calls,
# builds on the PC.
add_library(stream_lib STATIC child_process.cpp stream_check.cpp stream_worker.cpp)

target_include_directories(stream_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(stream_lib PUBLIC logger_lib os_lib record_lib Threads::Threads)
