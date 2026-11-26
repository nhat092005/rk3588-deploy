#include "rga_overlay.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "im2d.hpp"

namespace {
// The RGA takes a job of at most 50 tasks and an outline needs 4 of them.
constexpr std::size_t jobBoxes{12};
constexpr int minBoxSide{4};
constexpr int evenMask{1};
constexpr int checkBlockSide{4};
constexpr std::uint32_t blackColor{0U};
constexpr int filled{-1};

// Rounds down to an even number.
int evenDown(double value) noexcept {
    return static_cast<int>(std::floor(value)) & ~evenMask;
}

// Rounds up to an even number.
int evenUp(double value) noexcept {
    const int rounded{static_cast<int>(std::ceil(value))};
    return (rounded + evenMask) & ~evenMask;
}

constexpr std::array<std::uint32_t, 3> plainColors{helmetColor, vestColor, regularHatColor};

bool sameStyle(const BoxStyle& first, const BoxStyle& second) noexcept {
    return first.color == second.color && first.thickness == second.thickness;
}
} // namespace

std::vector<BoxStyle> classStylesFor(std::size_t count, int person, int noHelmet, int noVest) {
    std::vector<BoxStyle> styles;
    styles.reserve(count);
    std::size_t plain{0};
    for (std::size_t index{0}; index < count; ++index) {
        std::uint32_t color{unknownColor};
        const auto classId{static_cast<int>(index)};
        if (classId == person) {
            color = personColor;
        } else if (classId == noHelmet) {
            color = noHelmetColor;
        } else if (classId == noVest) {
            color = noVestColor;
        } else if (plain < plainColors.size()) {
            color = plainColors[plain++];
        }
        styles.push_back(BoxStyle{color, defaultBoxThickness});
    }
    return styles;
}

RgaOverlay::RgaOverlay(const OverlayConfig& config, const Logger& logger)
    : settings{config}, log{logger} {
    placed.reserve(config.capacity);
}

std::optional<RgaOverlay> RgaOverlay::create(const OverlayConfig& config, const Logger& log) {
    const auto badStyle{[](const BoxStyle& style) { return style.thickness <= 0; }};
    if (config.size.width <= 0 || config.size.height <= 0 ||
        ((config.size.width | config.size.height) & evenMask) != 0 ||
        config.stride < config.size.width || config.capacity == 0 ||
        badStyle(config.violatorStyle) || badStyle(config.unknownStyle) ||
        std::any_of(config.classStyles.begin(), config.classStyles.end(), badStyle)) {
        log.error("bad overlay config: ", config.size.width, "x", config.size.height, " stride ",
                  config.stride, " capacity ", config.capacity);
        return std::nullopt;
    }
    return std::optional<RgaOverlay>{RgaOverlay{config, log}};
}

bool RgaOverlay::check(const RgaImage& target) const noexcept {
    const rga_buffer_t buffer{wrapbuffer_handle(target.handle(), settings.size.width,
                                                settings.size.height, RK_FORMAT_YCbCr_420_SP,
                                                settings.stride, settings.size.height)};
    const IM_STATUS status{
        imrectangle(buffer, im_rect{0, 0, checkBlockSide, checkBlockSide}, blackColor, filled)};
    if (status != IM_STATUS_SUCCESS) {
        log.error("the RGA2 refuses the buffer (", imStrError_t(status),
                  "): it may have pages above 4 GB, take it from the lowHeap dma-heap");
        return false;
    }
    return true;
}

const BoxStyle& RgaOverlay::styleOf(const OverlayBox& box) const noexcept {
    if (box.violator) {
        return settings.violatorStyle;
    }
    const bool known{box.classId >= 0 &&
                     static_cast<std::size_t>(box.classId) < settings.classStyles.size()};
    return known ? settings.classStyles[static_cast<std::size_t>(box.classId)]
                 : settings.unknownStyle;
}

// Rounds the box to even pixels, clips it, and keeps it when it is big enough.
bool RgaOverlay::place(const OverlayBox& box) noexcept {
    const int left{std::max(evenDown(static_cast<double>(box.box.left)), 0)};
    const int top{std::max(evenDown(static_cast<double>(box.box.top)), 0)};
    const int right{std::min(evenUp(static_cast<double>(box.box.right)), settings.size.width)};
    const int bottom{std::min(evenUp(static_cast<double>(box.box.bottom)), settings.size.height)};
    if (right - left < minBoxSide || bottom - top < minBoxSide) {
        return false;
    }
    placed.push_back(Placed{left, top, right - left, bottom - top, styleOf(box), box.violator});
    return true;
}

bool RgaOverlay::draw(const RgaImage& target, const std::vector<OverlayBox>& boxes) noexcept {
    placed.clear();
    for (const OverlayBox& box : boxes) {
        if (placed.size() >= settings.capacity) {
            if (overflow == 0) {
                log.warn("overlay capacity ", settings.capacity, " is full, boxes are not drawn");
            }
            ++overflow;
            continue;
        }
        (void)place(box);
    }
    if (placed.empty()) {
        return true;
    }
    return submit(target);
}

// Orders the boxes (violators last, equal styles together), then sends them in jobs.
bool RgaOverlay::submit(const RgaImage& target) noexcept {
    std::stable_sort(placed.begin(), placed.end(), [](const Placed& first, const Placed& second) {
        if (first.violator != second.violator) {
            return !first.violator;
        }
        if (first.style.color != second.style.color) {
            return first.style.color < second.style.color;
        }
        return first.style.thickness < second.style.thickness;
    });
    const rga_buffer_t buffer{wrapbuffer_handle(target.handle(), settings.size.width,
                                                settings.size.height, RK_FORMAT_YCbCr_420_SP,
                                                settings.stride, settings.size.height)};
    std::size_t next{0};
    while (next < placed.size()) {
        const im_job_handle_t job{imbeginJob()};
        if (job == 0) {
            log.error("imbeginJob failed");
            return false;
        }
        std::size_t inJob{0};
        while (next < placed.size() && inJob < jobBoxes) {
            // The run of boxes with the same style, as many as the job still takes.
            std::size_t end{next};
            std::array<im_rect, jobBoxes> rects{};
            while (end < placed.size() && inJob + (end - next) < jobBoxes &&
                   sameStyle(placed[end].style, placed[next].style)) {
                const Placed& item{placed[end]};
                rects.at(end - next) = im_rect{item.left, item.top, item.width, item.height};
                ++end;
            }
            const BoxStyle& style{placed[next].style};
            const IM_STATUS status{imrectangleTaskArray(job, buffer, rects.data(),
                                                        static_cast<int>(end - next), style.color,
                                                        style.thickness)};
            if (status != IM_STATUS_SUCCESS) {
                log.error("imrectangleTaskArray: ", imStrError_t(status));
                (void)imcancelJob(job);
                return false;
            }
            inJob += end - next;
            next = end;
        }
        const IM_STATUS status{imendJob(job)};
        if (status != IM_STATUS_SUCCESS) {
            log.error("imendJob: ", imStrError_t(status));
            return false;
        }
    }
    return true;
}
