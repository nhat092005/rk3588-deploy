# Config file support: a reader of config JSON, reading a text file, finding the folder of the
# application. Plain POSIX and the standard library, so it builds on the PC too.
add_library(config_lib STATIC config_json.cpp file_text.cpp)

target_include_directories(config_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(config_lib PUBLIC logger_lib os_lib)
