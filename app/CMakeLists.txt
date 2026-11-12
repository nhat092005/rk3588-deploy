add_executable(rk3588_deploy main.cpp)

target_compile_features(rk3588_deploy PUBLIC cxx_std_17)

target_link_libraries(rk3588_deploy PRIVATE logger_lib)
