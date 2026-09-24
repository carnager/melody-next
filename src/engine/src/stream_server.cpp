// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/stream_server.hpp"

#include "trackknife/engine/token.hpp"
#include "trackknife/protocol/message.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::engine {
namespace {

// A request head is a line and a few headers; anything longer is not a
// decoder asking for a file.
constexpr std::size_t maximum_head_bytes = 16U * 1024U;
constexpr std::chrono::seconds head_timeout{10};
// Agents are a handful of rooms. More transfers than this at once is
// something other than agents.
constexpr std::size_t maximum_transfers = 32U;

[[nodiscard]] core::Error system_error(std::string message) {
    return core::Error{.code = core::ErrorCode::io,
                       .message = std::move(message),
                       .context = {{.key = "errno", .value = std::strerror(errno)}}};
}

[[nodiscard]] bool send_all(const int descriptor, const std::string_view bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto sent =
            ::send(descriptor, bytes.data() + written, bytes.size() - written, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(sent);
    }
    return true;
}

void answer_status(const int descriptor, const std::string_view status) {
    std::string response{"HTTP/1.1 "};
    response.append(status);
    response.append("\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    static_cast<void>(send_all(descriptor, response));
}

[[nodiscard]] std::optional<std::string> read_head(const int descriptor) {
    std::string head;
    const auto deadline = std::chrono::steady_clock::now() + head_timeout;
    std::array<char, 2048> buffer{};
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (head.size() > maximum_head_bytes) {
            return std::nullopt;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return std::nullopt;
        }
        pollfd watched{.fd = descriptor, .events = POLLIN, .revents = 0};
        const auto ready = ::poll(&watched, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return std::nullopt;
        }
        // The head is all a client sends; whatever follows it is nothing
        // this server reads, so reading past it loses nothing.
        const auto received = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
            return std::nullopt;
        }
        head.append(buffer.data(), static_cast<std::size_t>(received));
        // A request starts with its method. Anything else -- a protocol
        // client sent to this port by mistake -- is answered now rather than
        // left waiting out the timeout for a head that never comes.
        if (head.front() < 'A' || head.front() > 'Z') {
            return std::nullopt;
        }
    }
    return head;
}

[[nodiscard]] bool equal_ignoring_case(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        };
        if (lower(left[index]) != lower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::string_view> header(const std::string_view head,
                                                     const std::string_view name) {
    auto rest = head.substr(head.find("\r\n") + 2U);
    while (!rest.empty()) {
        const auto end = rest.find("\r\n");
        const auto line = rest.substr(0, end);
        if (line.empty()) {
            break;
        }
        if (const auto colon = line.find(':'); colon != std::string_view::npos &&
                                               equal_ignoring_case(line.substr(0, colon), name)) {
            auto value = line.substr(colon + 1U);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1U);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1U);
            }
            return value;
        }
        if (end == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(end + 2U);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> number(const std::string_view text) {
    std::int64_t value = 0;
    const auto* const last = text.data() + text.size();
    const auto [end, error] = std::from_chars(text.data(), last, value);
    if (text.empty() || error != std::errc{} || end != last || value < 0) {
        return std::nullopt;
    }
    return value;
}

struct Span final {
    std::int64_t first{0};
    std::int64_t last{0};
};

enum class RangeReading : std::uint8_t { whole, span, unsatisfiable };

// One range of `bytes=`; a list of them is answered with the whole file, as
// the standard lets a server do.
[[nodiscard]] RangeReading read_range(const std::optional<std::string_view> requested,
                                      const std::int64_t size, Span& span) {
    if (!requested || !requested->starts_with("bytes=") ||
        requested->find(',') != std::string_view::npos) {
        return RangeReading::whole;
    }
    const auto spec = requested->substr(6U);
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return RangeReading::whole;
    }
    const auto from = spec.substr(0, dash);
    const auto to = spec.substr(dash + 1U);
    if (from.empty()) {
        // The last n bytes.
        const auto suffix = number(to);
        if (!suffix || *suffix == 0 || size == 0) {
            return RangeReading::unsatisfiable;
        }
        span = Span{.first = size - std::min(*suffix, size), .last = size - 1};
        return RangeReading::span;
    }
    const auto first = number(from);
    if (!first) {
        return RangeReading::whole;
    }
    if (*first >= size) {
        return RangeReading::unsatisfiable;
    }
    auto last = size - 1;
    if (!to.empty()) {
        const auto given = number(to);
        if (!given || *given < *first) {
            return RangeReading::whole;
        }
        last = std::min(*given, size - 1);
    }
    span = Span{.first = *first, .last = last};
    return RangeReading::span;
}

} // namespace

