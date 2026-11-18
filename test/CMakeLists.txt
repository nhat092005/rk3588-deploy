file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Tests of the RGA and NPU wrappers need the board libraries: cross-compile only.
if(NOT CMAKE_CROSSCOMPILING)
    list(
        FILTER
        TEST_SOURCES
        EXCLUDE
        REGEX
        "test_(rga_letterbox|rga_snapshot|rga_overlay|rga_labels|jpeg_encoder|video_encoder|app_options|npu_infer)\\.cpp$"
    )
endif()

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(${TEST_NAME} ${TEST_SRC})

    # <build>/test/<library>/<name>, like the source tree.
    get_filename_component(TEST_DIR ${TEST_SRC} DIRECTORY)
    file(RELATIVE_PATH TEST_REL ${CMAKE_CURRENT_SOURCE_DIR} ${TEST_DIR})
    set_target_properties(
        ${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${TEST_REL}"
    )

    target_compile_features(${TEST_NAME} PUBLIC cxx_std_17)

    target_link_libraries(
        ${TEST_NAME}
        PRIVATE logger_lib
                dmabuf_lib
                letterbox_lib
                label_lib
                process_lib
                postproc_lib
                tracker_lib
                rule_lib
                store_lib
                hevc_nal_lib
                stats_lib
                clip_lib
                record_lib
    )
    if(CMAKE_CROSSCOMPILING)
        target_link_libraries(
            ${TEST_NAME}
            PRIVATE rga_lib
                    npu_lib
                    jpeg_lib
                    video_lib
                    options_lib
        )
    endif()
endforeach()
