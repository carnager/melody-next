// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// ADR-0228: how an engine and an output agent talk about audio. The engine
// sends `audition.*` requests; the agent answers them and reports its state
// as `audition.changed` events. One definition, used by both ends.

#include "trackknife/audio/local_audition.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/protocol/message.hpp"

#include <optional>
#include <string>

namespace trackknife::output {

inline constexpr auto changed_event = "audition.changed";

// What to play: a file the agent opens itself, or a stream it fetches.
struct Source final {
    // A path. Relative ones are under the agent's music root; absolute ones
    // are the same path on both machines, as a shared mount makes them.
    std::optional<std::string> path;
    std::optional<std::string> url;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    // ADR-0139: gain the engine knows and the file does not carry -- a
    // sidecar's, a CUE sheet's.
    std::optional<formats::ReplayGainInfo> replay_gain;

    friend bool operator==(const Source&, const Source&) = default;
};

[[nodiscard]] protocol::Json to_json(const Source& source);
[[nodiscard]] core::Result<Source> source_from_json(const protocol::Json& value);

// The agent's audio state. Its own paths are left out: they mean nothing to
// the engine, which knows what it asked for.
[[nodiscard]] protocol::Json to_json(const audio::LocalAuditionSnapshot& snapshot);
[[nodiscard]] audio::LocalAuditionSnapshot snapshot_from_json(const protocol::Json& value);

} // namespace trackknife::output
