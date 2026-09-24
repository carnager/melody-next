// SPDX-License-Identifier: GPL-3.0-only

#include "agent/speaker_arbiter.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <utility>

namespace trackknife::agent {

SpeakerArbiter::SpeakerArbiter(engine::Player* player) : player_(player) {
    if (player_ != nullptr) {
        parties_.push_back(Party{.name = {}, .guest = nullptr, .was_playing = false});
    }
}

SpeakerArbiter::~SpeakerArbiter() { stop(); }

void SpeakerArbiter::add_guest(std::string name, Agent& guest) {
    const std::lock_guard guard{mutex_};
    parties_.push_back(Party{.name = std::move(name), .guest = &guest, .was_playing = false});
}

void SpeakerArbiter::remove_guest(const Agent& guest) {
    const std::lock_guard guard{mutex_};
    std::erase_if(parties_, [&guest](const Party& party) { return party.guest == &guest; });
}

void SpeakerArbiter::start() {
    if (running_.exchange(true)) {
        return;
    }
    pause_.reset();
    watcher_ = std::thread{[this] {
        while (running_.load() && pause_.wait(std::chrono::milliseconds{100})) {
            settle();
        }
    }};
}

void SpeakerArbiter::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    pause_.interrupt();
    if (watcher_.joinable()) {
        watcher_.join();
    }
}

bool SpeakerArbiter::playing(const Party& party) const {
    if (party.guest != nullptr) {
        return party.guest->audition().snapshot().state == audio::LocalAuditionState::playing;
    }
    // Only this machine's own audio competes; its engine playing on an agent
    // elsewhere leaves these speakers free.
    const auto* local = player_->local_output();
    return local != nullptr && player_->current_output() == local &&
           player_->state().status == "playing";
}

void SpeakerArbiter::pause(const Party& party) const {
    if (party.guest != nullptr) {
        static_cast<void>(party.guest->audition().pause());
    } else {
        static_cast<void>(player_->pause());
    }
}

void SpeakerArbiter::settle() {
    const std::lock_guard guard{mutex_};
    std::vector<bool> now;
    now.reserve(parties_.size());
    for (const auto& party : parties_) {
        now.push_back(playing(party));
    }
    // The newest: one that has just started. Should two start in the same
    // tenth of a second, the last listed wins; the rest pause.
    std::optional<std::size_t> newest;
    for (std::size_t index = 0; index < parties_.size(); ++index) {
        if (now[index] && !parties_[index].was_playing) {
            newest = index;
        }
    }
    if (newest) {
        for (std::size_t index = 0; index < parties_.size(); ++index) {
            if (index != *newest && now[index]) {
                pause(parties_[index]);
                // Paused by this, whatever it reports this instant: its next
                // start is then the newest, however soon it comes.
                now[index] = false;
                const auto& winner = parties_[*newest];
                std::cerr << "melodyd: "
                          << (winner.guest != nullptr ? winner.name : std::string{"this engine"})
                          << " took the speakers from "
                          << (parties_[index].guest != nullptr ? parties_[index].name
                                                               : std::string{"this engine"})
                          << "\n";
            }
        }
    }
    for (std::size_t index = 0; index < parties_.size(); ++index) {
        parties_[index].was_playing = now[index];
    }
    // This machine's engine says which other engine holds its speakers.
    if (player_ != nullptr) {
        std::string holder;
        for (std::size_t index = 0; index < parties_.size(); ++index) {
            if (parties_[index].guest != nullptr && now[index]) {
                holder = parties_[index].name;
            }
        }
        player_->set_speakers_taken_by(std::move(holder));
    }
}

} // namespace trackknife::agent
