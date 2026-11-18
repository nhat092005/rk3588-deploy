# The H.265 stream helpers run on the PC too; the encoder links the board library, so it is
# cross-compile only.
add_library(hevc_nal_lib STATIC hevc_nal.cpp)
target_include_directories(hevc_nal_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_CROSSCOMPILING)
    add_library(video_lib STATIC video_encoder.cpp)
    target_include_directories(video_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        video_lib
        PUBLIC logger_lib
               dmabuf_lib
               mpp_lib
               hevc_nal_lib
               rockchip::mpp
    )
endif()
