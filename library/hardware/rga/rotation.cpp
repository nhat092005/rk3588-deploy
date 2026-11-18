#include "rotation.h"

#include <string_view>

std::optional<Rotation> parseRotation(const char* text) noexcept {
    const std::string_view name{text};
    if (name == "0") {
        return Rotation::None;
    }
    if (name == "90") {
        return Rotation::Rotate90;
    }
    if (name == "180") {
        return Rotation::Rotate180;
    }
    if (name == "270") {
        return Rotation::Rotate270;
    }
    return std::nullopt;
}
