add_library(tracker_lib STATIC lap_solver.cpp kalman_filter.cpp byte_tracker.cpp)

target_include_directories(tracker_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tracker_lib PUBLIC logger_lib postproc_lib)

# No fused multiply-add: the IoU and the Kalman filter must round like the numpy operations they are
# checked against.
target_compile_options(tracker_lib PRIVATE -ffp-contract=off)
