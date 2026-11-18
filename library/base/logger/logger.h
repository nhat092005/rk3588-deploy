#ifndef LOGGER_H
#define LOGGER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <type_traits>

/**
 * @brief Severity of a log line, from least to most verbose. A Logger writes the lines up to its
 *        own level; None writes nothing.
 */
enum class LogLevel : std::uint8_t { None, Error, Warn, Info, Debug, Verbose };

/** @brief Base for printing an integer. The value is the base. */
enum class Radix : std::uint8_t { Decimal = 10, Hexadecimal = 16 };

/** @brief An integer printed in hexadecimal, zero-padded to `width` digits. */
struct Hex {
    std::uint64_t value{0};
    int width{0};
};

/** @brief A floating-point value printed with `precision` digits after the point. */
struct Fixed {
    double value{0.0};
    int precision{2};
};

/**
 * @brief One log line in a fixed buffer, so logging allocates nothing. Text past the capacity is
 *        dropped.
 */
class LogLine {
public:
    /**
     * @brief Appends text.
     * @param text Text to add.
     */
    void append(std::string_view text) noexcept;

    /**
     * @brief Appends a C string.
     * @param text Zero-terminated string. A null pointer appends "(null)".
     */
    void append(const char* text) noexcept;

    /**
     * @brief Appends one character.
     * @param value Character to add.
     */
    void append(char value) noexcept;

    /**
     * @brief Appends "true" or "false".
     * @param value Flag to print.
     */
    void append(bool value) noexcept;

    /**
     * @brief Appends a floating-point value as Fixed with its default precision.
     * @param value Number to print.
     */
    void append(double value) noexcept;

    /**
     * @brief Appends an integer in hexadecimal.
     * @param value Integer and minimum digit count.
     */
    void append(Hex value) noexcept;

    /**
     * @brief Appends a floating-point value in fixed notation.
     * @param value Number and digits after the point.
     */
    void append(Fixed value) noexcept;

    /**
     * @brief Appends the text of an errno value, written into the fixed buffer.
     * @param error Error from the generic or system category. Another category appends its name,
     *        a colon and the value.
     */
    void append(const std::error_code& error) noexcept;

    /**
     * @brief Appends an unsigned value in `radix`, left-padded with zeros.
     * @param value Number to print.
     * @param radix Base of the digits.
     * @param width Minimum digit count. A longer number is not cut.
     */
    void appendPadded(unsigned long long value, Radix radix, int width) noexcept;

    /**
     * @brief Appends an integer in decimal. A signed type prints its sign.
     * @tparam Int Any integral type.
     * @param value Number to print.
     */
    template <typename Int, typename = std::enable_if_t<std::is_integral_v<Int>>>
    void append(Int value) noexcept {
        if constexpr (std::is_signed_v<Int>) {
            appendSigned(static_cast<long long>(value));
        } else {
            appendUnsigned(static_cast<unsigned long long>(value));
        }
    }

    /**
     * @brief The text appended so far.
     * @return A view of the internal buffer, valid while this LogLine exists.
     */
    std::string_view text() const noexcept;

private:
    void appendSigned(long long value) noexcept;
    void appendUnsigned(unsigned long long value) noexcept;

    static constexpr std::size_t capacity{512};

    std::array<char, capacity> buffer{};
    std::size_t used{0};
};

/**
 * @brief Writes lines of one tag up to one level. It has no shared state: every user owns its
 *        Logger and passes it on. Error and Warn go to stderr, the other levels to stdout, one
 *        write() per line.
 * @code
 *   const Logger log{"CAMERA", LogLevel::Debug};
 *   log.info("resolution ", width, "x", height);
 *   log.error("open failed: ", message);
 * @endcode
 */
class Logger {
public:
    /**
     * @brief Creates a logger.
     * @param tagName Prefix of every line. Not copied: it must outlive the Logger, for example a
     *        string literal.
     * @param maxLevel Most verbose level that is written.
     */
    constexpr Logger(std::string_view tagName, LogLevel maxLevel) noexcept
        : tag{tagName}, level{maxLevel} {}

    /**
     * @brief Writes one Error line to stderr.
     * @tparam Args Types of the values, each accepted by LogLine::append.
     * @param args Values appended to the line in order.
     */
    template <typename... Args> void error(Args... args) const noexcept {
        write(LogLevel::Error, args...);
    }

    /**
     * @brief Writes one Warn line to stderr.
     * @tparam Args Types of the values, each accepted by LogLine::append.
     * @param args Values appended to the line in order.
     */
    template <typename... Args> void warn(Args... args) const noexcept {
        write(LogLevel::Warn, args...);
    }

    /**
     * @brief Writes one Info line to stdout.
     * @tparam Args Types of the values, each accepted by LogLine::append.
     * @param args Values appended to the line in order.
     */
    template <typename... Args> void info(Args... args) const noexcept {
        write(LogLevel::Info, args...);
    }

    /**
     * @brief Writes one Debug line to stdout.
     * @tparam Args Types of the values, each accepted by LogLine::append.
     * @param args Values appended to the line in order.
     */
    template <typename... Args> void debug(Args... args) const noexcept {
        write(LogLevel::Debug, args...);
    }

    /**
     * @brief Writes one Verbose line to stdout.
     * @tparam Args Types of the values, each accepted by LogLine::append.
     * @param args Values appended to the line in order.
     */
    template <typename... Args> void verbose(Args... args) const noexcept {
        write(LogLevel::Verbose, args...);
    }

private:
    template <typename... Args> void write(LogLevel lineLevel, Args... args) const noexcept {
        if (lineLevel > level) {
            return;
        }
        LogLine line{};
        beginLine(line, lineLevel);
        (line.append(args), ...);
        endLine(line, lineLevel);
    }

    void beginLine(LogLine& line, LogLevel lineLevel) const noexcept;
    static void endLine(LogLine& line, LogLevel lineLevel) noexcept;

    std::string_view tag;
    LogLevel level;
};

#endif
