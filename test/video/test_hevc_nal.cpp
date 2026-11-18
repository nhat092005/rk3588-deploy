// The H.265 stream helpers: NAL unit type search and key frame detection on made-up Annex B bytes.
// Plain C++, runs on the PC. Exit code 0 when every check passes.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "hevc_nal.h"
#include "logger.h"

namespace {
constexpr std::size_t caseCount{12};

class Check {
public:
    explicit Check(const Logger& logger) : log{logger} {}

    void expect(bool condition, const char* what) {
        if (!condition) {
            log.error("FAIL: ", what);
            passed = false;
        }
    }

    bool ok() const {
        return passed;
    }

private:
    Logger log;
    bool passed{true};
};

struct Case {
    std::vector<std::uint8_t> bytes;
    bool key{false};
    bool vps{false};
    const char* what{nullptr};
};

// The first byte of a NAL header is type << 1: 0x26 is type 19, 0x28 type 20, 0x40 type 32 and
// 0x02 type 1.
const std::array<Case, caseCount>& cases() {
    static const std::array<Case, caseCount> table{{
        {{}, false, false, "an empty stream"},
        {{0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAF}, true, false, "IDR_W_RADL, 4 byte start code"},
        {{0x00, 0x00, 0x01, 0x28, 0x01}, true, false, "IDR_N_LP, 3 byte start code"},
        {{0x00, 0x00, 0x01, 0x02, 0x01, 0xAF}, false, false, "a P slice is not a key frame"},
        {{0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x00, 0x00, 0x00, 0x01, 0x26, 0x01},
         true,
         true,
         "VPS then IDR"},
        {{0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C}, false, true, "VPS alone"},
        {{0x00, 0x00, 0x01, 0x26}, true, false, "the NAL header is the last byte"},
        {{0x00, 0x00, 0x01}, false, false, "a start code without a header"},
        {{0x26, 0x01, 0x26, 0x01}, false, false, "type bytes without a start code"},
        {{0x00, 0x00, 0x02, 0x26, 0x01}, false, false, "00 00 02 is not a start code"},
        {{0x05, 0x00, 0x01, 0x26, 0x01}, false, false, "05 00 01 is not a start code"},
        {{0x00, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x01, 0x26, 0x01},
         true,
         false,
         "an IDR after a P slice"},
    }};
    return table;
}
} // namespace

int main() {
    const Logger log{"TEST_HEVC_NAL", LogLevel::Debug};
    Check check{log};
    for (const Case& item : cases()) {
        check.expect(hasKeyFrame(item.bytes.data(), item.bytes.size()) == item.key, item.what);
        check.expect(hasNalType(item.bytes.data(), item.bytes.size(), NalType::Vps) == item.vps,
                     item.what);
    }
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
