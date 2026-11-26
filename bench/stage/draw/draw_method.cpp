#include "draw_method.h"

#include <algorithm>
#include <cmath>

#include "im2d.hpp"

namespace {
constexpr int evenMask{1};
constexpr int cvThreads{8};
constexpr int chromaScale{2};
constexpr std::uint32_t lcgMultiplier{1664525U};
constexpr std::uint32_t lcgIncrement{1013904223U};
constexpr int lcgShift{8};
constexpr double lcgRange{16777216.0}; // 2^24: the values of next() after the shift
constexpr double minWidthShare{0.03};
constexpr double widthSpan{0.12};
constexpr double minHeightShare{0.10};
constexpr double heightSpan{0.40};
constexpr int minFrameSide{64};

// A linear congruential generator: the same boxes on every build and board.
class Lcg {
public:
    double unit() noexcept {
        state = (state * lcgMultiplier) + lcgIncrement;
        return static_cast<double>(state >> lcgShift) / lcgRange;
    }

private:
    std::uint32_t state{12345U};
};

int evenOf(double value) noexcept {
    return static_cast<int>(std::floor(value)) & ~evenMask;
}

// The colour as the RGA2 gets it (0xBBGGRR), in BT.601 limited range (Y 16 to 235, Cb and Cr
// 16 to 240) with the usual 8-bit integer matrix. It gives the bytes that the RGA2 writes for
// each of the six class colours (checked on the board); a seventh colour is not checked.
struct Yuv {
    std::uint8_t luma;
    std::uint8_t cb;
    std::uint8_t cr;
};

// Three numbers, one per colour channel: a row of the 8-bit integer matrix of BT.601 limited
// range, or a colour in the order red, green, blue.
struct Row {
    int red;
    int green;
    int blue;
};
constexpr Row lumaRow{66, 129, 25};
constexpr Row cbRow{-38, -74, 112};
constexpr Row crRow{112, -94, -18};

// One output byte: the row applied to the colour, rounded, plus the offset.
std::uint8_t channelOf(const Row& row, const Row& color, int offset) noexcept {
    constexpr int round{128};
    constexpr int shift{8};
    const int sum{(row.red * color.red) + (row.green * color.green) + (row.blue * color.blue) +
                  round};
    return static_cast<std::uint8_t>((sum >> shift) + offset);
}

Yuv yuvOf(std::uint32_t color) noexcept {
    constexpr int lumaBlack{16};
    constexpr int chromaMid{128};
    const Row rgb{static_cast<int>(color & 0xFFU), static_cast<int>((color >> 8U) & 0xFFU),
                  static_cast<int>((color >> 16U) & 0xFFU)};
    return Yuv{channelOf(lumaRow, rgb, lumaBlack), channelOf(cbRow, rgb, chromaMid),
               channelOf(crRow, rgb, chromaMid)};
}

// The rows [begin, end) of a plane that one call may write.
struct Rows {
    int begin{0};
    int end{0};
};

void fill(cv::Mat& plane, const cv::Rect& rect, const cv::Scalar& color, Rows rows) {
    const int top{std::max(rect.y, rows.begin)};
    const int bottom{std::min(rect.y + rect.height, rows.end)};
    if (bottom <= top || rect.width <= 0) {
        return;
    }
    plane(cv::Rect{rect.x, top, rect.width, bottom - top}).setTo(color);
}

// The outline: `line` pixels wide, inside the rectangle, as four bands.
void outline(cv::Mat& plane, const cv::Rect& box, int line, const cv::Scalar& color, Rows rows) {
    fill(plane, cv::Rect{box.x, box.y, box.width, line}, color, rows);
    fill(plane, cv::Rect{box.x, box.y + box.height - line, box.width, line}, color, rows);
    fill(plane, cv::Rect{box.x, box.y + line, line, box.height - (2 * line)}, color, rows);
    fill(plane, cv::Rect{box.x + box.width - line, box.y + line, line, box.height - (2 * line)},
         color, rows);
}

// The colour of a box (0xBBGGRR): the style of its class.
std::uint32_t colorOf(const DrawBox& box) noexcept {
    return defaultClassStyles.at(static_cast<std::size_t>(box.classId) % defaultClassStyles.size())
        .color;
}

// Draws every box into the rows [lumaRows) of the Y plane and the matching rows of the Cb Cr
// plane (a chroma row covers two luma rows, a chroma sample two columns).
void drawRows(cv::Mat& luma, cv::Mat& chroma, const std::vector<DrawBox>& boxes, Rows lumaRows) {
    const Rows chromaRows{lumaRows.begin / chromaScale, lumaRows.end / chromaScale};
    for (const DrawBox& box : boxes) {
        const Yuv color{yuvOf(colorOf(box))};
        outline(luma, cv::Rect{box.left, box.top, box.width, box.height}, defaultBoxThickness,
                cv::Scalar{static_cast<double>(color.luma)}, lumaRows);
        outline(chroma,
                cv::Rect{box.left / chromaScale, box.top / chromaScale, box.width / chromaScale,
                         box.height / chromaScale},
                defaultBoxThickness / chromaScale,
                cv::Scalar{static_cast<double>(color.cb), static_cast<double>(color.cr)},
                chromaRows);
    }
}

// The planes of an NV12 frame of `size` over the rows of `frame`.
struct Planes {
    cv::Mat luma;
    cv::Mat chroma;
};

Planes planesOf(cv::Mat& frame, ImageSize size) {
    return Planes{cv::Mat(size.height, size.width, CV_8UC1, frame.data, frame.step),
                  cv::Mat(size.height / chromaScale, size.width / chromaScale, CV_8UC2,
                          frame.data + (static_cast<std::size_t>(size.height) * frame.step),
                          frame.step)};
}
} // namespace

