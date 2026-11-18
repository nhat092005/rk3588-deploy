#include "mpp_source.h"

#include <utility>

#include "rk_mpi.h"

namespace {
constexpr const char* mppTag{"mpp_source"};
} // namespace

std::optional<MppSource> MppSource::import(DmaBuf& buffer, const Logger& log) {
    MppBufferInfo info{};
    info.type = MPP_BUFFER_TYPE_DRM;
    info.size = buffer.size();
    info.ptr = buffer.data();
    info.fd = buffer.fd();
    MppBuffer handleValue{nullptr};
    const MPP_RET status{
        mpp_buffer_import_with_tag(nullptr, &info, &handleValue, mppTag, "import")};
    if (status != MPP_OK || handleValue == nullptr) {
        log.error("mpp_buffer_import ret=", static_cast<int>(status));
        return std::nullopt;
    }
    return std::optional<MppSource>{MppSource{handleValue, log}};
}

MppSource::MppSource(MppSource&& other) noexcept
    : imported{std::exchange(other.imported, nullptr)}, log{other.log} {}

MppSource& MppSource::operator=(MppSource&& other) noexcept {
    if (this != &other) {
        release();
        imported = std::exchange(other.imported, nullptr);
        log = other.log;
    }
    return *this;
}

MppSource::~MppSource() {
    release();
}

void MppSource::release() noexcept {
    if (imported == nullptr) {
        return;
    }
    const MPP_RET status{mpp_buffer_put_with_caller(
        static_cast<MppBuffer>(std::exchange(imported, nullptr)), "release")};
    if (status != MPP_OK) {
        log.error("mpp_buffer_put ret=", static_cast<int>(status));
    }
}
