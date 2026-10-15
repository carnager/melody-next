// SPDX-License-Identifier: GPL-3.0-only

// What plays, as a line of text (playback.format) and as fields
// (playback.now_playing, protocol level 13): for a file the library does not
// index, as its queue entry says.

#include "recording_audition.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/now_playing_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/protocol/dispatch.hpp"
#include "trackknife/protocol/message.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;
using protocol::Json;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] Json call(protocol::Dispatcher& dispatcher, const std::string& method,
                        Json params = Json::object()) {
    const auto answer = dispatcher.dispatch(
        protocol::Request{.id = 1, .method = method, .params = std::move(params)});
    require(answer.result.has_value(), method + " answers");
    return *answer.result;
}

} // namespace

int main() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("trackknife-now-playing-" + trackknife::core::StableId::random().to_string());
    std::filesystem::create_directories(directory);
    engine::LocalCatalogue catalogue{directory / "lists.sqlite"};
    require(catalogue.prepare().has_value(), "the library opens");
    // Declared first so it outlives the player, which holds it.
    trackknife::testing::RecordingAudition silent;
    auto player = engine::Player::create_without_audio();
    require(player->set_output(&silent).has_value(), "the player plays on the recording");
    protocol::Dispatcher dispatcher;
    engine::register_now_playing_methods(dispatcher, catalogue, *player);

    require(call(dispatcher, "playback.now_playing").at("track").is_null(),
            "nothing playing: no track");

    // Not in the library: the queue entry's tags.
    const std::string title = "בגילגול";
    engine::QueueEntry playing;
    playing.source.raw_path = "/music/Shalom Hanoch/06 Track.flac";
    playing.duration_ms = 277'000;
    playing.title = title;
    playing.group.artist = "Shalom Hanoch";
    playing.group.album = "HaMeitav";
    playing.group.date = "2006";
    player->replace_queue({playing});
    require(player->play_entry(playing.entry_id).has_value(), "the entry plays");

    const auto track = call(dispatcher, "playback.now_playing").at("track");
    require(track.is_object(), "playing: a track");
    require(track.at("entry") == playing.entry_id.to_string(), "the entry playing");
    require(track.at("path") == protocol::encode_raw_path(playing.source.raw_path), "its path");
    require(track.at("title") == title, "its title, as the queue says");
    require(track.at("artist") == "Shalom Hanoch", "its artist");
    require(track.at("album") == "HaMeitav", "its album");
    require(track.at("date") == "2006", "its date");
    require(track.at("album_artist").is_null() && track.at("genre").is_null() &&
                track.at("track_number").is_null(),
            "what is not known is null, not empty");
    require(track.at("duration_ms") == 277'000, "its length");
    require(track.at("rating").is_null(), "unrated");

    const auto line =
        call(dispatcher, "playback.format", Json{{"format", "%artist% - %title% [%length%]"}});
    require(line.at("text") == "Shalom Hanoch - " + title + " [4:37]",
            "and as a line, as melody-cli current prints it");

    std::filesystem::remove_all(directory);
    std::cout << "now playing: as text and as fields\n";
    return EXIT_SUCCESS;
}
