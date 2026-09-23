// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/job_registry.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/output/agent_audition.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace trackknife::engine {

// ADR-0228: what the engine can play on -- its own audio, if it has any, and
// every output agent that has registered -- and which one it plays on now.
//
// An agent is kept by name once seen. One that drops is the same output when
// it comes back, and if it was the one playing, the music takes up where it
// stopped there, rather than moving somewhere else.
class Outputs final {
  public:
    static constexpr auto selection_key = "outputs.selected.v1";
    static constexpr auto local_id = "local";

    // `own_name` is what the engine's own audio is listed as: the engine's
    // name, so a client shows "gemenon", not "this machine".
    Outputs(Player& player, output::AgentPaths paths, Workspace* workspace, EventSink sink,
            std::string own_name = "this machine");
    Outputs(const Outputs&) = delete;
    Outputs& operator=(const Outputs&) = delete;
    Outputs(Outputs&&) = delete;
    Outputs& operator=(Outputs&&) = delete;
    ~Outputs();

    // A connection that sent agent.register: the server's agent handler.
    void admit(const protocol::Json& params, int descriptor);

    struct Listed final {
        std::string id;
        std::string name;
        bool local{false};
        bool online{false};
        bool selected{false};
        // For an agent: whether it opens files itself rather than streaming.
        bool files{true};
    };
    [[nodiscard]] std::vector<Listed> list() const;
    // "local", or "agent:<name>". Persisted, so a restart plays on the same.
    [[nodiscard]] core::Result<void> select(const std::string& id);
    // What was chosen last time. An agent chosen then and not connected yet
    // is waited for: playback resumes on it once it registers.
    void restore();

  private:
    // Called with mutex_ held.
    [[nodiscard]] std::unique_ptr<output::AgentAudition> make_agent(const std::string& name);
    void announce();

    Player* player_;
    std::string own_name_;
    output::AgentPaths paths_;
    Workspace* workspace_;
    EventSink sink_;
    mutable std::mutex mutex_;
    // Stable addresses: the player holds a pointer to whichever is playing.
    std::map<std::string, std::unique_ptr<output::AgentAudition>> agents_;
    std::string selected_;
};

void register_output_methods(protocol::Dispatcher& dispatcher, Outputs& outputs);

} // namespace trackknife::engine
