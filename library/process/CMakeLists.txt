add_library(process_lib STATIC arguments.cpp)

target_include_directories(process_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
