#include "rga_labels.h"

#include <algorithm>
#include <array>
#include <utility>

#include "im2d.hpp"
#include "label_font.h"

namespace {
// The RGA takes a job of at most 50 tasks, a label is one.
constexpr std::size_t jobLabels{40};
constexpr std::string_view unknownName{"?"};

// Width alignment of the RGA buffers (the same 16 as the sweeps of bench_preprocess).
constexpr int widthAlignment{16};

// One slot of the atlas: as high as the longest label (maxLabelChars) and as wide, rounded up to
// the RGA width alignment.
int slotWidth() noexcept {
    const int width{labelSize(maxLabelChars).width};
    return ((width + widthAlignment - 1) / widthAlignment) * widthAlignment;
}

int slotHeight() noexcept {
    return labelSize(maxLabelChars).height;
}

std::size_t slotStride() noexcept {
    return static_cast<std::size_t>(slotWidth()) * labelPixelBytes;
}
} // namespace

RgaLabels::RgaLabels(const OverlayConfig& overlayConfig, LabelConfig labelConfig, Logger logger,
                     DmaBuf atlasBuffer, RgaImage atlasHandle)
    : overlay{overlayConfig}, labels{std::move(labelConfig)}, log{logger},
      atlas{std::move(atlasBuffer)}, atlasImage{std::move(atlasHandle)} {
    placed.reserve(labels.capacity);
}

std::optional<RgaLabels> RgaLabels::create(const OverlayConfig& overlay, const LabelConfig& labels,
                                           const Logger& log) {
    if (overlay.size.width <= 0 || overlay.size.height <= 0 ||
        overlay.stride < overlay.size.width || labels.capacity == 0 || labels.classNames.empty()) {
        log.error("bad label config: ", overlay.size.width, "x", overlay.size.height, " capacity ",
                  labels.capacity, " names ", labels.classNames.size());
        return std::nullopt;
    }
    const std::size_t bytes{labels.capacity * static_cast<std::size_t>(slotHeight()) *
                            slotStride()};
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{lowHeap, bytes}, log)};
    if (!buffer) {
        return std::nullopt;
    }
    std::optional<RgaImage> handle{RgaImage::import(*buffer, log)};
    if (!handle) {
        return std::nullopt;
    }
    return std::optional<RgaLabels>{
        RgaLabels{overlay, labels, log, std::move(*buffer), std::move(*handle)}};
}

// The style of a box: the violator style, the style of its class, or the unknown style.
const BoxStyle& RgaLabels::styleOf(const OverlayBox& box) const noexcept {
    if (box.violator) {
        return overlay.violatorStyle;
    }
    const bool known{box.classId >= 0 &&
                     static_cast<std::size_t>(box.classId) < overlay.classStyles.size()};
    if (!known) {
        return overlay.unknownStyle;
    }
    return overlay.classStyles[static_cast<std::size_t>(box.classId)];
}

std::string_view RgaLabels::nameOf(const OverlayBox& box) const noexcept {
    const bool known{box.classId >= 0 &&
                     static_cast<std::size_t>(box.classId) < labels.classNames.size()};
    return known ? labels.classNames[static_cast<std::size_t>(box.classId)] : unknownName;
}

// Renders the label of one box into the next slot of the atlas.
void RgaLabels::renderOne(const OverlayBox& box) noexcept {
    std::array<char, maxLabelChars + 1> text{};
    const std::size_t length{formatLabel(
        LabelInfo{nameOf(box), box.trackId, box.score, box.violator}, text.data(), text.size())};
    const BoxStyle& style{styleOf(box)};
    const LabelSize size{labelSize(length)};
    const bool below{box.violator || box.classId == labels.personClass};
    const std::size_t slot{placed.size()};
    std::uint8_t* pixels{static_cast<std::uint8_t*>(atlas.data()) +
                         (slot * static_cast<std::size_t>(slotHeight()) * slotStride())};
    renderLabel(std::string_view{text.data(), length},
                LabelColors{style.color, textColorFor(style.color)}, pixels, slotStride());
    placed.push_back(Placed{slot, size, placeLabel(box.box, below, size, overlay.size)});
}

// Renders each label into its slot, the violators last.
void RgaLabels::render(const std::vector<OverlayBox>& boxes) noexcept {
    for (const bool violators : {false, true}) {
        for (const OverlayBox& box : boxes) {
            if (box.violator != violators) {
                continue;
            }
            if (placed.size() >= labels.capacity) {
                if (overflow == 0) {
                    log.warn("label capacity ", labels.capacity, " is full, labels are not drawn");
                }
                ++overflow;
                continue;
            }
            renderOne(box);
        }
    }
}

// Copies the slots of the placed labels into the image, in jobs.
bool RgaLabels::blit(const RgaImage& target) noexcept {
    const rga_buffer_t source{wrapbuffer_handle(atlasImage.handle(), slotWidth(),
                                                static_cast<int>(labels.capacity) * slotHeight(),
                                                RK_FORMAT_RGBA_8888)};
    const rga_buffer_t image{wrapbuffer_handle(target.handle(), overlay.size.width,
                                               overlay.size.height, RK_FORMAT_YCbCr_420_SP,
                                               overlay.stride, overlay.size.height)};
    const rga_buffer_t noPattern{};
    const im_rect noRect{};
    // Labels may overlap and the later one must be on top. Tasks of a job that go to different
    // RGA cores run in no fixed order (measured: the order of two overlapping labels changed from
    // run to run), so every task goes to the RGA2, which runs them in the order they were added.
    im_opt_t sameCore{};
    sameCore.core = IM_SCHEDULER_RGA2_CORE0;
    std::size_t next{0};
    while (next < placed.size()) {
        const im_job_handle_t job{imbeginJob()};
        if (job == 0) {
            log.error("imbeginJob failed");
            return false;
        }
        const std::size_t end{std::min(next + jobLabels, placed.size())};
        for (; next < end; ++next) {
            const Placed& item{placed[next]};
            const im_rect fromAtlas{0, static_cast<int>(item.slot) * slotHeight(), item.size.width,
                                    item.size.height};
            const im_rect toImage{item.position.x, item.position.y, item.size.width,
                                  item.size.height};
            const IM_STATUS status{improcessTask(job, source, image, noPattern, fromAtlas, toImage,
                                                 noRect, &sameCore, IM_ALPHA_BLEND_SRC_OVER)};
            if (status != IM_STATUS_SUCCESS) {
                log.error("improcessTask: ", imStrError_t(status));
                (void)imcancelJob(job);
                return false;
            }
        }
        const IM_STATUS status{imendJob(job)};
        if (status != IM_STATUS_SUCCESS) {
            log.error("imendJob: ", imStrError_t(status));
            return false;
        }
    }
    return true;
}

bool RgaLabels::draw(const RgaImage& target, const std::vector<OverlayBox>& boxes) noexcept {
    placed.clear();
    if (boxes.empty()) {
        return true;
    }
    if (!atlas.beginCpuAccess()) {
        return false;
    }
    render(boxes);
    if (!atlas.endCpuAccess()) {
        return false;
    }
    return placed.empty() || blit(target);
}
