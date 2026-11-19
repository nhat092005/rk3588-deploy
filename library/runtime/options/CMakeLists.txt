# Command line of the application. It reads the constants of the board libraries, so it is
# cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(options_lib STATIC app_options.cpp output_plan.cpp)
    target_include_directories(options_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        options_lib
        PUBLIC logger_lib
               config_lib
               letterbox_lib
               jpeg_lib
               video_lib
               rule_lib
               store_lib
               record_lib
               stream_lib
    )
endif()
