// SPDX-License-Identifier: GPL-3.0-only

#include "agent/speaker_arbiter.hpp"

#include <chrono>
#include <iostream>
#include <utility>

namespace trackknife::agent {

SpeakerArbiter::SpeakerArbiter(engine::Player& player, Agent& guest, std::string guest_name)
    : player_(&player), guest_(&guest), guest_name_(std::move(guest_name)) {}

SpeakerArbiter::~SpeakerArbiter() { stop(); }

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

bool SpeakerArbiter::local_playing() const {
    // Only this machine's own audio competes; an engine playing on an agent
    // elsewhere leaves these speakers free.
    const auto* local = player_->local_output();
    return local != nullptr && player_->current_output() == local &&
           player_->state().status == "playing";
}

bool SpeakerArbiter::guest_playing() {
    return guest_->audition().snapshot().state == audio::LocalAuditionState::playing;
}

void SpeakerArbiter::settle() {
    auto local = local_playing();
    auto guest = guest_playing();
    if (guest && !guest_was_playing_) {
        // The other engine started here: it is the newest, so this one's own
        // music stops, and says why.
        if (local) {
            static_cast<void>(player_->pause());
            std::cerr << "melodyd: " << guest_name_ << " took the speakers; paused\n";
        }
        player_->set_speakers_taken_by(guest_name_);
        // Paused by this, whatever the player reports this instant: its next
        // start is then the newest, however soon it comes.
        local = false;
    } else if (local && !local_was_playing_ && guest) {
        // This engine started while the other played: now it is the newest.
        static_cast<void>(guest_->audition().pause());
        player_->set_speakers_taken_by({});
        std::cerr << "melodyd: took the speakers back from " << guest_name_ << "\n";
        guest = false;
    } else if (!guest && guest_was_playing_) {
        player_->set_speakers_taken_by({});
    }
    local_was_playing_ = local;
    guest_was_playing_ = guest;
}

} // namespace trackknife::agent
