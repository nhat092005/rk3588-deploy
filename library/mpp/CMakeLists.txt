# Parts shared by the MPP encoders: links the board library, so it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(mpp_lib STATIC mpp_source.cpp)
    target_include_directories(mpp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mpp_lib PUBLIC logger_lib dmabuf_lib rockchip::mpp)
endif()
