// SPDX-License-Identifier: GPL-3.0-only

// ADR-0228: an engine playing on an output agent, both real and in one
// process -- the engine's player, its outputs and a socket on one side; the
// agent with this machine's audio on the other. The agent has its own copy of
// the music under its own root, so every path it plays went through the
// mapping.

#include "agent/agent.hpp"
#include "trackknife/engine/outputs.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
    return made;
}

// Over TCP with the engine's token, as an agent on another machine connects.
[[nodiscard]] std::unique_ptr<trackknife::agent::Agent>
start_agent(const std::uint16_t port, const std::filesystem::path& root,
            const std::string& token = "agent-test-token") {
    auto agent = trackknife::agent::Agent::create(trackknife::agent::AgentConfig{
        .server =
            protocol::Endpoint{.socket = {}, .host = "127.0.0.1", .port = port, .token = token},
        .name = "bedside",
        .music_root = root,
        .stream_only = false});
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
    engine::Outputs outputs{*player,
                            trackknife::output::AgentPaths{
                                .music_root = engine_root, .stream_base = {}, .stream_token = {}},
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

    returned->stop();
    static_cast<void>(player->stop());
    (*server)->stop();
    std::filesystem::remove_all(directory);
    std::cout << "output agent: 1 scenario\n";
    return EXIT_SUCCESS;
}
