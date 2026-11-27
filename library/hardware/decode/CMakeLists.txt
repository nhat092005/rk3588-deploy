# The video file as a frame source (libavformat + the MPP decoder): it links the libraries of the
# board sysroot, so it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(file_source_lib STATIC file_source.cpp)
    target_include_directories(file_source_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        file_source_lib
        PUBLIC logger_lib
               dmabuf_lib
               frame_source_lib
               sdk_av
               rockchip::mpp
    )
endif()
