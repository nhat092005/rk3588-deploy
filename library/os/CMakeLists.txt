# OS handles (file descriptor, mmap region, stop signal): header only.
add_library(os_lib INTERFACE)

target_include_directories(os_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(os_lib INTERFACE logger_lib)
