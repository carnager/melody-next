// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/track_description.hpp"

#include <cstdint>
#include <utility>

namespace trackknife::engine {

using protocol::Json;

Json describe_track(const persistence::LibraryTrackSnapshot& track) {
    Json fields = Json::object();
    for (const auto& [name, values] : track.facts.fields) {
        auto shown = Json::array();
        for (const auto& value : values) {
            shown.push_back(protocol::displayable_text(value.first));
        }
        fields[protocol::displayable_text(name)] = std::move(shown);
    }
    Json described{{"fields", std::move(fields)},
                   {"duration_ms", track.facts.duration_ms},
                   {"codec", protocol::displayable_text(track.facts.codec)},
                   {"sample_rate", track.facts.sample_rate},
                   {"bits", track.facts.bits},
                   {"channels", track.facts.channels},
                   {"rating", track.facts.rating},
                   {"album_rating", track.facts.album_rating},
                   {"revision", nullptr}};
    if (track.revision) {
        const auto& revision = *track.revision;
        described["revision"] = Json{{"device", revision.device},
                                     {"inode", revision.inode},
                                     {"size", revision.size},
                                     {"seconds", revision.modification_time_seconds},
                                     {"nanoseconds", revision.modification_time_nanoseconds}};
    }
    return described;
}

std::optional<persistence::LibraryTrackSnapshot> described_track(const Json& description,
                                                                 std::string raw_path) {
    if (!description.is_object()) {
        return std::nullopt;
    }
    persistence::LibraryTrackSnapshot track;
    track.raw_path = std::move(raw_path);
    if (const auto fields = description.find("fields");
        fields != description.end() && fields->is_object()) {
        for (const auto& [name, values] : fields->items()) {
            if (!values.is_array()) {
                return std::nullopt;
            }
            auto& kept = track.facts.fields[name];
            for (const auto& value : values) {
                if (!value.is_string()) {
                    return std::nullopt;
                }
                kept.emplace_back(value.get<std::string>(), std::string{});
            }
        }
    }
    const auto number = [&description](const char* key, const std::int64_t fallback) {
        const auto found = description.find(key);
        return found != description.end() && found->is_number_integer() ? found->get<std::int64_t>()
                                                                        : fallback;
    };
    track.facts.duration_ms = number("duration_ms", -1);
    track.facts.codec = description.value("codec", std::string{});
    track.facts.sample_rate = number("sample_rate", 0);
    track.facts.bits = number("bits", 0);
    track.facts.channels = number("channels", 0);
    track.facts.rating = number("rating", -1);
    track.facts.album_rating = number("album_rating", -1);
    if (const auto revision = description.find("revision");
        revision != description.end() && revision->is_object()) {
        const auto unsigned_of = [&revision](const char* key) {
            const auto found = revision->find(key);
            return found != revision->end() && found->is_number_unsigned()
                       ? found->get<std::uint64_t>()
                       : std::uint64_t{0};
        };
        const auto signed_of = [&revision](const char* key) {
            const auto found = revision->find(key);
            return found != revision->end() && found->is_number_integer()
                       ? found->get<std::int64_t>()
                       : std::int64_t{0};
        };
        track.revision =
            core::LocalSourceRevision{.device = unsigned_of("device"),
                                      .inode = unsigned_of("inode"),
                                      .size = unsigned_of("size"),
                                      .modification_time_seconds = signed_of("seconds"),
                                      .modification_time_nanoseconds = signed_of("nanoseconds")};
    }
    return track;
}

} // namespace trackknife::engine
