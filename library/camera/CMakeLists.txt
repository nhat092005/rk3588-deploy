add_library(camera_lib STATIC camera.cpp)

target_include_directories(camera_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(camera_lib PUBLIC logger_lib os_lib dmabuf_lib)
