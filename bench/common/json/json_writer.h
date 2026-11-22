#ifndef BENCH_JSON_WRITER_H
#define BENCH_JSON_WRITER_H

#include <string>
#include <string_view>
#include <vector>

#include "logger.h"

/**
 * @brief Builds a JSON text in memory, one element per line, indented by two spaces. The caller
 * keeps the structure right: every begin has its end, and inside an object every value follows a
 * key().
 *
 * The values have distinct names (text, number, integer, boolean): a `const char*` would
 * otherwise convert to bool.
 */
class JsonWriter {
public:
    void beginObject();
    void endObject();
    void beginArray();
    void endArray();

    /** @brief Writes the key of the next value of an object. */
    void key(std::string_view name);
    /** @brief Writes a string, escaped. */
    void text(std::string_view value);
    /** @brief Writes a number in its shortest exact form. NaN and infinity become null. */
    void number(double value);
    void integer(long long value);
    void boolean(bool value);

    /** @brief The text so far. */
    const std::string& str() const noexcept {
        return out;
    }

private:
    void separator();
    void newline();
    void close(char bracket);
    void quoted(std::string_view value);

    std::string out;
    // One flag per open object or array: true until its first element is written.
    std::vector<bool> first;
    bool afterKey{false};
};

/**
 * @brief Writes a text file, with a final newline.
 * @param path File to create or replace. Its folder must exist.
 * @param content The text.
 * @param log Logger that receives the reason of a failure.
 * @return false when the file cannot be written, which is logged.
 */
[[nodiscard]] bool writeTextFile(const std::string& path, std::string_view content,
                                 const Logger& log);

#endif // BENCH_JSON_WRITER_H
