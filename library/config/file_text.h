#ifndef FILE_TEXT_H
#define FILE_TEXT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "logger.h"

/** @brief What readTextFile found. */
enum class ReadStatus : std::uint8_t { Ok, Missing, Failed };

/** @brief The result of readTextFile: `text` is meaningful only when `status` is Ok. */
struct FileText {
    ReadStatus status{ReadStatus::Failed};
    std::string text;
};

/** @brief Largest config file accepted, in bytes. */
constexpr std::size_t maxTextFileBytes{65536};

/**
 * @brief Reads a whole text file.
 * @param path File to read.
 * @param log Logger that receives the reason of a failure (not of a missing file).
 * @return Ok with the text, Missing when the file does not exist, Failed on any other error or
 *         when the file is larger than maxTextFileBytes.
 */
FileText readTextFile(const std::string& path, const Logger& log);

/**
 * @brief The folder above the folder of the running executable: `.../deploy` for
 *        `.../deploy/bin/rk3588_deploy`.
 * @return The path without a trailing slash, or nothing when the executable cannot be found.
 */
std::optional<std::string> executableBaseDirectory();

#endif // FILE_TEXT_H
