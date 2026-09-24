// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "agent/agent.hpp"
#include "agent/speaker_arbiter.hpp"
#include "trackknife/discovery/mdns.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::agent {

// Plays for every engine on the network: an agent for each one found, gone
// when it says goodbye, the speakers shared by the arbiter.
class Guests final {
  public:
    struct Config final {
        // What each engine calls this machine among its outputs.
        std::string name;
        // For engines that want one (auth=1). Empty: none is sent.
        std::string password;
        // Where the engines' music is mounted here; without it, it streams.
        std::optional<std::filesystem::path> music_root;
        // This machine's own engine, which is not played for.
        std::string own_id;
        // Engines already played for another way (host:port), not twice.
        std::vector<std::string> already;
    };

    Guests(Config config, SpeakerArbiter& arbiter);
    Guests(const Guests&) = delete;
    Guests& operator=(const Guests&) = delete;
    ~Guests();

    // Starts looking; false when this machine cannot use multicast DNS.
    [[nodiscard]] bool start();
    void stop();
    // The engines played for now, by name.
    [[nodiscard]] std::vector<std::string> engines() const;

  private:
    void update(const std::vector<discovery::Found>& found);

    struct Guest final {
        std::string name;
        // Where it was reached. An engine on this machine is heard on every
        // interface, from a different address each time; any of them
        // reaches it, so the first is kept.
        std::string where;
        std::unique_ptr<Agent> agent;
    };

    Config config_;
    SpeakerArbiter* arbiter_;
    std::unique_ptr<discovery::Browser> browser_;
    mutable std::mutex mutex_;
    std::map<std::string, Guest> guests_; // by the engine's identity
};

} // namespace trackknife::agent
