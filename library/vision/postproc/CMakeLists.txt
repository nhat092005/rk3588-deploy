add_library(postproc_lib STATIC postprocess.cpp)

target_include_directories(postproc_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(postproc_lib PUBLIC logger_lib)

# No fused multiply-add: the IoU must round like the numpy float32 operations it is checked against.
target_compile_options(postproc_lib PRIVATE -ffp-contract=off)