struct StreamServer::Transfer final {
    int descriptor{-1};
    std::atomic<bool> done{false};
    std::thread worker;
};

core::Result<std::unique_ptr<StreamServer>> StreamServer::listen(const std::string& host,
                                                                 const std::uint16_t port,
                                                                 Resolve resolve) {
    if (!resolve) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "a stream server needs something to say what it serves",
                                           .context = {}});
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* found = nullptr;
    const auto service = std::to_string(port);
    if (const auto resolved =
            ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &found);
        resolved != 0) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "could not resolve the stream address",
                        .context = {{.key = "host", .value = host},
                                    {.key = "reason", .value = ::gai_strerror(resolved)}}});
    }
    int listener = -1;
    for (auto* candidate = found; candidate != nullptr; candidate = candidate->ai_next) {
        listener = ::socket(candidate->ai_family, candidate->ai_socktype | SOCK_CLOEXEC,
                            candidate->ai_protocol);
        if (listener < 0) {
            continue;
        }
        const int reuse = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (::bind(listener, candidate->ai_addr, candidate->ai_addrlen) == 0) {
            break;
        }
        ::close(listener);
        listener = -1;
    }
    ::freeaddrinfo(found);
    if (listener < 0) {
        return std::unexpected(system_error("could not bind the stream address"));
    }
    if (::listen(listener, 16) != 0) {
        const auto failed = system_error("could not listen for streams");
        ::close(listener);
        return std::unexpected(failed);
    }
    sockaddr_storage bound{};
    socklen_t length = sizeof(bound);
    std::uint16_t bound_port = port;
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        if (bound.ss_family == AF_INET) {
            bound_port = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
            bound_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
        }
    }
    std::array<int, 2> wakeup{-1, -1};
    if (::pipe2(wakeup.data(), O_CLOEXEC) != 0) {
        const auto failed = system_error("could not create the stream server's wakeup");
        ::close(listener);
        return std::unexpected(failed);
    }
    return std::unique_ptr<StreamServer>{new StreamServer{
        listener, wakeup[0], wakeup[1], bound_port, std::move(resolve)}};
}

StreamServer::StreamServer(const int listener, const int wakeup_read, const int wakeup_write,
                           const std::uint16_t port, Resolve resolve)
    : listener_(listener), wakeup_read_(wakeup_read), wakeup_write_(wakeup_write), port_(port),
      resolve_(std::move(resolve)) {}

StreamServer::~StreamServer() {
    stop();
    ::close(listener_);
    ::close(wakeup_read_);
    ::close(wakeup_write_);
}

void StreamServer::start() {
    if (running_.exchange(true)) {
        return;
    }
    acceptor_ = std::thread{[this] { accept_loop(); }};
}

void StreamServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    const char byte = 'x';
    while (::write(wakeup_write_, &byte, 1) < 0 && errno == EINTR) {
    }
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    std::vector<std::shared_ptr<Transfer>> transfers;
    {
        const std::lock_guard guard{mutex_};
        transfers.swap(transfers_);
        for (const auto& transfer : transfers) {
            if (transfer->done.load()) {
                continue;
            }
            // Unblocks a transfer parked on a paused agent's full socket.
            ::shutdown(transfer->descriptor, SHUT_RDWR);
        }
    }
    for (const auto& transfer : transfers) {
        if (transfer->worker.joinable()) {
            transfer->worker.join();
        }
    }
}

void StreamServer::reap() {
    const std::lock_guard guard{mutex_};
    std::erase_if(transfers_, [](const std::shared_ptr<Transfer>& transfer) {
        if (!transfer->done.load()) {
            return false;
        }
        if (transfer->worker.joinable()) {
            transfer->worker.join();
        }
        return true;
    });
}

