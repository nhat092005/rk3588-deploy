file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Tests of the RGA and NPU wrappers need the board libraries: cross-compile only.
set(BOARD_ONLY_TESTS
    test_rga_letterbox
    test_rga_snapshot
    test_rga_overlay
    test_rga_labels
    test_jpeg_encoder
    test_video_encoder
    test_app_options
    test_npu_infer
)

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    # test_volume -> BUILD_TEST_VOLUME
    string(REGEX REPLACE "^test_" "" TEST_SHORT ${TEST_NAME})
    string(TOUPPER ${TEST_SHORT} TEST_SWITCH)
    set(TEST_SWITCH BUILD_TEST_${TEST_SWITCH})
    if(NOT DEFINED ${TEST_SWITCH})
        message(FATAL_ERROR "${TEST_NAME}: add set(${TEST_SWITCH} ON) to cmake/build_config.cmake")
    endif()
    if(NOT ${TEST_SWITCH})
        message(STATUS "skip ${TEST_NAME}: ${TEST_SWITCH} is OFF")
        continue()
    endif()
    if(NOT CMAKE_CROSSCOMPILING AND TEST_NAME IN_LIST BOARD_ONLY_TESTS)
        message(STATUS "skip ${TEST_NAME}: it needs the board")
        continue()
    endif()

    add_executable(${TEST_NAME} ${TEST_SRC})

    # <build>/test/<library>/<name>, like the source tree.
    get_filename_component(TEST_DIR ${TEST_SRC} DIRECTORY)
    file(RELATIVE_PATH TEST_REL ${CMAKE_CURRENT_SOURCE_DIR} ${TEST_DIR})
    set_target_properties(
        ${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${TEST_REL}"
    )

    # bin/test/<library>/<name> on the board.
    install(TARGETS ${TEST_NAME} RUNTIME DESTINATION bin/test/${TEST_REL})

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
                stream_lib
                config_lib
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
