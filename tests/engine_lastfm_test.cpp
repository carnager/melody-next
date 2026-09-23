// SPDX-License-Identifier: GPL-3.0-only

// The engine's Last.fm: what it plays is scrobbled with no window open, love
// works for any client, and the session outlives a restart -- against a fake
// Last.fm on a local port, with the listening clock driven by the test.

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/lastfm.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/protocol/message.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace engine = trackknife::engine;
namespace core = trackknife::core;
namespace audio = trackknife::audio;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

class RecordingAudition final : public audio::Audition {
  public:
    [[nodiscard]] audio::LocalAuditionSnapshot snapshot() const override {
        const std::lock_guard guard{mutex_};
        audio::LocalAuditionSnapshot current;
        current.state = state_;
        current.raw_path = loaded_;
        // Milliseconds as samples at 1 kHz: exact, and what the player's
        // arithmetic expects.
        current.format = trackknife::formats::PcmFormat{1000, 2, "stereo"};
        current.position_sample = position_ms_;
        current.replay_gain_mode = gain_mode_;
        current.output_target = target_;
        return current;
    }
    [[nodiscard]] core::Result<void>
    load_selected_and_play(std::string raw_path, trackknife::formats::AudioSourceSelection,
                           std::optional<trackknife::formats::ReplayGainInfo>) override {
        const std::lock_guard guard{mutex_};
        loaded_ = std::move(raw_path);
        position_ms_ = 0;
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
        position_ms_ = position_ms;
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
        state_ = audio::LocalAuditionState::playing;
        return {};
    }
    [[nodiscard]] core::Result<void> pause() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::paused;
        return {};
    }
    [[nodiscard]] core::Result<void> stop() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::empty;
        loaded_.clear();
        return {};
    }
    [[nodiscard]] core::Result<void> seek_to_seconds(double) override { return {}; }
    [[nodiscard]] core::Result<void> set_volume_percent(int) override { return {}; }
    [[nodiscard]] core::Result<void>
    set_replay_gain_mode(const audio::ReplayGainMode mode) override {
        const std::lock_guard guard{mutex_};
        gain_mode_ = mode;
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
    [[nodiscard]] core::Result<void>
    set_output_target(std::optional<std::string> target) override {
        const std::lock_guard guard{mutex_};
        target_ = std::move(target);
        return {};
    }

    void advance_to(const std::int64_t position_ms) {
        const std::lock_guard guard{mutex_};
        position_ms_ = position_ms;
    }

  private:
    mutable std::mutex mutex_;
    audio::LocalAuditionState state_{audio::LocalAuditionState::empty};
    std::string loaded_;
    std::int64_t position_ms_{0};
    audio::ReplayGainMode gain_mode_{audio::ReplayGainMode::off};
    std::optional<std::string> target_;
};


[[nodiscard]] std::string url_decoded(const std::string& text) {
    std::string decoded;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '+') {
            decoded.push_back(' ');
        } else if (text[index] == '%' && index + 2U < text.size()) {
            decoded.push_back(static_cast<char>(std::stoi(text.substr(index + 1U, 2U), nullptr, 16)));
            index += 2U;
        } else {
            decoded.push_back(text[index]);
        }
    }
    return decoded;
}

// Last.fm, as far as the engine can tell: form posts in, JSON out.
class FakeLastFm final {
  public:
    FakeLastFm() {
        listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                    ::listen(listener_, 8) == 0,
                "the fake Last.fm listens");
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        worker_ = std::thread{[this] { serve(); }};
    }
    ~FakeLastFm() {
        running_.store(false);
        ::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        worker_.join();
    }
    [[nodiscard]] std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/2.0/";
    }
    void refuse_scrobbles(const bool refuse) { refuse_.store(refuse); }
    [[nodiscard]] std::vector<std::map<std::string, std::string>> requests() {
        const std::lock_guard guard{mutex_};
        return requests_;
    }

  private:
    void serve() {
        while (running_.load()) {
            const auto connection = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
            if (connection < 0) {
                continue;
            }
            std::string request;
            std::array<char, 4096> buffer{};
            std::size_t wanted = std::string::npos;
            while (true) {
                const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
                if (received <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<std::size_t>(received));
                const auto head = request.find("\r\n\r\n");
                if (head != std::string::npos && wanted == std::string::npos) {
                    const auto at = request.find("Content-Length: ");
                    wanted = head + 4U + (at != std::string::npos
                                              ? std::stoul(request.substr(at + 16U))
                                              : 0U);
                }
                if (wanted != std::string::npos && request.size() >= wanted) {
                    break;
                }
            }
            std::map<std::string, std::string> params;
            std::string body = request.substr(request.find("\r\n\r\n") + 4U);
            std::size_t start = 0;
            while (start < body.size()) {
                auto end = body.find('&', start);
                if (end == std::string::npos) {
                    end = body.size();
                }
                const auto pair = body.substr(start, end - start);
                const auto equals = pair.find('=');
                params[url_decoded(pair.substr(0, equals))] = url_decoded(pair.substr(equals + 1U));
                start = end + 1U;
            }
            {
                const std::lock_guard guard{mutex_};
                requests_.push_back(params);
            }
            std::string status = "200 OK";
            std::string answer = "{}";
            if (params["method"] == "track.scrobble") {
                if (refuse_.load()) {
                    status = "503 Service Unavailable";
                } else {
                    answer = R"({"scrobbles":{"@attr":{"accepted":1,"ignored":0}}})";
                }
            }
            const auto response = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n"
                                  "Content-Length: " + std::to_string(answer.size()) +
                                  "\r\nConnection: close\r\n\r\n" + answer;
            static_cast<void>(::send(connection, response.data(), response.size(), MSG_NOSIGNAL));
            ::close(connection);
        }
    }

    int listener_{-1};
    std::uint16_t port_{0};
    std::atomic_bool running_{true};
    std::atomic_bool refuse_{false};
    std::mutex mutex_;
    std::vector<std::map<std::string, std::string>> requests_;
    std::thread worker_;
};

