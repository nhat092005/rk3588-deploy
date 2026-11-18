# Letterbox geometry: plain C++, builds on the PC.
add_library(letterbox_lib STATIC letterbox_layout.cpp box_mapper.cpp rotation.cpp)

target_include_directories(letterbox_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# RGA wrapper: links the board library, so it is cross-compile only.
if(CMAKE_CROSSCOMPILING)
    add_library(rga_lib STATIC rga.cpp rga_overlay.cpp rga_labels.cpp)
    target_include_directories(rga_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        rga_lib
        PUBLIC logger_lib
               dmabuf_lib
               letterbox_lib
               label_lib
               rockchip::rga
    )
endif()
