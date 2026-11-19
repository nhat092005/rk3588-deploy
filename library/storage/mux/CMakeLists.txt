# MP4 files of the H.265 video, written with libavformat. It links the libraries of the board
# sysroot, so it is cross-compile only; the writers of files take the format through VideoOutput and
# build on the PC with the raw format alone.
if(CMAKE_CROSSCOMPILING)
    add_library(mux_lib STATIC mp4_writer.cpp)
    target_include_directories(mux_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mux_lib PUBLIC logger_lib store_lib sdk_av)
endif()
