#ifndef INPUT_VIDEOS_H
#define INPUT_VIDEOS_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "logger.h"

/**
 * @brief The video files that the demo runs for `input.path`.
 * @param path A video file (taken as it is, whatever its suffix), or a folder: every regular file
 *        in it whose suffix is `.mp4` (in any case), in name order. A name that is a folder or has
 *        another suffix is skipped.
 * @param log Logger that receives the reason of a failure.
 * @return The paths, in the order to run them (a folder gives `<path>/<name>`), or empty when the
 *         path does not exist or cannot be read, or a folder has no `.mp4` file, which is logged.
 */
std::optional<std::vector<std::string>> listInputVideos(std::string_view path, const Logger& log);

#endif // INPUT_VIDEOS_H
