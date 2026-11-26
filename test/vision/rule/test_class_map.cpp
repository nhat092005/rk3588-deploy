// makeClassMap: the 6 classes of the project, a list of another size and order, and every list it
// refuses. Exit code 0 when every check passes.
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "class_map.h"
#include "label_text.h"
#include "logger.h"
#include "violation_rule.h"

namespace {
class Check {
public:
    explicit Check(const Logger& logger) : log{logger} {}

    void expect(bool condition, const char* what) {
        if (!condition) {
            log.error("FAIL: ", what);
            passed = false;
        }
    }

    bool ok() const {
        return passed;
    }

private:
    Logger log;
    bool passed{true};
};

std::vector<ClassSpec> fiveClasses() {
    return {{"Helmet", ClassRole::None},
            {"NoHelmet", ClassRole::NoHelmet},
            {"NoVest", ClassRole::NoVest},
            {"Vest", ClassRole::None},
            {"Person", ClassRole::Person}};
}

void checkAccepted(Check& check, const Logger& quiet) {
    const ClassMap six{defaultClassMap()};
    check.expect(six.names.size() == 6 && six.names[4] == "RegularHat" && six.names[5] == "Person",
                 "the default list has 6 classes, RegularHat at 4 and Person at 5");
    check.expect(six.person == 5 && six.noHelmet == 1 && six.noVest == 2,
                 "the default roles are at 5, 1 and 2");
    check.expect(six.person == defaultPersonClass && six.noHelmet == defaultNoHelmetClass &&
                     six.noVest == defaultNoVestClass,
                 "the default roles are the defaults of the violation rule");
    const std::optional<ClassMap> five{makeClassMap(fiveClasses(), quiet)};
    check.expect(five && five->names.size() == 5 && five->person == 4 && five->noHelmet == 1 &&
                     five->noVest == 2,
                 "a 5-class list has Person where it is put");
    std::vector<ClassSpec> shuffled{{"Person", ClassRole::Person},
                                    {"a", ClassRole::NoVest},
                                    {"b_2-c", ClassRole::None},
                                    {"c", ClassRole::NoHelmet}};
    const std::optional<ClassMap> moved{makeClassMap(shuffled, quiet)};
    check.expect(moved && moved->person == 0 && moved->noVest == 1 && moved->noHelmet == 3,
                 "the roles follow the position, not a fixed order");
    const std::optional<ClassMap> longest{
        makeClassMap({{std::string(maxClassNameChars, 'x'), ClassRole::Person},
                      {"a", ClassRole::NoHelmet},
                      {"b", ClassRole::NoVest}},
                     quiet)};
    check.expect(longest.has_value(), "a name of the longest length");
}

void checkRefused(Check& check, const Logger& quiet) {
    check.expect(!makeClassMap({}, quiet), "an empty list");
    std::vector<ClassSpec> noPerson{fiveClasses()};
    noPerson[4].role = ClassRole::None;
    check.expect(!makeClassMap(noPerson, quiet), "no Person role");
    std::vector<ClassSpec> noHelmet{fiveClasses()};
    noHelmet[1].role = ClassRole::None;
    check.expect(!makeClassMap(noHelmet, quiet), "no no_helmet role");
    std::vector<ClassSpec> noVest{fiveClasses()};
    noVest[2].role = ClassRole::None;
    check.expect(!makeClassMap(noVest, quiet), "no no_vest role");
    std::vector<ClassSpec> twice{fiveClasses()};
    twice[3].role = ClassRole::NoVest;
    check.expect(!makeClassMap(twice, quiet), "a role given to two classes");
    std::vector<ClassSpec> empty{fiveClasses()};
    empty[0].name.clear();
    check.expect(!makeClassMap(empty, quiet), "an empty name");
    std::vector<ClassSpec> tooLong{fiveClasses()};
    tooLong[0].name = std::string(maxClassNameChars + 1, 'x');
    check.expect(!makeClassMap(tooLong, quiet), "a name longer than the label can hold");
    std::vector<ClassSpec> badChar{fiveClasses()};
    badChar[0].name = "Hard hat";
    check.expect(!makeClassMap(badChar, quiet), "a space in a name");
    badChar[0].name = "M\xC3\xBC";
    check.expect(!makeClassMap(badChar, quiet), "a non ASCII name");
    std::vector<ClassSpec> many(maxClasses + 1, ClassSpec{"x", ClassRole::None});
    many[0].role = ClassRole::Person;
    many[1].role = ClassRole::NoHelmet;
    many[2].role = ClassRole::NoVest;
    check.expect(!makeClassMap(many, quiet), "more classes than the limit");
}

void checkRoleNames(Check& check) {
    check.expect(parseClassRole("person") == ClassRole::Person &&
                     parseClassRole("no_helmet") == ClassRole::NoHelmet &&
                     parseClassRole("no_vest") == ClassRole::NoVest,
                 "the three role names");
    check.expect(!parseClassRole("Person") && !parseClassRole("") && !parseClassRole("helmet"),
                 "other role names are refused");
}

// The longest label: a violator, a 5 digit track id, the longest name and 100%.
void checkNameFitsLabel(Check& check) {
    const std::string name(maxClassNameChars, 'x');
    char text[maxLabelChars + 1]{};
    const std::size_t length{formatLabel(LabelInfo{name, 12345, 1.0F, true}, text, sizeof(text))};
    const std::string_view label{text, length};
    check.expect(label.size() == maxLabelChars && label.substr(label.size() - 4) == "100%",
                 "the longest name still leaves room for the percent");
}
} // namespace

int main() {
    const Logger log{"TEST_CLASS_MAP", LogLevel::Info};
    const Logger quiet{"TEST_CLASS_MAP", LogLevel::None};
    Check check{log};
    checkAccepted(check, quiet);
    checkRefused(check, quiet);
    checkRoleNames(check);
    checkNameFitsLabel(check);
    if (!check.ok()) {
        return EXIT_FAILURE;
    }
    log.info("PASS");
    return EXIT_SUCCESS;
}
