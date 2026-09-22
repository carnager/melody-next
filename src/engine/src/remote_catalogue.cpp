// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/remote_catalogue.hpp"

#include "trackknife/protocol/message.hpp"

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error malformed(std::string detail) {
    return core::Error{.code = core::ErrorCode::backend,
                       .message = "the engine sent an answer this client cannot read",
                       .context = {{.key = "detail", .value = std::move(detail)}}};
}

// Paths cross base64, so decoding one is where a protocol disagreement would
// surface. It is reported as the engine's fault rather than silently yielding
// an empty path that looks like a missing file.
[[nodiscard]] core::Result<std::vector<std::string>> decode_paths(const Json& answer,
                                                                  const char* member) {
    const auto found = answer.find(member);
    if (found == answer.end() || !found->is_array()) {
        return std::unexpected(malformed(member));
    }
    std::vector<std::string> paths;
    paths.reserve(found->size());
    for (const auto& value : *found) {
        if (!value.is_string()) {
            return std::unexpected(malformed(member));
        }
        auto raw = protocol::decode_raw_path(value.get<std::string>());
        if (!raw) {
            return std::unexpected(malformed(member));
        }
        paths.push_back(std::move(*raw));
    }
    return paths;
}

} // namespace

core::Result<std::vector<persistence::LibraryRoot>> RemoteCatalogue::roots() const {
    auto answer = client_->call("catalogue.roots");
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    const auto found = answer->find("roots");
    if (found == answer->end() || !found->is_array()) {
        return std::unexpected(malformed("roots"));
    }
    std::vector<persistence::LibraryRoot> roots;
    for (const auto& value : *found) {
        if (!value.is_object() || !value.contains("path")) {
            return std::unexpected(malformed("roots"));
        }
        auto raw = protocol::decode_raw_path(value.at("path").get<std::string>());
        if (!raw) {
            return std::unexpected(malformed("roots"));
        }
        roots.push_back({.raw_path = std::move(*raw),
                         .available = value.value("available", false),
                         .error = value.value("error", std::string{})});
    }
    return roots;
}

core::Result<std::vector<std::string>>
RemoteCatalogue::filter_paths(const query::CompiledTkq& compiled,
                              const core::CancellationToken&) const {
    // The engine compiles for itself, so the query crosses as its source. A
    // client and engine agreeing on a compiled form would be a second wire
    // contract for no gain.
    auto answer = client_->call("catalogue.filter_paths", Json{{"query", compiled.source}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return decode_paths(*answer, "paths");
}

core::Result<std::vector<unsigned>> RemoteCatalogue::ratings(const std::vector<std::string>& hashes,
                                                             const core::CancellationToken&) const {
    auto answer = client_->call("catalogue.ratings", Json{{"hashes", hashes}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    const auto found = answer->find("ratings");
    if (found == answer->end() || !found->is_array()) {
        return std::unexpected(malformed("ratings"));
    }
    std::vector<unsigned> ratings;
    ratings.reserve(found->size());
    for (const auto& value : *found) {
        if (!value.is_number_integer()) {
            return std::unexpected(malformed("ratings"));
        }
        ratings.push_back(value.get<unsigned>());
    }
    return ratings;
}

core::Result<void> RemoteCatalogue::set_rating(const std::string& hash, const bool album,
                                               const unsigned rating) {
    auto answer = client_->call("catalogue.set_rating",
                                Json{{"hash", hash}, {"album", album}, {"rating", rating}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return {};
}

core::Result<std::optional<std::string>>
RemoteCatalogue::artwork_source(const std::string& album_key,
                                const core::CancellationToken&) const {
    auto answer = client_->call("catalogue.artwork_source", Json{{"album_key", album_key}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    const auto found = answer->find("source");
    if (found == answer->end()) {
        return std::unexpected(malformed("source"));
    }
    // Null is an album with no cover, which is a success.
    if (found->is_null()) {
        return std::optional<std::string>{};
    }
    if (!found->is_string()) {
        return std::unexpected(malformed("source"));
    }
    auto raw = protocol::decode_raw_path(found->get<std::string>());
    if (!raw) {
        return std::unexpected(malformed("source"));
    }
    return std::optional<std::string>{std::move(*raw)};
}

namespace {

// Methods the engine does not expose yet. Reported as unsupported with the
// method named, rather than as an empty success that would look like a
// library with nothing in it.
[[nodiscard]] core::Error not_exposed(std::string method) {
    return core::Error{.code = core::ErrorCode::unsupported,
                       .message = "this engine does not expose that yet",
                       .context = {{.key = "method", .value = std::move(method)}}};
}

} // namespace

core::Result<void> RemoteCatalogue::add_root(const std::string&) {
    return std::unexpected(not_exposed("catalogue.add_root"));
}

core::Result<void> RemoteCatalogue::remove_root(const std::string&) {
    return std::unexpected(not_exposed("catalogue.remove_root"));
}

core::Result<persistence::LibraryPage>
RemoteCatalogue::query(const persistence::LibraryQuery&, const core::CancellationToken&) const {
    return std::unexpected(not_exposed("catalogue.query"));
}

core::Result<std::vector<std::string>>
RemoteCatalogue::paths(const persistence::LibraryQuery&, const core::CancellationToken&) const {
    return std::unexpected(not_exposed("catalogue.paths"));
}

core::Result<persistence::LibraryPage>
RemoteCatalogue::filter(const query::CompiledTkq&, std::size_t, std::size_t,
                        const core::CancellationToken&) const {
    return std::unexpected(not_exposed("catalogue.filter"));
}

core::Result<std::vector<persistence::LibraryTrackSnapshot>>
RemoteCatalogue::cached_tracks(const std::vector<std::string>&,
                               const core::CancellationToken&) const {
    return std::unexpected(not_exposed("catalogue.cached_tracks"));
}

core::Result<std::vector<std::array<std::int64_t, 6>>>
RemoteCatalogue::history_facts(const std::vector<persistence::LibraryHistorySource>&,
                               const core::CancellationToken&) const {
    return std::unexpected(not_exposed("catalogue.history_facts"));
}

} // namespace trackknife::engine
