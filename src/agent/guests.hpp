// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "agent/agent.hpp"
#include "agent/speaker_arbiter.hpp"
#include "trackknife/discovery/mdns.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::agent {

// ADR-0286: an [engine NAME] section -- an engine played for, or not,
// otherwise than the rest.
struct ListedEngine final {
    // The name it is announced by.
    std::string name;
    // Where it is reached while the network does not find it (WireGuard).
    // Its token is the password to give.
    std::optional<protocol::Endpoint> address;
    std::optional<std::filesystem::path> music_root;
    std::optional<StreamChoice> stream;
    // Empty: the one every engine is given.
    std::string password;
    // Whether it is played for at all.
    bool play{true};
};

struct GuestsConfig final {
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
    // ADR-0239: what is asked streamed, when there is no music root.
    StreamChoice stream{};
    std::vector<ListedEngine> listed{};
    // Whether an engine found and not listed is played for.
    bool play_found{true};
    // How long the network has to find a listed engine before its address
    // is used: at start nothing is found yet, and connecting by address only
    // to drop it a moment later for the engine found would be for nothing.
    std::chrono::milliseconds grace{std::chrono::seconds{5}};
};

// One engine to play for, and how.
struct Wanted final {
    // The engine's id when found; "address:NAME" when reached by address.
    std::string identity;
    std::string name;
    // Where it was reached, as host:port.
    std::string where;
    AgentConfig agent;
};

// Whom to play for, given what the network found and whether the grace is
// over. Pure, so it can be tested without a network. `config.already` is
// taken as it is: Guests makes its addresses numeric first.
[[nodiscard]] std::vector<Wanted> plan_guests(const GuestsConfig& config,
                                              const std::vector<discovery::Found>& found,
                                              bool grace_over);

// Plays for every engine on the network: an agent for each one found, gone
// when it says goodbye, the speakers shared by the arbiter. A listed engine
// with an address is reached there while it is not found.
class Guests final {
  public:
    using Config = GuestsConfig;

    Guests(Config config, SpeakerArbiter& arbiter);
    Guests(const Guests&) = delete;
    Guests& operator=(const Guests&) = delete;
    ~Guests();

    // Starts looking; false when this machine cannot use multicast DNS. A
    // listed engine's address is used either way.
    [[nodiscard]] bool start();
    void stop();
    // The engines played for now, by name.
    [[nodiscard]] std::vector<std::string> engines() const;

  private:
    void update(const std::vector<discovery::Found>& found);
    void settle_locked();

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
    std::vector<discovery::Found> found_;
    bool grace_over_{false};
    std::condition_variable_any grace_wake_;
    std::jthread grace_;
};

} // namespace trackknife::agent
