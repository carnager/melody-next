// SPDX-License-Identifier: GPL-3.0-only
//
// ADR-0302: two engines listening together through a melody-together server
// -- the real one -- each with its own copy of the album. The host plays,
// seeks and pauses; the listener follows, where the host is, from its own
// library -- or, for a track it lacks, from the host's copy; and, at a
// party, the room plays a track the friend suggested, from her copy.

#include "trackknife/core/posix.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/together.hpp"
#include "trackknife/protocol/message.hpp"

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

namespace {

namespace audio = trackknife::audio;
namespace core = trackknife::core;
namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;
using namespace std::chrono_literals;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

// Whether `holds` comes true within `within`.
[[nodiscard]] bool eventually(const std::function<bool()>& holds,
                              const std::chrono::milliseconds within = 8s) {
    const auto until = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < until) {
        if (holds()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return holds();
}

// An output whose position runs with the clock while it plays, and moves
// where a seek says: a speaker, as far as keeping in step can tell.
class ClockAudition final : public audio::Audition {
  public:
    [[nodiscard]] audio::LocalAuditionSnapshot snapshot() const override {
        const std::lock_guard guard{mutex_};
        audio::LocalAuditionSnapshot current;
        current.state = state_;
        current.raw_path = loaded_;
        current.format = trackknife::formats::PcmFormat{1000, 2, "stereo"};
        current.position_sample = position_locked();
        return current;
    }
    [[nodiscard]] core::Result<void>
    load_selected_and_play(std::string raw_path, trackknife::formats::AudioSourceSelection,
                           std::optional<trackknife::formats::ReplayGainInfo>) override {
        const std::lock_guard guard{mutex_};
        loaded_ = std::move(raw_path);
        base_ms_ = 0;
        since_ = std::chrono::steady_clock::now();
        state_ = audio::LocalAuditionState::playing;
        return {};
    }
    [[nodiscard]] core::Result<void>
    load_selected_segment_and_play(std::string raw_path, trackknife::formats::AudioSourceSelection,
                                   trackknife::formats::SampleRange,
                                   std::optional<trackknife::formats::ReplayGainInfo>) override {
        return load_selected_and_play(std::move(raw_path), {}, {});
    }
    [[nodiscard]] core::Result<void>
    restore_paused(std::string raw_path, core::LocalSourceRevision,
                   trackknife::formats::AudioSourceSelection,
                   std::optional<trackknife::formats::SampleRange>, const std::int64_t position_ms,
                   std::optional<trackknife::formats::ReplayGainInfo>) override {
        const std::lock_guard guard{mutex_};
        loaded_ = std::move(raw_path);
        base_ms_ = position_ms;
        state_ = audio::LocalAuditionState::paused;
        return {};
    }
    [[nodiscard]] core::Result<void>
    queue_gapless_next_selected(std::string, trackknife::formats::AudioSourceSelection,
                                std::optional<trackknife::formats::ReplayGainInfo>,
                                std::uint64_t) override {
        return {};
    }
    [[nodiscard]] core::Result<void> queue_gapless_next_selected_segment(
        std::string, trackknife::formats::AudioSourceSelection, trackknife::formats::SampleRange,
        std::optional<trackknife::formats::ReplayGainInfo>, std::uint64_t) override {
        return {};
    }
    [[nodiscard]] core::Result<void> clear_gapless_next() override { return {}; }
    [[nodiscard]] core::Result<void> play() override {
        const std::lock_guard guard{mutex_};
        if (state_ != audio::LocalAuditionState::playing) {
            since_ = std::chrono::steady_clock::now();
            state_ = audio::LocalAuditionState::playing;
        }
        return {};
    }
    [[nodiscard]] core::Result<void> pause() override {
        const std::lock_guard guard{mutex_};
        base_ms_ = position_locked();
        state_ = audio::LocalAuditionState::paused;
        return {};
    }
    [[nodiscard]] core::Result<void> stop() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::empty;
        loaded_.clear();
        base_ms_ = 0;
        return {};
    }
    [[nodiscard]] core::Result<void> seek_to_seconds(const double seconds) override {
        const std::lock_guard guard{mutex_};
        base_ms_ = static_cast<std::int64_t>(seconds * 1000.0);
        since_ = std::chrono::steady_clock::now();
        ++seeks_;
        return {};
    }
    [[nodiscard]] core::Result<void> set_volume_percent(int) override { return {}; }
    [[nodiscard]] core::Result<void> set_replay_gain_mode(audio::ReplayGainMode) override {
        return {};
    }
    [[nodiscard]] core::Result<void> set_replay_gain_preamps(audio::ReplayGainPreamps) override {
        return {};
    }
    [[nodiscard]] core::Result<void>
    set_buffer_config(audio::PlaybackBufferDurationConfig) override {
        return {};
    }
    [[nodiscard]] core::Result<void> refresh_output_devices() override { return {}; }
    [[nodiscard]] core::Result<void> set_output_target(std::optional<std::string>) override {
        return {};
    }

    [[nodiscard]] std::string loaded() const {
        const std::lock_guard guard{mutex_};
        return loaded_;
    }

  private:
    [[nodiscard]] std::int64_t position_locked() const {
        if (state_ != audio::LocalAuditionState::playing) {
            return base_ms_;
        }
        return base_ms_ + std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - since_)
                              .count();
    }

