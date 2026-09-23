// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/catalogue_methods.hpp"

#include "trackknife/protocol/message.hpp"
#include "trackknife/query/tkq.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error bad_params(std::string message, std::string member) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "param", .value = std::move(member)}}};
}

[[nodiscard]] core::Result<std::string> required_string(const Json& params, const char* name) {
    const auto found = params.find(name);
    if (found == params.end() || !found->is_string()) {
        return std::unexpected(bad_params("a string parameter is required", name));
    }
    return found->get<std::string>();
}

// A list of raw paths arrives base64 encoded, because a path is bytes.
[[nodiscard]] Json encoded_paths(const std::vector<std::string>& raw_paths) {
    auto encoded = Json::array();
    for (const auto& raw_path : raw_paths) {
        encoded.push_back(protocol::encode_raw_path(raw_path));
    }
    return encoded;
}

} // namespace

void register_catalogue_methods(protocol::Dispatcher& dispatcher, Catalogue& catalogue) {
    dispatcher.on("catalogue.roots", [&catalogue](const Json&) -> core::Result<Json> {
        auto roots = catalogue.roots();
        if (!roots) {
            return std::unexpected(std::move(roots.error()));
        }
        auto rendered = Json::array();
        for (const auto& root : *roots) {
            rendered.push_back(Json{{"path", protocol::encode_raw_path(root.raw_path)},
                                    {"available", root.available},
                                    {"error", root.error}});
        }
        return Json{{"roots", std::move(rendered)}};
    });

    // Mutating the root set. Adding a folder does not scan it; that is a
    // separate, long ask, which is why it is a job.
    dispatcher.on("catalogue.add_root", [&catalogue](const Json& params) -> core::Result<Json> {
        auto encoded = required_string(params, "path");
        if (!encoded) {
            return std::unexpected(std::move(encoded.error()));
        }
        auto raw_path = protocol::decode_raw_path(*encoded);
        if (!raw_path) {
            return std::unexpected(bad_params("path is not an encoded path", "path"));
        }
        auto added = catalogue.add_root(*raw_path);
        if (!added) {
            return std::unexpected(std::move(added.error()));
        }
        return Json{};
    });

    dispatcher.on("catalogue.remove_root", [&catalogue](const Json& params) -> core::Result<Json> {
        auto encoded = required_string(params, "path");
        if (!encoded) {
            return std::unexpected(std::move(encoded.error()));
        }
        auto raw_path = protocol::decode_raw_path(*encoded);
        if (!raw_path) {
            return std::unexpected(bad_params("path is not an encoded path", "path"));
        }
        auto removed = catalogue.remove_root(*raw_path);
        if (!removed) {
            return std::unexpected(std::move(removed.error()));
        }
        return Json{};
    });

    // Browsing. The kind decides what a page contains; the optional filters
    // narrow it the same way they do locally.
    const auto read_query = [](const Json& params) -> core::Result<persistence::LibraryQuery> {
        persistence::LibraryQuery request;
        if (const auto kind = params.find("kind"); kind != params.end()) {
            if (!kind->is_number_integer()) {
                return std::unexpected(bad_params("kind must be an integer", "kind"));
            }
            const auto raw = kind->get<int>();
            if (raw < 0 || raw > 2) {
                return std::unexpected(bad_params("kind is artist, album or track", "kind"));
            }
            request.kind = static_cast<persistence::LibraryEntryKind>(raw);
        }
        request.text = params.value("text", std::string{});
        // Keys are bytes: an album key carries its folder's path. They
        // travel encoded like paths, so a name that is not UTF-8 neither
        // breaks the message nor changes which album it names.
        if (const auto artist = params.find("artist");
            artist != params.end() && artist->is_string()) {
            auto decoded = protocol::decode_raw_path(artist->get<std::string>());
            if (!decoded) {
                return std::unexpected(bad_params("artist is not an encoded key", "artist"));
            }
            request.artist = std::move(*decoded);
        }
        if (const auto album = params.find("album_key");
            album != params.end() && album->is_string()) {
            auto decoded = protocol::decode_raw_path(album->get<std::string>());
            if (!decoded) {
                return std::unexpected(bad_params("album_key is not an encoded key", "album_key"));
            }
            request.album_key = std::move(*decoded);
        }
        if (const auto path = params.find("path"); path != params.end() && path->is_string()) {
            auto raw_path = protocol::decode_raw_path(path->get<std::string>());
            if (!raw_path) {
                return std::unexpected(bad_params("path is not an encoded path", "path"));
            }
            request.raw_path = std::move(*raw_path);
        }
        request.offset = params.value("offset", std::size_t{0});
        request.limit = params.value("limit", std::size_t{200});
        return request;
    };

    const auto render_page = [](const persistence::LibraryPage& page) {
        auto entries = Json::array();
        for (const auto& entry : page.entries) {
            Json rendered = Json::object();
            rendered["kind"] = static_cast<int>(entry.kind);
            rendered["key"] = protocol::encode_raw_path(entry.key);
            rendered["label"] = protocol::displayable_text(entry.label);
            rendered["artist"] = protocol::displayable_text(entry.artist);
            rendered["album"] = protocol::displayable_text(entry.album);
            rendered["tracks"] = entry.tracks;
            rendered["available"] = entry.available;
            rendered["track_number"] = entry.track_number;
            rendered["albums"] = entry.albums;
            rendered["rating_hash"] = entry.rating_hash;
            rendered["rating"] = entry.rating;
            entries.push_back(std::move(rendered));
        }
        return Json{{"entries", std::move(entries)}, {"more", page.more}};
    };

    dispatcher.on("catalogue.query",
                  [&catalogue, read_query, render_page](const Json& params) -> core::Result<Json> {
                      auto request = read_query(params);
                      if (!request) {
                          return std::unexpected(std::move(request.error()));
                      }
                      auto page = catalogue.query(*request);
                      if (!page) {
                          return std::unexpected(std::move(page.error()));
                      }
                      return render_page(*page);
                  });

    dispatcher.on("catalogue.paths",
                  [&catalogue, read_query](const Json& params) -> core::Result<Json> {
                      auto request = read_query(params);
                      if (!request) {
                          return std::unexpected(std::move(request.error()));
                      }
                      auto paths = catalogue.paths(*request);
                      if (!paths) {
                          return std::unexpected(std::move(paths.error()));
                      }
                      return Json{{"paths", encoded_paths(*paths)}};
                  });

    // A page of a compiled query. This is what the library search box runs:
    // filter_paths answers which tracks match, this answers what to show.
    dispatcher.on(
        "catalogue.filter", [&catalogue, render_page](const Json& params) -> core::Result<Json> {
            auto source = required_string(params, "query");
            if (!source) {
                return std::unexpected(std::move(source.error()));
            }
            auto compiled = query::compile_tkq(*source);
            if (!compiled) {
                return std::unexpected(std::move(compiled.error()));
            }
            auto page = catalogue.filter(*compiled, params.value("offset", std::size_t{0}),
                                         params.value("limit", std::size_t{200}));
            if (!page) {
                return std::unexpected(std::move(page.error()));
            }
            return render_page(*page);
        });

    dispatcher.on("catalogue.filter_paths", [&catalogue](const Json& params) -> core::Result<Json> {
        auto source = required_string(params, "query");
        if (!source) {
            return std::unexpected(std::move(source.error()));
        }
        // A malformed query is the caller's mistake, and the compiler already
        // says which part; that error is forwarded rather than restated.
        auto compiled = query::compile_tkq(*source);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        auto paths = catalogue.filter_paths(*compiled);
        if (!paths) {
            return std::unexpected(std::move(paths.error()));
        }
        return Json{{"paths", encoded_paths(*paths)}};
    });

    // Cached facts for a set of paths, in the order asked. This is what a
    // search result needs to render: without it a remote library returns
    // paths and no metadata, which looks like a broken library rather than a
    // missing method.
    dispatcher.on(
        "catalogue.cached_tracks", [&catalogue](const Json& params) -> core::Result<Json> {
            const auto found = params.find("paths");
            if (found == params.end() || !found->is_array()) {
                return std::unexpected(bad_params("an array of paths is required", "paths"));
            }
            std::vector<std::string> raw_paths;
            raw_paths.reserve(found->size());
            for (const auto& value : *found) {
                if (!value.is_string()) {
                    return std::unexpected(bad_params("each path must be encoded", "paths"));
                }
                auto raw = protocol::decode_raw_path(value.get<std::string>());
                if (!raw) {
                    return std::unexpected(bad_params("a path is not an encoded path", "paths"));
                }
                raw_paths.push_back(std::move(*raw));
            }
            auto snapshots = catalogue.cached_tracks(raw_paths);
            if (!snapshots) {
                return std::unexpected(std::move(snapshots.error()));
            }
            auto rendered = Json::array();
            for (const auto& snapshot : *snapshots) {
                Json entry = Json::object();
                entry["path"] = protocol::encode_raw_path(snapshot.raw_path);
                // Field values carry both their original and
                // lowercased forms, because the query engine matches
                // on one and displays the other.
                Json fields = Json::object();
                for (const auto& [name, values] : snapshot.facts.fields) {
                    auto pairs = Json::array();
                    for (const auto& [original, folded] : values) {
                        pairs.push_back(Json::array({protocol::displayable_text(original),
                                                     protocol::displayable_text(folded)}));
                    }
                    fields[protocol::displayable_text(name)] = std::move(pairs);
                }
                entry["fields"] = std::move(fields);
                entry["search_text"] = protocol::displayable_text(snapshot.facts.search_text);
                entry["title"] = protocol::displayable_text(snapshot.facts.title);
                entry["artist"] = protocol::displayable_text(snapshot.facts.artist);
                entry["album"] = protocol::displayable_text(snapshot.facts.album);
                entry["date"] = protocol::displayable_text(snapshot.facts.date);
                entry["codec"] = protocol::displayable_text(snapshot.facts.codec);
                entry["sample_rate"] = snapshot.facts.sample_rate;
                entry["bits"] = snapshot.facts.bits;
                entry["channels"] = snapshot.facts.channels;
                entry["duration_ms"] = snapshot.facts.duration_ms;
                entry["rating"] = snapshot.facts.rating;
                entry["album_rating"] = snapshot.facts.album_rating;
                // Absent history means unavailable, not unplayed, so
                // it is null rather than zeroes.
                entry["history"] =
                    snapshot.facts.history ? Json(*snapshot.facts.history) : Json(nullptr);
                rendered.push_back(std::move(entry));
            }
            return Json{{"tracks", std::move(rendered)}};
        });

    dispatcher.on("catalogue.ratings", [&catalogue](const Json& params) -> core::Result<Json> {
        const auto found = params.find("hashes");
        if (found == params.end() || !found->is_array()) {
            return std::unexpected(bad_params("an array of hashes is required", "hashes"));
        }
        std::vector<std::string> hashes;
        for (const auto& entry : *found) {
            if (!entry.is_string()) {
                return std::unexpected(bad_params("each hash must be a string", "hashes"));
            }
            hashes.push_back(entry.get<std::string>());
        }
        auto ratings = catalogue.ratings(hashes);
        if (!ratings) {
            return std::unexpected(std::move(ratings.error()));
        }
        return Json{{"ratings", *ratings}};
    });

    dispatcher.on("catalogue.set_rating", [&catalogue](const Json& params) -> core::Result<Json> {
        auto hash = required_string(params, "hash");
        if (!hash) {
            return std::unexpected(std::move(hash.error()));
        }
        const auto album = params.value("album", false);
        // Not is_number_unsigned(): a JSON 7 arrives as a signed integer
        // unless the sender went out of its way, and requiring unsignedness
        // would reject every ordinary client for no benefit. The range check
        // is what actually matters.
        const auto rating = params.find("rating");
        if (rating == params.end() || !rating->is_number_integer() ||
            rating->get<std::int64_t>() < 0) {
            return std::unexpected(bad_params("rating must be a non-negative integer", "rating"));
        }
        auto stored =
            catalogue.set_rating(*hash, album, static_cast<unsigned>(rating->get<std::int64_t>()));
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        // A void operation still answers, so the caller learns it completed.
        return Json{};
    });

    // Play counts and timestamps. Only the fields the lookup keys on cross the
    // wire -- path, revision, decoder selection, span and the album hash --
    // rather than a whole ListItem, most of which the engine would ignore.
    dispatcher.on(
        "catalogue.history_facts", [&catalogue](const Json& params) -> core::Result<Json> {
            const auto found = params.find("sources");
            if (found == params.end() || !found->is_array()) {
                return std::unexpected(bad_params("an array of sources is required", "sources"));
            }
            std::vector<persistence::LibraryHistorySource> sources;
            sources.reserve(found->size());
            for (const auto& value : *found) {
                if (!value.is_object() || !value.contains("path")) {
                    return std::unexpected(
                        bad_params("each source needs an encoded path", "sources"));
                }
                auto raw = protocol::decode_raw_path(value.at("path").get<std::string>());
                if (!raw) {
                    return std::unexpected(bad_params("a path is not an encoded path", "sources"));
                }
                persistence::LibraryHistorySource source;
                source.source.source = persistence::ListSource::local;
                source.source.source_reference = std::move(*raw);
                if (const auto revision = value.find("revision");
                    revision != value.end() && revision->is_array() && revision->size() == 5U) {
                    source.source.source_revision = core::LocalSourceRevision{
                        .device = (*revision)[0].get<std::uint64_t>(),
                        .inode = (*revision)[1].get<std::uint64_t>(),
                        .size = (*revision)[2].get<std::uint64_t>(),
                        .modification_time_seconds = (*revision)[3].get<std::int64_t>(),
                        .modification_time_nanoseconds = (*revision)[4].get<std::int64_t>()};
                }
                if (const auto selection = value.find("selection");
                    selection != value.end() && selection->is_object()) {
                    persistence::ListItemSourceSelection chosen;
                    if (const auto stream = selection->find("stream");
                        stream != selection->end() && stream->is_number_integer()) {
                        chosen.audio_stream_index = stream->get<int>();
                    }
                    if (const auto subsong = selection->find("subsong");
                        subsong != selection->end() && subsong->is_number_integer()) {
                        chosen.subsong_index = subsong->get<int>();
                    }
                    source.source.source_selection = chosen;
                }
                if (const auto segment = value.find("segment");
                    segment != value.end() && segment->is_object()) {
                    persistence::ListItemSegment span;
                    span.start_sample = segment->value("start", std::int64_t{0});
                    if (const auto end = segment->find("end");
                        end != segment->end() && end->is_number_integer()) {
                        span.end_sample = end->get<std::int64_t>();
                    }
                    source.source.segment = span;
                }
                source.album_hash = value.value("album_hash", std::string{});
                sources.push_back(std::move(source));
            }
            auto facts = catalogue.history_facts(sources);
            if (!facts) {
                return std::unexpected(std::move(facts.error()));
            }
            auto rendered = Json::array();
            for (const auto& entry : *facts) {
                rendered.push_back(entry);
            }
            return Json{{"facts", std::move(rendered)}};
        });

    dispatcher.on(
        "catalogue.artwork_source", [&catalogue](const Json& params) -> core::Result<Json> {
            auto encoded = required_string(params, "album_key");
            if (!encoded) {
                return std::unexpected(std::move(encoded.error()));
            }
            auto key = protocol::decode_raw_path(*encoded);
            if (!key) {
                return std::unexpected(bad_params("album_key is not an encoded key", "album_key"));
            }
            auto source = catalogue.artwork_source(*key);
            if (!source) {
                return std::unexpected(std::move(source.error()));
            }
            // An album with no artwork is a success carrying null,
            // not a not_found: the album exists, the cover does not.
            //
            // Built by assignment rather than brace initialisation:
            // Json{nullptr} is an array holding null, not null, and
            // the difference only shows up on the wire.
            Json answer = Json::object();
            answer["source"] = *source ? Json(protocol::encode_raw_path(**source)) : Json(nullptr);
            return answer;
        });
}

} // namespace trackknife::engine
