#ifndef BENCH_FREQ_STATE_H
#define BENCH_FREQ_STATE_H

#include <string>
#include <vector>

/** @brief The governor and the current frequency of one clock domain. */
struct FreqEntry {
    /** "policy0", "policy4", "policy6", "npu" or "ddr". */
    std::string name;
    std::string governor;
    /** The text of the sysfs file: kHz for a CPU policy, Hz for the NPU and the DDR. */
    std::string frequency;
};

/** @brief The clock state of the board at one moment. */
struct FreqState {
    std::vector<FreqEntry> entries;
    /** True when every CPU policy, the NPU and the DDR are read and their governor is
     *  "performance", the state that sh/env/lock_freq.sh lock sets. */
    bool locked{false};
};

/**
 * @brief Reads the clock state from sysfs (the paths that `lock_freq.sh status` prints). Needs no
 *        root. A file that cannot be read is left out and the state is not locked.
 * @return The state.
 */
FreqState readFreqState();

#endif // BENCH_FREQ_STATE_H
