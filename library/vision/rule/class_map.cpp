#include "class_map.h"

#include <array>
#include <utility>

namespace {
struct RoleName {
    ClassRole role;
    std::string_view name;
};

constexpr std::array<RoleName, 3> roleNames{{
    {ClassRole::Person, "person"},
    {ClassRole::NoHelmet, "no_helmet"},
    {ClassRole::NoVest, "no_vest"},
}};

bool isNameChar(char letter) {
    return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
           (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
}

bool validName(const std::string& name) {
    if (name.empty() || name.size() > maxClassNameChars) {
        return false;
    }
    for (const char letter : name) {
        if (!isNameChar(letter)) {
            return false;
        }
    }
    return true;
}

std::string_view roleText(ClassRole role) {
    for (const RoleName& entry : roleNames) {
        if (entry.role == role) {
            return entry.name;
        }
    }
    return "none";
}

// The index of a role in the map, set at the first class that has it. false when it was already
// set.
bool place(int& slot, std::size_t index) {
    if (slot >= 0) {
        return false;
    }
    slot = static_cast<int>(index);
    return true;
}
} // namespace

std::optional<ClassRole> parseClassRole(std::string_view text) {
    for (const RoleName& entry : roleNames) {
        if (entry.name == text) {
            return entry.role;
        }
    }
    return std::nullopt;
}

std::vector<ClassSpec> defaultClassSpecs() {
    return {{"Helmet", ClassRole::None},     {"NoHelmet", ClassRole::NoHelmet},
            {"NoVest", ClassRole::NoVest},   {"Vest", ClassRole::None},
            {"RegularHat", ClassRole::None}, {"Person", ClassRole::Person}};
}

ClassMap defaultClassMap() {
    const Logger log{"CLASS_MAP", LogLevel::None};
    return makeClassMap(defaultClassSpecs(), log).value_or(ClassMap{});
}

std::optional<ClassMap> makeClassMap(const std::vector<ClassSpec>& specs, const Logger& log) {
    if (specs.empty() || specs.size() > maxClasses) {
        log.error("classes: ", specs.size(), " given, from 1 to ", maxClasses, " are accepted");
        return std::nullopt;
    }
    ClassMap map;
    for (std::size_t index{0}; index < specs.size(); ++index) {
        const ClassSpec& spec{specs[index]};
        if (!validName(spec.name)) {
            log.error("classes: the name of class ", index, " (\"", spec.name, "\") must be 1 to ",
                      maxClassNameChars, " characters: letters, digits, _ or -");
            return std::nullopt;
        }
        map.names.push_back(spec.name);
        bool placed{true};
        switch (spec.role) {
        case ClassRole::Person:
            placed = place(map.person, index);
            break;
        case ClassRole::NoHelmet:
            placed = place(map.noHelmet, index);
            break;
        case ClassRole::NoVest:
            placed = place(map.noVest, index);
            break;
        case ClassRole::None:
            break;
        }
        if (!placed) {
            log.error("classes: the role ", roleText(spec.role),
                      " is given to more than one class");
            return std::nullopt;
        }
    }
    const std::array<std::pair<int, ClassRole>, 3> required{{{map.person, ClassRole::Person},
                                                             {map.noHelmet, ClassRole::NoHelmet},
                                                             {map.noVest, ClassRole::NoVest}}};
    for (const auto& [slot, role] : required) {
        if (slot < 0) {
            log.error("classes: no class has the role ", roleText(role));
            return std::nullopt;
        }
    }
    return map;
}
