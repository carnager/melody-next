// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/protocol/version.hpp"

namespace trackknife::protocol {

PeerVersion peer_version(const Json& engine_info) {
    PeerVersion version;
    const auto number = [&engine_info](const char* key, const int fallback) {
        const auto found = engine_info.find(key);
        return found != engine_info.end() && found->is_number_integer() ? found->get<int>()
                                                                        : fallback;
    };
    if (engine_info.is_object()) {
        version.protocol = number("protocol", 1);
        version.level = number("level", 0);
    }
    return version;
}

Compatibility compatibility(const PeerVersion engine) {
    if (engine.protocol != protocol_version) {
        return Compatibility::incompatible;
    }
    return engine.level < protocol_level ? Compatibility::older_engine : Compatibility::full;
}

std::string compatibility_message(const PeerVersion engine, const std::string_view engine_name,
                                  const std::string_view client) {
    const std::string name{engine_name.empty() ? std::string_view{"The engine"} : engine_name};
    switch (compatibility(engine)) {
    case Compatibility::full:
        return {};
    case Compatibility::older_engine:
        return name + "'s melodyd is older than this " + std::string{client} +
               "; some things will not work until it is updated.";
    case Compatibility::incompatible: {
        const bool engine_newer = engine.protocol > protocol_version;
        return name + " speaks protocol " + std::to_string(engine.protocol) + "; this " +
               std::string{client} + " speaks protocol " + std::to_string(protocol_version) +
               ". Update " + (engine_newer ? std::string{client} : name + "'s melodyd") + ".";
    }
    }
    return {};
}

} // namespace trackknife::protocol
