#ifndef CLIP_RECORDER_H
#define CLIP_RECORDER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "logger.h"

/** @brief Frame rate assumed when none is given. */
constexpr int defaultClipFps{30};

/** @brief Settings of a ClipRecorder. */
struct ClipConfig {
    /** Expected bytes per second of the stream (kilobits per second x 125), above 0. It sizes
     *  every buffer. */
    std::size_t bytesPerSecond{0};
    /** Seconds of video kept before an event and recorded after it, not negative. */
    double preSeconds{0.0};
    double postSeconds{0.0};
    /** Seconds between two key frames, above 0. A clip starts on a key frame, so it can begin up
     *  to this much earlier than preSeconds. */
    double gopSeconds{1.0};
    /** Frames per second of the stream, above 0. It sizes the packet index. */
    int framesPerSecond{defaultClipFps};
    /** Clips that can be recorded or waiting for the disk at once, above 0. */
    std::size_t clipCount{2};
};

/** @brief A finished clip. The bytes stay valid until ClipRecorder::release(). */
struct ClipRef {
    /** Slot to pass to release(). */
    std::size_t slot{0};
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    /** The identifier given to start(). */
    std::int64_t eventId{0};
    /** Seconds of the first packet and of the event, on the clock of addPacket(). */
    double startSeconds{0.0};
    double eventSeconds{0.0};
};

/** @brief State of the clips and the ring. Defined in clip_recorder.cpp. */
struct ClipState;

/**
 * @brief Keeps the last seconds of an H.265 stream in a ring and cuts a clip around an event: from
 *        the last key frame at least preSeconds before it to postSeconds after it. All storage is
 *        allocated by create(): addPacket(), start() and finished() allocate nothing. Move-only.
 *
 * Threads: addPacket(), start() and finished() belong to one thread, the one that encodes.
 * release() may be called from another thread, the one that writes the file.
 */
class ClipRecorder {
public:
    /**
     * @brief Checks the configuration and allocates the ring and the clip buffers.
     * @param config Sizes and times. The log is kept.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The recorder, or empty on a bad configuration, which is logged.
     */
    static std::optional<ClipRecorder> create(const ClipConfig& config, const Logger& log);

    ClipRecorder(const ClipRecorder&) = delete;
    ClipRecorder& operator=(const ClipRecorder&) = delete;
    ClipRecorder(ClipRecorder&& other) noexcept;
    ClipRecorder& operator=(ClipRecorder&& other) noexcept;
    ~ClipRecorder();

    /**
     * @brief Adds the next packet of the stream to the ring and to the clips being recorded. A
     *        clip whose end time has come (or whose buffer is full) becomes finished.
     * @param data Annex B bytes of one frame.
     * @param size Number of bytes. A packet larger than the ring is not kept (oversizedCount()).
     * @param keyFrame Whether the packet starts with a key frame.
     * @param seconds Time of the frame, never going back between calls.
     */
    void addPacket(const std::uint8_t* data, std::size_t size, bool keyFrame,
                   double seconds) noexcept;

    /**
     * @brief Starts a clip for an event at the time of the last packet added.
     * @param eventId Identifier that comes back in ClipRef.
     * @return false when no clip buffer is free or the ring holds no key frame yet. Both are
     *         counted. The event itself is not lost, only its clip.
     */
    bool start(std::int64_t eventId) noexcept;

    /** @brief Ends every clip that is being recorded now, as it is: at the end of the stream. */
    void finishAll() noexcept;

    /**
     * @brief The next finished clip, in the order of finishing.
     * @return The clip, or empty when none is finished. Pass its slot to release() when done.
     */
    std::optional<ClipRef> finished() noexcept;

    /**
     * @brief Gives a clip buffer back. Callable from any thread.
     * @param slot The ClipRef::slot of a finished clip, used once.
     */
    void release(std::size_t slot) noexcept;

    /** @brief Events that got no clip because every buffer was busy or no key frame was in the
     *         ring. */
    std::size_t skippedCount() const noexcept;

    /** @brief Packets that were larger than the ring and were not kept. Zero in normal use. */
    std::size_t oversizedCount() const noexcept;

private:
    explicit ClipRecorder(std::unique_ptr<ClipState> initial) noexcept;

    std::unique_ptr<ClipState> state;
};

#endif // CLIP_RECORDER_H