[[nodiscard]] std::size_t count_of(const std::vector<std::map<std::string, std::string>>& requests,
                                   const std::string& method) {
    std::size_t count = 0U;
    for (const auto& request : requests) {
        count += request.count("method") && request.at("method") == method ? 1U : 0U;
    }
    return count;
}

const engine::LastFm::Session session{.api_key = std::string(32U, 'k'),
                                      .secret = std::string(32U, 's'),
                                      .session_key = "session-key",
                                      .user = "listener"};

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-lastfm-" + core::StableId::random().to_string());
    std::filesystem::create_directories(directory);
    const auto state_file = directory / "lastfm.json";
    engine::LocalCatalogue catalogue{directory / "library.sqlite"};
    require(catalogue.prepare().has_value(), "the catalogue opens");
    auto player = engine::Player::create_without_audio();
    RecordingAudition audition;
    require(player->set_output(&audition).has_value(), "the player plays on the test's output");
    FakeLastFm fake;

    // A file outside the library: named by what the client queued it with.
    engine::QueueEntry song;
    song.source.raw_path = "/music/elsewhere/song.flac";
    song.duration_ms = 200'000;
    song.title = "Song";
    song.group.artist = "Artist";
    song.group.album = "Album";
    player->replace_queue({song});
    require(player->play_entry(song.entry_id).has_value(), "the song plays");

    {
        engine::LastFm lastfm{*player, catalogue, state_file, fake.endpoint()};
        require(!lastfm.status().enabled, "nothing is sent before a session is handed over");
        require(lastfm.set_session(session).has_value(), "a session is handed over");
        struct stat status{};
        require(::stat(state_file.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600,
                "and kept where only its owner can read it");

        // Last.fm is down when the listen completes: it waits in the outbox.
        fake.refuse_scrobbles(true);
        const std::int64_t wall = 1'800'000'000;
        for (std::int64_t second = 0; second <= 130; ++second) {
            audition.advance_to(second * 1'000);
            lastfm.tick(second * 1'000, wall + second);
        }
        auto requests = fake.requests();
        require(count_of(requests, "track.updateNowPlaying") == 1U, "now playing is said once");
        require(count_of(requests, "track.scrobble") >= 1U, "the listen is offered");
        require(lastfm.status().pending == 1U, "and kept while Last.fm refuses it");

        // Back: sent, once, as what was playing and when it started.
        fake.refuse_scrobbles(false);
        for (std::int64_t second = 131; second <= 400 && lastfm.status().pending > 0U; ++second) {
            audition.advance_to(std::min<std::int64_t>(second, 199) * 1'000);
            lastfm.tick(second * 1'000, wall + second);
        }
        require(lastfm.status().pending == 0U, "the listen is sent once Last.fm is back");
        requests = fake.requests();
        const auto& scrobble = requests.back();
        require(scrobble.at("method") == "track.scrobble", "as a scrobble");
        require(scrobble.at("artist") == "Artist" && scrobble.at("track") == "Song" &&
                    scrobble.at("album") == "Album",
                "of the song, by what it was queued as");
        require(scrobble.at("timestamp") == std::to_string(wall), "dated when it started");
        require(scrobble.at("sk") == "session-key" && scrobble.at("api_sig").size() == 32U,
                "signed with the session");
        lastfm.stop();
    }

    // The session outlives the engine, and love reaches Last.fm for any
    // client that asks, for the track playing.
    engine::LastFm again{*player, catalogue, state_file, fake.endpoint()};
    require(again.status().user == "listener" && again.status().enabled,
            "a restarted engine keeps its session");
    require(again.love(std::nullopt, std::nullopt, true).has_value(), "the playing track is loved");
    const auto requests = fake.requests();
    require(requests.back().at("method") == "track.love" && requests.back().at("track") == "Song",
            "as itself");
    require(again.clear().has_value() && !again.status().enabled, "and signing out stops it");

    std::filesystem::remove_all(directory);
    std::cout << "engine lastfm: 1 scenario\n";
    return EXIT_SUCCESS;
}
