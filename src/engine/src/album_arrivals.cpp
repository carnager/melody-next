// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/album_arrivals.hpp"

#include "trackknife/protocol/message.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The folder every one of `paths` is in.
[[nodiscard]] std::string common_folder(const std::vector<std::string>& paths) {
    std::optional<std::filesystem::path> common;
    for (const auto& raw : paths) {
        const auto folder = std::filesystem::path{raw}.parent_path();
        if (!common) {
            common = folder;
            continue;
        }
        std::filesystem::path shared;
        for (auto mine = common->begin(), theirs = folder.begin();
             mine != common->end() && theirs != folder.end() && *mine == *theirs;
             ++mine, ++theirs) {
            shared /= *mine;
        }
        common = shared;
    }
    return common ? common->native() : std::string{};
}

} // namespace

AlbumArrivals::AlbumArrivals(LocalCatalogue& catalogue, EventSink sink,
                             const std::chrono::seconds settle)
    : catalogue_(catalogue), sink_(std::move(sink)), settle_(settle), since_(now_seconds()),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

AlbumArrivals::~AlbumArrivals() {
    thread_.request_stop();
    woken_.notify_all();
}

void AlbumArrivals::changed() {
    {
        const std::scoped_lock held{mutex_};
        look_ = true;
    }
    woken_.notify_all();
}

void AlbumArrivals::run(const std::stop_token& stop) {
    // Albums seen arriving but not yet whole: looked at again until they are.
    bool waiting = false;
    while (!stop.stop_requested()) {
        {
            std::unique_lock held{mutex_};
            // Woken by a change, else every second while one is settling.
            woken_.wait_for(held, stop, waiting ? std::chrono::seconds{1} : std::chrono::hours{1},
                            [this] { return look_; });
            if (stop.stop_requested()) {
                return;
            }
            look_ = false;
        }
        auto arrived = catalogue_.albums_arrived_since(since_);
        if (!arrived) {
            waiting = false;
            continue;
        }
        const auto now = now_seconds();
        std::vector<std::string> whole;
        waiting = false;
        for (const auto& album : *arrived) {
            if (announced_.contains(album.album_key)) {
                continue;
            }
            if (now - album.last_added >= settle_.count()) {
                whole.push_back(album.album_key);
            } else {
                waiting = true;
            }
        }
        if (!whole.empty()) {
            announce(whole);
        }
    }
}

void AlbumArrivals::announce(const std::vector<std::string>& keys) {
    auto albums = Json::array();
    for (const auto& key : keys) {
        announced_.insert(key);
        auto tracks =
            catalogue_.query(persistence::LibraryQuery{.kind = persistence::LibraryEntryKind::track,
                                                       .text = {},
                                                       .artist = std::nullopt,
                                                       .album_key = key,
                                                       .raw_path = std::nullopt,
                                                       .offset = 0,
                                                       .limit = 5'000,
                                                       .newest_first = false,
                                                       .random = false,
                                                       .view = {},
                                                       .view_path = {},
                                                       .view_filter = {},
                                                       .folders = false,
                                                       .folder = std::nullopt});
        if (!tracks || tracks->entries.empty()) {
            continue;
        }
        std::vector<std::string> paths;
        auto encoded = Json::array();
        for (const auto& track : tracks->entries) {
            paths.push_back(track.key);
            encoded.push_back(protocol::encode_raw_path(track.key));
        }
        const auto& first = tracks->entries.front();
        albums.push_back(Json{{"key", protocol::encode_raw_path(key)},
                              {"artist", protocol::displayable_text(first.artist)},
                              {"album", protocol::displayable_text(first.album)},
                              {"date", protocol::displayable_text(first.date)},
                              {"folder", protocol::encode_raw_path(common_folder(paths))},
                              {"paths", std::move(encoded)}});
    }
    if (albums.empty() || !sink_) {
        return;
    }
    std::cerr << "melodyd: " << albums.size() << " new album(s) in the library\n";
    sink_(
        protocol::Event{.name = "catalogue.albums_added", .data = {{"albums", std::move(albums)}}});
}

} // namespace trackknife::engine
