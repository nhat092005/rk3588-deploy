# JPEG encoder: links the board library, so it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(jpeg_lib STATIC jpeg_encoder.cpp)
    target_include_directories(jpeg_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(jpeg_lib PUBLIC logger_lib dmabuf_lib mpp_lib rockchip::mpp)
endif()
