#include "dump_file.h"

#include <cstddef>
#include <fstream>

#include <ios>

bool writeImage(const std::string& path, const cv::Mat& image, const Logger& log) {
    if (!image.isContinuous()) {
        log.error(path, ": the image has gaps between rows");
        return false;
    }
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(image.data),
               static_cast<std::streamsize>(image.total() * image.elemSize()));
    if (!file) {
        log.error(path, ": cannot write the file");
        return false;
    }
    return true;
}
