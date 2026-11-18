#include "hevc_nal.h"

namespace {
constexpr std::size_t startCodeBytes{3};
constexpr std::uint8_t nalTypeMask{0x3F};
constexpr unsigned nalTypeShift{1U};
} // namespace

bool hasNalType(const std::uint8_t* data, std::size_t length, NalType type) noexcept {
    for (std::size_t at{0}; at + startCodeBytes < length; ++at) {
        if (data[at] != 0 || data[at + 1] != 0 || data[at + 2] != 1) {
            continue;
        }
        const auto found{
            static_cast<std::uint8_t>((data[at + startCodeBytes] >> nalTypeShift) & nalTypeMask)};
        if (found == static_cast<std::uint8_t>(type)) {
            return true;
        }
    }
    return false;
}

bool hasKeyFrame(const std::uint8_t* data, std::size_t length) noexcept {
    return hasNalType(data, length, NalType::IdrWithRadl) ||
           hasNalType(data, length, NalType::IdrNoRadl);
}
