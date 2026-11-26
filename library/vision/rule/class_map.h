#ifndef CLASS_MAP_H
#define CLASS_MAP_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "logger.h"

/**
 * @brief What a class means to the violation rule. A class with no role is only drawn.
 */
enum class ClassRole : std::uint8_t { None, Person, NoHelmet, NoVest };

/** @brief One class of the model, in the order of the model output (the position is the index). */
struct ClassSpec {
    std::string name;
    ClassRole role{ClassRole::None};
};

/** @brief Most classes accepted. */
constexpr std::size_t maxClasses{64};
/**
 * @brief Longest class name in characters. A label is `! #12345 name 100%` in at most 24
 *        characters (maxLabelChars of label_text.h), which leaves 10 for the name.
 */
constexpr std::size_t maxClassNameChars{10};

/** @brief The class list after the checks: names by index and the index of each role. */
struct ClassMap {
    std::vector<std::string> names;
    int person{-1};
    int noHelmet{-1};
    int noVest{-1};
};

/**
 * @brief The role named in a config file.
 * @param text `person`, `no_helmet` or `no_vest`.
 * @return The role, or empty for any other text.
 */
std::optional<ClassRole> parseClassRole(std::string_view text);

/**
 * @brief The classes of this project's 6-class model: Helmet, NoHelmet (no_helmet), NoVest
 *        (no_vest), Vest, RegularHat, Person (person). Used when the config gives no classes.
 */
std::vector<ClassSpec> defaultClassSpecs();

/** @brief makeClassMap of defaultClassSpecs(), which always passes. */
ClassMap defaultClassMap();

/**
 * @brief Checks a class list and finds the roles.
 * @param specs The classes by index.
 * @param log Receives the reason of a refusal.
 * @return The map, or empty when the list is empty or longer than maxClasses, a name is empty,
 *         longer than maxClassNameChars or has a character other than a letter, a digit, `_` or
 *         `-`, or one of the roles person, no_helmet and no_vest is missing or given twice.
 */
std::optional<ClassMap> makeClassMap(const std::vector<ClassSpec>& specs, const Logger& log);

#endif // CLASS_MAP_H
