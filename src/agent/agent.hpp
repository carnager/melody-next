// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_audition.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/interruptible_pause.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/protocol/client.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace trackknife::agent {

struct AgentConfig final {
    protocol::Endpoint server;
    std::string name;
    // Where the files are on this machine: relative paths the engine sends
    // are under it. Without one, only absolute paths and streams play.
    std::optional<std::filesystem::path> music_root;
    // Asks for streams rather than files: this machine cannot open them.
    bool stream_only{false};
};

// ADR-0228: an output agent. Connects to an engine, registers, and then plays
// what the engine asks on this machine's audio, reporting its state as it
// goes. It decides nothing about what plays next: the engine's player does.
class Agent final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<Agent>>
    create(AgentConfig config, std::unique_ptr<audio::LocalAuditionService> audition = nullptr);

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    Agent(Agent&&) = delete;
    Agent& operator=(Agent&&) = delete;
    ~Agent();

    // Connects in the background, and again whenever the connection drops.
    void start();
    void stop();
    [[nodiscard]] bool registered() const noexcept { return registered_.load(); }
    [[nodiscard]] audio::LocalAuditionService& audition() noexcept { return *audition_; }

  private:
    Agent(AgentConfig config, std::unique_ptr<audio::LocalAuditionService> audition);
    void connect_loop();
    void report_loop();
    [[nodiscard]] core::Result<int> register_with_engine();

    AgentConfig config_;
    std::string instance_;
    std::unique_ptr<audio::LocalAuditionService> audition_;
    protocol::Dispatcher dispatcher_;
    std::unique_ptr<engine::Server> server_;
    std::atomic_bool running_{false};
    std::atomic_bool registered_{false};
    engine::InterruptiblePause pause_;
    engine::InterruptiblePause report_pause_;
    std::thread connector_;
    std::thread reporter_;
};

} // namespace trackknife::agent
