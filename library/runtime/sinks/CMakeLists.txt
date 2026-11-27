# The writers of the video to the disk that run for the whole run: the archive in segments and the
# session in one file. (The live stream is in ../stream.) It links the board libraries, so it is
# cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(sinks_lib STATIC record_worker.cpp session_worker.cpp)
    target_include_directories(sinks_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        sinks_lib
        PUBLIC logger_lib options_lib record_lib
        PRIVATE mux_lib video_lib store_lib
    )
endif()
