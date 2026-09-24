// SPDX-License-Identifier: GPL-3.0-only

// ADR-0228: an engine playing on an output agent, both real and in one
// process -- the engine's player, its outputs and a socket on one side; the
// agent with this machine's audio on the other. The agent has its own copy of
// the music under its own root, so every path it plays went through the
// mapping.

#include "agent/agent.hpp"
#include "agent/speaker_arbiter.hpp"
#include "trackknife/engine/outputs.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/stream_server.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"

#include <taglib/flacfile.h>
#include <taglib/tpropertymap.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

namespace engine = trackknife::engine;
namespace core = trackknife::core;
namespace audio = trackknife::audio;
namespace protocol = trackknife::protocol;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] bool eventually(const std::function<bool()>& condition,
                              const std::chrono::seconds patience = std::chrono::seconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    return condition();
}

// Silence long enough to pause, move and seek in; the fixtures run for a
// second at most.
[[nodiscard]] bool write_silence(const std::filesystem::path& destination, const int seconds) {
    constexpr std::uint32_t rate = 44'100;
    const std::uint32_t data_bytes = rate * 2U * 2U * static_cast<std::uint32_t>(seconds);
    std::ofstream output{destination, std::ios::binary};
    const auto u32 = [&output](const std::uint32_t value) {
        output.put(static_cast<char>(value & 0xFFU));
        output.put(static_cast<char>((value >> 8U) & 0xFFU));
        output.put(static_cast<char>((value >> 16U) & 0xFFU));
        output.put(static_cast<char>((value >> 24U) & 0xFFU));
    };
    const auto u16 = [&output](const std::uint16_t value) {
        output.put(static_cast<char>(value & 0xFFU));
        output.put(static_cast<char>((value >> 8U) & 0xFFU));
    };
    output.write("RIFF", 4);
    u32(36U + data_bytes);
    output.write("WAVEfmt ", 8);
    u32(16U);
    u16(1U);
    u16(2U);
    u32(rate);
    u32(rate * 4U);
    u16(4U);
    u16(16U);
    output.write("data", 4);
    u32(data_bytes);
    const std::string zeros(data_bytes, '\0');
    output.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    return output.good();
}

[[nodiscard]] engine::QueueEntry entry(const std::filesystem::path& path) {
    engine::QueueEntry made;
    made.source.raw_path = path.string();
    // Gain as a client sends it with the queue, so what an agent applies can
    // be measured.
    made.replay_gain = trackknife::formats::ReplayGainInfo{
        .track_gain_db = -6.0, .track_peak = 0.5, .album_gain_db = -8.0, .album_peak = 0.5};
    return made;
}

// One HTTP exchange with the stream server, as raw as a decoder's: the
// response's head and body together.
[[nodiscard]] std::string fetch(const std::uint16_t port, const std::string& request) {
    const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(descriptor);
        return {};
    }
    static_cast<void>(::send(descriptor, request.data(), request.size(), MSG_NOSIGNAL));
    std::string response;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto received = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    ::close(descriptor);
    return response;
}

[[nodiscard]] std::string percent_encoded(const std::string& text) {
    std::string encoded;
    for (const auto character : text) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            encoded.push_back(character);
        } else {
            static constexpr char digits[] = "0123456789ABCDEF";
            const auto byte = static_cast<unsigned char>(character);
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4U]);
            encoded.push_back(digits[byte & 0x0FU]);
        }
    }
    return encoded;
}

[[nodiscard]] std::string stream_request(const std::filesystem::path& path,
                                         const std::string& token, const std::string& range = {}) {
    return "GET /stream?path=" + percent_encoded(protocol::encode_raw_path(path.string())) +
           "&token=" + percent_encoded(token) + " HTTP/1.1\r\nHost: engine\r\n" +
           (range.empty() ? std::string{} : "Range: " + range + "\r\n") + "\r\n";
}