    mutable std::mutex mutex_;
    audio::LocalAuditionState state_{audio::LocalAuditionState::empty};
    std::string loaded_;
    std::int64_t base_ms_{0};
    std::chrono::steady_clock::time_point since_;
    int seeks_{0};
};

// The real melody-together, on a port of its own, for as long as the test.
class TogetherServer final {
  public:
    TogetherServer(const std::filesystem::path& binary, const std::filesystem::path& directory) {
        // A free port: one the system gives, let go of for the server to take.
        const int probe = core::socket_cloexec(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "a free port");
        socklen_t length = sizeof(address);
        ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        ::close(probe);
        const auto password = directory / "together-password";
        std::ofstream{password} << "friends\n";
        const auto listen = "127.0.0.1:" + std::to_string(port_);
        child_ = ::fork();
        require(child_ >= 0, "the server starts");
        if (child_ == 0) {
            // Its rooms kept in the test's folder, never the real home's.
            const auto data = (directory / "server-data").string();
            ::execl(binary.c_str(), binary.c_str(), "-listen", listen.c_str(), "-password-file",
                    password.c_str(), "-data", data.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        // Listening once a connection is taken.
        require(eventually(
                    [this] {
                        const int attempt = core::socket_cloexec(AF_INET, SOCK_STREAM, 0);
                        sockaddr_in to{};
                        to.sin_family = AF_INET;
                        to.sin_port = htons(port_);
                        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                        const bool connected =
                            ::connect(attempt, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0;
                        ::close(attempt);
                        return connected;
                    },
                    10s),
                "the server listens");
    }
    TogetherServer(const TogetherServer&) = delete;
    TogetherServer& operator=(const TogetherServer&) = delete;
    ~TogetherServer() {
        ::kill(child_, SIGTERM);
        int status = 0;
        ::waitpid(child_, &status, 0);
    }
    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  private:
    pid_t child_{-1};
    std::uint16_t port_{0};
};

void materialize(const std::filesystem::path& fixtures, const char* name,
                 const std::filesystem::path& into) {
    std::ifstream input{fixtures / name};
    require(input.good(), "the fixture opens");
    std::string base64((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::erase(base64, '\n');
    const auto decoded = protocol::decode_raw_path(base64);
    require(decoded.has_value(), "the fixture decodes");
    std::filesystem::create_directories(into.parent_path());
    std::ofstream output{into, std::ios::binary};
    output.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
}

// An engine, as far as listening together needs one: a library, a player
// on a clock-driven output, and the session.
struct Engine final {
    engine::LocalCatalogue catalogue;
    std::unique_ptr<engine::Player> player;
    ClockAudition audition;
    std::unique_ptr<engine::Together> together;

    explicit Engine(const std::filesystem::path& directory)
        : catalogue{directory / "library.sqlite"}, player{engine::Player::create_without_audio()} {
        require(catalogue.prepare().has_value(), "the catalogue opens");
        require(catalogue.add_root((directory / "music").string()).has_value(),
                "its music folder is added");
        trackknife::persistence::LibraryScanProgress progress;
        require(catalogue.scan({}, progress).has_value(), "and scanned");
        require(player->set_output(&audition).has_value(), "the player plays on the test's output");
        together = std::make_unique<engine::Together>(*player, catalogue, nullptr,
                                                      directory / "together");
    }
    [[nodiscard]] std::int64_t position() const { return player->state().position_ms; }
};

// A second of silence as WAV, untagged: a track no other library has.
void write_wave(const std::filesystem::path& path) {
    constexpr std::uint32_t rate = 44'100;
    constexpr std::uint32_t frames = rate * 2U;
    constexpr std::uint32_t data = frames * 4U;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    const auto u32 = [&out](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.put(static_cast<char>((value >> shift) & 0xFFU));
        }
    };
    const auto u16 = [&out](std::uint16_t value) {
        out.put(static_cast<char>(value & 0xFFU));
        out.put(static_cast<char>(value >> 8U));
    };
    out << "RIFF";
    u32(36U + data);
    out << "WAVEfmt ";
    u32(16U);
    u16(1U);
    u16(2U);
    u32(rate);
    u32(rate * 4U);
    u16(4U);
    u16(16U);
    out << "data";
    u32(data);
    const std::string silence(data, '\0');
    out.write(silence.data(), static_cast<std::streamsize>(silence.size()));
}

void invites_and_queries() {
    const auto invite = engine::Together::parse_invite(
        "https://together.example.org/rooms/abc123#key456\n");
    require(invite && invite->server == "https://together.example.org" && invite->room == "abc123" &&
                invite->key == "key456",
            "an invite is the server, the room and the key");
    require(engine::Together::invite_text(*invite) ==
                "https://together.example.org/rooms/abc123#key456",
            "and is written back the same");
    require(!engine::Together::parse_invite("together.example.org/rooms/abc#k"),
            "an invite without a scheme is none");
    require(!engine::Together::parse_invite("https://x/rooms/abc"), "nor one without a key");
    const auto queries = engine::Together::track_queries(protocol::Json{
        {"musicbrainztrackid", "id-1"}, {"title", "Say \"Hi\""}, {"artist", "A"}, {"album", "B"}});
    require(queries.size() == 3U, "by id, by name and album, by name");
    require(queries[0] == R"(musicbrainztrackid IS "id-1")", "the id first");
    require(queries[1] ==
                R"(title IS "Say ""Hi""" AND (artist IS "A" OR albumartist IS "A") AND album IS "B")",
            "then what it is called, quotes doubled");
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 3, "usage: engine_together_test <fixture-dir> <melody-together>");
    invites_and_queries();
    const std::filesystem::path fixtures{argv[1]};
    const auto directory = std::filesystem::temp_directory_path() /
                           ("melody-together-" + core::StableId::random().to_string());
    // Both have the album; only the host has the other track.
    materialize(fixtures, "rich-metadata-flac.b64", directory / "host/music/Album/01.flac");
    materialize(fixtures, "tagged-tone-flac.b64", directory / "host/music/Other/01.flac");
    materialize(fixtures, "rich-metadata-flac.b64", directory / "friend/music/Copy/track.flac");
    write_wave(directory / "friend/music/Mine/only-hers.wav");
    write_wave(directory / "friend/music/Mine/also-hers.wav");
    TogetherServer server{argv[2], directory};
    Engine host{directory / "host"};
    Engine friend_{directory / "friend"};

    require(!host.together->host(server.url(), "wrong").has_value(),
            "a wrong server password makes no room");
    const auto invite = host.together->host(server.url(), "friends", {}, "friday-doom", "letmein");
    require(invite.has_value(), "the host makes a room");
    require(*invite == server.url() + "/rooms/friday-doom#letmein",
            "named as asked, joined with the password chosen");
    require(host.together->status().role == "host", "and is its host");
    require(!friend_.together->join(*invite + "x", "Ana").has_value(),
            "a wrong invite opens nothing");
    require(friend_.together->join(*invite, "Ana").has_value(), "the friend joins");

    // The host plays its copy, a minute in.
    engine::QueueEntry album;
    album.source.raw_path = (directory / "host/music/Album/01.flac").string();
    album.duration_ms = 600'000;
    album.title = "Metadata Fixture";
    host.player->replace_queue({album});
    require(host.player->play_entry(album.entry_id).has_value(), "the host plays");
    require(host.player->seek_ms(60'000).has_value(), "a minute in");

    // The friend plays her own copy -- found by its MusicBrainz track id --
    // where the host is.
    const auto friend_copy = (directory / "friend/music/Copy/track.flac").string();
    require(eventually([&] { return friend_.audition.loaded() == friend_copy; }),
            "the friend plays her own copy");
    require(eventually([&] { return std::abs(friend_.position() - host.position()) < 400; }),
            "where the host is");
    require(friend_.together->status().track.find("Metadata Fixture") != std::string::npos,
            "and knows what plays");
    require(eventually([&] { return host.together->status().listeners ==
                                    std::vector<std::string>{"Ana"}; }),
            "the host sees her listening");

    // A seek, followed.
    require(host.player->seek_ms(300'000).has_value(), "the host seeks");
    require(eventually([&] { return std::abs(friend_.position() - host.position()) < 400; }),
            "and the friend with it");
    // A drift, put right: the friend's speaker falls behind.
    require(friend_.player->seek_ms(friend_.position() - 3'000).has_value(), "she falls behind");
    require(eventually([&] { return std::abs(friend_.position() - host.position()) < 400; }),
            "and is brought back");

    // A pause, and on again.
    require(host.player->pause().has_value(), "the host pauses");
    require(eventually([&] { return friend_.player->state().status == "paused"; }),
            "the friend pauses");
    require(host.player->resume().has_value(), "the host plays on");
    require(eventually([&] {
                return friend_.player->state().status == "playing" &&
                       std::abs(friend_.position() - host.position()) < 400;
            }),
            "and the friend with it, together");

    // A track only the host has: its copy, sent once by the host's engine and
    // fetched by the friend's, played where the host is.
    engine::QueueEntry other;
    other.source.raw_path = (directory / "host/music/Other/01.flac").string();
    other.duration_ms = 600'000;
    host.player->replace_queue({other});
    require(host.player->play_entry(other.entry_id).has_value(), "the host plays another");
    const auto downloads = (directory / "friend/together/downloads").string();
    require(eventually([&] { return friend_.audition.loaded().starts_with(downloads); }, 30s),
            "the friend plays the host's copy");
    require(eventually([&] { return std::abs(friend_.position() - host.position()) < 400; }),
            "where the host is");
    require(friend_.together->status().message.empty(), "and nothing is wrong");

    // An engine restarting -- an update -- comes back into its room: the
    // friend's, in step again; the host's, with the room still its own.
    friend_.together->suspend();
    require(friend_.together->status().role.empty(), "the friend's engine stops");
    friend_.together->resume();
    require(friend_.together->status().role == "listener", "and is back in the room");
    require(eventually([&] {
                return friend_.player->state().status == "playing" &&
                       std::abs(friend_.position() - host.position()) < 400;
            }),
            "in step again");
    host.together->suspend();
    host.together->resume();
    require(host.together->status().role == "host", "the host's engine comes back as host");
    require(host.player->seek_ms(120'000).has_value(), "and goes on");
    require(eventually([&] { return std::abs(friend_.position() - host.position()) < 400; }),
            "the friend still following");

    // Phase 4, a party: the friend suggests two tracks only she has, and
    // votes for the second. The room's turn is decided as the host's own
    // track ends, so the votes cast while it played count: the second plays,
    // the host from her copy, she from her file.
    require(host.together->party(true).has_value(), "the host makes it a party");
    require(!friend_.together->party(true).has_value(), "a listener does not");
    require(eventually([&] { return friend_.together->status().party; }), "she sees the party");
    const auto first = (directory / "friend/music/Mine/only-hers.wav").string();
    const auto second = (directory / "friend/music/Mine/also-hers.wav").string();
    require(friend_.together->suggest(first).has_value(), "she suggests a track of hers");
    const auto wanted = friend_.together->suggest(second);
    require(wanted.has_value(), "and another");
    require(friend_.together->vote(*wanted).has_value(), "and votes for the second");
    require(eventually(
                [&] {
                    const auto room = host.together->status();
                    return room.suggestions.size() == 2U && room.suggestions.front().ready &&
                           room.suggestions.front().id == *wanted && room.suggestions.back().ready;
                },
                30s),
            "the host sees both, the second leading, their copies come");
    engine::QueueEntry own;
    own.source.raw_path = album.source.raw_path;
    own.duration_ms = 600'000;
    host.player->replace_queue({own});
    require(host.player->play_entry(own.entry_id).has_value(), "the host's own track begins");
    std::this_thread::sleep_for(4s);
    require(host.player->request_entries().empty() && host.together->status().suggestions.size() == 2U,
            "nothing taken while it plays: the votes still count");
    require(host.player->seek_ms(600'000 - 15'000).has_value(), "the host's track nears its end");
    require(eventually([&] { return !host.player->request_entries().empty(); }, 30s),
            "the room's turn is next");
    require(eventually([&] {
                const auto left = host.together->status().suggestions;
                return left.size() == 1U && left.front().id != *wanted;
            }),
            "the most wanted taken, the other waiting");
    require(host.player->step(1).has_value(), "the host's track ends");
    const auto host_downloads = (directory / "host/together/downloads").string();
    require(eventually([&] { return host.audition.loaded().starts_with(host_downloads); }),
            "the host plays her copy");
    require(eventually([&] { return friend_.audition.loaded() == second; }),
            "and she her own file of it, in the room");
    require(eventually([&] {
                const auto room = friend_.together->status();
                return room.by == "Ana" && !room.history.empty();
            }),
            "the room says whose it is, and what played before");
    // The host is one of the guests: its pick waits for votes like hers.
    require(host.together->suggest((directory / "host/music/Other/01.flac").string()).has_value(),
            "the host suggests one of its own");
    require(eventually([&] {
                // Hers, two seconds long, are each taken as the last ends.
                const auto room = friend_.together->status().suggestions;
                return std::ranges::any_of(room,
                                           [](const auto& each) { return each.name == "the host"; });
            }),
            "and she sees it among hers");
    // As a suggestion ends -- anyone's, not only the host's own tracks -- the
    // next most wanted follows: hers, two seconds long, near their end as they
    // begin.
    require(eventually([&] { return !host.player->request_entries().empty(); }, 30s),
            "the next is chosen as the suggestion playing ends");

    // Leaving, and the room closing.
    friend_.together->leave();
    require(friend_.together->status().role.empty(), "she has left");
    require(eventually([&] { return host.together->status().listeners.empty(); }),
            "and the host sees it");
    require(friend_.together->join(*invite, "Bo").has_value(), "a friend comes back");
    host.together->leave();
    require(eventually([&] {
                return friend_.together->status().message == "the host closed the room";
            }),
            "and is told when the host closes the room");
    friend_.together->leave();

    std::filesystem::remove_all(directory);
    std::cout << "engine together: ok\n";
    return EXIT_SUCCESS;
}
