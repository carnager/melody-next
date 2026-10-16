// SPDX-License-Identifier: GPL-3.0-only

#include "agent/guests.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <utility>

namespace trackknife::agent {
namespace {

// "host:port" with the host as an IPv4 address, so a name and the address
// it stands for are recognised as one engine.
[[nodiscard]] std::string numeric(const std::string& endpoint) {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
        return endpoint;
    }
    const auto host = endpoint.substr(0, colon);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
        return endpoint;
    }
    std::array<char, INET_ADDRSTRLEN> text{};
    const auto* address = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
    ::inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size());
    ::freeaddrinfo(found);
    return std::string{text.data()} + endpoint.substr(colon);
}

// The section of an engine found: by the name it is announced by, else by
// the address it was heard at -- a section named after its host.
[[nodiscard]] const ListedEngine* listed_for(const GuestsConfig& config, const std::string& name,
                                             const std::string& where) {
    if (const auto named = std::ranges::find(config.listed, name, &ListedEngine::name);
        named != config.listed.end()) {
        return &*named;
    }
    const auto at = std::ranges::find_if(config.listed, [&where](const ListedEngine& listed) {
        return !listed.where.empty() && listed.where == where;
    });
    return at == config.listed.end() ? nullptr : &*at;
}

[[nodiscard]] AgentConfig agent_for(const GuestsConfig& config, const ListedEngine* listed,
                                    protocol::Endpoint server) {
    auto music_root = listed != nullptr && listed->music_root ? listed->music_root
                                                              : config.music_root;
    const bool stream_only = !music_root.has_value();
    return AgentConfig{.server = std::move(server),
                       .name = config.name,
                       .music_root = std::move(music_root),
                       .stream_only = stream_only,
                       .stream = listed != nullptr && listed->stream ? *listed->stream
                                                                     : config.stream};
}

} // namespace

std::vector<Wanted> plan_guests(const GuestsConfig& config,
                                const std::vector<discovery::Found>& found,
                                const bool grace_over) {
    std::vector<Wanted> wanted;
    std::set<std::string> identities;
    // Found under their name, whether played for or not: a listed engine
    // found is not also reached by its address.
    std::set<std::string> found_names;
    for (const auto& engine : found) {
        const auto id = engine.txt.contains("id") ? engine.txt.at("id") : std::string{};
        const auto where = engine.address + ":" + std::to_string(engine.port);
        if ((!config.own_id.empty() && id == config.own_id) ||
            std::ranges::contains(config.already, where)) {
            continue;
        }
        found_names.insert(engine.instance);
        const auto* listed = listed_for(config, engine.instance, where);
        if (listed != nullptr) {
            found_names.insert(listed->name);
        }
        if (!(listed != nullptr ? listed->play : config.play_found)) {
            continue;
        }
        // An engine is itself, whichever address it was heard from: keyed
        // by address, one on this machine -- heard once per interface --
        // was a new engine with every announcement, and each new guest
        // replaced the last one's registration.
        const auto identity = id.empty() ? engine.instance : id;
        if (!identities.insert(identity).second) {
            continue;
        }
        const auto& password =
            listed != nullptr && !listed->password.empty() ? listed->password : config.password;
        const bool wants_password = engine.txt.contains("auth") && engine.txt.at("auth") == "1";
        if (wants_password && password.empty()) {
            continue;
        }
        wanted.push_back(Wanted{
            .identity = identity,
            .name = engine.instance,
            .where = where,
            .agent = agent_for(config, listed,
                               protocol::Endpoint{.socket = {},
                                                  .host = engine.address,
                                                  .port = engine.port,
                                                  .token = wants_password ? password
                                                                          : std::string{}})});
    }
    if (!grace_over) {
        return wanted;
    }
    for (const auto& listed : config.listed) {
        if (!listed.address || !listed.play || found_names.contains(listed.name)) {
            continue;
        }
        auto server = *listed.address;
        if (server.token.empty()) {
            server.token = listed.password.empty() ? config.password : listed.password;
        }
        const auto where = server.describe();
        wanted.push_back(Wanted{.identity = "address:" + listed.name,
                                .name = listed.name,
                                .where = where,
                                .agent = agent_for(config, &listed, std::move(server))});
    }
    return wanted;
}

