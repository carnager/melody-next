// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/protocol/client.hpp"

#include "trackknife/protocol/dispatch.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

namespace trackknife::protocol {
namespace {

[[nodiscard]] core::Error transport_error(std::string message) {
    return core::Error{.code = core::ErrorCode::io,
                       .message = std::move(message),
                       .context = {{.key = "errno", .value = std::strerror(errno)}}};
}

} // namespace

Client::Client(const int descriptor) : descriptor_(descriptor) {}

Client::~Client() { close(); }

core::Result<std::unique_ptr<Client>> Client::connect(const std::filesystem::path& socket_path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto text = socket_path.string();
    if (text.size() + 1U > sizeof(address.sun_path)) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "socket path is too long",
                                           .context = {{.key = "path", .value = text}}});
    }
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1U);

    const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return std::unexpected(transport_error("could not create a socket"));
    }
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        auto error = transport_error("could not reach the engine");
        ::close(descriptor);
        return std::unexpected(std::move(error));
    }

    std::unique_ptr<Client> client{new Client{descriptor}};
    client->reader_ = std::thread{[raw = client.get()] { raw->read_loop(); }};
    return client;
}

std::optional<Endpoint> Endpoint::parse(std::string_view text, std::string token) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    constexpr std::string_view scheme{"tcp://"};
    const bool explicit_tcp = text.starts_with(scheme);
    if (explicit_tcp) {
        text.remove_prefix(scheme.size());
    }
    if (!explicit_tcp && text.find('/') != std::string_view::npos) {
        Endpoint endpoint;
        endpoint.socket = std::filesystem::path{std::string{text}};
        return endpoint;
    }
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon + 1U >= text.size()) {
        return std::nullopt;
    }
    auto host = text.substr(0, colon);
    // [::1]:6601 -- the brackets are the URL spelling, not part of the address.
    if (host.size() >= 2U && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2U);
    }
    unsigned port = 0;
    for (const auto digit : text.substr(colon + 1U)) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        port = port * 10U + static_cast<unsigned>(digit - '0');
        if (port > 65535U) {
            return std::nullopt;
        }
    }
    if (port == 0U || host.empty()) {
        return std::nullopt;
    }
    Endpoint endpoint;
    endpoint.host = std::string{host};
    endpoint.port = static_cast<std::uint16_t>(port);
    endpoint.token = std::move(token);
    return endpoint;
}

std::string Endpoint::describe() const {
    if (!tcp()) {
        return socket.string();
    }
    const bool literal_v6 = host.find(':') != std::string::npos;
    return (literal_v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

core::Result<std::unique_ptr<Client>> Client::connect(const Endpoint& endpoint) {
    if (!endpoint.tcp()) {
        return connect(endpoint.socket);
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* found = nullptr;
    const auto service = std::to_string(endpoint.port);
    if (const auto resolved = ::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &found);
        resolved != 0) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::io,
                        .message = "could not resolve the engine's address",
                        .context = {{.key = "host", .value = endpoint.host},
                                    {.key = "reason", .value = ::gai_strerror(resolved)}}});
    }
    int descriptor = -1;
    for (auto* candidate = found; candidate != nullptr; candidate = candidate->ai_next) {
        descriptor = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (descriptor < 0) {
            continue;
        }
        if (::connect(descriptor, candidate->ai_addr, candidate->ai_addrlen) == 0) {
            break;
        }
        ::close(descriptor);
        descriptor = -1;
    }
    ::freeaddrinfo(found);
    if (descriptor < 0) {
        return std::unexpected(transport_error("could not reach the engine"));
    }

    std::unique_ptr<Client> client{new Client{descriptor}};
    client->reader_ = std::thread{[raw = client.get()] { raw->read_loop(); }};
    // First, before anything a caller does: until this succeeds the engine
    // refuses every other request, and a caller should learn that here rather
    // than from whichever call it happens to make first.
    auto admitted = client->call("session.authenticate", Json{{"token", endpoint.token}});
    if (!admitted) {
        return std::unexpected(std::move(admitted.error()));
    }
    return client;
}

