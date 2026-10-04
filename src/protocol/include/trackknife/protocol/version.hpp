// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/protocol/message.hpp"

#include <string>
#include <string_view>

namespace trackknife::protocol {

// ADR-0260: what this build speaks. `protocol_version` changes only when a
// release breaks clients; `protocol_level` goes up with every release that
// adds to the protocol -- the additions of each are listed in
// docs/protocol-levels.md -- and never down within a protocol.
inline constexpr int protocol_version = 1;
inline constexpr int protocol_level = 3;

// What an engine says it speaks, from its engine.info answer. One too old to
// say is protocol 1, level 0.
struct PeerVersion {
    int protocol{1};
    int level{0};

    friend bool operator==(const PeerVersion&, const PeerVersion&) = default;
};
[[nodiscard]] PeerVersion peer_version(const Json& engine_info);

enum class Compatibility {
    // The same protocol and level, or a newer level: everything works.
    full,
    // The same protocol at a lower level: what it lacks does not work.
    older_engine,
    // Another protocol: nothing can be relied on.
    incompatible,
};
[[nodiscard]] Compatibility compatibility(PeerVersion engine);

// The one sentence a client says of it, naming the engine and which side to
// update; empty when there is nothing to say. `client` is what the user runs:
// "Trackknife", "melody-cli".
[[nodiscard]] std::string compatibility_message(PeerVersion engine, std::string_view engine_name,
                                                std::string_view client);

} // namespace trackknife::protocol