std::string_view drawLabel(DrawKind kind) noexcept {
    switch (kind) {
    case DrawKind::Cv1:
        return "cv1";
    case DrawKind::Cv8:
        return "cv8";
    case DrawKind::Rga2:
        break;
    }
    return "R2";
}

std::vector<DrawBox> makeDrawBoxes(ImageSize size, int count) {
    std::vector<DrawBox> boxes;
    if (size.width < minFrameSide || size.height < minFrameSide || count <= 0) {
        return boxes;
    }
    Lcg random;
    for (int index{0}; index < count; ++index) {
        DrawBox box{};
        box.width = evenOf(size.width * (minWidthShare + (widthSpan * random.unit())));
        box.height = evenOf(size.height * (minHeightShare + (heightSpan * random.unit())));
        box.left = evenOf((size.width - box.width) * random.unit());
        box.top = evenOf((size.height - box.height) * random.unit());
        box.classId = index % static_cast<int>(defaultClassStyles.size());
        boxes.push_back(box);
    }
    return boxes;
}

std::optional<DrawMethod> DrawMethod::create(DrawKind kind, const Spec& spec,
                                             const std::vector<DrawBox>& boxes, const Logger& log) {
    std::optional<SourceFrame> owned{makeSourceFrame(spec.pattern, spec.size, spec.heap, log)};
    if (!owned) {
        return std::nullopt;
    }
    DrawMethod draw{kind, std::move(*owned), log};
    draw.ordered = boxes;
    std::stable_sort(draw.ordered.begin(), draw.ordered.end(),
                     [](const DrawBox& first, const DrawBox& second) {
                         return colorOf(first) < colorOf(second);
                     });
    draw.view = nv12View(draw.frame);
    if (spec.fresh) {
        draw.pristine = makeSourceFrame(spec.pattern, spec.size, spec.heap, log);
        if (!draw.pristine) {
            return std::nullopt;
        }
    }
    if (kind != DrawKind::Rga2) {
        return draw;
    }
    OverlayConfig config{};
    config.size = spec.size;
    config.stride = spec.size.width;
    config.capacity = std::max(defaultOverlayCapacity, boxes.size());
    draw.overlay = RgaOverlay::create(config, log);
    if (!draw.overlay) {
        return std::nullopt;
    }
    for (const DrawBox& box : boxes) {
        draw.overlayBoxes.push_back(
            OverlayBox{PixelBox{static_cast<float>(box.left), static_cast<float>(box.top),
                                static_cast<float>(box.left + box.width),
                                static_cast<float>(box.top + box.height)},
                       box.classId, false, 0.0F, -1});
    }
    draw.refused = !draw.overlay->check(draw.frame.image);
    // check() drew a block in the corner: draw the pattern again, so every frame starts equal.
    if (!draw.frame.buffer.beginCpuAccess()) {
        return std::nullopt;
    }
    fillNv12(spec.pattern, draw.view, spec.size);
    if (!draw.frame.buffer.endCpuAccess()) {
        return std::nullopt;
    }
    return draw;
}

int DrawMethod::prepare() noexcept {
    threads = method == DrawKind::Cv8 ? cvThreads : 1;
    cv::setNumThreads(threads);
    return method == DrawKind::Rga2 ? 1 : cv::getNumThreads();
}

bool DrawMethod::runCpu() noexcept {
    if (!frame.buffer.beginCpuAccess()) {
        return false;
    }
    Planes planes{planesOf(view, frame.size)};
    if (method == DrawKind::Cv1) {
        drawRows(planes.luma, planes.chroma, ordered, Rows{0, frame.size.height});
    } else {
        // One band of rows per task: no two tasks write the same byte.
        const int height{frame.size.height};
        cv::parallel_for_(cv::Range(0, cvThreads), [&](const cv::Range& range) {
            for (int band{range.start}; band < range.end; ++band) {
                const int begin{
                    static_cast<int>(static_cast<long long>(height) * band / cvThreads) &
                    ~evenMask};
                const int end{band + 1 == cvThreads
                                  ? height
                                  : static_cast<int>(static_cast<long long>(height) * (band + 1) /
                                                     cvThreads) &
                                        ~evenMask};
                drawRows(planes.luma, planes.chroma, ordered, Rows{begin, end});
            }
        });
    }
    return frame.buffer.endCpuAccess();
}

bool DrawMethod::refresh() noexcept {
    if (!pristine) {
        return true;
    }
    const auto wrap{[this](const SourceFrame& source) {
        return wrapbuffer_handle(source.image.handle(), frame.size.width, frame.size.height,
                                 RK_FORMAT_YCbCr_420_SP, frame.size.width, frame.size.height);
    }};
    const IM_STATUS status{imcopy(wrap(*pristine), wrap(frame))};
    if (status != IM_STATUS_SUCCESS) {
        log.error("imcopy of the fresh frame: ", imStrError_t(status));
        return false;
    }
    return true;
}

bool DrawMethod::run() noexcept {
    if (method != DrawKind::Rga2) {
        return runCpu();
    }
    if (refused || !overlay) {
        return false;
    }
    return overlay->draw(frame.image, overlayBoxes);
}

Failure DrawMethod::failure() const {
    if (refused) {
        return Failure{MethodStatus::Rejected,
                       "the RGA2 refused the buffer (the library logs the driver text)"};
    }
    return Failure{MethodStatus::Failed, "a draw call or a cache sync failed, see the log"};
}

bool DrawMethod::copyOutput(cv::Mat& out) {
    if (!frame.buffer.beginCpuAccess()) {
        return false;
    }
    view.copyTo(out);
    return true;
}
