#include "output_plan.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {
constexpr int evenMask{1};
constexpr std::size_t outputCount{4};

// The stream of that height, made if there is none yet.
StreamPlan& streamOf(std::vector<StreamPlan>& plans, int height) {
    const auto found{std::find_if(plans.begin(), plans.end(), [height](const StreamPlan& plan) {
        return plan.height == height;
    })};
    if (found != plans.end()) {
        return *found;
    }
    plans.push_back(StreamPlan{height, false, false, false, false});
    return plans.back();
}

// The height an output asks for: 0 for the camera size.
int heightOf(bool downscale, int height) {
    return downscale ? height : 0;
}

// Sorts by height, the largest first, and 0 (the camera size) before all.
int rank(const StreamPlan& plan) {
    return plan.height == 0 ? std::numeric_limits<int>::max() : plan.height;
}
} // namespace

std::vector<StreamPlan> planStreams(const AppOptions& options, bool archive, bool session,
                                    bool stream) {
    std::vector<StreamPlan> plans;
    if (archive) {
        streamOf(plans, heightOf(options.archiveDownscale, options.archiveHeight)).archive = true;
    }
    if (session) {
        streamOf(plans, heightOf(options.sessionDownscale, options.sessionHeight)).session = true;
    }
    if (stream) {
        streamOf(plans, 0).stream = true;
    }
    if (options.violation && options.violationClip) {
        streamOf(plans, heightOf(options.violationClipDownscale, options.violationClipHeight))
            .clip = true;
    }
    std::stable_sort(
        plans.begin(), plans.end(),
        [](const StreamPlan& left, const StreamPlan& right) { return rank(left) > rank(right); });
    return plans;
}

int scaledWidth(ImageSize full, int height) {
    const std::int64_t width{static_cast<std::int64_t>(full.width) * height / full.height};
    return static_cast<int>(width & ~static_cast<std::int64_t>(evenMask));
}

int scaledKbps(int kbps, ImageSize full, int height) {
    const std::int64_t pixels{static_cast<std::int64_t>(full.width) * full.height};
    const std::int64_t small{static_cast<std::int64_t>(scaledWidth(full, height)) * height};
    const std::int64_t scaled{static_cast<std::int64_t>(kbps) * small / pixels};
    return static_cast<int>(
        std::min<std::int64_t>(kbps, std::max<std::int64_t>(scaled, minScaledKbps)));
}

std::string downscaleProblem(const AppOptions& options, int fullHeight, bool archive,
                             bool session) {
    struct Output {
        bool on;
        bool downscale;
        int height;
        const char* name;
    };
    const std::array<Output, outputCount> outputs{{
        {options.violation && options.violationImage, options.violationImageDownscale,
         options.violationImageHeight, "violation.image.downscale.height"},
        {options.violation && options.violationClip, options.violationClipDownscale,
         options.violationClipHeight, "violation.clip.downscale.height"},
        {archive, options.archiveDownscale, options.archiveHeight, "archive.downscale.height"},
        {session, options.sessionDownscale, options.sessionHeight, "session.downscale.height"},
    }};
    for (const Output& output : outputs) {
        if (output.on && output.downscale && output.height >= fullHeight) {
            return std::string{output.name} + " " + std::to_string(output.height) +
                   " is not below the camera height " + std::to_string(fullHeight);
        }
    }
    return {};
}
