# What the pipeline does with a frame once it has its boxes: the upright copy, the boxes and labels,
# the H.265 video, the clips and the JPEG evidence of a violation. It links the board libraries, so
# it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(
        output_lib STATIC frame_slot.cpp evidence_hub.cpp evidence_worker.cpp output_branch.cpp
    )
    target_include_directories(output_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        output_lib
        PUBLIC logger_lib
               os_lib
               options_lib
               camera_lib
               dmabuf_lib
               rga_lib
               mpp_lib
               clip_lib
               record_lib
               jpeg_lib
               store_lib
               rule_lib
               tracker_lib
               postproc_lib
        PRIVATE mux_lib video_lib
    )
endif()
