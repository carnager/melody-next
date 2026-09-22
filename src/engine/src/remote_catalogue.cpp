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

core::Result<persistence::LibraryScanResult>
RemoteCatalogue::scan(const core::CancellationToken& cancellation,
                      persistence::LibraryScanProgress& progress) {
    auto outcome = client_->run_job(
        "catalogue.scan", Json::object(),
        [&progress](const Json& reported) {
            // The counters the caller is already polling are fed from the
            // job's progress events, so a remote scan looks like a local one.
            progress.visited.store(reported.value("visited", std::size_t{0}));
            progress.indexed.store(reported.value("indexed", std::size_t{0}));
            progress.failed.store(reported.value("failed", std::size_t{0}));
        },
        cancellation);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    // The job reports its own error in the outcome rather than failing the
    // submission, since by then the caller has already been told it started.
    if (const auto failed = outcome->find("error"); failed != outcome->end()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::backend,
                                           .message = failed->get<std::string>(),
                                           .context = {}});
    }
    progress.visited.store(outcome->value("visited", std::size_t{0}));
    progress.indexed.store(outcome->value("indexed", std::size_t{0}));
    progress.failed.store(outcome->value("failed", std::size_t{0}));
    return persistence::LibraryScanResult{.cancelled = outcome->value("cancelled", false),
                                          .incomplete = outcome->value("incomplete", false)};
}

