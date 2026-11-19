// parseConfigJson: the objects it reads (strings, numbers, booleans, spaces, an empty object, a
// repeated key) and the texts it refuses, with the offset of the failure. Exit code 0 when every
// check passes.
#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#include "config_json.h"
#include "logger.h"

namespace {
constexpr std::size_t mixedPairs{6};
constexpr std::size_t exponentPairs{3};
constexpr std::size_t trueAt{4};
constexpr std::size_t falseAt{5};
// Places in the entries of checkGroups and of its second text.
constexpr std::size_t emptyGroupAt{4};
constexpr std::size_t recordGroupAt{5};
constexpr std::size_t enabledAt{6};
constexpr std::size_t secondPartAt{4};
constexpr std::size_t secondPartEntries{5};

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

void checkValues(Check& check) {
    const JsonParse parse{parseConfigJson(
        R"({"model": "a/b.rknn", "width": 3840, "gap": -0.5, "big": 1.5e+3, "record": true, "off": false})")};
    check.expect(parse.ok, "a mixed object is read");
    check.expect(parse.entries.size() == mixedPairs, "six pairs");
    if (parse.entries.size() != mixedPairs) {
        return;
    }
    check.expect(parse.entries[0].key == "model" && parse.entries[0].value == "a/b.rknn" &&
                     parse.entries[0].kind == JsonKind::String,
                 "a string");
    check.expect(parse.entries[1].value == "3840" && parse.entries[1].kind == JsonKind::Number,
                 "an integer");
    check.expect(parse.entries[2].value == "-0.5" && parse.entries[2].kind == JsonKind::Number,
                 "a negative real");
    check.expect(parse.entries[3].value == "1.5e+3" && parse.entries[3].kind == JsonKind::Number,
                 "an exponent");
    check.expect(parse.entries[trueAt].value == "true" &&
                     parse.entries[trueAt].kind == JsonKind::Boolean,
                 "true");
    check.expect(parse.entries[falseAt].value == "false" &&
                     parse.entries[falseAt].kind == JsonKind::Boolean,
                 "false");
}

void checkGroups(Check& check) {
    const JsonParse parse{parseConfigJson(
        R"({"model": "m", "camera": {"width": 3840, "node": "/dev/video11"}, "empty": {}, "record": {"enabled": true}})")};
    check.expect(parse.ok, "a top key and groups");
    constexpr std::size_t groupedEntries{7};
    check.expect(parse.entries.size() == groupedEntries, "seven entries: pairs and group starts");
    if (parse.entries.size() != groupedEntries) {
        return;
    }
    check.expect(parse.entries[0].group.empty() && parse.entries[0].key == "model",
                 "a top pair has no group");
    check.expect(parse.entries[1].kind == JsonKind::Object && parse.entries[1].key == "camera" &&
                     parse.entries[1].group.empty() && parse.entries[1].value.empty(),
                 "a group starts with an Object entry");
    check.expect(parse.entries[2].group == "camera" && parse.entries[2].key == "width" &&
                     parse.entries[2].value == "3840",
                 "a pair of a group carries the group");
    check.expect(parse.entries[3].group == "camera" && parse.entries[3].key == "node",
                 "the second pair of the group");
    check.expect(parse.entries[emptyGroupAt].kind == JsonKind::Object &&
                     parse.entries[emptyGroupAt].key == "empty",
                 "an empty group still appears");
    check.expect(parse.entries[recordGroupAt].kind == JsonKind::Object &&
                     parse.entries[recordGroupAt].key == "record",
                 "the last group starts");
    check.expect(parse.entries[enabledAt].group == "record" &&
                     parse.entries[enabledAt].key == "enabled" &&
                     parse.entries[enabledAt].kind == JsonKind::Boolean,
                 "a boolean in the last group");
    const JsonParse back{parseConfigJson(R"({"a": {"x": 1}, "b": 2, "a": {"y": 3}})")};
    check.expect(back.ok && back.entries.size() == secondPartEntries &&
                     back.entries[secondPartAt].group == "a" &&
                     back.entries[secondPartAt].key == "y",
                 "a group given twice, and a top pair after a group, keep their place");
}

void checkNestedGroups(Check& check) {
    const JsonParse parse{
        parseConfigJson(R"({"v": {"dir": "/d", "clip": {"enabled": true}, "keep": 3}})")};
    check.expect(parse.ok, "a group inside a group");
    constexpr std::size_t nestedEntries{5};
    if (!parse.ok || parse.entries.size() != nestedEntries) {
        check.expect(false, "five entries: two group starts and three pairs");
        return;
    }
    check.expect(parse.entries[0].kind == JsonKind::Object && parse.entries[0].key == "v" &&
                     parse.entries[0].group.empty(),
                 "the outer group starts at the top");
    check.expect(parse.entries[2].kind == JsonKind::Object && parse.entries[2].key == "clip" &&
                     parse.entries[2].group == "v",
                 "the inner group starts in the outer one");
    check.expect(parse.entries[3].group == "v.clip" && parse.entries[3].key == "enabled" &&
                     parse.entries[3].kind == JsonKind::Boolean,
                 "a pair of the inner group carries both names");
    check.expect(parse.entries[4].group == "v" && parse.entries[4].key == "keep",
                 "a pair after the inner group is back in the outer one");
}

void checkShapes(Check& check) {
    check.expect(parseConfigJson("{}").ok && parseConfigJson("{}").entries.empty(), "empty object");
    check.expect(parseConfigJson("  \n\t{ }  \n").ok, "spaces around an empty object");
    check.expect(parseConfigJson("{\"a\":1}").ok, "no spaces");
    check.expect(parseConfigJson("{\r\n  \"a\" :\t1 ,\r\n  \"b\" : \"x\"\r\n}\r\n").ok,
                 "spaces and line breaks between the tokens");
    const JsonParse twice{parseConfigJson(R"({"a": 1, "a": 2})")};
    check.expect(twice.ok && twice.entries.size() == 2 && twice.entries[1].value == "2",
                 "a key given twice is kept twice, in order");
    const JsonParse empty{parseConfigJson(R"({"a": ""})")};
    check.expect(empty.ok && empty.entries[0].value.empty(), "an empty string");
    const JsonParse utf8{parseConfigJson("{\"a\": \"d\xC3\xA9\"}")};
    check.expect(utf8.ok && utf8.entries[0].value == "d\xC3\xA9", "UTF-8 bytes pass through");
    const JsonParse zero{parseConfigJson(R"({"a": 0, "b": 0.5, "c": -0})")};
    check.expect(zero.ok, "0, 0.5 and -0");
    const JsonParse exponent{parseConfigJson(R"({"a": 1E5, "b": 2e-2, "c": 3.5E+1})")};
    check.expect(exponent.ok && exponent.entries.size() == exponentPairs, "E, e- and E+ exponents");
}

struct Bad {
    std::string_view text;
    std::size_t offset;
    const char* what;
};

void checkRefusals(Check& check) {
    constexpr std::array<Bad, 33> cases{{
        {"", 0, "empty text"},
        {"   ", 3, "only spaces"},
        {"[]", 0, "an array"},
        {"\"a\"", 0, "a string at the top"},
        {"{", 1, "no closing brace"},
        {"{\"a\"", 4, "a key and nothing else"},
        {"{\"a\" 1}", 5, "no colon"},
        {"{\"a\":}", 5, "no value"},
        {"{\"a\":1,}", 7, "a comma before the closing brace"},
        {"{,}", 1, "a lone comma"},
        {"{\"a\":1 \"b\":2}", 7, "no comma between the pairs"},
        {"{a: 1}", 1, "a key without quotes"},
        {"{'a': 1}", 1, "a key in single quotes"},
        {"{\"a\": 'x'}", 6, "a value in single quotes"},
        {"{\"a\": null}", 6, "null"},
        {"{\"a\": [1]}", 6, "an array value"},
        {"{\"a\": {\"b\": {\"c\": {\"d\": 1}}}}", 18, "a third level of groups"},
        {"{\"a\": {\"b\": 1,}}", 14, "a comma before the end of a group"},
        {"{\"a\": {\"b\": 1}", 14, "a group that is not closed"},
        {"{\"a\": {b: 1}}", 7, "a key without quotes in a group"},
        {"{\"a\": \"x\\ny\"}", 8, "a backslash in a string"},
        {"{\"a\": \"x\ny\"}", 8, "a line break in a string"},
        {"{\"a\": \"abc}", 11, "a string that does not end"},
        {"{\"a\": 01}", 7, "a leading zero"},
        {"{\"a\": +1}", 6, "a plus sign"},
        {"{\"a\": 1.}", 8, "a point without digits"},
        {"{\"a\": .5}", 6, "a number that starts with a point"},
        {"{\"a\": 1e}", 8, "an exponent without digits"},
        {"{\"a\": -}", 7, "a minus alone"},
        {"{\"a\": tru}", 6, "a truncated true"},
        {"{\"a\": True}", 6, "True with a capital"},
        {"{\"a\": 1} x", 9, "text after the object"},
    }};
    for (const Bad& bad : cases) {
        const JsonParse parse{parseConfigJson(bad.text)};
        check.expect(!parse.ok, bad.what);
        if (!parse.ok) {
            check.expect(parse.offset == bad.offset, bad.what);
        }
    }
    const JsonParse twoObjects{parseConfigJson("{} {}")};
    check.expect(!twoObjects.ok, "two objects");
}
} // namespace

int main() {
    const Logger log{"TEST_CONFIG_JSON", LogLevel::Info};
    Check check{log};
    checkValues(check);
    checkGroups(check);
    checkNestedGroups(check);
    checkShapes(check);
    checkRefusals(check);
    if (!check.ok()) {
        return EXIT_FAILURE;
    }
    log.info("PASS");
    return EXIT_SUCCESS;
}
