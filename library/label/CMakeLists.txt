# Label text: the font, the text of a label, its bitmap and its place. Plain C++, builds on the PC.
add_library(label_lib STATIC label_text.cpp)

target_include_directories(label_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(label_lib PUBLIC letterbox_lib)
