// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/outputs.hpp"

#include "trackknife/protocol/client.hpp"

#include <unistd.h>

#include <chrono>
#include <utility>

namespace trackknife::engine {
namespace {

using protocol::Json;

constexpr std::string_view agent_prefix{"agent:"};

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] core::Error not_found(const std::string& id) {
    return core::Error{.code = core::ErrorCode::not_found,
                       .message = "no such output",
                       .context = {{.key = "output", .value = id}}};
}

} // namespace

Outputs::Outputs(Player& player, output::AgentPaths paths, Workspace* workspace, EventSink sink)
    : player_(&player), paths_(std::move(paths)), workspace_(workspace), sink_(std::move(sink)) {}

Outputs::~Outputs() {
    // The player must not be left pointing at an agent about to go.
    static_cast<void>(player_->set_output(player_->local_output()));
}

void Outputs::admit(const Json& params, const int descriptor) {
    const auto name = params.value("name", std::string{});
    if (name.empty()) {
        // Registered without a name: nothing to call it, nothing to keep.
        ::close(descriptor);
        return;
    }
    const bool files = params.value("files", true);
    output::AgentAudition* agent = nullptr;
    bool selected = false;
    std::optional<output::AgentAudition::LastHeard> heard;
    {
        const std::lock_guard guard{mutex_};
        auto& slot = agents_[name];
        if (!slot) {
            slot = std::make_unique<output::AgentAudition>(name, paths_);
        }
        agent = slot.get();
        selected = selected_ == std::string{agent_prefix} + name;
        // Before the new connection wipes it: where it was when it dropped.
        heard = agent->last_heard();
    }
    agent->attach(protocol::Client::adopt(descriptor), files);
    if (selected) {
        // The music was here when the agent went away; it takes up there.
        static_cast<void>(
            player_->resume_output(heard ? std::optional{heard->position_ms} : std::nullopt,
                                   heard ? std::optional{heard->playing} : std::nullopt));
    }
    announce();
}

std::vector<Outputs::Listed> Outputs::list() const {
    const std::lock_guard guard{mutex_};
    std::vector<Listed> listed;
    if (player_->local_output() != nullptr) {
        listed.push_back(Listed{.id = local_id,
                                .name = "this machine",
                                .local = true,
                                .online = true,
                                .selected = selected_ == local_id,
                                .files = true});
    }
    for (const auto& [name, agent] : agents_) {
        const auto id = std::string{agent_prefix} + name;
        listed.push_back(Listed{.id = id,
                                .name = name,
                                .local = false,
                                .online = agent->online(),
                                .selected = selected_ == id,
                                .files = agent->files()});
    }
    return listed;
}

core::Result<void> Outputs::select(const std::string& id) {
    audio::Audition* chosen = nullptr;
    {
        const std::lock_guard guard{mutex_};
        if (id == local_id) {
            chosen = player_->local_output();
            if (chosen == nullptr) {
                return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                                   .message = "this machine has no audio output",
                                                   .context = {}});
            }
        } else if (id.starts_with(agent_prefix)) {
            const auto found = agents_.find(id.substr(agent_prefix.size()));
            if (found == agents_.end()) {
                return std::unexpected(not_found(id));
            }
            chosen = found->second.get();
        } else {
            return std::unexpected(not_found(id));
        }
        selected_ = id;
    }
    if (workspace_ != nullptr) {
        static_cast<void>(workspace_->save_engine_state(selection_key, id, now_ms()));
    }
    // An agent chosen while it is away is where playback waits; switching
    // reports that it could not take the music up yet, which is not a
    // refusal of the choice.
    static_cast<void>(player_->set_output(chosen));
    announce();
    return {};
}

void Outputs::restore() {
    std::string id;
    if (workspace_ != nullptr) {
        if (auto stored = workspace_->load_engine_state(selection_key); stored && *stored) {
            id = **stored;
        }
    }
    if (id.empty()) {
        id = player_->local_output() != nullptr ? std::string{local_id} : std::string{};
    }
    if (id.starts_with(agent_prefix)) {
        // Not connected yet after a restart: known by name, so the choice
        // holds until it registers.
        const std::lock_guard guard{mutex_};
        auto& slot = agents_[id.substr(agent_prefix.size())];
        if (!slot) {
            slot = std::make_unique<output::AgentAudition>(id.substr(agent_prefix.size()), paths_);
        }
    }
    if (!id.empty()) {
        static_cast<void>(select(id));
    }
}

void Outputs::announce() {
    if (!sink_) {
        return;
    }
    auto outputs = Json::array();
    for (const auto& listed : list()) {
        outputs.push_back(Json{{"id", listed.id},
                               {"name", listed.name},
                               {"local", listed.local},
                               {"online", listed.online},
                               {"selected", listed.selected},
                               {"files", listed.files}});
    }
    sink_(protocol::Event{.name = "outputs.changed", .data = Json{{"outputs", outputs}}});
}

void register_output_methods(protocol::Dispatcher& dispatcher, Outputs& outputs) {
    dispatcher.on("outputs.list", [&outputs](const Json&) -> core::Result<Json> {
        auto listed = Json::array();
        for (const auto& output : outputs.list()) {
            listed.push_back(Json{{"id", output.id},
                                  {"name", output.name},
                                  {"local", output.local},
                                  {"online", output.online},
                                  {"selected", output.selected},
                                  {"files", output.files}});
        }
        return Json{{"outputs", std::move(listed)}};
    });
    dispatcher.on("outputs.select", [&outputs](const Json& params) -> core::Result<Json> {
        const auto id = params.find("id");
        if (id == params.end() || !id->is_string()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = "an output id is required",
                                               .context = {{.key = "param", .value = "id"}}});
        }
        auto selected = outputs.select(id->get<std::string>());
        if (!selected) {
            return std::unexpected(std::move(selected.error()));
        }
        return Json::object();
    });
}

} // namespace trackknife::engine
