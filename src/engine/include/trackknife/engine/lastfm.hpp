// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/listen_accounting.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/interruptible_pause.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace trackknife::engine {

// Last.fm, where playback happens: scrobbles and "now playing" for what this
// engine plays, with or without a window open, and love for any client.
//
// Signing in needs a browser, so a client does it and hands the session over
// (lastfm.set_session); one session serves every engine it is given to. The
// session is kept in a file only its owner can read, and never sent back out.
// The rules for what counts as a listen are core::ListenAccounting, the ones
// Trackknife used when it scrobbled itself.
class LastFm final {
  public:
    struct Session final {
        std::string api_key;
        std::string secret;
        std::string session_key;
        std::string user;
    };
    struct Status final {
        std::string user; // empty: no session
        bool enabled{false};
        std::size_t pending{0};
        std::string message;
    };

    // `endpoint` is Last.fm's API root; tests give a fake one.
    LastFm(Player& player, Catalogue& catalogue, std::filesystem::path state_file,
           std::string endpoint = "https://ws.audioscrobbler.com/2.0/");
    LastFm(const LastFm&) = delete;
    LastFm(LastFm&&) = delete;
    LastFm& operator=(const LastFm&) = delete;
    LastFm& operator=(LastFm&&) = delete;
    ~LastFm();

    // Samples the player and sends what is due, about once a second.
    void start();
    void stop();

    [[nodiscard]] core::Result<void> set_session(Session session);
    // Signs out: the session and anything not yet sent are dropped.
    [[nodiscard]] core::Result<void> clear();
    [[nodiscard]] Status status() const;
    // Loves or unloves a track; with no artist and title, the one playing.
    [[nodiscard]] core::Result<void> love(std::optional<std::string> artist,
                                          std::optional<std::string> title, bool loved);

    // One round of what the worker does, at `now_ms` on a monotonic clock
    // and `wall_s` in Unix seconds: for tests, which cannot wait out a track.
    void tick(std::int64_t now_ms, std::int64_t wall_s);

  private:
    struct Track final {
        std::string artist;
        std::string title;
        std::string album;
        double duration{0.0};
    };
    struct Pending final {
        Track track;
        std::int64_t timestamp{0};
    };

    void load();
    [[nodiscard]] bool save_locked();
    [[nodiscard]] std::optional<Track> playing_track(const Player::State& state);
    void sample_locked(std::int64_t now_ms, std::int64_t wall_s);
    void flush(std::int64_t now_ms);
    [[nodiscard]] core::Result<protocol::Json> call(const std::string& method,
                                                    std::vector<std::pair<std::string, std::string>> params,
                                                    const Session& session);

    Player* player_;
    Catalogue* catalogue_;
    std::filesystem::path state_file_;
    std::string endpoint_;

    mutable std::mutex mutex_;
    std::optional<Session> session_;
    bool enabled_{false};
    std::deque<Pending> pending_;
    std::string message_;
    bool blocked_{false};      // the state file could not be read; never overwrite it
    bool auth_failed_{false};  // the session was refused; wait for a new one
    core::ListenAccounting listen_;
    std::string listen_identity_;
    std::optional<Track> listen_track_;
    std::int64_t started_{0};
    std::optional<Track> now_playing_;
    std::int64_t retry_at_ms_{0};
    int failures_{0};
    // The last entry's tags, so the library is not asked every second.
    core::StableId cached_entry_;
    std::optional<Track> cached_track_;

    std::atomic_bool running_{false};
    InterruptiblePause pause_;
    std::thread worker_;
};

void register_lastfm_methods(protocol::Dispatcher& dispatcher, LastFm& lastfm);

} // namespace trackknife::engine