// A fixture, decoded from its base64 text.
[[nodiscard]] bool write_fixture(const std::string& name, const std::filesystem::path& destination) {
    std::ifstream input{std::filesystem::path{TRACKKNIFE_AUDIO_FIXTURE_DIR} / name};
    const std::string encoded{std::istreambuf_iterator<char>{input}, {}};
    static constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    std::string decoded;
    std::uint32_t buffer = 0U;
    int bits = 0;
    for (const auto character : encoded) {
        const auto value = alphabet.find(character);
        if (value == std::string_view::npos) {
            continue;
        }
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    std::ofstream output{destination, std::ios::binary};
    output.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
    return !decoded.empty() && output.good();
}

// Over TCP with the engine's token, as an agent on another machine connects.
[[nodiscard]] std::unique_ptr<trackknife::agent::Agent>
start_agent(const std::uint16_t port, const std::optional<std::filesystem::path>& root,
            const std::string& token = "agent-test-token", const std::string& name = "bedside") {
    auto agent = trackknife::agent::Agent::create(trackknife::agent::AgentConfig{
        .server =
            protocol::Endpoint{.socket = {}, .host = "127.0.0.1", .port = port, .token = token},
        .name = name,
        .music_root = root,
        .stream_only = !root.has_value()});
    if (!agent) {
        return nullptr;
    }
    (*agent)->start();
    return std::move(*agent);
}

} // namespace

