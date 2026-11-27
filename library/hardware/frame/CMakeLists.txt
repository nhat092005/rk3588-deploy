# The interface that the pipeline reads its frames from: the camera, and later a video file.
add_library(frame_source_lib INTERFACE)
target_include_directories(frame_source_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frame_source_lib INTERFACE dmabuf_lib os_lib)