core::Result<void> RemoteCatalogue::add_root(const std::string& raw_path) {
    auto answer =
        client_->call("catalogue.add_root", Json{{"path", protocol::encode_raw_path(raw_path)}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return {};
}

core::Result<void> RemoteCatalogue::remove_root(const std::string& raw_path) {
    auto answer =
        client_->call("catalogue.remove_root", Json{{"path", protocol::encode_raw_path(raw_path)}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return {};
}

namespace {

[[nodiscard]] Json encode_query(const persistence::LibraryQuery& request) {
    Json params = Json::object();
    params["kind"] = static_cast<int>(request.kind);
    params["text"] = request.text;
    if (request.artist) {
        params["artist"] = *request.artist;
    }
    if (request.album_key) {
        params["album_key"] = *request.album_key;
    }
    if (request.raw_path) {
        params["path"] = protocol::encode_raw_path(*request.raw_path);
    }
    params["offset"] = request.offset;
    params["limit"] = request.limit;
    return params;
}

[[nodiscard]] core::Result<persistence::LibraryPage> decode_page(const Json& answer) {
    const auto entries = answer.find("entries");
    if (entries == answer.end() || !entries->is_array()) {
        return std::unexpected(malformed("entries"));
    }
    persistence::LibraryPage page;
    page.more = answer.value("more", false);
    for (const auto& value : *entries) {
        if (!value.is_object()) {
            return std::unexpected(malformed("entries"));
        }
        persistence::LibraryEntry entry;
        entry.kind = static_cast<persistence::LibraryEntryKind>(value.value("kind", 0));
        entry.key = value.value("key", std::string{});
        entry.label = value.value("label", std::string{});
        entry.artist = value.value("artist", std::string{});
        entry.album = value.value("album", std::string{});
        entry.tracks = value.value("tracks", std::size_t{0});
        entry.available = value.value("available", std::size_t{0});
        entry.track_number = value.value("track_number", 0);
        entry.albums = value.value("albums", std::size_t{0});
        entry.rating_hash = value.value("rating_hash", std::string{});
        entry.rating = value.value("rating", 0U);
        page.entries.push_back(std::move(entry));
    }
    return page;
}

} // namespace

core::Result<persistence::LibraryPage>
RemoteCatalogue::query(const persistence::LibraryQuery& request,
                       const core::CancellationToken&) const {
    auto answer = client_->call("catalogue.query", encode_query(request));
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return decode_page(*answer);
}

core::Result<persistence::LibraryPage>
RemoteCatalogue::filter(const query::CompiledTkq& compiled, const std::size_t offset,
                        const std::size_t limit, const core::CancellationToken&) const {
    // As with filter_paths, the query crosses as its source and the engine
    // compiles for itself; agreeing on a compiled form would be a second wire
    // contract for no gain.
    auto answer = client_->call(
        "catalogue.filter", Json{{"query", compiled.source}, {"offset", offset}, {"limit", limit}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return decode_page(*answer);
}

core::Result<std::vector<std::string>>
RemoteCatalogue::paths(const persistence::LibraryQuery& request,
                       const core::CancellationToken&) const {
    auto answer = client_->call("catalogue.paths", encode_query(request));
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return decode_paths(*answer, "paths");
}

core::Result<std::vector<persistence::LibraryTrackSnapshot>>
RemoteCatalogue::cached_tracks(const std::vector<std::string>& raw_paths,
                               const core::CancellationToken&) const {
    auto encoded = Json::array();
    for (const auto& raw_path : raw_paths) {
        encoded.push_back(protocol::encode_raw_path(raw_path));
    }
    auto answer = client_->call("catalogue.cached_tracks", Json{{"paths", std::move(encoded)}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    const auto tracks = answer->find("tracks");
    if (tracks == answer->end() || !tracks->is_array()) {
        return std::unexpected(malformed("tracks"));
    }
    std::vector<persistence::LibraryTrackSnapshot> snapshots;
    snapshots.reserve(tracks->size());
    for (const auto& value : *tracks) {
        if (!value.is_object() || !value.contains("path")) {
            return std::unexpected(malformed("tracks"));
        }
        auto raw = protocol::decode_raw_path(value.at("path").get<std::string>());
        if (!raw) {
            return std::unexpected(malformed("tracks"));
        }
        persistence::LibraryTrackSnapshot snapshot;
        snapshot.raw_path = std::move(*raw);
        if (const auto fields = value.find("fields");
            fields != value.end() && fields->is_object()) {
            for (const auto& [name, values] : fields->items()) {
                if (!values.is_array()) {
                    return std::unexpected(malformed("tracks"));
                }
                std::vector<std::pair<std::string, std::string>> pairs;
                for (const auto& pair : values) {
                    if (!pair.is_array() || pair.size() != 2U) {
                        return std::unexpected(malformed("tracks"));
                    }
                    pairs.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
                }
                snapshot.facts.fields.emplace(name, std::move(pairs));
            }
        }
        snapshot.facts.search_text = value.value("search_text", std::string{});
        snapshot.facts.title = value.value("title", std::string{});
        snapshot.facts.artist = value.value("artist", std::string{});
        snapshot.facts.album = value.value("album", std::string{});
        snapshot.facts.date = value.value("date", std::string{});
        snapshot.facts.codec = value.value("codec", std::string{});
        snapshot.facts.sample_rate = value.value("sample_rate", std::int64_t{0});
        snapshot.facts.bits = value.value("bits", std::int64_t{0});
        snapshot.facts.channels = value.value("channels", std::int64_t{0});
        snapshot.facts.duration_ms = value.value("duration_ms", std::int64_t{-1});
        snapshot.facts.rating = value.value("rating", std::int64_t{-1});
        snapshot.facts.album_rating = value.value("album_rating", std::int64_t{-1});
        // Null history means unavailable; zeroes would mean never played.
        if (const auto history = value.find("history");
            history != value.end() && history->is_array() && history->size() == 6U) {
            std::array<std::int64_t, 6> facts{};
            for (std::size_t index = 0; index < 6U; ++index) {
                facts[index] = (*history)[index].get<std::int64_t>();
            }
            snapshot.facts.history = facts;
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

core::Result<std::vector<std::array<std::int64_t, 6>>>
RemoteCatalogue::history_facts(const std::vector<persistence::LibraryHistorySource>& sources,
                               const core::CancellationToken&) const {
    auto encoded = Json::array();
    for (const auto& source : sources) {
        Json entry = Json::object();
        entry["path"] = protocol::encode_raw_path(source.source.source_reference);
        if (source.source.source_revision) {
            const auto& revision = *source.source.source_revision;
            entry["revision"] = Json::array({revision.device, revision.inode, revision.size,
                                             revision.modification_time_seconds,
                                             revision.modification_time_nanoseconds});
        }
        if (source.source.source_selection) {
            Json selection = Json::object();
            if (source.source.source_selection->audio_stream_index) {
                selection["stream"] = *source.source.source_selection->audio_stream_index;
            }
            if (source.source.source_selection->subsong_index) {
                selection["subsong"] = *source.source.source_selection->subsong_index;
            }
            entry["selection"] = std::move(selection);
        }
        if (source.source.segment) {
            Json segment = Json::object();
            segment["start"] = source.source.segment->start_sample;
            if (source.source.segment->end_sample) {
                segment["end"] = *source.source.segment->end_sample;
            }
            entry["segment"] = std::move(segment);
        }
        entry["album_hash"] = source.album_hash;
        encoded.push_back(std::move(entry));
    }
    auto answer = client_->call("catalogue.history_facts", Json{{"sources", std::move(encoded)}});
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    const auto facts = answer->find("facts");
    if (facts == answer->end() || !facts->is_array()) {
        return std::unexpected(malformed("facts"));
    }
    std::vector<std::array<std::int64_t, 6>> history;
    history.reserve(facts->size());
    for (const auto& value : *facts) {
        if (!value.is_array() || value.size() != 6U) {
            return std::unexpected(malformed("facts"));
        }
        std::array<std::int64_t, 6> row{};
        for (std::size_t index = 0; index < 6U; ++index) {
            row[index] = value[index].get<std::int64_t>();
        }
        history.push_back(row);
    }
    if (history.size() != sources.size()) {
        // One row per source, in order, or a caller would silently read
        // another track's play count.
        return std::unexpected(malformed("facts"));
    }
    return history;
}

} // namespace trackknife::engine
