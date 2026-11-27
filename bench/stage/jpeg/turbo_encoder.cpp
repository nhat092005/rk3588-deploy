#include "turbo_encoder.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <opencv2/core.hpp>

#include "timing.h"

// jpeglib.h needs the C standard types declared above.
#include <jpeglib.h>

namespace {
constexpr int blockSize{16};
constexpr int chromaBlockRows{8};
constexpr int lumaIndex{0};
constexpr int cbIndex{1};
constexpr int crIndex{2};
constexpr int exitLibjpeg{1};
constexpr std::size_t outputCapacity{16'777'216};
constexpr int minQuality{1};
constexpr int maxQuality{100};
constexpr int halved{2};
constexpr int lumaSampling{2};

// The libjpeg error handler: print the message and end the process.
void onError(j_common_ptr info) {
    std::array<char, JMSG_LENGTH_MAX> text{};
    (*info->err->format_message)(info, text.data());
    // NOLINTNEXTLINE(cert-err33-c): the process ends next, a failed message has no remedy.
    std::fprintf(stderr, "libjpeg: %s\n", text.data());
    std::exit(exitLibjpeg);
}
} // namespace

struct TurboState {
    TurboState(const TurboConfig& config, const Logger& logger) : settings{config}, log{logger} {}
    TurboState(const TurboState&) = delete;
    TurboState& operator=(const TurboState&) = delete;
    TurboState(TurboState&&) = delete;
    TurboState& operator=(TurboState&&) = delete;
    ~TurboState() {
        jpeg_destroy_compress(&compressor);
        std::free(output); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    }

    TurboConfig settings;
    Logger log;
    jpeg_compress_struct compressor{};
    jpeg_error_mgr errors{};
    // The output buffer of libjpeg: malloc'ed, large enough that libjpeg never reallocates it.
    unsigned char* output{nullptr};
    unsigned long outputSize{0};
    std::array<cv::Mat, 2> chroma;
};

std::optional<TurboEncoder> TurboEncoder::create(const TurboConfig& config, const Logger& log) {
    if (config.width <= 0 || config.height <= 0 || config.width % blockSize != 0 ||
        config.height % blockSize != 0 || config.quality < minQuality ||
        config.quality > maxQuality) {
        log.error("bad turbo config: ", config.width, "x", config.height, " quality ",
                  config.quality);
        return std::nullopt;
    }
    auto state{std::make_unique<TurboState>(config, log)};
    state->compressor.err = jpeg_std_error(&state->errors);
    state->errors.error_exit = onError;
    jpeg_create_compress(&state->compressor);
    // libjpeg reallocates the buffer with realloc() if a JPEG did not fit, so it comes from
    // malloc().
    // NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    state->output = static_cast<unsigned char*>(std::malloc(outputCapacity));
    // NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    if (state->output == nullptr) {
        log.error("no memory for the JPEG buffer");
        return std::nullopt;
    }
    jpeg_compress_struct& cinfo{state->compressor};
    cinfo.image_width = static_cast<JDIMENSION>(config.width);
    cinfo.image_height = static_cast<JDIMENSION>(config.height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, config.quality, TRUE);
    cinfo.raw_data_in = TRUE;
    cinfo.comp_info[lumaIndex].h_samp_factor = lumaSampling;
    cinfo.comp_info[lumaIndex].v_samp_factor = lumaSampling;
    cinfo.comp_info[cbIndex].h_samp_factor = 1;
    cinfo.comp_info[cbIndex].v_samp_factor = 1;
    cinfo.comp_info[crIndex].h_samp_factor = 1;
    cinfo.comp_info[crIndex].v_samp_factor = 1;
    for (cv::Mat& plane : state->chroma) {
        plane.create(config.height / halved, config.width / halved, CV_8UC1);
    }
    return std::optional<TurboEncoder>{TurboEncoder{std::move(state)}};
}

TurboEncoder::TurboEncoder(std::unique_ptr<TurboState> initial) noexcept
    : state{std::move(initial)} {}
TurboEncoder::TurboEncoder(TurboEncoder&& other) noexcept = default;
TurboEncoder& TurboEncoder::operator=(TurboEncoder&& other) noexcept = default;
TurboEncoder::~TurboEncoder() = default;

TurboResult TurboEncoder::encode(const std::uint8_t* nv12) noexcept {
    TurboState& turbo{*state};
    const int width{turbo.settings.width};
    const int height{turbo.settings.height};
    const std::size_t lumaBytes{static_cast<std::size_t>(width) * static_cast<std::size_t>(height)};
    TurboResult result{};

    const double splitBegin{wallMs()};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the CbCr plane follows Y.
    const std::uint8_t* chromaStart{nv12 + lumaBytes};
    const cv::Mat interleaved{
        height / halved, width / halved, CV_8UC2,
        const_cast<std::uint8_t*>(chromaStart)}; // NOLINT(cppcoreguidelines-pro-type-const-cast)
    cv::split(interleaved, turbo.chroma.data());
    result.splitMs = wallMs() - splitBegin;

    jpeg_compress_struct& cinfo{turbo.compressor};
    // libjpeg stores the length of the JPEG in outputSize, so the capacity is set again.
    turbo.outputSize = outputCapacity;
    jpeg_mem_dest(&cinfo, &turbo.output, &turbo.outputSize);
    jpeg_start_compress(&cinfo, TRUE);
    std::array<JSAMPROW, blockSize> lumaRows{};
    std::array<JSAMPROW, chromaBlockRows> cbRows{};
    std::array<JSAMPROW, chromaBlockRows> crRows{};
    std::array<JSAMPARRAY, 3> image{lumaRows.data(), cbRows.data(), crRows.data()};
    while (cinfo.next_scanline < cinfo.image_height) {
        const int row{static_cast<int>(cinfo.next_scanline)};
        for (int line{0}; line < blockSize; ++line) {
            const std::size_t offset{static_cast<std::size_t>(row + line) *
                                     static_cast<std::size_t>(width)};
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-const-cast)
            lumaRows.at(static_cast<std::size_t>(line)) = const_cast<std::uint8_t*>(nv12 + offset);
        }
        for (int line{0}; line < chromaBlockRows; ++line) {
            const int chromaRow{(row / halved) + line};
            cbRows.at(static_cast<std::size_t>(line)) =
                turbo.chroma.at(0).ptr<std::uint8_t>(chromaRow);
            crRows.at(static_cast<std::size_t>(line)) =
                turbo.chroma.at(1).ptr<std::uint8_t>(chromaRow);
        }
        jpeg_write_raw_data(&cinfo, image.data(), blockSize);
    }
    jpeg_finish_compress(&cinfo);
    result.bytes = turbo.outputSize;
    return result;
}

const std::uint8_t* TurboEncoder::data() const noexcept {
    return state->output;
}
