// Link and load check for the board libraries: RGA, MPP, RKNN runtime.
// Exit code 0 only if RGA reports a version, MPP creates and initialises an H.265 encoder,
// and rknn_init rejects a junk model (proves librknnrt loads and the symbol resolves).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "im2d.hpp"
#include "rk_mpi.h"
#include "logger.h"
#include "rknn_api.h"

namespace {
constexpr std::size_t junkModelSize{64};

// Owns one MPP context.
class MppContext {
public:
    MppContext(MppCtx rawContext, Logger logger) noexcept : context{rawContext}, log{logger} {}
    MppContext(const MppContext&) = delete;
    MppContext& operator=(const MppContext&) = delete;
    MppContext(MppContext&& other) noexcept
        : context{std::exchange(other.context, nullptr)}, log{other.log} {}
    MppContext& operator=(MppContext&& other) noexcept {
        if (this != &other) {
            destroyAndLog();
            context = std::exchange(other.context, nullptr);
            log = other.log;
        }
        return *this;
    }
    // A failure to destroy is logged by the destructor. After an explicit destroy() nothing is
    // logged twice.
    ~MppContext() {
        destroyAndLog();
    }

    [[nodiscard]] MPP_RET initHevcEncoder() noexcept {
        return mpp_init(context, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
    }

    // mpp_destroy. Safe to call twice.
    [[nodiscard]] MPP_RET destroy() noexcept {
        if (context == nullptr) {
            return MPP_OK;
        }
        return mpp_destroy(std::exchange(context, nullptr));
    }

private:
    void destroyAndLog() noexcept {
        const MPP_RET status{destroy()};
        if (status != MPP_OK) {
            log.error("MPP: mpp_destroy ret=", static_cast<int>(status));
        }
    }

    MppCtx context{nullptr};
    Logger log;
};

// Owns one RKNN context.
class RknnContext {
public:
    RknnContext(rknn_context rawContext, Logger logger) noexcept
        : context{rawContext}, loaded{true}, log{logger} {}
    RknnContext(const RknnContext&) = delete;
    RknnContext& operator=(const RknnContext&) = delete;
    RknnContext(RknnContext&& other) noexcept
        : context{std::exchange(other.context, 0)}, loaded{std::exchange(other.loaded, false)},
          log{other.log} {}
    RknnContext& operator=(RknnContext&& other) noexcept {
        if (this != &other) {
            destroyAndLog();
            context = std::exchange(other.context, 0);
            loaded = std::exchange(other.loaded, false);
            log = other.log;
        }
        return *this;
    }
    // A failure to destroy is logged by the destructor. After an explicit destroy() nothing is
    // logged twice.
    ~RknnContext() {
        destroyAndLog();
    }

    // rknn_destroy. Safe to call twice.
    [[nodiscard]] int destroy() noexcept {
        if (!loaded) {
            return 0;
        }
        loaded = false;
        return rknn_destroy(context);
    }

private:
    void destroyAndLog() noexcept {
        const int status{destroy()};
        if (status != 0) {
            log.error("RKNN: rknn_destroy ret=", status);
        }
    }

    rknn_context context{0};
    bool loaded{false};
    Logger log;
};

// RGA must report a version string.
bool checkRga(const Logger& log) {
    const char* version{querystring(RGA_VERSION)};
    if (version == nullptr || std::string_view{version}.empty()) {
        log.error("RGA: querystring(RGA_VERSION) returned null or empty");
        return false;
    }
    log.info("RGA: ", version);
    return true;
}

// MPP must create a context and initialise an H.265 encoder in it.
bool checkMpp(const Logger& log) {
    MppCtx rawContext{nullptr};
    MppApi* api{nullptr};
    const MPP_RET created{mpp_create(&rawContext, &api)};
    log.info("MPP: mpp_create ret=", static_cast<int>(created));
    if (created != MPP_OK) {
        log.error("MPP: H.265 encoder init failed");
        return false;
    }
    MppContext context{rawContext, log};
    bool passed{true};
    const MPP_RET initialised{context.initHevcEncoder()};
    log.info("MPP: mpp_init(ENC,HEVC) ret=", static_cast<int>(initialised));
    if (initialised != MPP_OK) {
        log.error("MPP: H.265 encoder init failed");
        passed = false;
    }
    const MPP_RET destroyed{context.destroy()};
    if (destroyed != MPP_OK) {
        log.error("MPP: mpp_destroy ret=", static_cast<int>(destroyed));
        passed = false;
    }
    return passed;
}

// librknnrt must load and reject a junk model.
bool checkRknn(const Logger& log) {
    std::array<char, junkModelSize> junk{};
    rknn_context rawContext{0};
    const int status{
        rknn_init(&rawContext, junk.data(), static_cast<std::uint32_t>(junk.size()), 0, nullptr)};
    log.info("RKNN: rknn_init(junk) ret=", status, " (nonzero expected)");
    if (status != 0) {
        return true;
    }
    RknnContext context{rawContext, log};
    log.error("RKNN: junk model was accepted");
    const int destroyed{context.destroy()};
    if (destroyed != 0) {
        log.error("RKNN: rknn_destroy ret=", destroyed);
    }
    return false;
}
} // namespace

int main() {
    const Logger log{"PROBE_LINK", LogLevel::Debug};
    const bool rgaPassed{checkRga(log)};
    const bool mppPassed{checkMpp(log)};
    const bool rknnPassed{checkRknn(log)};
    const bool passed{rgaPassed && mppPassed && rknnPassed};
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
