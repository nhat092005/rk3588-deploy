#ifndef STORE_NAMES_H
#define STORE_NAMES_H

#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>

/** @brief Characters of the time that starts a violation or recording name: `HHMMSS`. */
constexpr std::size_t timeNameLength{6};

/**
 * @brief Whether the text is not empty and holds only the digits 0 to 9.
 * @param text The text to test.
 */
bool allDigits(std::string_view text);

/**
 * @brief The time that a day directory name and the start of a file or directory name give.
 * @param day A day directory name, exactly `YYYY-MM-DD`.
 * @param name A name that starts with exactly `HHMMSS` (anything may follow).
 * @return The local time, or empty when either name does not have that form or a number is out
 *         of range. The one place that reads these names, so that the age of a violation and of a
 *         recording come from the same rule.
 */
std::optional<std::time_t> parseTime(std::string_view day, std::string_view name);

#endif // STORE_NAMES_H
