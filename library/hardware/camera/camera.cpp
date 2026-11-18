#include "camera.h"

#include <cerrno>
#include <fstream>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>

namespace {
static_assert(maxPlanes == VIDEO_MAX_PLANES, "maxPlanes must equal VIDEO_MAX_PLANES");

constexpr std::size_t minBuffers{2};
constexpr int pollTimeoutMs{2000};
constexpr std::uint32_t targetPixelFormat{V4L2_PIX_FMT_NV12};
constexpr v4l2_buf_type captureType{V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
constexpr std::size_t fourccLength{4};
constexpr double microsecondsPerSecond{1'000'000.0};
constexpr unsigned int bitsPerByte{8};
constexpr unsigned int byteMask{0xFF};
constexpr int capsHexWidth{8};
// V4L2 selection takes the single-planar capture type, also on a multi-planar node.
constexpr v4l2_buf_type selectionType{V4L2_BUF_TYPE_VIDEO_CAPTURE};

// ioctl that retries on EINTR.
int retryIoctl(int descriptor, unsigned long request, void* argument) noexcept {
    int result{ioctl(descriptor, request, argument)};
    while (result == -1 && errno == EINTR) {
        result = ioctl(descriptor, request, argument);
    }
    return result;
}

// V4L2 fourcc code as a printable string.
std::array<char, fourccLength + 1> fourccText(std::uint32_t code) noexcept {
    std::array<char, fourccLength + 1> text{};
    for (std::size_t position{0}; position < fourccLength; ++position) {
        const unsigned int shift{bitsPerByte * static_cast<unsigned int>(position)};
        text.at(position) = static_cast<char>((code >> shift) & byteMask);
    }
    return text;
}

enum class CropTarget : std::uint8_t {
    Current = V4L2_SEL_TGT_CROP,
    Bounds = V4L2_SEL_TGT_CROP_BOUNDS
};

// VIDIOC_G_SELECTION of one crop target.
std::optional<CropRect> readCrop(int descriptor, CropTarget target) noexcept {
    v4l2_selection selection{};
    selection.type = selectionType;
    selection.target = static_cast<std::uint32_t>(target);
    if (retryIoctl(descriptor, VIDIOC_G_SELECTION, &selection) < 0) {
        return std::nullopt;
    }
    return CropRect{selection.r.left, selection.r.top, static_cast<int>(selection.r.width),
                    static_cast<int>(selection.r.height)};
}

// VIDIOC_S_SELECTION of the crop rectangle. The driver may adjust it: read it back.
bool writeCrop(int descriptor, const CropRect& rect) noexcept {
    v4l2_selection selection{};
    selection.type = selectionType;
    selection.target = V4L2_SEL_TGT_CROP;
    selection.r.left = rect.left;
    selection.r.top = rect.top;
    selection.r.width = static_cast<std::uint32_t>(rect.width);
    selection.r.height = static_cast<std::uint32_t>(rect.height);
    return retryIoctl(descriptor, VIDIOC_S_SELECTION, &selection) == 0;
}

// The largest area of `bounds` with the aspect ratio width:height, centred. Offsets and sizes
// are even.
CropRect centeredCrop(const CropRect& bounds, int width, int height) noexcept {
    std::int64_t cropWidth{bounds.width};
    std::int64_t cropHeight{cropWidth * height / width};
    if (cropHeight > bounds.height) {
        cropHeight = bounds.height;
        cropWidth = cropHeight * width / height;
    }
    cropWidth -= cropWidth % 2;
    cropHeight -= cropHeight % 2;
    std::int64_t left{bounds.left + (bounds.width - cropWidth) / 2};
    std::int64_t top{bounds.top + (bounds.height - cropHeight) / 2};
    left -= left % 2;
    top -= top % 2;
    return CropRect{static_cast<int>(left), static_cast<int>(top), static_cast<int>(cropWidth),
                    static_cast<int>(cropHeight)};
}

void logCrop(const Logger& log, const char* label, const CropRect& rect) noexcept {
    log.info("CROP ", label, " left=", rect.left, " top=", rect.top, " ", rect.width, "x",
             rect.height);
}
} // namespace

Camera::~Camera() {
    std::ignore = close();
}

std::optional<Camera> Camera::open(const CameraConfig& config, const Logger& log) {
    FileDescriptor device{::open(config.node, O_RDWR | O_NONBLOCK | O_CLOEXEC), log};
    if (!device.valid()) {
        log.error("open ", config.node, " failed: ", lastError());
        return std::nullopt;
    }
    log.info("open ", config.node, " fd=", device.get());

    Camera camera{std::move(device), log};
    if (!camera.queryCapabilities() || !camera.enumerateFormats() || !camera.setCrop(config) ||
        !camera.setFormat(config) || !camera.queryBufferLengths() ||
        !camera.allocateBuffers(config) || !camera.startStreaming()) {
        return std::nullopt;
    }
    return std::optional<Camera>{std::move(camera)};
}

// VIDIOC_QUERYCAP. Requires multi-planar capture and streaming in device_caps.
bool Camera::queryCapabilities() const noexcept {
    v4l2_capability capability{};
    if (retryIoctl(device.get(), VIDIOC_QUERYCAP, &capability) < 0) {
        log.error("QUERYCAP failed: ", lastError());
        return false;
    }
    const std::uint32_t caps{(capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                                 ? capability.device_caps
                                 : capability.capabilities};
    // The kernel fills these fields with zero-terminated text in __u8 arrays.
    log.info("QUERYCAP driver=", reinterpret_cast<const char*>(&capability.driver[0]),
             " card=", reinterpret_cast<const char*>(&capability.card[0]),
             " bus=", reinterpret_cast<const char*>(&capability.bus_info[0]), " caps=0x",
             Hex{caps, capsHexWidth});

    const bool multiPlanar{(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0};
    const bool canStream{(caps & V4L2_CAP_STREAMING) != 0};
    if (!multiPlanar || !canStream) {
        log.error("need CAPTURE_MPLANE and STREAMING: mplane=", multiPlanar,
                  " streaming=", canStream);
        return false;
    }
    return true;
}

// VIDIOC_ENUM_FMT. Logs every pixel format, requires the target one.
bool Camera::enumerateFormats() const noexcept {
    bool foundTarget{false};
    v4l2_fmtdesc description{};
    description.type = captureType;
    while (retryIoctl(device.get(), VIDIOC_ENUM_FMT, &description) == 0) {
        // The kernel fills the description with zero-terminated text in a __u8 array.
        log.info("ENUM_FMT [", description.index, "] ", fourccText(description.pixelformat).data(),
                 " (", reinterpret_cast<const char*>(&description.description[0]), ")");
        foundTarget = foundTarget || description.pixelformat == targetPixelFormat;
        ++description.index;
    }
    const std::error_code error{lastError()};
    if (error != std::errc::invalid_argument) {
        log.error("ENUM_FMT failed: ", error);
        return false;
    }
    if (!foundTarget) {
        log.error("format ", fourccText(targetPixelFormat).data(), " not offered");
        return false;
    }
    return true;
}

// Crop the ISP to the aspect ratio of the requested size, so it scales and does not stretch. The
// original crop is kept for close().
bool Camera::setCrop(const CameraConfig& config) noexcept {
    if (config.width <= 0 || config.height <= 0) {
        log.error("bad size ", config.width, "x", config.height);
        return false;
    }
    const std::optional<CropRect> bounds{readCrop(device.get(), CropTarget::Bounds)};
    const std::optional<CropRect> current{readCrop(device.get(), CropTarget::Current)};
    if (!bounds || !current) {
        log.error("G_SELECTION failed: ", lastError());
        return false;
    }
    const CropRect wanted{centeredCrop(*bounds, config.width, config.height)};
    if (!writeCrop(device.get(), wanted)) {
        log.error("S_SELECTION failed: ", lastError());
        return false;
    }
    originalCrop = *current;
    cropChanged = true;
    logCrop(log, "bounds", *bounds);
    const std::optional<CropRect> applied{readCrop(device.get(), CropTarget::Current)};
    if (!applied) {
        log.error("G_SELECTION failed: ", lastError());
        return false;
    }
    logCrop(log, "applied", *applied);
    if (*applied != wanted) {
        logCrop(log, "wanted", wanted);
        log.warn("driver changed the crop");
    }
    return true;
}

// VIDIOC_S_FMT then G_FMT. Logs and keeps what the driver returned.
bool Camera::setFormat(const CameraConfig& config) noexcept {
    const std::uint32_t width{static_cast<std::uint32_t>(config.width)};
    const std::uint32_t height{static_cast<std::uint32_t>(config.height)};
    v4l2_format request{};
    request.type = captureType;
    request.fmt.pix_mp.width = width;
    request.fmt.pix_mp.height = height;
    request.fmt.pix_mp.pixelformat = targetPixelFormat;
    request.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (retryIoctl(device.get(), VIDIOC_S_FMT, &request) < 0) {
        log.error("S_FMT failed: ", lastError());
        return false;
    }

    v4l2_format current{};
    current.type = captureType;
    if (retryIoctl(device.get(), VIDIOC_G_FMT, &current) < 0) {
        log.error("G_FMT failed: ", lastError());
        return false;
    }
    const v4l2_pix_format_mplane& pix{current.fmt.pix_mp};
    const std::size_t planeTotal{pix.num_planes};
    log.info("G_FMT ", pix.width, "x", pix.height, " ", fourccText(pix.pixelformat).data(),
             " planes=", planeTotal);
    if (pix.pixelformat != targetPixelFormat) {
        log.error("driver changed pixelformat to ", fourccText(pix.pixelformat).data());
        return false;
    }
    if (planeTotal == 0 || planeTotal > maxPlanes) {
        log.error("bad num_planes ", planeTotal);
        return false;
    }
    for (std::size_t plane{0}; plane < planeTotal; ++plane) {
        log.info("  plane ", plane, " bytesperline=", pix.plane_fmt[plane].bytesperline,
                 " sizeimage=", pix.plane_fmt[plane].sizeimage);
        sizeImage.at(plane) = pix.plane_fmt[plane].sizeimage;
    }
    if (pix.width != width || pix.height != height) {
        log.warn("driver changed size: asked ", width, "x", height, " got ", pix.width, "x",
                 pix.height);
    }
    const bool fullRange{pix.quantization == V4L2_QUANTIZATION_FULL_RANGE};
    log.info("  quantization ", fullRange ? "full range" : "limited or default");
    acceptedFormat = CameraFormat{static_cast<int>(pix.width), static_cast<int>(pix.height),
                                  static_cast<int>(pix.plane_fmt[0].bytesperline), fullRange};
    planes = planeTotal;
    return true;
}

// The driver rejects a DMABUF shorter than the buffers it allocates itself, and those are longer
// than sizeimage when the height is not a multiple of 16 (1080 rows use 1088). Ask for one mmap
// buffer, read its plane lengths with QUERYBUF, then free it.
bool Camera::queryBufferLengths() noexcept {
    v4l2_requestbuffers request{};
    request.count = 1;
    request.type = captureType;
    request.memory = V4L2_MEMORY_MMAP;
    if (retryIoctl(device.get(), VIDIOC_REQBUFS, &request) < 0) {
        log.error("REQBUFS (length probe) failed: ", lastError());
        return false;
    }
    std::array<v4l2_plane, maxPlanes> planeInfo{};
    v4l2_buffer buffer{};
    buffer.type = captureType;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = 0;
    buffer.length = static_cast<std::uint32_t>(planes);
    buffer.m.planes = planeInfo.data();
    const bool queried{retryIoctl(device.get(), VIDIOC_QUERYBUF, &buffer) == 0};
    const std::error_code queryError{lastError()};
    request.count = 0;
    const bool freed{retryIoctl(device.get(), VIDIOC_REQBUFS, &request) == 0};
    const std::error_code freeError{lastError()};
    if (!queried) {
        log.error("QUERYBUF (length probe) failed: ", queryError);
        return false;
    }
    if (!freed) {
        log.error("REQBUFS 0 (length probe) failed: ", freeError);
        return false;
    }
    for (std::size_t plane{0}; plane < planes; ++plane) {
        const std::size_t length{planeInfo.at(plane).length};
        log.info("  plane ", plane, " buffer length=", length, " (sizeimage ", sizeImage.at(plane),
                 ")");
        if (length < sizeImage.at(plane)) {
            log.error("driver buffer length ", length, " is below sizeimage");
            return false;
        }
        bufferLength.at(plane) = length;
    }
    return true;
}

// REQBUFS for DMABUF memory, one DmaBuf per plane and buffer, QBUF for every buffer.
bool Camera::allocateBuffers(const CameraConfig& config) noexcept {
    if (config.buffers < minBuffers || config.buffers > maxBufferCount) {
        log.error("bad buffer count ", config.buffers);
        return false;
    }
    v4l2_requestbuffers request{};
    request.count = static_cast<std::uint32_t>(config.buffers);
    request.type = captureType;
    request.memory = V4L2_MEMORY_DMABUF;
    if (retryIoctl(device.get(), VIDIOC_REQBUFS, &request) < 0) {
        log.error("REQBUFS failed: ", lastError());
        return false;
    }
    const std::size_t granted{request.count};
    log.info("REQBUFS asked ", config.buffers, " got ", granted);
    if (granted < minBuffers) {
        log.error("need at least ", minBuffers, " buffers");
        return false;
    }
    if (granted > config.buffers) {
        log.warn("driver allocated ", granted, " buffers, using ", config.buffers);
    }
    buffersInUse = granted < config.buffers ? granted : config.buffers;
    memories.reserve(buffersInUse * planes);

    for (std::size_t index{0}; index < buffersInUse; ++index) {
        for (std::size_t plane{0}; plane < planes; ++plane) {
            std::optional<DmaBuf> memory{
                DmaBuf::allocate(DmaBufConfig{config.heap, bufferLength.at(plane)}, log)};
            if (!memory) {
                return false;
            }
            memories.push_back(std::move(*memory));
        }
        if (!queue(index)) {
            return false;
        }
    }
    return true;
}

bool Camera::startStreaming() noexcept {
    v4l2_buf_type type{captureType};
    if (retryIoctl(device.get(), VIDIOC_STREAMON, &type) < 0) {
        log.error("STREAMON failed: ", lastError());
        return false;
    }
    streaming = true;
    return true;
}

WaitStatus Camera::waitFrame(const StopSignal& stop) const noexcept {
    std::array<pollfd, 2> watched{};
    pollfd& frameReady{watched.at(0)};
    pollfd& stopRequested{watched.at(1)};
    frameReady.fd = device.get();
    frameReady.events = POLLIN;
    stopRequested.fd = stop.descriptor();
    stopRequested.events = POLLIN;

    int ready{poll(watched.data(), watched.size(), pollTimeoutMs)};
    while (ready < 0 && errno == EINTR) {
        ready = poll(watched.data(), watched.size(), pollTimeoutMs);
    }
    if (ready < 0) {
        log.error("poll failed: ", lastError());
        return WaitStatus::Failed;
    }
    if (ready == 0) {
        return WaitStatus::Timeout;
    }
    if (stopRequested.revents != 0) {
        return WaitStatus::Stopped;
    }
    if ((frameReady.revents & POLLIN) == 0) {
        log.error("poll reported an error on the video node, revents=0x",
                  Hex{static_cast<std::uint64_t>(frameReady.revents)});
        return WaitStatus::Failed;
    }
    return WaitStatus::Ready;
}

Dequeued Camera::dequeue() const noexcept {
    std::array<v4l2_plane, maxPlanes> planeInfo{};
    v4l2_buffer buffer{};
    buffer.type = captureType;
    buffer.memory = V4L2_MEMORY_DMABUF;
    buffer.length = static_cast<std::uint32_t>(planes);
    buffer.m.planes = planeInfo.data();
    if (retryIoctl(device.get(), VIDIOC_DQBUF, &buffer) < 0) {
        const std::error_code error{lastError()};
        if (error == std::errc::resource_unavailable_try_again) {
            return {DequeueStatus::NotReady, Frame{}};
        }
        log.error("DQBUF failed: ", error);
        return {DequeueStatus::Failed, Frame{}};
    }
    if (buffer.index >= buffersInUse) {
        log.error("DQBUF returned unknown buffer ", buffer.index);
        return {DequeueStatus::Failed, Frame{}};
    }
    Frame frame{};
    frame.index = buffer.index;
    frame.sequence = buffer.sequence;
    if ((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
        frame.captureSeconds =
            static_cast<double>(buffer.timestamp.tv_sec) +
            (static_cast<double>(buffer.timestamp.tv_usec) / microsecondsPerSecond);
    }
    frame.flaggedError = (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0;
    for (std::size_t plane{0}; plane < planes; ++plane) {
        frame.bytesUsed.at(plane) = planeInfo.at(plane).bytesused;
    }
    return {DequeueStatus::Frame, frame};
}

// VIDIOC_QBUF (DMABUF: every plane is given as a descriptor and a length).
bool Camera::queue(std::size_t index) const noexcept {
    std::array<v4l2_plane, maxPlanes> planeInfo{};
    for (std::size_t plane{0}; plane < planes; ++plane) {
        const DmaBuf& memory{planeBuffer(index, plane)};
        planeInfo.at(plane).m.fd = memory.fd();
        planeInfo.at(plane).length = static_cast<std::uint32_t>(memory.size());
    }
    v4l2_buffer buffer{};
    buffer.type = captureType;
    buffer.memory = V4L2_MEMORY_DMABUF;
    buffer.index = static_cast<std::uint32_t>(index);
    buffer.length = static_cast<std::uint32_t>(planes);
    buffer.m.planes = planeInfo.data();
    if (retryIoctl(device.get(), VIDIOC_QBUF, &buffer) < 0) {
        log.error("QBUF ", index, " failed: ", lastError());
        return false;
    }
    return true;
}

bool Camera::writeFrame(const Frame& frame, const char* path) const {
    std::ofstream file{path, std::ios::binary};
    if (!file) {
        log.error("open ", path, " failed");
        return false;
    }
    bool synced{true};
    for (std::size_t plane{0}; plane < planes; ++plane) {
        const DmaBuf& memory{planeBuffer(frame.index, plane)};
        std::size_t used{frame.bytesUsed.at(plane)};
        if (used > memory.size()) {
            log.warn("plane ", plane, " bytesused ", used, " exceeds length ", memory.size(),
                     ", clamped");
            used = memory.size();
        }
        // The camera wrote this memory, so the CPU cache is refreshed before the read.
        if (!memory.beginCpuAccess()) {
            synced = false;
            continue;
        }
        file.write(static_cast<const char*>(memory.data()), static_cast<std::streamsize>(used));
        synced = memory.endCpuAccess() && synced;
    }
    file.close();
    if (!file || !synced) {
        log.error("writing ", path, " failed");
        return false;
    }
    log.info("first frame written to ", path);
    return true;
}

bool Camera::close() noexcept {
    bool success{true};
    if (streaming && device.valid()) {
        v4l2_buf_type type{captureType};
        if (retryIoctl(device.get(), VIDIOC_STREAMOFF, &type) < 0) {
            log.error("STREAMOFF failed: ", lastError());
            success = false;
        }
    }
    streaming = false;
    // Each DmaBuf unmaps and closes itself, and logs a failure.
    memories.clear();
    if (cropChanged && device.valid()) {
        if (!writeCrop(device.get(), originalCrop)) {
            log.error("restoring the crop failed: ", lastError());
            success = false;
        }
        cropChanged = false;
    }
    const std::error_code error{device.close()};
    if (error) {
        log.error("close failed: ", error);
        success = false;
    }
    return success;
}
