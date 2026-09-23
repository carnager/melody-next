// SPDX-License-Identifier: GPL-3.0-only

#include "agent/agent.hpp"

#include "trackknife/output/audition_methods.hpp"
#include "trackknife/output/audition_wire.hpp"
#include "trackknife/protocol/message.hpp"

#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

namespace trackknife::agent {
namespace {

using protocol::Json;

constexpr std::chrono::seconds reconnect_delay{2};
constexpr std::chrono::milliseconds report_interval{250};
constexpr std::chrono::seconds handshake_timeout{10};

[[nodiscard]] core::Error handshake_error(std::string message) {
    return core::Error{.code = core::ErrorCode::io, .message = std::move(message), .context = {}};
}

[[nodiscard]] std::string random_instance() {
    std::array<unsigned char, 8> bytes{};
    if (::getrandom(bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size())) {
        return "pid-" + std::to_string(::getpid());
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0FU]);
    }
    return text;
}

[[nodiscard]] bool send_line(const int descriptor, const std::string& line) {
    const auto payload = line + "\n";
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto sent =
            ::send(descriptor, payload.data() + written, payload.size() - written, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(sent);
    }
    return true;
}

// One line, before the connection is handed to the server that serves it:
// read a byte at a time so nothing after the newline is taken from it.
[[nodiscard]] core::Result<std::string> read_line(const int descriptor) {
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + handshake_timeout;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return std::unexpected(handshake_error("the engine did not answer"));
        }
        pollfd watched{.fd = descriptor, .events = POLLIN, .revents = 0};
        if (::poll(&watched, 1, static_cast<int>(left.count())) <= 0) {
            continue;
        }
        char byte = 0;
        const auto received = ::recv(descriptor, &byte, 1, 0);
        if (received <= 0) {
            return std::unexpected(handshake_error("the engine closed the connection"));
        }
        if (byte == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        line.push_back(byte);
    }
}

// Sends one request and reads until its answer; events before it are skipped.
[[nodiscard]] core::Result<Json> ask(const int descriptor, const std::int64_t id,
                                     const std::string& method, const Json& params) {
    if (!send_line(descriptor, protocol::encode_message(protocol::Request{
                                   .id = id, .method = method, .params = params}))) {
        return std::unexpected(handshake_error("could not write to the engine"));
    }
    while (true) {
        auto line = read_line(descriptor);
        if (!line) {
            return std::unexpected(std::move(line.error()));
        }
        if (line->starts_with("HTTP/")) {
            return std::unexpected(handshake_error(
                "that is melodyd's stream port (--http); --server wants the port given to "
                "melodyd --listen"));
        }
        auto parsed = protocol::parse_message(*line);
        if (!parsed) {
            continue;
        }
        const auto* response = std::get_if<protocol::Response>(&*parsed);
        if (response == nullptr || response->id != id) {
            continue;
        }
        if (response->error) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::backend,
                            .message = response->error->message,
                            .context = {{.key = "code", .value = response->error->code}}});
        }
        return response->result.value_or(Json::object());
    }
}

} // namespace

core::Result<std::unique_ptr<Agent>>
Agent::create(AgentConfig config, std::unique_ptr<audio::LocalAuditionService> audition) {
    if (config.name.empty()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "an agent needs a name",
                                           .context = {}});
    }
    if (!audition) {
        auto created = audio::LocalAuditionService::create();
        if (!created) {
            return std::unexpected(std::move(created.error()));
        }
        audition = std::move(*created);
        static_cast<void>(audition->refresh_output_devices());
    }
    return std::unique_ptr<Agent>{new Agent{std::move(config), std::move(audition)}};
}

Agent::Agent(AgentConfig config, std::unique_ptr<audio::LocalAuditionService> audition)
    : config_(std::move(config)), instance_(random_instance()), audition_(std::move(audition)) {
    output::register_audition_methods(dispatcher_, *audition_, config_.music_root);
    server_ = engine::Server::detached(dispatcher_);
}

Agent::~Agent() { stop(); }

