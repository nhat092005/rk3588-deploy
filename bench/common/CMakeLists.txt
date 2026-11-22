# Parts that every stage bench shares: the timing helpers, the synthetic frames, the raw dumps, the
# JSON result writer and the clock state of the board.
add_subdirectory(timing)
add_subdirectory(frame)
add_subdirectory(dump)
add_subdirectory(json)
add_subdirectory(platform)
