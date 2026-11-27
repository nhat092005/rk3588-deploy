# The parts that make the application: the options, the live stream, the output of a frame (video,
# clips, evidence), the writers of the video to the disk, and the pipeline that uses them.
add_subdirectory(options)
add_subdirectory(stream)
add_subdirectory(output)
add_subdirectory(sinks)
add_subdirectory(pipeline)