void Client::on_event(EventHandler handler) {
    const std::lock_guard guard{handler_mutex_};
    handler_ = std::move(handler);
}

bool Client::connected() const noexcept { return open_.load(); }

void Client::close() {
    if (closed_.exchange(true)) {
        return;
    }
    open_.store(false);
    if (descriptor_ >= 0) {
        // Half-closing wakes the reader without the descriptor being pulled
        // out from under it.
        ::shutdown(descriptor_, SHUT_RDWR);
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    fail_everything("the connection was closed");
}

void Client::fail_everything(const std::string& reason) {
    const std::lock_guard guard{mutex_};
    failure_ = reason;
    for (auto& [id, slot] : pending_) {
        slot->abandoned = true;
    }
    for (auto& [job_id, job] : jobs_) {
        job->abandoned = true;
    }
    arrived_.notify_all();
}

core::Result<void> Client::write_line(const std::string& line) {
    if (!open_.load()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::io, .message = "the connection is closed", .context = {}});
    }
    auto payload = line;
    payload.push_back('\n');
    const std::lock_guard guard{write_mutex_};
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto sent =
            ::send(descriptor_, payload.data() + written, payload.size() - written, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(transport_error("could not send to the engine"));
        }
        written += static_cast<std::size_t>(sent);
    }
    return {};
}

void Client::read_loop() {
    std::string pending;
    std::array<char, 4096> buffer{};
    while (open_.load()) {
        const auto received = ::recv(descriptor_, buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (received == 0) {
            break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(received));

        std::size_t start = 0;
        while (true) {
            const auto newline = pending.find('\n', start);
            if (newline == std::string::npos) {
                break;
            }
            auto line = pending.substr(start, newline - start);
            start = newline + 1U;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            auto parsed = parse_message(line);
            if (!parsed) {
                // A line the engine sent that this client cannot read is the
                // engine's problem to fix, not a reason to drop the
                // connection and lose everything else on it.
                continue;
            }
            if (const auto* response = std::get_if<Response>(&*parsed)) {
                const std::lock_guard guard{mutex_};
                if (const auto found = pending_.find(response->id); found != pending_.end()) {
                    found->second->response = *response;
                    arrived_.notify_all();
                }
                // A response to an id nobody is waiting for is dropped: the
                // caller timed out or went away, and there is nothing to do
                // with the answer.
                continue;
            }
            if (const auto* event = std::get_if<Event>(&*parsed)) {
                // A job event is delivered to whoever is waiting on that job
                // as well as to the general handler: run_job must not depend
                // on a caller having installed one, and a caller that did
                // install one should still see everything.
                if (event->name == "job.progress" || event->name == "job.finished") {
                    const auto identity = event->data.find("job_id");
                    if (identity != event->data.end() && identity->is_string()) {
                        std::shared_ptr<RunningJob> waiting;
                        {
                            const std::lock_guard guard{mutex_};
                            if (const auto found = jobs_.find(identity->get<std::string>());
                                found != jobs_.end()) {
                                waiting = found->second;
                            }
                        }
                        if (waiting) {
                            if (event->name == "job.progress") {
                                if (waiting->on_progress) {
                                    waiting->on_progress(event->data);
                                }
                            } else {
                                const std::lock_guard guard{mutex_};
                                waiting->outcome = event->data.value("outcome", Json::object());
                                arrived_.notify_all();
                            }
                        }
                    }
                }
                EventHandler handler;
                {
                    const std::lock_guard guard{handler_mutex_};
                    handler = handler_;
                }
                if (handler) {
                    handler(*event);
                }
            }
        }
        pending.erase(0, start);
    }
    open_.store(false);
    fail_everything("the engine closed the connection");
}

core::Result<Json> Client::call(const std::string& method, const Json& params,
                                const std::chrono::milliseconds timeout) {
    auto slot = std::make_shared<Pending>();
    std::int64_t id = 0;
    {
        const std::lock_guard guard{mutex_};
        if (!failure_.empty()) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::io, .message = failure_, .context = {}});
        }
        id = next_id_++;
        pending_.emplace(id, slot);
    }

    const auto sent =
        write_line(encode_message(Request{.id = id, .method = method, .params = params}));
    if (!sent) {
        const std::lock_guard guard{mutex_};
        pending_.erase(id);
        return std::unexpected(sent.error());
    }

    std::unique_lock lock{mutex_};
    const auto answered = arrived_.wait_for(
        lock, timeout, [&slot] { return slot->response.has_value() || slot->abandoned; });
    pending_.erase(id);
    if (!answered) {
        // A timeout is an error, not a hang. An engine that has gone away
        // must not wedge the thread that asked it something.
        return std::unexpected(core::Error{.code = core::ErrorCode::io,
                                           .message = "the engine did not answer in time",
                                           .context = {{.key = "method", .value = method}}});
    }
    if (!slot->response) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::io,
                        .message = failure_.empty() ? "the connection was lost" : failure_,
                        .context = {{.key = "method", .value = method}}});
    }
    const auto& response = *slot->response;
    if (response.error) {
        // The wire code is mapped back by name; an unknown one lands on
        // invariant rather than failing, so a newer engine reporting
        // something this client has never heard of is still usable.
        core::Error error{.code = error_code_from_name(response.error->code),
                          .message = response.error->message,
                          .context = {}};
        for (const auto& [key, value] : response.error->context.items()) {
            error.context.push_back(
                {.key = key, .value = value.is_string() ? value.get<std::string>() : value.dump()});
        }
        return std::unexpected(std::move(error));
    }
    return response.result.value_or(Json{});
}

