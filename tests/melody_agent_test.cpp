// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/melody_agent.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/decoder.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <string>
#include <thread>

namespace {

int failures = 0;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n';     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

void write_u16(std::ofstream& output, const std::uint16_t value) {
    output.put(static_cast<char>(value & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
}

void write_u32(std::ofstream& output, const std::uint32_t value) {
    write_u16(output, static_cast<std::uint16_t>(value & 0xFFFFU));
    write_u16(output, static_cast<std::uint16_t>((value >> 16U) & 0xFFFFU));
}

void write_wav(const std::filesystem::path& path) {
    constexpr int rate = 44'100;
    constexpr int frames = rate / 2;
    std::ofstream output{path, std::ios::binary};
    output.write("RIFF", 4);
    write_u32(output, 36U + static_cast<std::uint32_t>(frames * 4));
    output.write("WAVEfmt ", 8);
    write_u32(output, 16U);
    write_u16(output, 1U);
    write_u16(output, 2U);
    write_u32(output, rate);
    write_u32(output, rate * 4U);
    write_u16(output, 4U);
    write_u16(output, 16U);
    output.write("data", 4);
    write_u32(output, static_cast<std::uint32_t>(frames * 4));
    for (int index = 0; index < frames; ++index) {
        const auto sample = static_cast<std::int16_t>(
            std::sin(2.0 * std::numbers::pi * 440.0 * index / rate) * 8'000.0);
        write_u16(output, static_cast<std::uint16_t>(sample));
        write_u16(output, static_cast<std::uint16_t>(sample));
    }
}

[[nodiscard]] std::string read_line(const int socket) {
    std::string line;
    char byte = 0;
    while (::recv(socket, &byte, 1U, 0) == 1) {
        if (byte == '\n') {
            break;
        }
        if (byte != '\r') {
            line.push_back(byte);
        }
    }
    return line;
}

void send_text(const int socket, const std::string& text) {
    std::size_t sent = 0U;
    while (sent < text.size()) {
        const auto count = ::send(socket, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        CHECK(count > 0);
        if (count <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(count);
    }
}

struct FakeMelody {
    int listener{-1};
    unsigned port{0U};
    std::jthread worker;
    std::atomic_bool registered{false};
    std::atomic_bool played{false};
    std::string registration;

    FakeMelody() {
        listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(listener >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(::listen(listener, 4) == 0);
        socklen_t size = sizeof(address);
        CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port = ntohs(address.sin_port);
        worker = std::jthread{[this] {
            const auto control = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (control < 0) {
                return;
            }
            send_text(control, "OK MPD 0.24.0\n");
            registration = read_line(control);
            registered.store(registration.starts_with(
                "agent_register \"" + trackknife::audio::default_melody_agent_name() + "\" v2 "));
            send_text(control, "OK\n");

            const auto sync = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            CHECK(sync >= 0);
            send_text(sync, "OK MPD 0.24.0\n");
            CHECK(read_line(sync) == "status");
            send_text(sync, "playlist: 7\nOK\n");
            CHECK(read_line(sync) == "playlistinfo");
            send_text(sync, "file: Album/tone.wav\nPos: 0\nduration: 0.5\n"
                            "X-SongId: song-42\nX-ReplayGainTrack: -3.0\n"
                            "X-ReplayGainAlbum: -8.0\nOK\n");
            CHECK(read_line(sync) == "status");
            send_text(sync, "playlist: 7\nOK\n");
            ::close(sync);

            send_text(control, "replaygain album\n");
            for (int lines = 0; lines < 10 && read_line(control) != "OK"; ++lines) {
            }
            send_text(control, "volume 37.500000\n");
            for (int lines = 0; lines < 10 && read_line(control) != "OK"; ++lines) {
            }
            send_text(control, "play 0 next=-1 paused=1\n");
            for (int lines = 0; lines < 10; ++lines) {
                const auto response = read_line(control);
                if (response == "OK") {
                    played.store(true);
                    break;
                }
            }
            send_text(control, "ping\n");
            for (int lines = 0; lines < 10 && read_line(control) != "OK"; ++lines) {
            }
            ::close(control);
        }};
    }

    ~FakeMelody() {
        ::shutdown(listener, SHUT_RDWR);
        ::close(listener);
        worker.request_stop();
    }
};

struct HttpAudioServer {
    int listener{-1};
    unsigned port{0U};
    std::jthread worker;

    explicit HttpAudioServer(const std::filesystem::path& source) {
        std::ifstream input{source, std::ios::binary};
        const std::string body{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
        listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(listener >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(::listen(listener, 2) == 0);
        socklen_t size = sizeof(address);
        CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port = ntohs(address.sin_port);
        worker = std::jthread{[this, body] {
            const auto client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                return;
            }
            std::array<char, 4096U> request{};
            static_cast<void>(::recv(client, request.data(), request.size(), 0));
            const auto headers = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            send_text(client, headers);
            send_text(client, body);
            ::close(client);
        }};
    }

    ~HttpAudioServer() {
        ::shutdown(listener, SHUT_RDWR);
        ::close(listener);
        worker.request_stop();
    }
};

void sourceResolutionIsContainedAndStable() {
    const trackknife::audio::MelodyAgentQueueItem item{
        .position = 4,
        .uri = "Artist/Album/01.flac",
        .song_id = "stable-song",
        .duration_seconds = 1.0,
        .track_gain_db = {},
        .album_gain_db = {},
    };
    auto direct = trackknife::audio::resolve_melody_agent_source(
        {.name = "test",
         .host = "server",
         .port = 6600U,
         .local_music_root = "/mnt/music",
         .stream_base_url = {},
         .stream_format = {},
         .maximum_bit_rate = {},
         .reconnect_delay = std::chrono::milliseconds{1},
         .report_period = std::chrono::milliseconds{1}},
        item);
    CHECK(direct && direct->second && direct->first == "/mnt/music/Artist/Album/01.flac");
    auto escaped_item = item;
    escaped_item.uri = "../secret.flac";
    CHECK(!trackknife::audio::resolve_melody_agent_source(
        {.name = "test",
         .host = "server",
         .port = 6600U,
         .local_music_root = "/mnt/music",
         .stream_base_url = {},
         .stream_format = {},
         .maximum_bit_rate = {},
         .reconnect_delay = std::chrono::milliseconds{1},
         .report_period = std::chrono::milliseconds{1}},
        escaped_item));
    auto streamed = trackknife::audio::resolve_melody_agent_source(
        {.name = "test",
         .host = "server",
         .port = 6600U,
         .local_music_root = {},
         .stream_base_url = "https://melody.example",
         .stream_format = {},
         .maximum_bit_rate = {},
         .reconnect_delay = std::chrono::milliseconds{1},
         .report_period = std::chrono::milliseconds{1}},
        item);
    CHECK(streamed && !streamed->second &&
          streamed->first == "https://melody.example/api/v1/stream/stable-song");
}

void sourceResolutionAcceptsTrailingRootSeparators() {
    trackknife::audio::MelodyAgentConfig config;
    trackknife::audio::MelodyAgentQueueItem item;
    item.uri = "Artist/Album/01.flac";
    for (const auto* root : {"/mnt/music", "/mnt/music/", "/mnt/music///", "/mnt/music/./"}) {
        config.local_music_root = root;
        const auto resolved = trackknife::audio::resolve_melody_agent_source(config, item);
        CHECK(resolved && resolved->second && resolved->first == "/mnt/music/Artist/Album/01.flac");
        for (const auto* invalid : {"../secret.flac", "Artist/../../secret.flac", "/secret.flac"}) {
            auto escaped = item;
            escaped.uri = invalid;
            CHECK(!trackknife::audio::resolve_melody_agent_source(config, escaped));
        }
    }
    config.local_music_root = "/";
    const auto resolved = trackknife::audio::resolve_melody_agent_source(config, item);
    CHECK(resolved && resolved->first == "/Artist/Album/01.flac");
}

void v2RegistrationQueueAndPlaybackFixture() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-melody-agent-" + trackknife::core::StableId::random().to_string());
    std::filesystem::create_directories(root / "Album");
    write_wav(root / "Album/tone.wav");
    FakeMelody server;
    auto player = trackknife::audio::LocalAuditionService::create();
    CHECK(player.has_value());
    if (!player) {
        return;
    }
    auto endpoint = trackknife::audio::MelodyAgentService::create(
        {.name = trackknife::audio::default_melody_agent_name(),
         .host = "127.0.0.1",
         .port = server.port,
         .local_music_root = root.native() + "/",
         .stream_base_url = {},
         .stream_format = "flac",
         .maximum_bit_rate = 192'000U,
         .reconnect_delay = std::chrono::milliseconds{20},
         .report_period = std::chrono::milliseconds{20}},
        **player);
    CHECK(endpoint.has_value());
    CHECK((*endpoint)->name() == trackknife::audio::default_melody_agent_name());
    for (int attempt = 0; attempt < 200 && (!server.registered.load() || !server.played.load());
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    CHECK(server.registered.load());
    CHECK(server.played.load());
    for (int attempt = 0; attempt < 200 && !(**player).snapshot().replay_gain_override; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto playback = (**player).snapshot();
    CHECK(playback.replay_gain_mode == trackknife::audio::ReplayGainMode::album);
    CHECK(playback.volume_percent == 38);
    CHECK(playback.replay_gain_override.has_value());
    if (playback.replay_gain_override) {
        CHECK(playback.replay_gain_override->track_gain_db == -3.0);
        CHECK(playback.replay_gain_override->album_gain_db == -8.0);
        CHECK(std::abs(trackknife::audio::replay_gain_multiplier(*playback.replay_gain_override,
                                                                 playback.replay_gain_mode) -
                       std::pow(10.0F, -8.0F / 20.0F)) < 0.0001F);
    }
    if (endpoint) {
        auto snapshot = (*endpoint)->snapshot();
        for (int attempt = 0; attempt < 200 && !snapshot.track_gain_db; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
            snapshot = (*endpoint)->snapshot();
        }
        CHECK(snapshot.queue_version == 7U);
        CHECK(snapshot.current_position == 0);
        CHECK(snapshot.visible_track_identity == "song-42");
        CHECK(snapshot.using_direct_source);
        CHECK(snapshot.replay_gain_mode == trackknife::audio::ReplayGainMode::album);
        CHECK(snapshot.track_gain_db == -3.0);
        CHECK(snapshot.album_gain_db == -8.0);
        CHECK(std::abs(snapshot.effective_gain_multiplier - std::pow(10.0F, -8.0F / 20.0F)) <
              0.0001F);
    }
    if (endpoint) {
        endpoint->reset();
    }
    player->reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void networkStreamBypassesOnlyLocalRevisionChecks() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-network-audio-" + trackknife::core::StableId::random().to_string());
    std::filesystem::create_directories(root);
    const auto source = root / "tone.wav";
    write_wav(source);
    HttpAudioServer server{source};
    auto player = trackknife::audio::LocalAuditionService::create();
    CHECK(player.has_value());
    if (player) {
        CHECK(
            (*player)->set_replay_gain_mode(trackknife::audio::ReplayGainMode::album).has_value());
        const trackknife::formats::ReplayGainInfo gain{
            .track_gain_db = -3.0,
            .track_peak = {},
            .album_gain_db = -8.0,
            .album_peak = {},
        };
        const auto accepted = (*player)->load_network_stream_and_play(
            "http://127.0.0.1:" + std::to_string(server.port) + "/stream/song-42", gain);
        CHECK(accepted.has_value());
        for (int attempt = 0; attempt < 200 && !(*player)->snapshot().format; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        const auto snapshot = (*player)->snapshot();
        CHECK(snapshot.format.has_value());
        CHECK(snapshot.replay_gain_mode == trackknife::audio::ReplayGainMode::album);
        CHECK(snapshot.replay_gain_override == gain);
        CHECK(std::abs(trackknife::audio::replay_gain_multiplier(*snapshot.replay_gain_override,
                                                                 snapshot.replay_gain_mode) -
                       std::pow(10.0F, -8.0F / 20.0F)) < 0.0001F);
        CHECK(!snapshot.error || snapshot.error->message.find(
                                     "local source could not be observed") == std::string::npos);
        player->reset();
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

} // namespace

int main(const int argc, char** argv) {
    if (argc == 3 && std::string_view{argv[1]} == "--decode-url") {
        auto decoder = trackknife::formats::AudioDecoder::open(argv[2]);
        if (!decoder) {
            std::cerr << decoder.error().message << '\n';
            return 1;
        }
        const auto gain = decoder->replay_gain();
        std::cout << "track=" << (gain.track_gain_db ? std::to_string(*gain.track_gain_db) : "none")
                  << " album="
                  << (gain.album_gain_db ? std::to_string(*gain.album_gain_db) : "none") << '\n';
        return gain.track_gain_db || gain.album_gain_db ? 0 : 1;
    }
    if (argc == 3 &&
        (std::string_view{argv[1]} == "--live" || std::string_view{argv[1]} == "--live-playback")) {
        const auto playback_probe = std::string_view{argv[1]} == "--live-playback";
        auto player = trackknife::audio::LocalAuditionService::create();
        CHECK(player.has_value());
        if (!player) {
            return 1;
        }
        auto endpoint = trackknife::audio::MelodyAgentService::create(
            {.name = "Trackknife protocol probe",
             .host = argv[2],
             .port = 6600U,
             .local_music_root = {},
             .stream_base_url = {},
             .stream_format = {},
             .maximum_bit_rate = {},
             .reconnect_delay = std::chrono::milliseconds{100},
             .report_period = std::chrono::milliseconds{100}},
            **player);
        CHECK(endpoint.has_value());
        if (!endpoint) {
            return 1;
        }
        for (int attempt = 0; attempt < (playback_probe ? 750 : 100) &&
                              ((*endpoint)->snapshot().queue_item_count == 0U ||
                               (playback_probe && (*endpoint)->snapshot().current_position < 0));
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        if (playback_probe) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        const auto snapshot = (*endpoint)->snapshot();
        std::cout << "registered=" << snapshot.registered << " queue=" << snapshot.queue_item_count
                  << " gains=" << snapshot.queue_gain_item_count
                  << " pos=" << snapshot.current_position << " received_track="
                  << (snapshot.received_track_gain_db
                          ? std::to_string(*snapshot.received_track_gain_db)
                          : "none")
                  << " player_track="
                  << (snapshot.track_gain_db ? std::to_string(*snapshot.track_gain_db) : "none")
                  << " multiplier=" << snapshot.effective_gain_multiplier
                  << " peak_before=" << (**player).snapshot().decoded_peak_before_gain
                  << " peak_after=" << (**player).snapshot().decoded_peak_after_gain << '\n';
        return snapshot.registered && snapshot.queue_item_count > 0U &&
                       snapshot.queue_gain_item_count > 0U &&
                       (!playback_probe ||
                        (snapshot.current_position >= 0 && snapshot.track_gain_db.has_value()))
                   ? 0
                   : 1;
    }
    std::array<char, 256> hostname{};
    CHECK(::gethostname(hostname.data(), hostname.size()) == 0);
    CHECK(trackknife::audio::MelodyAgentConfig{}.name ==
          "Trackknife @ " + std::string{hostname.data()});
    sourceResolutionIsContainedAndStable();
    sourceResolutionAcceptsTrailingRootSeparators();
    v2RegistrationQueueAndPlaybackFixture();
    networkStreamBypassesOnlyLocalRevisionChecks();
    return failures == 0 ? 0 : 1;
}
