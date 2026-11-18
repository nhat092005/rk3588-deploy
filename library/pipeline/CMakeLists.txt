# The pipeline links the board libraries, so it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(
        pipeline_lib STATIC
        pipeline.cpp
        frame_slot.cpp
        evidence_hub.cpp
        evidence_worker.cpp
        output_branch.cpp
        record_worker.cpp
    )
    target_include_directories(pipeline_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        pipeline_lib
        PUBLIC logger_lib
               os_lib
               options_lib
               dmabuf_lib
               rga_lib
               mpp_lib
               clip_lib
               record_lib
               store_lib
               rule_lib
               tracker_lib
               postproc_lib
        PRIVATE camera_lib
                npu_lib
                stats_lib
                jpeg_lib
                video_lib
                Threads::Threads
    )
endif()
