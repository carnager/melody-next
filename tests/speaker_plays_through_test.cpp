// SPDX-License-Identifier: GPL-3.0-only

// ADR-0274: a phone's speaker whose connection drops -- on mobile data the
// phone locking does it -- plays on from its buffer, and on its return says
// what it plays now. The engine takes it at its word: no reload, no winding
// back to where it dropped. A different process is taken up as before.
//
// The speaker here is a stand-in on the protocol, as the phone speaks it:
// no audio, so this runs anywhere.

#include "trackknife/engine/outputs.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/output/audition_wire.hpp"
#include "trackknife/protocol/message.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace engine = trackknife::engine;
namespace core = trackknife::core;
namespace audio = trackknife::audio;
namespace protocol = trackknife::protocol;
namespace output = trackknife::output;
using protocol::Json;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool eventually(const std::function<bool()>& condition,
                const std::chrono::milliseconds limit = std::chrono::milliseconds{5'000}) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return condition();
}

[[nodiscard]] bool write_silence(const std::filesystem::path& destination) {
    constexpr std::uint32_t rate = 8'000;
    constexpr std::uint32_t data_bytes = rate * 2U * 30U;
    std::ofstream file{destination, std::ios::binary};
    const auto u32 = [&file](const std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            file.put(static_cast<char>((value >> shift) & 0xFFU));
        }
    };
    const auto u16 = [&file](const std::uint16_t value) {
        file.put(static_cast<char>(value & 0xFFU));
        file.put(static_cast<char>((value >> 8U) & 0xFFU));
    };
    file.write("RIFF", 4);
    u32(36U + data_bytes);
    file.write("WAVEfmt ", 8);
    u32(16U);
    u16(1U);
    u16(1U);
    u32(rate);
    u32(rate * 2U);
    u16(2U);
    u16(16U);
    file.write("data", 4);
    u32(data_bytes);
    const std::string zeros(data_bytes, '\0');
    file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    return file.good();
}

// A report as the phone sends one: playing or not, at a place, of a stream
// at 1 kHz -- positions in milliseconds, which the engine reads the same.
[[nodiscard]] Json report(const bool playing, const std::int64_t position_ms,
                          const std::uint64_t handovers = 0U) {
    audio::LocalAuditionSnapshot snapshot;
    snapshot.state = playing ? audio::LocalAuditionState::playing : audio::LocalAuditionState::paused;
    snapshot.format = trackknife::formats::PcmFormat{};
    snapshot.format->sample_rate = 1'000;
    snapshot.format->channels = 2;
    snapshot.position_sample = position_ms;
    snapshot.chain_transitions = handovers;
    return output::to_json(snapshot);
}

// The phone's speaker on the protocol: answers whatever the engine asks,
// and remembers what that was.
class Speaker final {
  public:
    Speaker(const std::uint16_t port, const std::string& instance, std::optional<Json> holding) {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::connect(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0,
                "the speaker connects");
        reader_ = std::thread{[this] { read(); }};
        send(Json{{"id", 1}, {"method", "session.authenticate"}, {"params", {{"password", "token"}}}});
        Json registration{{"name", "phone"}, {"instance", instance}, {"files", false},
                          {"protocol", 1}, {"stream", {{"format", "original"}}}};
        if (holding) {
            registration["report"] = *holding;
        }
        send(Json{{"id", 2}, {"method", "agent.register"}, {"params", registration}});
        require(eventually([this] { return answered(2); }), "the speaker is registered");
    }
    Speaker(const Speaker&) = delete;
    Speaker& operator=(const Speaker&) = delete;
    Speaker(Speaker&&) = delete;
    Speaker& operator=(Speaker&&) = delete;

    ~Speaker() { drop(); }

    // Gone without a word, as a connection the network took.
    void drop() {
        if (descriptor_ >= 0) {
            ::shutdown(descriptor_, SHUT_RDWR);
            ::close(descriptor_);
            descriptor_ = -1;
        }
        if (reader_.joinable()) {
            reader_.join();
        }
    }

    void tell(const Json& data) { send(Json{{"event", "audition.changed"}, {"data", data}}); }

    [[nodiscard]] std::vector<std::string> asked() const {
        const std::lock_guard guard{mutex_};
        return asked_;
    }

  private:
    void send(const Json& message) {
        const auto line = message.dump() + "\n";
        const std::lock_guard guard{writing_};
        static_cast<void>(::send(descriptor_, line.data(), line.size(), MSG_NOSIGNAL));
    }

    [[nodiscard]] bool answered(const int id) const {
        const std::lock_guard guard{mutex_};
        return std::ranges::contains(answers_, id);
    }

