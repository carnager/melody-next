// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 0: playback policy must be exercisable without constructing
// BenchMainWindow. This suite links only Trackknife::Audio -- no Qt, no
// widgets -- which is the property the phase is actually after. The behaviour
// itself is unchanged; it simply lives somewhere a headless engine can reach.

#include "trackknife/audio/playback_anchors.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/audio/track_source.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void modes_default_to_off() {
    namespace audio = trackknife::audio;
    const audio::PlaybackModes modes;
    require(!modes.repeat && !modes.random && !modes.album_random,
            "toggles must default to off");
    require(!modes.single_active() && !modes.consume_active(),
            "tri-state modes must default to inactive");
}

void cycling_visits_off_on_oneshot_and_wraps() {
    namespace audio = trackknife::audio;
    auto state = audio::ModeState::off;
    state = audio::next_mode_state(state);
    require(state == audio::ModeState::on, "off must cycle to on");
    state = audio::next_mode_state(state);
    require(state == audio::ModeState::oneshot, "on must cycle to one-shot");
    state = audio::next_mode_state(state);
    require(state == audio::ModeState::off, "one-shot must cycle back to off");
}

void one_shot_modes_expire_once_and_report_the_change() {
    namespace audio = trackknife::audio;
    audio::PlaybackModes modes;

    // Off and on are stable: expiring them is a no-op, and reporting no change
    // is what keeps the workspace from writing settings on every track.
    require(!modes.expire_single(), "expiring an off mode must report no change");
    modes.single = audio::ModeState::on;
    require(!modes.expire_single(), "a plain on mode must survive a track");
    require(modes.single == audio::ModeState::on, "expiry must not disturb on");

    modes.single = audio::ModeState::oneshot;
    require(modes.expire_single(), "a one-shot mode must report its expiry");
    require(modes.single == audio::ModeState::off, "a one-shot mode must revert to off");
    require(!modes.expire_single(), "expiry must not repeat");

    modes.consume = audio::ModeState::oneshot;
    require(modes.expire_consume(), "consume must expire independently of single");
    require(modes.consume == audio::ModeState::off, "consume must revert to off");
    require(modes.single == audio::ModeState::off, "expiring one mode must not touch the other");
}

void persisted_values_are_clamped_rather_than_trusted() {
    namespace audio = trackknife::audio;
    require(audio::mode_state_from_int(0) == audio::ModeState::off, "0 is off");
    require(audio::mode_state_from_int(1) == audio::ModeState::on, "1 is on");
    require(audio::mode_state_from_int(2) == audio::ModeState::oneshot, "2 is one-shot");
    // A settings file is user-writable and may hold anything.
    require(audio::mode_state_from_int(3) == audio::ModeState::off, "out of range means off");
    require(audio::mode_state_from_int(-1) == audio::ModeState::off, "negative means off");
    require(audio::mode_state_from_int(99'999) == audio::ModeState::off, "far out of range is off");
}

void active_predicates_cover_both_live_states() {
    namespace audio = trackknife::audio;
    audio::PlaybackModes modes;
    modes.single = audio::ModeState::on;
    modes.consume = audio::ModeState::oneshot;
    require(modes.single_active(), "on counts as active");
    require(modes.consume_active(), "one-shot counts as active");
    modes.single = audio::ModeState::off;
    require(!modes.single_active(), "off does not count as active");
}

void a_track_source_reports_emptiness_and_compares_by_value() {
    namespace audio = trackknife::audio;
    namespace formats = trackknife::formats;

    const audio::TrackSource nothing;
    require(nothing.empty(), "a default source means nothing is playing");

    audio::TrackSource file;
    file.raw_path = "/music/track.flac";
    require(!file.empty(), "a source with a path is not empty");

    // Raw OS bytes, not assumed to be UTF-8.
    audio::TrackSource invalid_utf8;
    invalid_utf8.raw_path = std::string{"/music/broken-\xff.flac", 22U};
    require(!invalid_utf8.empty(), "an undecodable path is still a path");

    // The selection and segment are part of identity: the same file, a
    // different subsong, is a different source.
    auto subsong = file;
    subsong.selection.subsong_index = 2;
    require(!(file == subsong), "a different subsong is a different source");

    auto ranged = file;
    ranged.segment = formats::SampleRange{.start_sample = 0, .end_sample = 44'100};
    require(!(file == ranged), "a different span is a different source");
    require(ranged == ranged, "a source equals itself");
}

void anchors_describe_position_without_rows() {
    namespace audio = trackknife::audio;
    namespace core = trackknife::core;

    audio::PlaybackAnchors anchors;
    require(!anchors.playing(), "default anchors mean nothing is playing");

    // A current entry alone is not playback: without a document there is no
    // list to resolve it against.
    anchors.current = core::StableId::random();
    require(!anchors.playing(), "an entry without a document is not playback");
    anchors.document = core::StableId::random();
    require(anchors.playing(), "an entry in a document is playback");

    // The two in-flight anchors are abandoned as a unit, which is the
    // invariant forget_transition exists to hold.
    anchors.queued = core::StableId::random();
    anchors.requested = core::StableId::random();
    anchors.request_return = core::StableId::random();
    const auto return_point = anchors.request_return;
    anchors.forget_transition();
    require(anchors.queued.is_nil() && anchors.requested.is_nil(),
            "forgetting a transition must drop both halves");
    require(anchors.request_return == return_point,
            "a transition is not the return point; forgetting one must not drop the other");
    require(anchors.playing(), "forgetting a transition must not stop playback");

    anchors.source.raw_path = "/music/track.flac";
    anchors.clear();
    require(!anchors.playing() && anchors.source.empty() && anchors.request_return.is_nil(),
            "clearing must leave nothing behind");
}

void anchors_compare_by_value() {
    namespace audio = trackknife::audio;
    namespace core = trackknife::core;
    audio::PlaybackAnchors left;
    left.document = core::StableId::random();
    left.current = core::StableId::random();
    auto right = left;
    require(left == right, "a copy equals its source");
    right.current = core::StableId::random();
    require(!(left == right), "a different current entry is a different position");
}

} // namespace

int main() {
    anchors_describe_position_without_rows();
    anchors_compare_by_value();
    a_track_source_reports_emptiness_and_compares_by_value();
    modes_default_to_off();
    cycling_visits_off_on_oneshot_and_wraps();
    one_shot_modes_expire_once_and_report_the_change();
    persisted_values_are_clamped_rather_than_trusted();
    active_predicates_cover_both_live_states();
    return EXIT_SUCCESS;
}
