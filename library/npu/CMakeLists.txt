# NPU wrapper: links the board library, so it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(npu_lib STATIC npu_infer.cpp)
    target_include_directories(npu_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(npu_lib PUBLIC logger_lib dmabuf_lib rockchip::rknn)
endif()