void StreamServer::accept_loop() {
    while (running_.load()) {
        std::array<pollfd, 2> watched{pollfd{.fd = listener_, .events = POLLIN, .revents = 0},
                                      pollfd{.fd = wakeup_read_, .events = POLLIN, .revents = 0}};
        if (::poll(watched.data(), watched.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if ((watched[1].revents & POLLIN) != 0) {
            return;
        }
        if ((watched[0].revents & POLLIN) == 0) {
            continue;
        }
        const auto accepted = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (accepted < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            return;
        }
        reap();
        const std::lock_guard guard{mutex_};
        if (transfers_.size() >= maximum_transfers) {
            answer_status(accepted, "503 Service Unavailable");
            ::close(accepted);
            continue;
        }
        auto transfer = std::make_shared<Transfer>();
        transfer->descriptor = accepted;
        transfer->worker = std::thread{[this, transfer] { serve(transfer); }};
        transfers_.push_back(std::move(transfer));
    }
}

void StreamServer::serve(const std::shared_ptr<Transfer>& transfer) {
    const auto descriptor = transfer->descriptor;
    const auto finish = [this, &transfer, descriptor] {
        // Under the lock, so stop() never shuts down a descriptor already
        // closed and perhaps reused.
        const std::lock_guard guard{mutex_};
        ::close(descriptor);
        transfer->done.store(true);
    };
    const auto head = read_head(descriptor);
    if (!head) {
        answer_status(descriptor, "400 Bad Request");
        finish();
        return;
    }
    const std::string_view text{*head};
    const auto line = text.substr(0, text.find("\r\n"));
    const auto method_end = line.find(' ');
    const auto target_end = line.find(' ', method_end + 1U);
    if (method_end == std::string_view::npos || target_end == std::string_view::npos) {
        answer_status(descriptor, "400 Bad Request");
        finish();
        return;
    }
    const auto method = line.substr(0, method_end);
    const auto target = line.substr(method_end + 1U, target_end - method_end - 1U);
    const bool head_only = method == "HEAD";
    if (method != "GET" && !head_only) {
        answer_status(descriptor, "405 Method Not Allowed");
        finish();
        return;
    }
    const auto question = target.find('?');
    if (target.substr(0, question) != "/stream" || question == std::string_view::npos) {
        answer_status(descriptor, "404 Not Found");
        finish();
        return;
    }
    const auto query = target.substr(question + 1U);
    // Which file, if any, is the engine's to say; this only turns its
    // answer into a status.
    const auto resolved = resolve_(query);
    if (!resolved) {
        const auto code = resolved.error().code;
        answer_status(descriptor, code == core::ErrorCode::unauthorized       ? "403 Forbidden"
                                  : code == core::ErrorCode::not_found        ? "404 Not Found"
                                  : code == core::ErrorCode::invalid_argument ? "400 Bad Request"
                                                                              : "503 Service Unavailable");
        finish();
        return;
    }
    const auto* raw_path = &*resolved;
    const auto file = ::open(raw_path->c_str(), O_RDONLY | O_CLOEXEC);
    struct stat status{};
    if (file < 0 || ::fstat(file, &status) != 0 || !S_ISREG(status.st_mode)) {
        if (file >= 0) {
            ::close(file);
        }
        answer_status(descriptor, "404 Not Found");
        finish();
        return;
    }
    const std::int64_t size = status.st_size;
    Span span{.first = 0, .last = size - 1};
    const auto reading = read_range(header(text, "Range"), size, span);
    if (reading == RangeReading::unsatisfiable) {
        std::string response{"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */"};
        response.append(std::to_string(size));
        response.append("\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        static_cast<void>(send_all(descriptor, response));
        ::close(file);
        finish();
        return;
    }
    const auto length = size == 0 ? std::int64_t{0} : span.last - span.first + 1;
    std::string response;
    if (reading == RangeReading::span) {
        response = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " +
                   std::to_string(span.first) + "-" + std::to_string(span.last) + "/" +
                   std::to_string(size) + "\r\n";
    } else {
        response = "HTTP/1.1 200 OK\r\n";
    }
    response += "Content-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n"
                "Content-Length: " +
                std::to_string(length) + "\r\nConnection: close\r\n\r\n";
    if (send_all(descriptor, response) && !head_only) {
        // Read and sent rather than sendfile(2), which raises SIGPIPE when a
        // decoder hangs up mid-file -- as one does on every seek.
        std::vector<char> buffer(std::size_t{64} * 1024U);
        auto offset = span.first;
        auto left = length;
        while (left > 0 && running_.load()) {
            const auto read = ::pread(file, buffer.data(),
                                      static_cast<std::size_t>(std::min<std::int64_t>(
                                          left, static_cast<std::int64_t>(buffer.size()))),
                                      offset);
            if (read < 0 && errno == EINTR) {
                continue;
            }
            if (read <= 0 ||
                !send_all(descriptor, std::string_view{buffer.data(),
                                                       static_cast<std::size_t>(read)})) {
                break;
            }
            offset += read;
            left -= read;
        }
    }
    ::close(file);
    finish();
}

} // namespace trackknife::engine
