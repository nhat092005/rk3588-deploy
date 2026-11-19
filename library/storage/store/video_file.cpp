#include "video_file.h"

#include <cerrno>
#include <cstddef>

#include <unistd.h>

#include "handles.h"

namespace {
// Puts the bytes on the descriptor as they are.
class RawFileWriter final : public VideoFileWriter {
public:
    RawFileWriter(int descriptor, const Logger& logger) : file{descriptor}, log{logger} {}

    bool write(const std::uint8_t* data, std::size_t size, bool /*keyFrame*/,
               double /*seconds*/) override {
        return writeBytes(file, data, size, log);
    }

    bool flush() override {
        return true;
    }

    bool finish() override {
        return true;
    }

private:
    int file;
    Logger log;
};
} // namespace

std::optional<VideoFormat> parseVideoFormat(std::string_view name) noexcept {
    if (name == "h265") {
        return VideoFormat::H265;
    }
    if (name == "mp4") {
        return VideoFormat::Mp4;
    }
    return std::nullopt;
}

bool writeBytes(int descriptor, const std::uint8_t* data, std::size_t size,
                const Logger& log) noexcept {
    std::size_t done{0};
    while (done < size) {
        const ssize_t count{::write(descriptor, data + done, size - done)};
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            log.error("write: ", lastError());
            return false;
        }
        done += static_cast<std::size_t>(count);
    }
    return true;
}

std::unique_ptr<VideoFileWriter> makeRawFileWriter(int descriptor, const Logger& log) {
    return std::make_unique<RawFileWriter>(descriptor, log);
}
