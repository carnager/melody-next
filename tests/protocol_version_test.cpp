// SPDX-License-Identifier: GPL-3.0-only

// ADR-0260: a client decides once, from engine.info, whether and how it can
// work with an engine.

#include "trackknife/protocol/version.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace protocol = trackknife::protocol;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    using protocol::Compatibility;
    using protocol::Json;

    require(protocol::peer_version(Json{{"protocol", 1}, {"level", 3}}) ==
                protocol::PeerVersion{.protocol = 1, .level = 3},
            "an engine says what it speaks");
    require(protocol::peer_version(Json{{"name", "old"}}) ==
                protocol::PeerVersion{.protocol = 1, .level = 0},
            "one too old to say is protocol 1, level 0");
    require(protocol::peer_version(Json{{"protocol", 1}}).level == 0,
            "and one that says its protocol but no level is level 0");

    const protocol::PeerVersion same{.protocol = protocol::protocol_version,
                                     .level = protocol::protocol_level};
    require(protocol::compatibility(same) == Compatibility::full, "the same: everything works");
    require(protocol::compatibility_message(same, "gemenon", "Trackknife").empty(),
            "and nothing is said");
    const protocol::PeerVersion newer{.protocol = protocol::protocol_version,
                                      .level = protocol::protocol_level + 1};
    require(protocol::compatibility(newer) == Compatibility::full &&
                protocol::compatibility_message(newer, "gemenon", "Trackknife").empty(),
            "a newer level serves an older client fully");
    const protocol::PeerVersion older{.protocol = protocol::protocol_version,
                                      .level = protocol::protocol_level - 1};
    require(protocol::compatibility(older) == Compatibility::older_engine,
            "an older level is used, without what it lacks");
    require(protocol::compatibility_message(older, "gemenon", "Trackknife") ==
                "gemenon's melodyd is older than this Trackknife; some things will not work "
                "until it is updated.",
            "and said so");
    const protocol::PeerVersion next{.protocol = protocol::protocol_version + 1, .level = 0};
    require(protocol::compatibility(next) == Compatibility::incompatible,
            "another protocol is not used");
    require(protocol::compatibility_message(next, "gemenon", "melody-cli") ==
                "gemenon speaks protocol 2; this melody-cli speaks protocol 1. Update melody-cli.",
            "naming the side to update: this one, for a newer engine");
    require(protocol::compatibility_message(protocol::PeerVersion{.protocol = 0, .level = 0},
                                            "gemenon", "Trackknife")
                .ends_with("Update gemenon's melodyd."),
            "the engine, for an older one");
    return EXIT_SUCCESS;
}
