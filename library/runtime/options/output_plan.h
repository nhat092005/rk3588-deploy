#ifndef OUTPUT_PLAN_H
#define OUTPUT_PLAN_H

#include <string>
#include <vector>

#include "app_options.h"
#include "letterbox_layout.h"

/** @brief Lowest bitrate of a scaled stream, in kilobits per second. */
constexpr int minScaledKbps{500};

/**
 * @brief One H.265 stream that the output branch encodes, and who takes its packets. A scaled
 *        stream is encoded at a smaller size than the camera frame.
 */
struct StreamPlan {
    /** Height of the picture in pixels (upright), 0 for the size of the camera frame. */
    int height{0};
    bool archive{false};
    bool session{false};
    bool stream{false};
    bool clip{false};
};

/**
 * @brief The streams that the options ask for. Outputs with the same height share one stream, so a
 *        stream is encoded once however many outputs read it. The larger streams come first, the
 *        full size one (height 0) before all.
 * @param options What the user set.
 * @param archive Whether a recording queue exists (the archive is on).
 * @param session Whether a session queue exists.
 * @param stream Whether a stream queue exists (the live stream may fail to start although it is
 *        on).
 * @return One entry per distinct height. Empty when nothing reads a video stream.
 */
std::vector<StreamPlan> planStreams(const AppOptions& options, bool archive, bool session,
                                    bool stream);

/**
 * @brief Width of a picture of size `full` scaled to `height`: the proportions are kept and the
 *        width is made even, rounded down.
 */
int scaledWidth(ImageSize full, int height);

/**
 * @brief Bitrate of a scaled stream: the bitrate of the full size one in proportion to the
 *        number of pixels, but at least minScaledKbps (and at most `kbps`).
 */
int scaledKbps(int kbps, ImageSize full, int height);

/**
 * @brief The first output whose height is not below the camera picture, as text for the log, or
 *        empty when the heights of every output that is on are below `fullHeight`.
 * @param options What the user set.
 * @param fullHeight Height of the upright camera picture in pixels.
 * @param archive Whether the archive is on.
 * @param session Whether the session video is on.
 */
std::string downscaleProblem(const AppOptions& options, int fullHeight, bool archive, bool session);

#endif // OUTPUT_PLAN_H
