#include "stream_check.h"

#include <array>
#include <cstdlib>
#include <memory>

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "handles.h"

namespace {
constexpr std::string_view rtspScheme{"rtsp://"};
constexpr std::string_view defaultRtspPort{"554"};
constexpr std::chrono::milliseconds connectTimeout{2'000};
constexpr std::string_view ffmpegName{"ffmpeg"};

// The part of the address before the path, without a user name and password.
std::string_view authorityOf(std::string_view url) {
    std::string_view rest{url.substr(rtspScheme.size())};
    rest = rest.substr(0, rest.find('/'));
    const std::size_t atSign{rest.rfind('@')};
    return atSign == std::string_view::npos ? rest : rest.substr(atSign + 1);
}

std::error_code errorOf(int code) {
    return {code, std::generic_category()};
}

// Waits for a connection that is in progress and reads how it ended.
std::error_code finishConnect(int descriptor, std::chrono::milliseconds timeout) {
    pollfd waiting{descriptor, POLLOUT, 0};
    const int ready{::poll(&waiting, 1, static_cast<int>(timeout.count()))};
    if (ready == 0) {
        return errorOf(ETIMEDOUT);
    }
    if (ready < 0) {
        return lastError();
    }
    int result{0};
    socklen_t length{sizeof(result)};
    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &result, &length) != 0) {
        return lastError();
    }
    return errorOf(result);
}

std::error_code connectTo(const addrinfo& entry, std::chrono::milliseconds timeout,
                          const Logger& log) {
    const FileDescriptor sock{::socket(entry.ai_family,
                                       entry.ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                       entry.ai_protocol),
                              log};
    if (!sock.valid()) {
        return lastError();
    }
    if (::connect(sock.get(), entry.ai_addr, entry.ai_addrlen) == 0) {
        return {};
    }
    return errno == EINPROGRESS ? finishConnect(sock.get(), timeout) : lastError();
}
} // namespace

std::optional<StreamEndpoint> parseRtspUrl(std::string_view url) {
    if (url.substr(0, rtspScheme.size()) != rtspScheme) {
        return std::nullopt;
    }
    std::string_view authority{authorityOf(url)};
    StreamEndpoint endpoint;
    endpoint.port = std::string{defaultRtspPort};
    std::string_view host{authority};
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t close{authority.find(']')};
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, close - 1);
        authority = authority.substr(close + 1);
    } else {
        const std::size_t colon{authority.rfind(':')};
        host = authority.substr(0, colon);
        authority = colon == std::string_view::npos ? std::string_view{} : authority.substr(colon);
    }
    if (host.empty()) {
        return std::nullopt;
    }
    if (!authority.empty()) {
        if (authority.front() != ':' || authority.size() == 1) {
            return std::nullopt;
        }
        endpoint.port = std::string{authority.substr(1)};
    }
    endpoint.host = std::string{host};
    return endpoint;
}

bool programInPath(std::string_view name) {
    const char* path{std::getenv("PATH")};
    if (path == nullptr || name.empty()) {
        return false;
    }
    std::string_view folders{path};
    while (!folders.empty()) {
        const std::size_t colon{folders.find(':')};
        const std::string_view folder{folders.substr(0, colon)};
        const std::string candidate{std::string{folder.empty() ? "." : folder} + "/" +
                                    std::string{name}};
        if (::access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
        folders = colon == std::string_view::npos ? std::string_view{} : folders.substr(colon + 1);
    }
    return false;
}

std::error_code tryConnect(const StreamEndpoint& endpoint, std::chrono::milliseconds timeout,
                           const Logger& log) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found{nullptr};
    if (getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &found) != 0) {
        return errorOf(EHOSTUNREACH);
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> list{found, freeaddrinfo};
    std::error_code last{errorOf(EHOSTUNREACH)};
    for (const addrinfo* entry{list.get()}; entry != nullptr; entry = entry->ai_next) {
        last = connectTo(*entry, timeout, log);
        if (!last) {
            return {};
        }
    }
    return last;
}

bool checkStreamReady(std::string_view url, const Logger& log) {
    const std::optional<StreamEndpoint> endpoint{parseRtspUrl(url)};
    if (!endpoint) {
        log.error("the stream address must look like rtsp://host[:port]/path, got ", url);
        return false;
    }
    if (!programInPath(ffmpegName)) {
        log.error("the stream is on but ", ffmpegName,
                  " is not in PATH: install it (sudo apt install ", ffmpegName,
                  ") or run with --stream false");
        return false;
    }
    const std::error_code error{tryConnect(*endpoint, connectTimeout, log)};
    if (error) {
        log.error("the stream is on but cannot connect to ", endpoint->host, ":", endpoint->port,
                  " (", error.message(),
                  "): start the RTSP server first (bash deploy/sh/stream/run_mediamtx.sh) or run "
                  "with --stream false");
        return false;
    }
    return true;
}
