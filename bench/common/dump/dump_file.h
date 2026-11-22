#ifndef BENCH_DUMP_FILE_H
#define BENCH_DUMP_FILE_H

#include <string>

#include <opencv2/core.hpp>

#include "logger.h"

/**
 * @brief Writes the bytes of an image to a file, row after row with no header, so that numpy
 *        reads it with `fromfile` and a reshape.
 * @param path File to create or replace. Its folder must exist.
 * @param image Continuous image (no gap between rows).
 * @param log Logger that receives the reason of a failure.
 * @return false when the image is not continuous or the file cannot be written, which is logged.
 */
[[nodiscard]] bool writeImage(const std::string& path, const cv::Mat& image, const Logger& log);

#endif // BENCH_DUMP_FILE_H
