#include "input_videos.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace {
namespace fs = std::filesystem;

constexpr std::string_view mp4Suffix{".mp4"};

bool hasMp4Suffix(const fs::path& file) {
    std::string suffix{file.extension().string()};
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    return suffix == mp4Suffix;
}
} // namespace

std::optional<std::vector<std::string>> listInputVideos(std::string_view path, const Logger& log) {
    const fs::path input{path};
    std::error_code error;
    const fs::file_status status{fs::status(input, error)};
    if (error || !fs::exists(status)) {
        log.error("input.path ", path, " does not exist");
        return std::nullopt;
    }
    if (fs::is_regular_file(status)) {
        return std::vector<std::string>{input.string()};
    }
    if (!fs::is_directory(status)) {
        log.error("input.path ", path, " is neither a file nor a folder");
        return std::nullopt;
    }
    std::vector<std::string> videos;
    for (fs::directory_iterator entry{input, error}; !error && entry != fs::directory_iterator{};
         entry.increment(error)) {
        std::error_code entryError;
        if (entry->is_regular_file(entryError) && hasMp4Suffix(entry->path())) {
            videos.push_back(entry->path().string());
        }
    }
    if (error) {
        log.error("input.path ", path, " cannot be read: ", error.message());
        return std::nullopt;
    }
    if (videos.empty()) {
        log.error("input.path ", path, " has no .mp4 file");
        return std::nullopt;
    }
    std::sort(videos.begin(), videos.end());
    return videos;
}
