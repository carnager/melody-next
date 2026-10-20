// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/interruptible_pause.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/transcode_cache.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::engine {

// ADR-0302: listening together, the engine's part. Hosting, it tells a
// melody-together server what it plays -- each track, seek and pause --
// on the server's clock; listening, it follows: it finds the host's track
// in its own library and plays it where the host is, seeking back when it
// drifts. The session is the engine's, so it goes on with every window
// closed and plays through whatever speakers the engine has.
class Together final {
  public:
    // Where a room is: the server, the room's name, and the key that opens
    // it. An invite says all three in one line, to be sent to a friend:
    //   https://together.example.org/rooms/ROOM#INVITE
    struct Invite final {
        std::string server;
        std::string room;
        std::string key;
    };
    [[nodiscard]] static std::optional<Invite> parse_invite(std::string_view text);
    [[nodiscard]] static std::string invite_text(const Invite& invite);

    // The queries that find a described track in a library, in the order to
    // try them: by MusicBrainz track id, then by artist, title and album,
    // then without the album.
    [[nodiscard]] static std::vector<std::string> track_queries(const protocol::Json& track);

    // ADR-0302 phase 4: a track a friend suggested in a party room.
    struct Suggestion final {
        std::uint64_t id{0};
        std::string name; // who suggested it
        std::string title;
        std::string artist;
        std::size_t votes{0};
        bool voted{false}; // by this engine's listener
        bool ready{false}; // its copy has come, so it can play
    };

    struct Status final {
        std::string role; // "", "host" or "listener"
        std::string invite;
        std::vector<std::string> listeners;
        // What plays in the room, as "Artist — Title".
        std::string track;
        // Something to say: a track not in this library, the room closed, the
        // server not reached.
        std::string message;
        bool party{false};
        // Most wanted first.
        std::vector<Suggestion> suggestions;
    };

    // `directory`: where a listener keeps the host's copies it fetched, and
    // the host its own transcodes when the engine keeps none (`transcodes`).
    Together(Player& player, Catalogue& catalogue, EventSink sink, std::filesystem::path directory,
             TranscodeCache* transcodes = nullptr);
    Together(const Together&) = delete;
    Together(Together&&) = delete;
    Together& operator=(const Together&) = delete;
    Together& operator=(Together&&) = delete;
    ~Together();

    // A room on `server`, made with its password; this engine its host. The
    // invite to send to friends.
    // `room`: the room's name, else one is made; `name`: the host's, at a
    // party.
    [[nodiscard]] core::Result<std::string> host(const std::string& server,
                                                 const std::string& password,
                                                 const std::string& name = {},
                                                 const std::string& room = {});
    // Into the room an invite names, as `name`.
    [[nodiscard]] core::Result<void> join(const std::string& invite, const std::string& name);
    // Out of the room -- closing it, as its host.
    void leave();
    // The engine stops: the session ends here without a word to the room, and
    // is kept -- resume() takes it up again when the engine starts.
    void suspend();
    // Back into the room this engine was in when it stopped, if any.
    void resume();
    [[nodiscard]] Status status() const;

    // Phase 4. The host makes the room a party, or ends it.
    [[nodiscard]] core::Result<void> party(bool on);
    // Anyone in a party -- the host too -- suggests a track of its library:
    // its copy sent, the suggestion's number answered.
    [[nodiscard]] core::Result<std::uint64_t> suggest(const std::string& raw_path);
    // A vote for a suggestion, or taken back -- the host's as anyone's.
    [[nodiscard]] core::Result<void> vote(std::uint64_t id);
    // The host removes a suggestion.
    [[nodiscard]] core::Result<void> remove(std::uint64_t id);

  private:
    struct Session;
    // Ends the session there is, if any; under control_. Leaving, the room
    // is told and the session forgotten; else it is kept for resume().
    void end(bool leaving = true);
    void begin(std::shared_ptr<Session> session, Status status);
    void remember(const Session& session, const std::string& invite) const;
    void forget() const;
    // The room is gone: the session ends, and is not taken up again.
    void gone(Session& session, std::string message);
    void run(std::shared_ptr<Session> session);
    void host_round(Session& session);
    void listen_round(Session& session);
    void follow(Session& session, const protocol::Json& state);
    // ADR-0302 phase 2: the host's copy of a track sent, once, for whoever
    // lacks one; fetched by a listener who does, ahead when it can.
    void send(Session& session, const QueueEntry& entry);
    void fetch(Session& session, const std::string& entry);
    void play_what_is_due(Session& session);
    void start(Session& session, const std::string& path);
    [[nodiscard]] std::string library_copy(Session& session, const std::string& entry,
                                           const protocol::Json& track);
    [[nodiscard]] protocol::Json describe(Session& session, const QueueEntry& entry);
    [[nodiscard]] protocol::Json describe_file(const std::string& raw_path);
    // What the room says besides what plays: whether it is a party, and the
    // suggestions.
    void read_room(Session& session, const protocol::Json& answer);
    // The host's: the room's turn, when its own track has begun.
    void take_turn(Session& session, const Player::State& state);
    [[nodiscard]] std::shared_ptr<Session> current() const;
    void keep_in_step(Session& session);
    [[nodiscard]] core::Result<void> measure_clock(Session& session);
    void changed();
    void say(std::string message);

    Player* player_;
    Catalogue* catalogue_;
    EventSink sink_;
    std::filesystem::path directory_;
    TranscodeCache* transcodes_;
    std::unique_ptr<TranscodeCache> own_transcodes_;
    // Starting and ending a session, one at a time.
    std::mutex control_;
    mutable std::mutex mutex_;
    std::shared_ptr<Session> session_;
    Status status_;
    std::thread worker_;
};

void register_together_methods(protocol::Dispatcher& dispatcher, Together& together);

} // namespace trackknife::engine
