// The start-up check of the stream: the RTSP address is split into host and port, `ffmpeg` is
// looked up in PATH, and a TCP connection to a listening port works while one to a closed port or
// an unknown host is refused. Plain C++ and POSIX sockets on the loopback, runs on the PC. Exit
// code 0 when every check passes.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "handles.h"
#include "logger.h"
#include "stream_check.h"

namespace {
constexpr std::chrono::milliseconds wait{1'000};
constexpr int backlog{1};

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

bool endpointIs(std::string_view url, const char* host, const char* port) {
    const std::optional<StreamEndpoint> endpoint{parseRtspUrl(url)};
    return endpoint && endpoint->host == host && endpoint->port == port;
}

void checkParse(Check& check) {
    check.expect(endpointIs("rtsp://127.0.0.1:8554/live", "127.0.0.1", "8554"), "host and port");
    check.expect(endpointIs("rtsp://board/live", "board", "554"), "default port 554");
    check.expect(endpointIs("rtsp://board", "board", "554"), "no path");
    check.expect(endpointIs("rtsp://user:pass@host:1/x", "host", "1"), "user name and password");
    check.expect(endpointIs("rtsp://[::1]:8554/live", "::1", "8554"), "IPv6 with a port");
    check.expect(endpointIs("rtsp://[::1]/live", "::1", "554"), "IPv6 without a port");
    check.expect(!parseRtspUrl("http://host:8554/live"), "another scheme is refused");
    check.expect(!parseRtspUrl("host:8554/live"), "no scheme is refused");
    check.expect(!parseRtspUrl("rtsp://"), "no host is refused");
    check.expect(!parseRtspUrl("rtsp:///live"), "an empty host is refused");
    check.expect(!parseRtspUrl("rtsp://host:/live"), "an empty port is refused");
    check.expect(!parseRtspUrl("rtsp://[::1/live"), "an open bracket is refused");
}

void checkPath(Check& check) {
    check.expect(programInPath("sh"), "sh is in PATH");
    check.expect(!programInPath("no_such_program_for_the_stream_check"), "a missing program");
    check.expect(!programInPath(""), "an empty name");
}

// A socket that listens on a free port of the loopback. Returns the port, and the descriptor
// stays open in `listener`.
std::uint16_t listenOnLoopback(FileDescriptor& listener, const Logger& log) {
    listener = FileDescriptor{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), log};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length{sizeof(address)};
    if (!listener.valid() ||
        ::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), backlog) != 0 ||
        ::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        std::abort();
    }
    return ntohs(address.sin_port);
}

void checkConnect(Check& check, const Logger& log) {
    FileDescriptor listener;
    const std::uint16_t port{listenOnLoopback(listener, log)};
    const StreamEndpoint open{"127.0.0.1", std::to_string(port)};
    check.expect(!tryConnect(open, wait, log), "a listening port accepts");
    check.expect(checkStreamReady("rtsp://127.0.0.1:" + std::to_string(port) + "/live", log) ==
                     programInPath("ffmpeg"),
                 "the check passes with ffmpeg and a listener, fails without ffmpeg");
    check.expect(!listener.close(), "the listener closed");
    check.expect(static_cast<bool>(tryConnect(open, wait, log)), "a closed port is refused");
    check.expect(!checkStreamReady("rtsp://127.0.0.1:" + std::to_string(port) + "/live", log),
                 "the check fails when nothing listens");
    const StreamEndpoint unknown{"no-such-host.invalid", "8554"};
    check.expect(static_cast<bool>(tryConnect(unknown, wait, log)), "an unknown host fails");
    check.expect(!checkStreamReady("http://x/live", log), "a bad address fails the check");
}
} // namespace

int main() {
    const Logger log{"TEST_STREAM_CHECK", LogLevel::Debug};
    Check check{log};
    checkParse(check);
    checkPath(check);
    checkConnect(check, log);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
