# The pipeline: the threads that take a frame from the source, run the model and hand the result to
# the output and the sinks. It links the board libraries, so it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(pipeline_lib STATIC pipeline.cpp)
    target_include_directories(pipeline_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        pipeline_lib
        PUBLIC logger_lib os_lib options_lib
        PRIVATE output_lib
                sinks_lib
                camera_lib
                file_source_lib
                npu_lib
                stats_lib
                stream_lib
                dmabuf_lib
                rga_lib
                store_lib
                rule_lib
                tracker_lib
                postproc_lib
                Threads::Threads
    )
endif()
