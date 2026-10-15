// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/now_playing_methods.hpp"

#include "track_format.hpp"

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/persistence/tkq_row.hpp"
#include "trackknife/protocol/message.hpp"

#include <algorithm>

namespace trackknife::engine {
namespace {

using protocol::Json;

// A file the library does not index: what its queue entry says of it.
[[nodiscard]] persistence::TkqRowFacts from_queue(const Player& player,
                                                  const Player::State& state) {
    persistence::TkqRowFacts facts;
    const auto queue = player.queue();
    const auto entry = std::ranges::find(queue, state.entry, &QueueEntry::entry_id);
    if (entry != queue.end()) {
        facts.title = entry->title;
        facts.artist =
            entry->group.artist.empty() ? entry->group.album_artist : entry->group.artist;
        facts.album = entry->group.album;
        facts.date = entry->group.date;
    }
    facts.duration_ms = state.duration_ms;
    return facts;
}

// A tag's first value as written, or null.
[[nodiscard]] Json first_tag(const persistence::TkqRowFacts& facts, const std::string& name) {
    const auto found = facts.fields.find(name);
    if (found == facts.fields.end() || found->second.empty()) {
        return nullptr;
    }
    return protocol::displayable_text(found->second.front().first);
}

} // namespace

void register_now_playing_methods(protocol::Dispatcher& dispatcher, const Catalogue& catalogue,
                                  Player& player) {
    dispatcher.on("playback.format",
                  [&catalogue, &player](const Json& params) -> core::Result<Json> {
                      const auto format = params.find("format");
                      if (format == params.end() || !format->is_string()) {
                          return std::unexpected(
                              core::Error{.code = core::ErrorCode::invalid_argument,
                                          .message = "a format is required",
                                          .context = {{.key = "param", .value = "format"}}});
                      }
                      auto program = compile_client_format(
                          format->get<std::string>(), titleformat::FormatContextKind::now_playing);
                      if (!program) {
                          return std::unexpected(std::move(program.error()));
                      }
                      const auto state = player.state();
                      if (state.source.raw_path.empty()) {
                          return Json{{"text", nullptr}};
                      }
                      auto indexed = catalogue.cached_tracks({state.source.raw_path});
                      auto facts = indexed && !indexed->empty() ? std::move(indexed->front().facts)
                                                                : from_queue(player, state);
                      name_by_file(facts, state.source.raw_path);
                      auto host = track_fields(facts, state.source.raw_path);
                      host["playbackstate"] = state.status;
                      host["playbacktime"] = clock(state.position_ms);
                      // What the player says of the length beats what the index last saw.
                      if (state.duration_ms >= 0) {
                          host["length"] = clock(state.duration_ms);
                          host["playbackremaining"] = clock(state.duration_ms - state.position_ms);
                      }
                      auto text = persistence::tkq_format(*program, facts, host);
                      if (!text) {
                          return std::unexpected(std::move(text.error()));
                      }
                      return Json{{"text", protocol::displayable_text(*text)}};
                  });

    // What plays, as fields rather than a line of text -- for a client that
    // lays it out itself (a home automation's media card, say). Asked when
    // the playing entry changes, not with every state: the state goes out
    // many times a second, and this reads the library.
    dispatcher.on("playback.now_playing", [&catalogue, &player](const Json&) -> core::Result<Json> {
        const auto state = player.state();
        if (state.source.raw_path.empty()) {
            return Json{{"track", nullptr}};
        }
        auto indexed = catalogue.cached_tracks({state.source.raw_path});
        auto facts = indexed && !indexed->empty() ? std::move(indexed->front().facts)
                                                  : from_queue(player, state);
        name_by_file(facts, state.source.raw_path);
        Json track = Json::object();
        track["entry"] = state.entry.to_string();
        track["instance"] = state.instance;
        track["path"] = protocol::encode_raw_path(state.source.raw_path);
        track["title"] = protocol::displayable_text(facts.title);
        track["artist"] =
            facts.artist.empty() ? Json(nullptr) : Json(protocol::displayable_text(facts.artist));
        track["album"] =
            facts.album.empty() ? Json(nullptr) : Json(protocol::displayable_text(facts.album));
        track["album_artist"] = first_tag(facts, "albumartist");
        track["date"] =
            facts.date.empty() ? Json(nullptr) : Json(protocol::displayable_text(facts.date));
        track["track_number"] = first_tag(facts, "tracknumber");
        track["disc_number"] = first_tag(facts, "discnumber");
        track["genre"] = first_tag(facts, "genre");
        // What the player says of the length beats what the index last saw.
        track["duration_ms"] = state.duration_ms >= 0 ? state.duration_ms : facts.duration_ms;
        track["rating"] = facts.rating > 0 ? Json(facts.rating) : Json(nullptr);
        return Json{{"track", std::move(track)}};
    });
}

} // namespace trackknife::engine
