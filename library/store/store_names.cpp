#include "store_names.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace {
constexpr std::size_t dayNameLength{10}; // YYYY-MM-DD
constexpr int firstMonth{1};
constexpr int lastMonth{12};
constexpr int lastDay{31};
constexpr int lastHour{23};
constexpr int lastMinute{59};
constexpr int lastSecond{60};
constexpr int yearBase{1900};
constexpr int hundred{100};
constexpr int dayCharsAt{8};
constexpr int monthCharsAt{5};
constexpr int hourPairAt{0};
constexpr int minutePairAt{2};
constexpr int secondPairAt{4};
constexpr int decimalBase{10};
constexpr std::size_t yearEnd{4}; // "YYYY" is day[0..4)
constexpr std::size_t firstDash{4};
constexpr std::size_t secondDash{7};
constexpr std::size_t monthStart{5};
constexpr std::size_t monthLength{2};
constexpr std::size_t dayStart{8};
constexpr std::size_t dayLength{2};

// The number written by the two digits of `text` that start at `start`.
int twoDigits(std::string_view text, int start) {
    const auto first{static_cast<std::size_t>(start)};
    return ((text[first] - '0') * decimalBase) + (text[first + 1] - '0');
}
} // namespace

bool allDigits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char letter) {
        return std::isdigit(static_cast<unsigned char>(letter)) != 0;
    });
}

std::optional<std::time_t> parseTime(std::string_view day, std::string_view name) {
    if (day.size() != dayNameLength || day[firstDash] != '-' || day[secondDash] != '-' ||
        !allDigits(day.substr(0, yearEnd)) || !allDigits(day.substr(monthStart, monthLength)) ||
        !allDigits(day.substr(dayStart, dayLength))) {
        return std::nullopt;
    }
    if (name.size() < timeNameLength || !allDigits(name.substr(0, timeNameLength))) {
        return std::nullopt;
    }
    std::tm parts{};
    parts.tm_year = ((twoDigits(day, 0) * hundred) + twoDigits(day, 2)) - yearBase;
    parts.tm_mon = twoDigits(day, monthCharsAt) - 1;
    parts.tm_mday = twoDigits(day, dayCharsAt);
    parts.tm_hour = twoDigits(name, hourPairAt);
    parts.tm_min = twoDigits(name, minutePairAt);
    parts.tm_sec = twoDigits(name, secondPairAt);
    parts.tm_isdst = -1;
    if (parts.tm_mon + 1 < firstMonth || parts.tm_mon + 1 > lastMonth || parts.tm_mday < 1 ||
        parts.tm_mday > lastDay || parts.tm_hour > lastHour || parts.tm_min > lastMinute ||
        parts.tm_sec > lastSecond) {
        return std::nullopt;
    }
    const std::time_t time{std::mktime(&parts)};
    if (time == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return time;
}
