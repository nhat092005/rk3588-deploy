#include "file_text.h"

#include <array>
#include <cerrno>
#include <climits>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "handles.h"

namespace {
constexpr std::size_t chunkBytes{4096};

// The part of a path before its last slash, or nothing when there is no slash.
std::optional<std::string> parentOf(const std::string& path) {
    const std::size_t slash{path.find_last_of('/')};
    if (slash == std::string::npos) {
        return std::nullopt;
    }
    return path.substr(0, slash);
}
} // namespace

FileText readTextFile(const std::string& path, const Logger& log) {
    FileText result;
    const FileDescriptor file{::open(path.c_str(), O_RDONLY | O_CLOEXEC), log};
    if (!file.valid()) {
        if (errno == ENOENT) {
            result.status = ReadStatus::Missing;
        } else {
            log.error("cannot open ", path, ": ", std::strerror(errno));
        }
        return result;
    }
    std::array<char, chunkBytes> chunk{};
    while (true) {
        const ssize_t got{::read(file.get(), chunk.data(), chunk.size())};
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            log.error("cannot read ", path, ": ", std::strerror(errno));
            return result;
        }
        if (got == 0) {
            break;
        }
        result.text.append(chunk.data(), static_cast<std::size_t>(got));
        if (result.text.size() > maxTextFileBytes) {
            log.error(path, " is larger than ", maxTextFileBytes, " bytes");
            return result;
        }
    }
    result.status = ReadStatus::Ok;
    return result;
}

std::optional<std::string> executableBaseDirectory() {
    std::array<char, PATH_MAX> buffer{};
    const ssize_t length{::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1)};
    if (length <= 0) {
        return std::nullopt;
    }
    const std::string executable(buffer.data(), static_cast<std::size_t>(length));
    const std::optional<std::string> folder{parentOf(executable)};
    if (!folder) {
        return std::nullopt;
    }
    return parentOf(*folder);
}