void Agent::start() {
    if (running_.exchange(true)) {
        return;
    }
    pause_.reset();
    report_pause_.reset();
    connector_ = std::thread{[this] { connect_loop(); }};
    reporter_ = std::thread{[this] { report_loop(); }};
}

void Agent::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    pause_.interrupt();
    report_pause_.interrupt();
    server_->stop();
    if (connector_.joinable()) {
        connector_.join();
    }
    if (reporter_.joinable()) {
        reporter_.join();
    }
    registered_.store(false);
}

core::Result<int> Agent::register_with_engine() {
    auto opened = protocol::open_connection(config_.server);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    const auto descriptor = *opened;
    const auto fail = [descriptor](core::Error error) -> core::Result<int> {
        ::close(descriptor);
        return std::unexpected(std::move(error));
    };
    // ADR-0223: the password first, when there is one, like any client.
    if (!config_.server.token.empty()) {
        auto admitted =
            ask(descriptor, 1, "session.authenticate", Json{{"password", config_.server.token}});
        if (!admitted) {
            return fail(std::move(admitted.error()));
        }
    }
    auto registered = ask(descriptor, 2, "agent.register",
                          Json{{"name", config_.name},
                               {"instance", instance_},
                               {"files", !config_.stream_only},
                               {"protocol", 1}});
    if (!registered) {
        return fail(std::move(registered.error()));
    }
    return descriptor;
}

void Agent::connect_loop() {
    std::string last_problem;
    while (running_.load()) {
        auto descriptor = register_with_engine();
        if (!descriptor) {
            if (descriptor.error().message != last_problem) {
                last_problem = descriptor.error().message;
                std::cerr << "melody-agent: " << last_problem << "; retrying\n";
            }
            static_cast<void>(pause_.wait(reconnect_delay));
            continue;
        }
        last_problem.clear();
        std::cerr << "melody-agent: registered as \"" << config_.name << "\"\n";
        // The connection turns round: from here the engine asks.
        server_->attach(*descriptor);
        registered_.store(true);
        while (running_.load() && server_->connections() > 0U) {
            static_cast<void>(pause_.wait(std::chrono::milliseconds{200}));
        }
        registered_.store(false);
        if (running_.load()) {
            std::cerr << "melody-agent: the engine went away; reconnecting\n";
            // Whatever was playing belonged to that connection. Nothing
            // loaded is nothing to stop, and saying it could not be is noise.
            if (audition_->snapshot().state != audio::LocalAuditionState::empty) {
                static_cast<void>(audition_->stop());
            }
            static_cast<void>(pause_.wait(reconnect_delay));
        }
    }
}

void Agent::report_loop() {
    const auto sink = server_->sink();
    Json last;
    std::string last_problem;
    bool was_registered = false;
    while (running_.load()) {
        const bool now_registered = registered_.load();
        if (now_registered && !was_registered) {
            // A new connection has heard nothing yet.
            last = Json{};
        }
        was_registered = now_registered;
        const auto snapshot = audition_->snapshot();
        // What went wrong is said here too: the engine hears it in the
        // report, but whoever set this machine up is looking at this log.
        const auto problem = snapshot.error ? snapshot.error->message : std::string{};
        if (problem != last_problem) {
            if (!problem.empty()) {
                std::cerr << "melody-agent: " << problem;
                for (const auto& [key, value] : snapshot.error->context) {
                    std::cerr << " (" << key << ": " << value << ")";
                }
                std::cerr << "\n";
            }
            last_problem = problem;
        }
        auto report = output::to_json(snapshot);
        const bool moving = snapshot.state == audio::LocalAuditionState::playing ||
                            snapshot.state == audio::LocalAuditionState::buffering ||
                            snapshot.state == audio::LocalAuditionState::draining;
        // On every change, and while playing, so the position the engine
        // shows keeps moving.
        if (registered_.load() && (moving || report != last)) {
            sink(protocol::Event{.name = output::changed_event, .data = report});
            last = std::move(report);
        }
        static_cast<void>(report_pause_.wait(report_interval));
    }
}

} // namespace trackknife::agent
