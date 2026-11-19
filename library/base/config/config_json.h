#ifndef CONFIG_JSON_H
#define CONFIG_JSON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/** @brief The kind of a value in a config file. Object is the start of a group of pairs. */
enum class JsonKind : std::uint8_t { String, Number, Boolean, Object };

/**
 * @brief One pair. A pair inside a group has the name of the group in `group` (`a.b` for a group
 *        `b` inside the group `a`, `a.b.c` one level deeper), a pair at the top has an empty `group`. A group itself is an
 *        entry of kind Object with an empty value, followed by its pairs; it carries the name of
 *        its parent group in `group`. A Boolean has the text `true` or `false`.
 */
struct JsonEntry {
    std::string group;
    std::string key;
    std::string value;
    JsonKind kind{JsonKind::String};
};

/** @brief The result of parseConfigJson. */
struct JsonParse {
    bool ok{false};
    /** On failure: the byte offset in the text where the reading stopped, and why. */
    std::size_t offset{0};
    const char* reason{""};
    /** The entries in the order of the text. A key given twice appears twice. */
    std::vector<JsonEntry> entries;
};

/**
 * @brief Reads the JSON object of a config file: pairs `"key": value` whose value is a string, a
 *        number, `true`, `false`, or a group, which is an object of such pairs and of groups (three
 * levels).
 * @param text The whole text. Only whitespace may follow the closing brace.
 * @return ok with the entries, or the offset and the reason of the first failure.
 *
 * Not accepted, by design: a fourth level of groups, arrays, `null`, a backslash or a control
 * character in a string, a number that is not in the JSON grammar, a comma before `}`.
 */
JsonParse parseConfigJson(std::string_view text);

#endif // CONFIG_JSON_H