Guests::Guests(Config config, SpeakerArbiter& arbiter)
    : config_(std::move(config)), arbiter_(&arbiter) {
    for (auto& endpoint : config_.already) {
        endpoint = numeric(endpoint);
    }
    for (auto& listed : config_.listed) {
        if (listed.address && listed.where.empty()) {
            listed.where = numeric(listed.address->describe());
        }
    }
}

Guests::~Guests() { stop(); }

bool Guests::start() {
    // A listed engine's address waits for the network to have had its say.
    grace_ = std::jthread{[this](const std::stop_token stop) {
        std::unique_lock lock{mutex_};
        static_cast<void>(grace_wake_.wait_for(lock, stop, config_.grace, [] { return false; }));
        if (stop.stop_requested()) {
            return;
        }
        grace_over_ = true;
        settle_locked();
    }};
    auto browser = discovery::Browser::start(
        [this](const std::vector<discovery::Found>& found) { update(found); });
    if (!browser) {
        std::cerr << "melody: cannot look for engines: " << browser.error().message << "\n";
        return false;
    }
    browser_ = std::move(*browser);
    return true;
}

void Guests::stop() {
    browser_.reset();
    if (grace_.joinable()) {
        grace_.request_stop();
        grace_.join();
    }
    const std::lock_guard guard{mutex_};
    for (auto& [identity, guest] : guests_) {
        arbiter_->remove_guest(*guest.agent);
        guest.agent->stop();
    }
    guests_.clear();
}

std::vector<std::string> Guests::engines() const {
    const std::lock_guard guard{mutex_};
    std::vector<std::string> names;
    names.reserve(guests_.size());
    for (const auto& [identity, guest] : guests_) {
        names.push_back(guest.name);
    }
    return names;
}

void Guests::update(const std::vector<discovery::Found>& found) {
    const std::lock_guard guard{mutex_};
    found_ = found;
    settle_locked();
}

void Guests::settle_locked() {
    auto plan = plan_guests(config_, found_, grace_over_);
    std::set<std::string> present;
    for (const auto& wanted : plan) {
        present.insert(wanted.identity);
    }
    // Gone first: an engine reached by its address and now found is played
    // for under the same name, and its new registration must not be taken
    // down by the old connection closing after it.
    for (auto guest = guests_.begin(); guest != guests_.end();) {
        if (present.contains(guest->first)) {
            ++guest;
            continue;
        }
        std::cerr << "melody: " << guest->second.name << " is gone\n";
        arbiter_->remove_guest(*guest->second.agent);
        guest->second.agent->stop();
        guest = guests_.erase(guest);
    }
    for (auto& wanted : plan) {
        if (guests_.contains(wanted.identity)) {
            continue;
        }
        auto audition = audio::LocalAuditionService::create();
        if (!audition) {
            std::cerr << "melody: no audio here to play " << wanted.name
                      << " on: " << audition.error().message << "\n";
            continue;
        }
        static_cast<void>((*audition)->refresh_output_devices());
        auto agent = Agent::create(std::move(wanted.agent), std::move(*audition));
        if (!agent) {
            std::cerr << "melody: cannot play for " << wanted.name << ": "
                      << agent.error().message << "\n";
            continue;
        }
        (*agent)->start();
        arbiter_->add_guest(wanted.name, **agent);
        std::cerr << "melody: playing for " << wanted.name << " at " << wanted.where << "\n";
        guests_.emplace(wanted.identity, Guest{.name = wanted.name,
                                               .where = wanted.where,
                                               .agent = std::move(*agent)});
    }
}

} // namespace trackknife::agent