    void read() {
        std::string pending;
        char buffer[4096];
        while (true) {
            const auto received = ::recv(descriptor_, buffer, sizeof buffer, 0);
            if (received <= 0) {
                return;
            }
            pending.append(buffer, static_cast<std::size_t>(received));
            for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                const auto message = Json::parse(pending.substr(0, end), nullptr, false);
                pending.erase(0, end + 1U);
                if (!message.is_object()) {
                    continue;
                }
                if (const auto method = message.find("method"); method != message.end()) {
                    {
                        const std::lock_guard guard{mutex_};
                        asked_.push_back(method->get<std::string>());
                    }
                    if (message.contains("id")) {
                        send(Json{{"id", message["id"]}, {"result", Json::object()}});
                    }
                } else if (message.contains("id") && message["id"].is_number_integer()) {
                    const std::lock_guard guard{mutex_};
                    answers_.push_back(message["id"].get<int>());
                }
            }
        }
    }

    int descriptor_{-1};
    std::thread reader_;
    std::mutex writing_;
    mutable std::mutex mutex_;
    std::vector<std::string> asked_;
    std::vector<int> answers_;
};

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-speaker-" + core::StableId::random().to_string());
    std::filesystem::create_directories(directory);
    require(write_silence(directory / "one.wav") && write_silence(directory / "two.wav"),
            "the music is written");

    auto workspace = engine::Workspace::open(directory / "lists.sqlite");
    require(workspace.has_value(), "the workspace opens");
    auto player = engine::Player::create_without_audio();
    protocol::Dispatcher dispatcher;
    engine::register_playback_methods(dispatcher, *player);
    auto server = engine::Server::listen_tcp("127.0.0.1", 0, dispatcher, "token");
    require(server.has_value(), "the engine listens");
    engine::Outputs outputs{*player,
                            output::AgentPaths{.music_root = directory,
                                               .stream_port = 1,
                                               .stream_host = {},
                                               .stream_token = "stream"},
                            &*workspace, (*server)->sink()};
    (*server)->on_agent([&outputs](const Json& params, const int descriptor) {
        outputs.admit(params, descriptor);
    });
    (*server)->start();
    const auto port = (*server)->port();

    std::vector<engine::QueueEntry> queue(2);
    queue[0].source.raw_path = (directory / "one.wav").string();
    queue[1].source.raw_path = (directory / "two.wav").string();

    auto speaker = std::make_unique<Speaker>(port, "process-1", std::nullopt);
    require(eventually([&] { return !outputs.list().empty() && outputs.list().front().selected; }),
            "the phone is what the engine plays on");
    player->replace_queue(queue);
    require(player->play_entry(queue[0].entry_id).has_value(), "playing starts");
    require(eventually([&] { return std::ranges::contains(speaker->asked(), "audition.load"); }),
            "the phone is given the track");
    speaker->tell(report(true, 1'000));
    require(eventually([&] { return player->state().status == "playing"; }), "and plays it");

    // Locked, out of doors: the connection goes. The engine sees the phone
    // gone mid-track; the phone plays on.
    speaker->drop();
    require(eventually([&] { return !outputs.list().front().online; }), "the phone is gone");

    // Back, the same process, having played on to 7 s.
    speaker = std::make_unique<Speaker>(port, "process-1", report(true, 7'000));
    require(eventually([&] { return player->state().status == "playing"; }),
            "the engine plays on with it");
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    require(!std::ranges::contains(speaker->asked(), "audition.load"),
            "nothing is loaded again: what it plays is taken as it is");
    require(player->state().position_ms >= 6'950, "at the place it reached, not where it dropped");
    require(player->state().entry == queue[0].entry_id, "the same entry");

    // It played into what was armed while away: the engine moves on with it.
    if (player->state().gapless_entry == queue[1].entry_id) {
        speaker->drop();
        require(eventually([&] { return !outputs.list().front().online; }), "gone again");
        speaker = std::make_unique<Speaker>(port, "process-1", report(true, 500, 1U));
        require(eventually([&] {
                    static_cast<void>(player->advance_if_ended());
                    return player->state().entry == queue[1].entry_id;
                }),
                "the engine follows it to the next track");
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        require(!std::ranges::contains(speaker->asked(), "audition.load"),
                "still nothing loaded again");
    } else {
        std::cerr << "speaker plays through: nothing armed here; the handover is not tried\n";
    }

    // A new process -- the app started again -- holds nothing the engine
    // gave it: taken up, as it always was.
    const auto entry = player->state().entry;
    speaker->drop();
    require(eventually([&] { return !outputs.list().front().online; }), "gone once more");
    speaker = std::make_unique<Speaker>(port, "process-2", report(true, 2'000));
    require(eventually([&] { return std::ranges::contains(speaker->asked(), "audition.load"); }),
            "another process is given the track again");
    require(player->state().entry == entry, "the one that was playing");

    speaker.reset();
    (*server)->stop();
    std::filesystem::remove_all(directory);
    return EXIT_SUCCESS;
}
