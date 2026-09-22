// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace trackknife::audio {

// Repeat/random are plain toggles; single and consume are MPD-compatible
// tri-states where `oneshot` applies to the current track only and then
// reverts. The workspace cycles a mode with the successor below, which is why
// the enumerators keep their historical 0/1/2 values: they are what the
// settings file already holds.
enum class ModeState : int {
    off = 0,
    on = 1,
    oneshot = 2,
};

[[nodiscard]] constexpr ModeState next_mode_state(const ModeState state) noexcept {
    return static_cast<ModeState>((static_cast<int>(state) + 1) % 3);
}

// A persisted value of unknown provenance is clamped rather than trusted; an
// out-of-range setting means off, not undefined behaviour.
[[nodiscard]] constexpr ModeState mode_state_from_int(const int value) noexcept {
    switch (value) {
    case 1:
        return ModeState::on;
    case 2:
        return ModeState::oneshot;
    default:
        return ModeState::off;
    }
}

// ADR-0220 Phase 0: playback decisions belong to the engine, not to the widget
// that happens to display them. This is the part of that state with no Qt
// dependency and no list to consult -- pure policy, exercisable without
// constructing a window.
struct PlaybackModes final {
    bool repeat{false};
    bool random{false};
    bool album_random{false};
    ModeState single{ModeState::off};
    ModeState consume{ModeState::off};

    [[nodiscard]] constexpr bool single_active() const noexcept { return single != ModeState::off; }
    [[nodiscard]] constexpr bool consume_active() const noexcept {
        return consume != ModeState::off;
    }

    // A one-shot mode fires for a single track and then reverts. Returns true
    // when the state actually changed, so a caller knows to persist the
    // settings and refresh the controls, and stays quiet otherwise.
    constexpr bool expire_single() noexcept {
        if (single != ModeState::oneshot) {
            return false;
        }
        single = ModeState::off;
        return true;
    }
    constexpr bool expire_consume() noexcept {
        if (consume != ModeState::oneshot) {
            return false;
        }
        consume = ModeState::off;
        return true;
    }

    friend bool operator==(const PlaybackModes&, const PlaybackModes&) = default;
};

} // namespace trackknife::audio
