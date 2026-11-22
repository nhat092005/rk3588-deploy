#include "npu_load.h"

#include <pthread.h>
#include <ctime>
#include <cstring>
#include <utility>

namespace {
constexpr int idleValue{114}; // the padding value of the model input, any value would do
constexpr double millisPerSecond{1000.0};
constexpr double nanosPerMilli{1.0e6};
} // namespace

std::unique_ptr<NpuLoad> NpuLoad::start(const NpuConfig& config, const Logger& log) {
    std::optional<NpuInfer> model{NpuInfer::create(config, log)};
    if (!model) {
        return nullptr;
    }
    std::optional<DmaBuf> input{DmaBuf::allocate(DmaBufConfig{lowHeap, model->inputBytes()}, log)};
    if (!input) {
        return nullptr;
    }
    std::memset(input->data(), idleValue, input->size());
    if (!input->endCpuAccess()) {
        return nullptr;
    }
    // The private constructor rules out std::make_unique.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,modernize-make-unique)
    std::unique_ptr<NpuLoad> load{new NpuLoad{std::move(*input), std::move(*model)}};
    // The model keeps the buffer it is attached to: attach it where it stays.
    load->inputId = load->infer.attachInput(load->buffer);
    if (!load->inputId) {
        return nullptr;
    }
    load->worker = std::thread{[raw = load.get(), input = *load->inputId] { raw->loop(input); }};
    return load;
}

NpuLoad::~NpuLoad() {
    stopping.store(true);
    if (worker.joinable()) {
        worker.join();
    }
}

void NpuLoad::loop(InputId input) {
    while (!stopping.load()) {
        const double begin{wallMs()};
        if (!infer.run(input)) {
            failure.store(true);
            return;
        }
        const double elapsed{wallMs() - begin};
        const std::lock_guard<std::mutex> lock{guard};
        samples.push_back(elapsed);
    }
}

NpuWindow NpuLoad::takeWindow() {
    std::vector<double> taken;
    {
        const std::lock_guard<std::mutex> lock{guard};
        taken.swap(samples);
    }
    NpuWindow window{};
    window.runs = taken.size();
    window.latency = percentilesOf(taken);
    return window;
}

double NpuLoad::cpuMs() {
    clockid_t clock{};
    timespec now{};
    if (pthread_getcpuclockid(worker.native_handle(), &clock) != 0 ||
        clock_gettime(clock, &now) != 0) {
        return 0.0;
    }
    return (static_cast<double>(now.tv_sec) * millisPerSecond) +
           (static_cast<double>(now.tv_nsec) / nanosPerMilli);
}
