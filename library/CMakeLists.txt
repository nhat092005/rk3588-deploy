# Groups of libraries. Each group lists its libraries; the order below follows what depends on what
# (base, then hardware and vision, then storage, then runtime).
add_subdirectory(base)
add_subdirectory(hardware)
add_subdirectory(vision)
add_subdirectory(storage)
add_subdirectory(runtime)
