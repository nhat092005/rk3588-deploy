file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(${TEST_NAME} ${TEST_SRC})

    target_compile_features(${TEST_NAME} PUBLIC cxx_std_17)

    target_link_libraries(${TEST_NAME} PRIVATE logger_lib)
endforeach()
