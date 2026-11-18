add_library(dmabuf_lib STATIC dmabuf.cpp)

target_include_directories(dmabuf_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(dmabuf_lib PUBLIC logger_lib os_lib)
