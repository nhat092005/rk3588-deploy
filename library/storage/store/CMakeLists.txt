# Violation store: plain C++ and POSIX file calls, builds on the PC.
add_library(store_lib STATIC violation_store.cpp violation_retention.cpp store_names.cpp volume.cpp)

target_include_directories(store_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(store_lib PUBLIC logger_lib os_lib rule_lib)
