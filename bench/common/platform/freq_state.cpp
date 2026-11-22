#include "freq_state.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
constexpr std::string_view cpuRoot{"/sys/devices/system/cpu/cpufreq"};
constexpr std::string_view npuRoot{"/sys/class/devfreq/fdab0000.npu"};
constexpr std::string_view ddrRoot{"/sys/class/devfreq/dmc"};
constexpr std::string_view lockedGovernor{"performance"};

std::optional<std::string> firstLine(const std::filesystem::path& path) {
    std::ifstream file{path};
    std::string line;
    if (!std::getline(file, line)) {
        return std::nullopt;
    }
    return line;
}

// One domain: its governor and frequency files. False when either cannot be read.
bool addEntry(FreqState& state, const std::string& name, const std::filesystem::path& governor,
              const std::filesystem::path& frequency) {
    const std::optional<std::string> governorText{firstLine(governor)};
    const std::optional<std::string> frequencyText{firstLine(frequency)};
    if (!governorText || !frequencyText) {
        return false;
    }
    state.entries.push_back(FreqEntry{name, *governorText, *frequencyText});
    return true;
}
} // namespace

FreqState readFreqState() {
    FreqState state;
    bool complete{true};
    std::error_code error;
    std::vector<std::filesystem::path> policies;
    for (std::filesystem::directory_iterator it{std::filesystem::path{cpuRoot}, error};
         !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
        if (it->path().filename().string().rfind("policy", 0) == 0) {
            policies.push_back(it->path());
        }
    }
    std::sort(policies.begin(), policies.end());
    complete = !policies.empty();
    for (const std::filesystem::path& policy : policies) {
        complete = addEntry(state, policy.filename().string(), policy / "scaling_governor",
                            policy / "scaling_cur_freq") &&
                   complete;
    }
    complete = addEntry(state, "npu", std::filesystem::path{npuRoot} / "governor",
                        std::filesystem::path{npuRoot} / "cur_freq") &&
               complete;
    complete = addEntry(state, "ddr", std::filesystem::path{ddrRoot} / "governor",
                        std::filesystem::path{ddrRoot} / "cur_freq") &&
               complete;
    state.locked = complete && std::all_of(state.entries.begin(), state.entries.end(),
                                           [](const FreqEntry& entry) {
                                               return entry.governor == lockedGovernor;
                                           });
    return state;
}
