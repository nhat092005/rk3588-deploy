# The application links the board libraries, so it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_executable(rk3588_deploy main.cpp)

    target_compile_features(rk3588_deploy PUBLIC cxx_std_17)

    target_link_libraries(rk3588_deploy PRIVATE logger_lib process_lib options_lib pipeline_lib)

    # bin/rk3588_deploy on the board: see "cmake --install" in README.md
    install(TARGETS rk3588_deploy RUNTIME DESTINATION bin)
endif()
