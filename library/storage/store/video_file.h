#ifndef VIDEO_FILE_H
#define VIDEO_FILE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "logger.h"

/** @brief How the H.265 video is stored in a file. */
enum class VideoFormat : std::uint8_t {
    /** The encoder's bytes as they are (Annex B). Plays when it is cut short. */
    H265,
    /** Fragmented MP4: one fragment per key frame, so a file cut short still plays up to the last
     *  complete fragment. */
    Mp4
};

/** @brief File name suffixes of the two formats, with the dot. */
constexpr std::string_view h265Suffix{".h265"};
constexpr std::string_view mp4Suffix{".mp4"};

/** @brief One packet (one frame) of a video that is kept in a block of bytes: the packets follow
 *         each other in the block, so a place is the sum of the sizes before it. */
struct PacketInfo {
    std::size_t size{0};
    bool keyFrame{false};
    /** Time of the frame, in seconds. */
    double seconds{0.0};
};

/**
 * @brief The format that a name stands for.
 * @param name "h265" or "mp4", nothing else.
 * @return The format, or empty for any other text.
 */
std::optional<VideoFormat> parseVideoFormat(std::string_view name) noexcept;

/**
 * @brief Puts the packets of one video file on a file descriptor in one format. The first packet
 *        must be a key frame, which carries the VPS, SPS and PPS. Not thread safe, one thread
 *        writes.
 */
class VideoFileWriter {
public:
    VideoFileWriter() = default;
    VideoFileWriter(const VideoFileWriter&) = delete;
    VideoFileWriter& operator=(const VideoFileWriter&) = delete;
    VideoFileWriter(VideoFileWriter&&) = delete;
    VideoFileWriter& operator=(VideoFileWriter&&) = delete;
    virtual ~VideoFileWriter() = default;

    /**
     * @brief Adds one packet (one frame).
     * @param data Annex B bytes of the frame.
     * @param size Number of bytes, above 0.
     * @param keyFrame Whether the frame is a key frame.
     * @param seconds Time of the frame, never going back between calls.
     * @return false when the descriptor refused a write or the packet was refused (logged).
     */
    [[nodiscard]] virtual bool write(const std::uint8_t* data, std::size_t size, bool keyFrame,
                                     double seconds) = 0;

    /**
     * @brief Hands to the descriptor everything that is complete, so that an fsync that follows
     *        covers it. A format that keeps a group of frames (MP4 keeps the frames since the last
     *        key frame) leaves the unfinished group behind.
     * @return false when the descriptor refused a write.
     */
    [[nodiscard]] virtual bool flush() = 0;

    /**
     * @brief Ends the file: writes what is left. No more writes after it.
     * @return false when the descriptor refused a write.
     */
    [[nodiscard]] virtual bool finish() = 0;
};

/**
 * @brief Makes a writer for a file that is open and empty. The descriptor stays with the caller,
 *        which has to keep it open as long as the writer lives.
 * @return The writer, or null when it cannot be made (logged).
 */
using VideoFileFactory = std::unique_ptr<VideoFileWriter> (*)(int descriptor, const Logger& log);

/** @brief A format as the writers of files use it: the suffix of the names and the factory. */
struct VideoOutput {
    std::string_view suffix;
    VideoFileFactory open;
};

/**
 * @brief Writes every byte to a descriptor, retrying an interrupted write.
 * @return false on an error (the cause is logged).
 */
bool writeBytes(int descriptor, const std::uint8_t* data, std::size_t size,
                const Logger& log) noexcept;

/** @brief A writer that puts the bytes on the descriptor as they are (`.h265`). */
std::unique_ptr<VideoFileWriter> makeRawFileWriter(int descriptor, const Logger& log);

/** @brief The raw H.265 format: what the writers use unless they are given another. */
inline constexpr VideoOutput rawVideoOutput{h265Suffix, &makeRawFileWriter};

#endif // VIDEO_FILE_H
