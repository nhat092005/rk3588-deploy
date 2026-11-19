#ifndef STREAM_CHECK_H
#define STREAM_CHECK_H

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "logger.h"

/** @brief The host and port of an RTSP address, as getaddrinfo() wants them. */
struct StreamEndpoint {
    std::string host;
    std::string port;
};

/**
 * @brief Splits `rtsp://[user:password@]host[:port]/path`.
 * @param url The address, which must start with `rtsp://`.
 * @return The host (without the brackets of an IPv6 address) and the port (554 when the address
 *         gives none), or empty when the address is not of that form.
 */
std::optional<StreamEndpoint> parseRtspUrl(std::string_view url);

/**
 * @brief Whether a program is an executable file in one of the folders of PATH.
 * @param name A bare program name, not empty.
 */
bool programInPath(std::string_view name);

/**
 * @brief Tries one TCP connection and closes it at once.
 * @param endpoint Where to connect.
 * @param timeout How long to wait for the answer.
 * @param log Logger for the descriptor of the socket.
 * @return No error when the connection was made, else why not (refused, timed out, no such host).
 */
std::error_code tryConnect(const StreamEndpoint& endpoint, std::chrono::milliseconds timeout,
                           const Logger& log);

/**
 * @brief The start-up check of a stream that was asked for: the address is valid, `ffmpeg` is
 *        there, and something listens at the address (the RTSP server). Call it only when the
 *        stream is on, before the camera is opened.
 * @param url The RTSP address, see StreamConfig and defaultStreamUrl.
 * @param log Logger that receives the reason and what to do about it.
 * @return false when the stream cannot work, which was logged: the caller ends the run.
 */
[[nodiscard]] bool checkStreamReady(std::string_view url, const Logger& log);

#endif // STREAM_CHECK_H
