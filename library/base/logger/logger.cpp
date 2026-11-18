#include "logger.h"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <ctime>
#include <system_error>

#include <unistd.h>

namespace {
constexpr std::string_view colorReset{"\033[0m"};
constexpr std::string_view colorRed{"\033[31m"};
constexpr std::string_view colorYellow{"\033[33m"};
constexpr std::string_view colorGreen{"\033[32m"};
constexpr std::string_view colorBlue{"\033[34m"};
constexpr std::string_view colorWhite{"\033[37m"};
constexpr std::size_t numberCapacity{48};
constexpr std::size_t errnoTextCapacity{128};
constexpr std::size_t timestampCapacity{32};
constexpr int tmYearBase{1900};
constexpr long nanosPerMilli{1'000'000};

unsigned long long nonNegative(long long value) noexcept {
    return value < 0 ? 0ULL : static_cast<unsigned long long>(value);
}

std::string_view levelColor(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Error:
        return colorRed;
    case LogLevel::Warn:
        return colorYellow;
    case LogLevel::Info:
        return colorGreen;
    case LogLevel::Debug:
        return colorBlue;
    case LogLevel::Verbose:
        return colorWhite;
    case LogLevel::None:
        return colorReset;
    }
    return colorReset;
}

char levelLetter(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Error:
        return 'E';
    case LogLevel::Warn:
        return 'W';
    case LogLevel::Info:
        return 'I';
    case LogLevel::Debug:
        return 'D';
    case LogLevel::Verbose:
        return 'V';
    case LogLevel::None:
        return ' ';
    }
    return ' ';
}

// Local time as "YYYY-MM-DD hh:mm:ss.mmm". Appends nothing when the clock cannot be read.
void appendTimestamp(LogLine& line) noexcept {
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return;
    }
    std::tm parts{};
    if (localtime_r(&now.tv_sec, &parts) == nullptr) {
        return;
    }
    line.appendPadded(nonNegative(parts.tm_year + tmYearBase), Radix::Decimal, 4);
    line.append('-');
    line.appendPadded(nonNegative(parts.tm_mon + 1), Radix::Decimal, 2);
    line.append('-');
    line.appendPadded(nonNegative(parts.tm_mday), Radix::Decimal, 2);
    line.append(' ');
    line.appendPadded(nonNegative(parts.tm_hour), Radix::Decimal, 2);
    line.append(':');
    line.appendPadded(nonNegative(parts.tm_min), Radix::Decimal, 2);
    line.append(':');
    line.appendPadded(nonNegative(parts.tm_sec), Radix::Decimal, 2);
    line.append('.');
    line.appendPadded(nonNegative(now.tv_nsec / nanosPerMilli), Radix::Decimal, 3);
}

// write() the whole text. A failed write cannot be reported anywhere, so the line is dropped.
void writeAll(int descriptor, std::string_view text) noexcept {
    while (!text.empty()) {
        const ssize_t written{write(descriptor, text.data(), text.size())};
        if (written > 0) {
            text.remove_prefix(static_cast<std::size_t>(written));
        } else if (written == 0 || errno != EINTR) {
            return;
        }
    }
}
} // namespace

void LogLine::append(std::string_view text) noexcept {
    for (const char character : text) {
        if (used == capacity) {
            return;
        }
        buffer.at(used) = character;
        ++used;
    }
}

void LogLine::append(const char* text) noexcept {
    append(text != nullptr ? std::string_view{text} : std::string_view{"(null)"});
}

void LogLine::append(char value) noexcept {
    append(std::string_view{&value, 1});
}

void LogLine::append(bool value) noexcept {
    append(value ? std::string_view{"true"} : std::string_view{"false"});
}

void LogLine::append(double value) noexcept {
    append(Fixed{value});
}

void LogLine::append(Hex value) noexcept {
    appendPadded(value.value, Radix::Hexadecimal, value.width);
}

void LogLine::append(Fixed value) noexcept {
    std::array<char, numberCapacity> storage{};
    const std::to_chars_result result{std::to_chars(storage.begin(), storage.end(), value.value,
                                                    std::chars_format::fixed, value.precision)};
    if (result.ec == std::errc{}) {
        append(std::string_view{storage.data(),
                                static_cast<std::size_t>(result.ptr - storage.begin())});
    }
}

void LogLine::append(const std::error_code& error) noexcept {
    if (error.category() != std::generic_category() && error.category() != std::system_category()) {
        append(std::string_view{error.category().name()});
        append(':');
        append(error.value());
        return;
    }
    std::array<char, errnoTextCapacity> storage{};
    // GNU strerror_r returns the text, which may be a static string instead of `storage`.
    append(std::string_view{strerror_r(error.value(), storage.data(), storage.size())});
}

void LogLine::appendPadded(unsigned long long value, Radix radix, int width) noexcept {
    std::array<char, numberCapacity> storage{};
    const std::to_chars_result result{
        std::to_chars(storage.begin(), storage.end(), value, static_cast<int>(radix))};
    if (result.ec != std::errc{}) {
        return;
    }
    const std::size_t length{static_cast<std::size_t>(result.ptr - storage.begin())};
    for (std::size_t padding{length}; padding < static_cast<std::size_t>(width); ++padding) {
        append('0');
    }
    append(std::string_view{storage.data(), length});
}

std::string_view LogLine::text() const noexcept {
    return {buffer.data(), used};
}

void LogLine::appendSigned(long long value) noexcept {
    std::array<char, numberCapacity> storage{};
    const std::to_chars_result result{std::to_chars(storage.begin(), storage.end(), value)};
    if (result.ec == std::errc{}) {
        append(std::string_view{storage.data(),
                                static_cast<std::size_t>(result.ptr - storage.begin())});
    }
}

void LogLine::appendUnsigned(unsigned long long value) noexcept {
    appendPadded(value, Radix::Decimal, 0);
}

void Logger::beginLine(LogLine& line, LogLevel lineLevel) const noexcept {
    line.append(levelColor(lineLevel));
    line.append(levelLetter(lineLevel));
    line.append(" (");
    appendTimestamp(line);
    line.append(") ");
    line.append(tag);
    line.append(": ");
}

void Logger::endLine(LogLine& line, LogLevel lineLevel) noexcept {
    line.append(colorReset);
    line.append('\n');
    writeAll(lineLevel <= LogLevel::Warn ? STDERR_FILENO : STDOUT_FILENO, line.text());
}