int main(int argc, char** argv) {
    require(argc >= 1, "no arguments are needed");
    static_cast<void>(argv);
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-output-agent-" + core::StableId::random().to_string());
    // The engine's music and the agent's copy: the same files, different roots.
    const auto engine_root = directory / "engine-music";
    const auto agent_root = directory / "agent-music";
    std::filesystem::create_directories(engine_root);
    std::filesystem::create_directories(agent_root);
    for (const auto& root : {engine_root, agent_root}) {
        require(write_silence(root / "one.wav", 30) && write_silence(root / "two.wav", 2),
                "the test audio must be written");
    }

    auto workspace = engine::Workspace::open(directory / "lists.sqlite");
    require(workspace.has_value(), "the workspace must open");
    auto player = engine::Player::create_without_audio();
    protocol::Dispatcher dispatcher;
    engine::register_playback_methods(dispatcher, *player);
    auto server = engine::Server::listen_tcp("127.0.0.1", 0, dispatcher, "agent-test-token");
    require(server.has_value(), "the engine must listen");
    const auto port = (*server)->port();
    // Streams for an agent with no copy: whatever the player holds, nothing
    // else.
    const std::string stream_token{"stream-test-token"};
    auto streams = engine::StreamServer::listen(
        "127.0.0.1", 0, stream_token,
        [&player](const std::string& raw_path) { return player->holds(raw_path); });
    require(streams.has_value(), "the engine must serve streams");
    (*streams)->start();
    const auto stream_port = (*streams)->port();
    engine::Outputs outputs{*player,
                            trackknife::output::AgentPaths{.music_root = engine_root,
                                                           .stream_port = stream_port,
                                                           .stream_host = {},
                                                           .stream_token = stream_token},
                            &*workspace, (*server)->sink()};
    engine::register_output_methods(dispatcher, outputs);
    (*server)->on_agent([&outputs](const protocol::Json& params, const int descriptor) {
        outputs.admit(params, descriptor);
    });
    (*server)->start();

    auto agent = start_agent(port, agent_root);
    if (!agent) {
        std::cerr << "output agent: no audio output here; skipping\n";
        (*server)->stop();
        std::filesystem::remove_all(directory);
        return EXIT_SUCCESS;
    }
    // A wrong token is refused before the agent can register.
    {
        auto impostor = start_agent(port, agent_root, "not-the-token");
        require(impostor != nullptr, "a second agent starts");
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        require(!impostor->registered(), "an agent with the wrong token is refused");
        impostor->stop();
    }
    require(eventually([&] { return agent->registered(); }), "the agent registers");
    require(eventually([&] {
                const auto listed = outputs.list();
                return listed.size() == 1U && listed.front().id == "agent:bedside" &&
                       listed.front().online && listed.front().files;
            }),
            "and is listed, online, with files of its own");
    // An engine with no audio of its own plays on the first agent there is.
    require(eventually([&] { return outputs.list().front().selected; }),
            "the first agent is taken up when there is nothing else to play on");

    // Played on the agent, from the agent's own copy.
    require(outputs.select("agent:bedside").has_value(), "the agent can be chosen");
    const std::vector<engine::QueueEntry> entries{entry(engine_root / "one.wav"),
                                                  entry(engine_root / "two.wav")};
    player->replace_queue(entries);
    require(player->play_entry(entries[0].entry_id).has_value(), "playing on the agent starts");
    require(eventually([&] {
                return agent->audition().snapshot().raw_path == (agent_root / "one.wav").string();
            }),
            "the agent opens the file under its own root");
    require(eventually([&] { return player->state().status == "playing"; }),
            "and the engine reports it playing");
    require(player->state().entry == entries[0].entry_id, "the entry it names");

    require(player->pause().has_value(), "pausing reaches the agent");
    require(eventually([&] {
                return agent->audition().snapshot().state == audio::LocalAuditionState::paused;
            }),
            "the agent pauses");
    // Reported back: the engine knows it is paused, which is what the agent
    // coming back must honour.
    require(eventually([&] { return player->state().status == "paused"; }),
            "and the engine hears that it did");
    const auto paused_at = player->state().position_ms;

    // What the engine was asked holds for whichever agent plays: gain set now
    // must reach an agent that starts afresh, which begins with it off.
    require(player->set_replay_gain_mode(audio::ReplayGainMode::album).has_value(),
            "album gain is set");
    require(player->set_replay_gain_preamps(audio::ReplayGainPreamps{.with_gain_db = 3.0F,
                                                                     .without_gain_db = -2.0F})
                .has_value(),
            "and a preamp");
    // Album gain -8 dB with a +3 dB preamp: the track plays at -5 dB.
    const auto expected = static_cast<float>(std::pow(10.0, -5.0 / 20.0));
    const auto applies = [&expected](trackknife::agent::Agent& playing) {
        const auto snapshot = playing.audition().snapshot();
        return snapshot.replay_gain_mode == audio::ReplayGainMode::album &&
               std::abs(snapshot.effective_replay_gain_multiplier - expected) < 0.01F;
    };
    require(eventually([&] { return applies(*agent); }), "the agent plays at album gain");

    // The agent goes away -- a reboot -- and comes back under its name. The
    // music is still there, where it was, still paused.
    agent->stop();
    agent.reset();
    require(eventually([&] { return !outputs.list().front().online; }),
            "a gone agent is listed offline");
    require(player->state().status != "playing", "and nothing is taken for playing");
    auto returned = start_agent(port, agent_root);
    require(returned != nullptr && eventually([&] { return returned->registered(); }),
            "the agent comes back");
    require(eventually([&] {
                const auto snapshot = returned->audition().snapshot();
                return snapshot.raw_path == (agent_root / "one.wav").string() &&
                       snapshot.state == audio::LocalAuditionState::paused;
            }),
            "and takes up the same track, paused");
    require(eventually([&] { return player->state().position_ms >= paused_at - 50; }),
            "at the same place");
    require(eventually([&] {
                const auto snapshot = returned->audition().snapshot();
                return snapshot.replay_gain_mode == audio::ReplayGainMode::album &&
                       snapshot.replay_gain_preamps.with_gain_db == 3.0F &&
                       snapshot.replay_gain_preamps.without_gain_db == -2.0F;
            }),
            "with the gain it was set to, not a new agent's off");
    require(eventually([&] { return applies(*returned); }),
            "and the track it takes up plays at that gain, not louder");

    // The engine's player decides what comes next, and the agent plays it.
    require(player->seek_ms(29'000).has_value(), "seeking reaches the agent");
    require(player->resume().has_value(), "resuming plays on the agent");
    require(eventually(
                [&] {
                    static_cast<void>(player->advance_if_ended());
                    return player->state().entry == entries[1].entry_id;
                },
                std::chrono::seconds{20}),
            "the player moves to the next entry when the agent finishes one");
    require(eventually([&] {
                return returned->audition().snapshot().raw_path ==
                       (agent_root / "two.wav").string();
            }),
            "which the agent plays from its own copy");

    // A file the agent has no copy of is streamed to it instead.
    require(write_silence(engine_root / "three.wav", 5), "the test audio must be written");
    const std::vector<engine::QueueEntry> missing{entry(engine_root / "three.wav")};
    player->replace_queue(missing);
    require(player->play_entry(missing[0].entry_id).has_value(),
            "a file the agent lacks still plays");
    require(eventually([&] {
                const auto snapshot = returned->audition().snapshot();
                return snapshot.raw_path.starts_with("http://127.0.0.1:" +
                                                     std::to_string(stream_port) + "/stream?") &&
                       snapshot.state == audio::LocalAuditionState::playing;
            }),
            "streamed from the engine");
    static_cast<void>(player->stop());
    player->replace_queue(entries);
    returned->stop();

    // The stream server hands out what the player holds, and only that.
    const auto held = engine_root / "one.wav";
    const auto whole = fetch(stream_port, stream_request(held, stream_token));
    require(whole.starts_with("HTTP/1.1 200 OK\r\n") &&
                whole.find("Accept-Ranges: bytes") != std::string::npos &&
                whole.ends_with(std::string(64, '\0')),
            "a held file is served whole");
    const auto part = fetch(stream_port, stream_request(held, stream_token, "bytes=0-3"));
    require(part.starts_with("HTTP/1.1 206 Partial Content\r\n") &&
                part.find("Content-Range: bytes 0-3/") != std::string::npos &&
                part.ends_with("\r\n\r\nRIFF"),
            "and in part, for a decoder that seeks");
    require(fetch(stream_port, stream_request(held, "a guess"))
                .starts_with("HTTP/1.1 403 Forbidden\r\n"),
            "a wrong token is refused");
    const auto elsewhere = directory / "lists.sqlite";
    require(fetch(stream_port, stream_request(elsewhere, stream_token))
                .starts_with("HTTP/1.1 404 Not Found\r\n"),
            "a file the player does not hold is not there, token or not");
    require(fetch(stream_port, stream_request(held, stream_token, "bytes=999999999-"))
                .starts_with("HTTP/1.1 416 "),
            "a range past the end is refused");

    // An agent with no music of its own streams from the engine, seeks in
    // the stream and moves on to the next entry as one with files does.
    auto kitchen = start_agent(port, std::nullopt, "agent-test-token", "kitchen");
    require(kitchen != nullptr && eventually([&] { return kitchen->registered(); }),
            "a streaming agent registers");
    require(eventually([&] {
                for (const auto& listed : outputs.list()) {
                    if (listed.id == "agent:kitchen") {
                        return listed.online && !listed.files;
                    }
                }
                return false;
            }),
            "and is listed as one that streams");
    require(outputs.select("agent:kitchen").has_value(), "the streaming agent can be chosen");
    player->replace_queue(entries);
    require(player->play_entry(entries[0].entry_id).has_value(), "playing on it starts");
    const auto stream_prefix = "http://127.0.0.1:" + std::to_string(stream_port) + "/stream?";
    require(eventually([&] {
                const auto snapshot = kitchen->audition().snapshot();
                return snapshot.raw_path.starts_with(stream_prefix) &&
                       snapshot.state == audio::LocalAuditionState::playing;
            }),
            "the agent plays the engine's stream");
    require(eventually([&] { return player->state().status == "playing"; }),
            "and the engine reports it playing");
    require(player->seek_ms(29'000).has_value(), "seeking in a stream reaches the agent");
    require(eventually([&] { return player->state().position_ms >= 28'900; }),
            "and the stream is read from there");
    require(eventually(
                [&] {
                    static_cast<void>(player->advance_if_ended());
                    return player->state().entry == entries[1].entry_id;
                },
                std::chrono::seconds{20}),
            "a streamed track ends and the next follows");
    require(eventually([&] {
                const auto snapshot = kitchen->audition().snapshot();
                return snapshot.raw_path.find(percent_encoded(protocol::encode_raw_path(
                           (engine_root / "two.wav").string()))) != std::string::npos;
            }),
            "streamed as well");

    // A file's own gain tags, no gain from the client: taken up partway
    // through by a restarted agent, the stream starts past the tags, so the
    // engine sends the gain it reads from its copy.
    const auto tagged = engine_root / "tagged.flac";
    require(write_fixture("rich-metadata-long-flac.b64", tagged), "the tagged file is written");
    {
        TagLib::FLAC::File file{tagged.c_str()};
        auto properties = file.properties();
        properties.replace("REPLAYGAIN_TRACK_GAIN", TagLib::String{"-6.00 dB"});
        properties.replace("REPLAYGAIN_TRACK_PEAK", TagLib::String{"0.5"});
        properties.replace("REPLAYGAIN_ALBUM_GAIN", TagLib::String{"-8.00 dB"});
        properties.replace("REPLAYGAIN_ALBUM_PEAK", TagLib::String{"0.5"});
        file.setProperties(properties);
        require(file.save(), "and tagged with ReplayGain");
    }
    auto untold = entry(tagged);
    untold.replay_gain.reset();
    player->replace_queue({untold});
    require(player->play_entry(untold.entry_id).has_value(), "the tagged file plays");
    require(eventually([&] { return kitchen->audition().snapshot().format.has_value(); }),
            "on the streaming agent");
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    require(player->pause().has_value(), "and is paused partway through");
    require(eventually([&] { return player->state().status == "paused"; }), "paused");
    kitchen->stop();
    kitchen.reset();
    require(eventually([&] {
                for (const auto& listed : outputs.list()) {
                    if (listed.id == "agent:kitchen") {
                        return !listed.online;
                    }
                }
                return false;
            }),
            "the streaming agent goes away");
    kitchen = start_agent(port, std::nullopt, "agent-test-token", "kitchen");
    require(kitchen != nullptr && eventually([&] { return kitchen->registered(); }),
            "and comes back");
    require(eventually([&] {
                const auto snapshot = kitchen->audition().snapshot();
                return snapshot.format.has_value() && applies(*kitchen);
            }),
            "taking the track up at its tagged gain, not at full level");

    kitchen->stop();
    static_cast<void>(player->stop());

    // An engine with speakers of its own and this engine playing on them
    // through its built-in agent: the newest to start playing has them.
    if (auto host = engine::Player::create(); host) {
        (*host)->replace_queue({entry(engine_root / "one.wav")});
        require((*host)->play_entry((*host)->queue().front().entry_id).has_value(),
                "the host plays its own music");
        require(eventually([&] { return (*host)->state().status == "playing"; }), "playing");
        auto built_in = start_agent(port, agent_root, "agent-test-token", "desk");
        require(built_in != nullptr && eventually([&] { return built_in->registered(); }),
                "its built-in agent registers with the other engine");
        trackknife::agent::SpeakerArbiter arbiter{&**host};
        arbiter.add_guest("the server", *built_in);
        arbiter.start();
        require(outputs.select("agent:desk").has_value(), "the other engine chooses it");
        player->replace_queue(entries);
        require(player->play_entry(entries[0].entry_id).has_value(),
                "and starts playing on those speakers");
        require(eventually([&] { return (*host)->state().status == "paused"; }),
                "the host's own music pauses: the other engine is newest");
        require((*host)->state().speakers_taken_by == "the server", "and it says who took them");
        require((*host)->resume().has_value(), "the host plays again");
        require(eventually([&] {
                    return built_in->audition().snapshot().state ==
                           audio::LocalAuditionState::paused;
                }),
                "now it is newest: the other engine's music pauses here");
        require(eventually([&] { return player->state().status == "paused"; }),
                "and that engine hears so");
        require(eventually([&] { return (*host)->state().speakers_taken_by.empty(); }),
                "the speakers are the host's again");
        arbiter.stop();
        built_in->stop();
        static_cast<void>((*host)->stop());
    }
    static_cast<void>(player->stop());
    (*streams)->stop();
    (*server)->stop();
    std::filesystem::remove_all(directory);
    std::cout << "output agent: 4 scenarios\n";
    return EXIT_SUCCESS;
}