core::Result<Json> Client::run_job(const std::string& job, const Json& params,
                                   const std::function<void(const Json&)>& on_progress,
                                   const core::CancellationToken& cancellation) {
    Json submission = Json::object();
    submission["job"] = job;
    if (!params.is_null() && !params.empty()) {
        submission["params"] = params;
    }
    auto submitted = call("job.submit", submission);
    if (!submitted) {
        return std::unexpected(std::move(submitted.error()));
    }
    const auto identity = submitted->find("job_id");
    if (identity == submitted->end() || !identity->is_string()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::backend,
                                           .message = "the engine did not name the job it started",
                                           .context = {{.key = "job", .value = job}}});
    }
    const auto job_id = identity->get<std::string>();

    auto slot = std::make_shared<RunningJob>();
    slot->on_progress = on_progress;
    {
        const std::lock_guard guard{mutex_};
        jobs_.emplace(job_id, slot);
    }

    // No overall timeout: a scan legitimately runs for minutes. What bounds
    // the wait is the connection dying, which abandons the job, or the caller
    // cancelling.
    bool asked_to_stop = false;
    std::unique_lock lock{mutex_};
    while (!slot->outcome && !slot->abandoned) {
        arrived_.wait_for(lock, std::chrono::milliseconds{100});
        if (!asked_to_stop && cancellation.is_cancellation_requested()) {
            asked_to_stop = true;
            lock.unlock();
            static_cast<void>(call("job.cancel", Json{{"job_id", job_id}}));
            lock.lock();
        }
    }
    const auto outcome = slot->outcome;
    jobs_.erase(job_id);
    if (!outcome) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::io,
                        .message = failure_.empty() ? "the connection was lost" : failure_,
                        .context = {{.key = "job", .value = job}}});
    }
    return *outcome;
}

core::Result<void> Client::notify(const std::string& method, const Json& params) {
    return write_line(encode_message(Notification{.method = method, .params = params}));
}

} // namespace trackknife::protocol
