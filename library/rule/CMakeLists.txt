add_library(rule_lib STATIC violation_rule.cpp)

target_include_directories(rule_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rule_lib PUBLIC logger_lib postproc_lib tracker_lib)
