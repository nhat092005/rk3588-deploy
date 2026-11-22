# Parts that every stage bench shares: the timing helpers, the synthetic frames, the raw dumps, the
# JSON result writer, the clock state and CPU load of the board and the NPU background load.
add_subdirectory(timing)
add_subdirectory(frame)
add_subdirectory(dump)
add_subdirectory(json)
add_subdirectory(platform)
add_subdirectory(npu_load)
