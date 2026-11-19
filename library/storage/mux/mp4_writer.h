#ifndef MP4_WRITER_H
#define MP4_WRITER_H

#include <memory>

#include "logger.h"
#include "video_file.h"

/**
 * @brief A writer that puts the H.265 packets in a fragmented MP4 (libavformat): one fragment per
 *        key frame, so a file that is cut short (a power cut, a killed process) plays up to the
 *        last complete fragment. The picture size and the VPS, SPS and PPS are read from the first
 *        packet, which must be a key frame, so the caller needs to know nothing about the stream.
 *        Time is the `seconds` of the packets, so a gap in the stream shows as a pause.
 *
 * The frames since the last key frame stay in memory until the next key frame (about a second, a
 * few megabytes at 4K): flush() hands over the fragments that are complete.
 *
 * @param descriptor A file that is open for writing and empty. The writer never closes it.
 * @param log Logger that the writer keeps for the rest of its life.
 * @return The writer, or null when libavformat cannot make one (logged).
 */
std::unique_ptr<VideoFileWriter> makeMp4Writer(int descriptor, const Logger& log);

/** @brief The MP4 format, for the writers of files. */
inline constexpr VideoOutput mp4VideoOutput{mp4Suffix, &makeMp4Writer};

/** @brief The suffix and the writer of a format. */
inline constexpr VideoOutput videoOutputOf(VideoFormat format) noexcept {
    return format == VideoFormat::Mp4 ? mp4VideoOutput : rawVideoOutput;
}

#endif // MP4_WRITER_H
