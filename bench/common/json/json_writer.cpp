#include "json_writer.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>

namespace {
constexpr std::size_t numberBuffer{32};
constexpr unsigned char firstPrintable{0x20};
constexpr int hexBase{16};
constexpr int hexDigits{4};
constexpr std::size_t indentWidth{2};
} // namespace

void JsonWriter::newline() {
    out += '\n';
    out.append(first.size() * indentWidth, ' ');
}

void JsonWriter::close(char bracket) {
    const bool empty{first.back()};
    first.pop_back();
    if (!empty) {
        newline();
    }
    out += bracket;
}

void JsonWriter::separator() {
    if (afterKey) {
        afterKey = false;
        return;
    }
    if (!first.empty()) {
        if (!first.back()) {
            out += ',';
        }
        first.back() = false;
        newline();
    }
}

void JsonWriter::quoted(std::string_view value) {
    out += '"';
    for (const char symbol : value) {
        const auto code{static_cast<unsigned char>(symbol)};
        if (symbol == '"' || symbol == '\\') {
            out += '\\';
            out += symbol;
        } else if (code < firstPrintable) {
            // \u00XX: the rest of the control characters.
            constexpr std::string_view digits{"0123456789abcdef"};
            out += "\\u";
            for (int shift{(hexDigits - 1) * 4}; shift >= 0; shift -= 4) {
                out += digits.at(static_cast<std::size_t>((code >> shift) % hexBase));
            }
        } else {
            out += symbol;
        }
    }
    out += '"';
}

void JsonWriter::beginObject() {
    separator();
    out += '{';
    first.push_back(true);
}

void JsonWriter::endObject() {
    close('}');
}

void JsonWriter::beginArray() {
    separator();
    out += '[';
    first.push_back(true);
}

void JsonWriter::endArray() {
    close(']');
}

void JsonWriter::key(std::string_view name) {
    separator();
    quoted(name);
    out += ": ";
    afterKey = true;
}

void JsonWriter::text(std::string_view value) {
    separator();
    quoted(value);
}

void JsonWriter::number(double value) {
    separator();
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    std::array<char, numberBuffer> buffer{};
    const std::to_chars_result result{
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value)};
    out.append(buffer.data(), result.ptr);
}

void JsonWriter::integer(long long value) {
    separator();
    out += std::to_string(value);
}

void JsonWriter::boolean(bool value) {
    separator();
    out += value ? "true" : "false";
}

bool writeTextFile(const std::string& path, std::string_view content, const Logger& log) {
    std::ofstream file{path, std::ios::binary};
    file << content << '\n';
    file.close();
    if (!file) {
        log.error("writing ", path, " failed");
        return false;
    }
    return true;
}
