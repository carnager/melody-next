// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/formats/decoder.hpp"

#include <optional>
#include <string>

namespace trackknife::audio {

// What to decode: a file, which independently playable thing inside it, and
// which span of that thing. The three travel together everywhere -- a cue
// sheet track, a tracker subsong and a chaptered container are all "this path,
// this selection, this range" -- and LocalAuditionSnapshot already spells the
// triple out inline, twice, for the current and queued sources.
//
// ADR-0220 Phase 0: this lived in src/bench/local_list_model.hpp, a Qt header,
// so playback state that holds one could not leave the widget layer. Nothing
// about it needs Qt: raw_path is raw OS bytes with no UTF-8 assumption, and
// the formats types are already Qt-free.
struct TrackSource final {
    // Raw OS path bytes. Not assumed to be valid UTF-8; presentation layers
    // apply the lossless escaped form.
    std::string raw_path;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;

    // An empty path is how "nothing is playing" is spelled throughout the
    // workspace. Naming it keeps that convention explicit rather than leaving
    // callers to remember which field carries the signal.
    [[nodiscard]] bool empty() const noexcept { return raw_path.empty(); }

    friend bool operator==(const TrackSource&, const TrackSource&) = default;
};

} // namespace trackknife::audio
