#include "config_json.h"

namespace {
constexpr unsigned char firstPrintable{0x20};

bool isSpace(char letter) {
    return letter == ' ' || letter == '\t' || letter == '\n' || letter == '\r';
}

bool isDigit(char letter) {
    return letter >= '0' && letter <= '9';
}

// A cursor over the text. Every reader stops at the first character it does not own.
class Reader {
public:
    explicit Reader(std::string_view text) : input{text} {}

    JsonParse run() {
        skipSpace();
        if (!take('{')) {
            return fail("expected {");
        }
        if (!readMembers("")) {
            return std::move(result);
        }
        skipSpace();
        if (more()) {
            return fail("text after the closing }");
        }
        result.ok = true;
        return std::move(result);
    }

private:
    // Records the failure and returns the result, so that run() can return it as it is.
    JsonParse fail(const char* why) {
        result.ok = false;
        result.offset = at;
        result.reason = why;
        return std::move(result);
    }

    // Records the failure at the current offset. Always false.
    bool refuse(const char* why) {
        result.ok = false;
        result.offset = at;
        result.reason = why;
        return false;
    }

    // The pairs of one object, after its opening brace, up to and including its closing brace.
    // `group` is empty for the top object.
    bool readMembers(const std::string& group) {
        skipSpace();
        if (take('}')) {
            return true;
        }
        while (true) {
            JsonEntry entry;
            entry.group = group;
            skipSpace();
            if (!readString(entry.key)) {
                return refuse("expected a key in double quotes");
            }
            skipSpace();
            if (!take(':')) {
                return refuse("expected : after the key");
            }
            skipSpace();
            if (more() && peek() == '{') {
                if (!readGroup(std::move(entry), group)) {
                    return false;
                }
            } else {
                if (!readValue(entry)) {
                    return refuse("expected a string, a number, true or false");
                }
                result.entries.push_back(std::move(entry));
            }
            skipSpace();
            if (take(',')) {
                continue;
            }
            if (take('}')) {
                return true;
            }
            return refuse("expected , or }");
        }
    }

    // A group named by `entry`, inside the object `parent`: its entry, then its members. Two levels
    // at most: a group of a group, but not a third one.
    bool readGroup(JsonEntry entry, const std::string& parent) {
        if (parent.find('.') != std::string::npos) {
            return refuse("a group inside a group of a group");
        }
        ++at;
        entry.kind = JsonKind::Object;
        const std::string name{parent.empty() ? entry.key : parent + "." + entry.key};
        result.entries.push_back(std::move(entry));
        return readMembers(name);
    }

    bool more() const {
        return at < input.size();
    }

    char peek() const {
        return input[at];
    }

    bool take(char expected) {
        if (more() && peek() == expected) {
            ++at;
            return true;
        }
        return false;
    }

    void skipSpace() {
        while (more() && isSpace(peek())) {
            ++at;
        }
    }

    // A string in double quotes without escapes: no backslash, no control character.
    bool readString(std::string& out) {
        if (!take('"')) {
            return false;
        }
        const std::size_t start{at};
        while (more() && peek() != '"') {
            if (peek() == '\\' || static_cast<unsigned char>(peek()) < firstPrintable) {
                return false;
            }
            ++at;
        }
        if (!more()) {
            return false;
        }
        out.assign(input.substr(start, at - start));
        ++at;
        return true;
    }

    bool readWord(std::string_view word) {
        if (input.substr(at, word.size()) != word) {
            return false;
        }
        at += word.size();
        return true;
    }

    // The number of the JSON grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    bool readNumber(std::string& out) {
        const std::size_t start{at};
        take('-');
        if (take('0')) {
            // a leading 0 stands alone
        } else if (more() && isDigit(peek())) {
            skipDigits();
        } else {
            return false;
        }
        if (take('.')) {
            if (!more() || !isDigit(peek())) {
                return false;
            }
            skipDigits();
        }
        if (take('e') || take('E')) {
            if (!take('+')) {
                take('-');
            }
            if (!more() || !isDigit(peek())) {
                return false;
            }
            skipDigits();
        }
        out.assign(input.substr(start, at - start));
        return true;
    }

    void skipDigits() {
        while (more() && isDigit(peek())) {
            ++at;
        }
    }

    bool readValue(JsonEntry& entry) {
        if (more() && peek() == '"') {
            entry.kind = JsonKind::String;
            return readString(entry.value);
        }
        if (readWord("true")) {
            entry.kind = JsonKind::Boolean;
            entry.value = "true";
            return true;
        }
        if (readWord("false")) {
            entry.kind = JsonKind::Boolean;
            entry.value = "false";
            return true;
        }
        entry.kind = JsonKind::Number;
        return readNumber(entry.value);
    }

    std::string_view input;
    std::size_t at{0};
    JsonParse result;
};
} // namespace

JsonParse parseConfigJson(std::string_view text) {
    return Reader{text}.run();
}
