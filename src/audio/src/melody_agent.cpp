// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/melody_agent.hpp"

#include "trackknife/core/stable_id.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <ranges>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

namespace trackknife::audio {
namespace {

[[nodiscard]] core::Error agent_error(std::string message) {
    return {.code = core::ErrorCode::backend, .message = std::move(message), .context = {}};
}

class Socket final {
  public:
    Socket() = default;
    explicit Socket(const int descriptor) : descriptor_(descriptor) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : descriptor_(other.descriptor_.exchange(-1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            descriptor_.store(other.descriptor_.exchange(-1));
        }
        return *this;
    }
    ~Socket() { close(); }
    [[nodiscard]] int get() const noexcept { return descriptor_.load(); }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_.load() >= 0; }
    void close() noexcept {
        const auto descriptor = descriptor_.exchange(-1);
        if (descriptor >= 0) {
            ::shutdown(descriptor, SHUT_RDWR);
            ::close(descriptor);
        }
    }

  private:
    std::atomic_int descriptor_{-1};
};

[[nodiscard]] core::Result<Socket> connect_tcp(const std::string& host, const unsigned port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const auto resolved = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (resolved != 0) {
        return std::unexpected(
            agent_error("resolving Melody host failed: " + std::string{gai_strerror(resolved)}));
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard{addresses, &freeaddrinfo};
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        Socket socket{::socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC,
                               address->ai_protocol)};
        if (socket && ::connect(socket.get(), address->ai_addr, address->ai_addrlen) == 0) {
            return socket;
        }
    }
    return std::unexpected(agent_error("connecting to Melody failed"));
}

[[nodiscard]] core::Result<void> send_all(const int socket, const std::string_view bytes) {
    std::size_t sent = 0U;
    while (sent < bytes.size()) {
        const auto count = ::send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) {
            return std::unexpected(agent_error("writing the Melody agent connection failed"));
        }
        sent += static_cast<std::size_t>(count);
    }
    return {};
}

[[nodiscard]] core::Result<std::string> read_line(const int socket, std::string& buffered,
                                                  const int timeout_ms) {
    while (true) {
        if (const auto newline = buffered.find('\n'); newline != std::string::npos) {
            auto line = buffered.substr(0U, newline);
            buffered.erase(0U, newline + 1U);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        pollfd descriptor{.fd = socket, .events = POLLIN, .revents = 0};
        const auto ready = ::poll(&descriptor, 1U, timeout_ms);
        if (ready == 0) {
            return std::unexpected(core::Error{.code = core::ErrorCode::io,
                                               .message = "Melody read timed out",
                                               .context = {{.key = "timeout", .value = "1"}}});
        }
        if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return std::unexpected(agent_error("the Melody agent connection closed"));
        }
        std::array<char, 4096U> chunk{};
        const auto count = ::recv(socket, chunk.data(), chunk.size(), 0);
        if (count <= 0) {
            return std::unexpected(agent_error("the Melody agent connection closed"));
        }
        buffered.append(chunk.data(), static_cast<std::size_t>(count));
        if (buffered.size() > 1U * 1024U * 1024U) {
            return std::unexpected(agent_error("Melody sent an overlong protocol line"));
        }
    }
}

[[nodiscard]] std::string quote_mpd(const std::string_view value) {
    std::string result{"\""};
    for (const auto character : value) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] std::pair<std::string, std::vector<std::string>>
parse_command(const std::string_view line) {
    std::vector<std::string> words;
    std::string current;
    bool quoted = false;
    bool escaped = false;
    for (const auto character : line) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
        } else if (character == '\\' && quoted) {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
        } else if (character == ' ' && !quoted) {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    if (words.empty()) {
        return {};
    }
    auto command = std::move(words.front());
    words.erase(words.begin());
    return {std::move(command), std::move(words)};
}

template <typename Number>
[[nodiscard]] std::optional<Number> number(const std::string_view value) {
    Number result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size() ? std::optional{result}
                                                                      : std::nullopt;
}

[[nodiscard]] std::optional<double> floating(const std::string_view value) {
    // The Melody wire protocol is locale-independent and always uses a dot
    // decimal separator. std::strtod follows the process locale (often a
    // comma-decimal locale in the GUI), so use from_chars for protocol text.
    return number<double>(value);
}

[[nodiscard]] ReplayGainMode replay_gain_mode(const std::string_view mode) {
    if (mode == "track") {
        return ReplayGainMode::track;
    }
    if (mode == "album" || mode == "auto") {
        return ReplayGainMode::album;
    }
    return ReplayGainMode::off;
}

[[nodiscard]] std::string state_name(const LocalAuditionState state) {
    if (state == LocalAuditionState::playing || state == LocalAuditionState::buffering ||
        state == LocalAuditionState::draining) {
        return "play";
    }
    if (state == LocalAuditionState::paused || state == LocalAuditionState::ready) {
        return "pause";
    }
    return "stop";
}

[[nodiscard]] std::string default_stream_base(const std::string& host) {
    const auto address =
        host.find(':') != std::string::npos && !host.starts_with('[') ? "[" + host + "]" : host;
    return "http://" + address + ":6701";
}

} // namespace

core::Result<std::pair<std::string, bool>>
resolve_melody_agent_source(const MelodyAgentConfig& config, const MelodyAgentQueueItem& item) {
    if (config.local_music_root) {
        const std::filesystem::path relative{item.uri};
        if (relative.empty() || relative.is_absolute()) {
            return std::unexpected(agent_error("Melody queue URI is not a relative file path"));
        }
        for (const auto& component : relative) {
            if (component == ".." || component == "." || component.empty()) {
                return std::unexpected(agent_error("Melody queue URI escapes the music root"));
            }
        }
        const auto root = std::filesystem::path{*config.local_music_root}.lexically_normal();
        const auto joined = (root / relative).lexically_normal();
        auto root_it = root.begin();
        auto joined_it = joined.begin();
        for (; root_it != root.end() && joined_it != joined.end(); ++root_it, ++joined_it) {
            if (*root_it != *joined_it) {
                return std::unexpected(agent_error("Melody queue URI escapes the music root"));
            }
        }
        if (root_it != root.end()) {
            return std::unexpected(agent_error("Melody queue URI escapes the music root"));
        }
        return std::pair{joined.native(), true};
    }
    if (item.song_id.empty()) {
        return std::unexpected(agent_error("Melody queue item has no stream identity"));
    }
    auto base = config.stream_base_url.value_or(default_stream_base(config.host));
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return std::pair{base + "/api/v1/stream/" + item.song_id, false};
}

struct MelodyAgentService::Impl {
    Impl(MelodyAgentConfig value, LocalAuditionService& audition)
        : config(std::move(value)), player(audition),
          instance_id(core::StableId::random().to_string()),
          worker([this](const std::stop_token stop) { run(stop); }) {}

    ~Impl() {
        worker.request_stop();
        control.close();
        if (worker.joinable()) {
            worker.join();
        }
        static_cast<void>(player.stop());
    }

    [[nodiscard]] MelodyAgentSnapshot get_snapshot() const {
        MelodyAgentSnapshot result;
        {
            const std::scoped_lock lock{snapshot_mutex};
            result = published;
        }
        const auto playback = player.snapshot();
        result.replay_gain_mode = playback.replay_gain_mode;
        result.track_gain_db = playback.effective_replay_gain_info.track_gain_db;
        result.album_gain_db = playback.effective_replay_gain_info.album_gain_db;
        result.effective_gain_multiplier = playback.effective_replay_gain_multiplier;
        return result;
    }

    void publish(const std::function<void(MelodyAgentSnapshot&)>& change) {
        const std::scoped_lock lock{snapshot_mutex};
        change(published);
    }

    [[nodiscard]] core::Result<std::uint64_t> fetch_queue() {
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto socket = connect_tcp(config.host, config.port);
            if (!socket) {
                return std::unexpected(socket.error());
            }
            std::string buffered;
            auto greeting = read_line(socket->get(), buffered, 5'000);
            if (!greeting || !greeting->starts_with("OK MPD")) {
                return std::unexpected(agent_error("invalid Melody queue-sync greeting"));
            }
            if (auto sent = send_all(socket->get(), "status\n"); !sent) {
                return std::unexpected(sent.error());
            }
            std::uint64_t before = 0U;
            while (true) {
                auto line = read_line(socket->get(), buffered, 5'000);
                if (!line) {
                    return std::unexpected(line.error());
                }
                if (*line == "OK") {
                    break;
                }
                if (line->starts_with("playlist: ")) {
                    before = number<std::uint64_t>(line->substr(10U)).value_or(0U);
                }
            }
            if (auto sent = send_all(socket->get(), "playlistinfo\n"); !sent) {
                return std::unexpected(sent.error());
            }
            std::vector<MelodyAgentQueueItem> loaded;
            std::map<std::string, std::string> fields;
            const auto flush = [&] {
                if (!fields.contains("file")) {
                    return;
                }
                MelodyAgentQueueItem item;
                item.position =
                    number<int>(fields["Pos"]).value_or(static_cast<int>(loaded.size()));
                item.uri = fields["file"];
                item.song_id = fields["X-SongId"];
                item.duration_seconds = floating(fields["duration"]).value_or(0.0);
                item.track_gain_db = floating(fields["X-ReplayGainTrack"]);
                item.album_gain_db = floating(fields["X-ReplayGainAlbum"]);
                loaded.push_back(std::move(item));
            };
            while (true) {
                auto line = read_line(socket->get(), buffered, 5'000);
                if (!line) {
                    return std::unexpected(line.error());
                }
                if (*line == "OK") {
                    flush();
                    break;
                }
                if (line->starts_with("ACK")) {
                    return std::unexpected(agent_error("Melody rejected playlistinfo: " + *line));
                }
                const auto separator = line->find(": ");
                if (separator == std::string::npos) {
                    continue;
                }
                const auto key = line->substr(0U, separator);
                if (key == "file" && !fields.empty()) {
                    flush();
                    fields.clear();
                }
                fields[key] = line->substr(separator + 2U);
            }
            if (auto sent = send_all(socket->get(), "status\n"); !sent) {
                return std::unexpected(sent.error());
            }
            std::uint64_t after = before;
            while (true) {
                auto line = read_line(socket->get(), buffered, 5'000);
                if (!line) {
                    return std::unexpected(line.error());
                }
                if (*line == "OK") {
                    break;
                }
                if (line->starts_with("playlist: ")) {
                    after = number<std::uint64_t>(line->substr(10U)).value_or(before);
                }
            }
            if (before == after) {
                const auto item_count = loaded.size();
                const auto gain_count =
                    static_cast<std::size_t>(std::ranges::count_if(loaded, [](const auto& item) {
                        return item.track_gain_db.has_value() || item.album_gain_db.has_value();
                    }));
                const std::scoped_lock lock{queue_mutex};
                queue = std::move(loaded);
                publish([&](auto& state) {
                    state.queue_item_count = item_count;
                    state.queue_gain_item_count = gain_count;
                });
                return after;
            }
        }
        return std::unexpected(agent_error("Melody queue changed repeatedly during sync"));
    }

    [[nodiscard]] std::optional<MelodyAgentQueueItem> queue_item(const int position) const {
        const std::scoped_lock lock{queue_mutex};
        const auto found = std::ranges::find(queue, position, &MelodyAgentQueueItem::position);
        return found == queue.end() ? std::nullopt : std::optional{*found};
    }

    [[nodiscard]] std::optional<MelodyAgentQueueItem> refreshed_queue_item(const int position) {
        auto item = queue_item(position);
        if (item && (item->track_gain_db || item->album_gain_db)) {
            return item;
        }
        if (auto version = fetch_queue(); version) {
            publish([&](auto& state) { state.queue_version = *version; });
            item = queue_item(position);
        }
        return item;
    }

    [[nodiscard]] core::Result<void> load(const MelodyAgentQueueItem& item, const bool preload,
                                          const bool paused) {
        auto source = resolve_melody_agent_source(config, item);
        if (!source) {
            return std::unexpected(source.error());
        }
        std::optional<formats::ReplayGainInfo> gain;
        if (item.track_gain_db || item.album_gain_db) {
            gain.emplace();
            gain->track_gain_db = item.track_gain_db;
            gain->album_gain_db = item.album_gain_db;
        }
        auto result = source->second
                          ? (preload ? player.queue_gapless_next_selected(source->first, {}, gain)
                                     : player.load_selected_and_play(source->first, {}, gain))
                          : (preload ? player.queue_gapless_network_stream(source->first, gain)
                                     : player.load_network_stream_and_play(source->first, gain));
        if (!result) {
            return result;
        }
        if (!preload && gain) {
            result = player.set_replay_gain_info(*gain);
            if (!result) {
                return result;
            }
        }
        if (!preload && paused) {
            static_cast<void>(player.pause());
        }
        publish([&](auto& state) {
            if (preload) {
                state.preloaded_position = item.position;
            } else {
                state.current_position = item.position;
                state.visible_track_identity = !item.song_id.empty() ? item.song_id : item.uri;
                state.using_direct_source = source->second;
                state.received_track_gain_db = item.track_gain_db;
                state.received_album_gain_db = item.album_gain_db;
            }
        });
        return {};
    }

    [[nodiscard]] std::string handle(const std::string& line) {
        auto [command, arguments] = parse_command(line);
        const auto ack = [&command](const std::string& message) {
            return "ACK [56@0] {" + command + "} " + message + "\n";
        };
        if (command == "ping") {
            return "OK\n";
        }
        if (command == "queue_changed") {
            auto synced = fetch_queue();
            if (!synced) {
                return ack(synced.error().message);
            }
            publish([&](auto& state) { state.queue_version = *synced; });
            return "OK\n";
        }
        if (command == "play") {
            if (arguments.empty() || !number<int>(arguments[0])) {
                return ack("invalid queue position");
            }
            const auto position = *number<int>(arguments[0]);
            auto item = refreshed_queue_item(position);
            if (!item) {
                return ack("queue position is out of range");
            }
            bool paused = false;
            int next = -1;
            double seek = -1.0;
            for (const auto& argument : arguments | std::views::drop(1U)) {
                if (argument == "paused=1") {
                    paused = true;
                } else if (argument.starts_with("next=")) {
                    next = number<int>(std::string_view{argument}.substr(5U)).value_or(-1);
                } else if (argument.starts_with("seek=")) {
                    seek = floating(argument.substr(5U)).value_or(-1.0);
                }
            }
            if (auto loaded = load(*item, false, paused); !loaded) {
                return ack(loaded.error().message);
            }
            if (seek >= 0.0) {
                static_cast<void>(player.seek_to_seconds(seek));
            }
            if (next >= 0) {
                if (auto following = refreshed_queue_item(next); following) {
                    static_cast<void>(load(*following, true, false));
                }
            }
            return "OK\n";
        }
        if (command == "preload") {
            if (arguments.empty() || !number<int>(arguments[0])) {
                return ack("invalid queue position");
            }
            const auto position = *number<int>(arguments[0]);
            if (position < 0) {
                static_cast<void>(player.clear_gapless_next());
                publish([](auto& state) { state.preloaded_position = -1; });
                return "OK\n";
            }
            auto item = refreshed_queue_item(position);
            if (!item) {
                return ack("queue position is out of range");
            }
            auto loaded = load(*item, true, false);
            return loaded ? "OK\n" : ack(loaded.error().message);
        }
        core::Result<void> result;
        if (command == "pause") {
            result = player.pause();
        } else if (command == "resume") {
            result = player.play();
        } else if (command == "stop") {
            result = player.stop();
            publish([](auto& state) {
                state.current_position = -1;
                state.preloaded_position = -1;
                state.visible_track_identity.clear();
            });
        } else if (command == "volume" && !arguments.empty() && floating(arguments[0])) {
            result = player.set_volume_percent(
                std::clamp(static_cast<int>(std::lround(*floating(arguments[0]))), 0, 100));
        } else if (command == "replaygain" && !arguments.empty()) {
            result = player.set_replay_gain_mode(replay_gain_mode(arguments[0]));
        } else if (command == "seek" && !arguments.empty() && floating(arguments[0])) {
            result = player.seek_to_seconds(*floating(arguments[0]));
        } else if (command == "get_property" && !arguments.empty()) {
            const auto playback = player.snapshot();
            const auto rate = playback.format ? playback.format->sample_rate : 1;
            if (arguments[0] == "pause") {
                const auto paused = playback.state == LocalAuditionState::paused ||
                                    playback.state == LocalAuditionState::ready ||
                                    playback.state == LocalAuditionState::empty ||
                                    playback.state == LocalAuditionState::ended;
                return paused ? "value: true\nOK\n" : "value: false\nOK\n";
            }
            if (arguments[0] == "time-pos") {
                return "value: " +
                       std::to_string(static_cast<double>(playback.position_sample) / rate) +
                       "\nOK\n";
            }
            if (arguments[0] == "duration") {
                const auto duration =
                    playback.end_sample ? static_cast<double>(*playback.end_sample) / rate : 0.0;
                return "value: " + std::to_string(duration) + "\nOK\n";
            }
            if (arguments[0] == "volume") {
                return "value: " + std::to_string(playback.volume_percent) + "\nOK\n";
            }
            return ack("unknown property: " + arguments[0]);
        } else if (command == "set_property" && arguments.size() >= 2U) {
            if (arguments[0] == "pause") {
                result = arguments[1] == "true" || arguments[1] == "1" || arguments[1] == "yes"
                             ? player.pause()
                             : player.play();
            } else if (arguments[0] == "time-pos" && floating(arguments[1])) {
                result = player.seek_to_seconds(*floating(arguments[1]));
            } else if (arguments[0] == "volume" && floating(arguments[1])) {
                result = player.set_volume_percent(
                    std::clamp(static_cast<int>(std::lround(*floating(arguments[1]))), 0, 100));
            } else if (arguments[0] == "replaygain") {
                result = player.set_replay_gain_mode(replay_gain_mode(arguments[1]));
            } else {
                return ack("unknown property: " + arguments[0]);
            }
        } else {
            return "ACK [5@0] {" + command + "} unknown command\n";
        }
        return result ? "OK\n" : ack(result.error().message);
    }

    [[nodiscard]] core::Result<void> session(const std::stop_token stop) {
        auto connected = connect_tcp(config.host, config.port);
        if (!connected) {
            return std::unexpected(connected.error());
        }
        control = std::move(*connected);
        std::string buffered;
        auto greeting = read_line(control.get(), buffered, 5'000);
        if (!greeting || !greeting->starts_with("OK MPD")) {
            return std::unexpected(agent_error("invalid Melody greeting"));
        }
        publish([](auto& state) { state.connected = true; });
        std::string registration =
            "agent_register " + quote_mpd(config.name) + " v2 instance=" + instance_id;
        if (!config.stream_format.empty()) {
            registration += " format=" + quote_mpd(config.stream_format);
        }
        if (config.maximum_bit_rate) {
            registration += " max_bitrate=" + std::to_string(*config.maximum_bit_rate);
        }
        registration += '\n';
        if (auto sent = send_all(control.get(), registration); !sent) {
            return sent;
        }
        auto response = read_line(control.get(), buffered, 5'000);
        if (!response || *response != "OK") {
            return std::unexpected(agent_error("Melody rejected agent v2 registration"));
        }
        publish([](auto& state) { state.registered = true; });
        if (auto version = fetch_queue(); version) {
            publish([&](auto& state) { state.queue_version = *version; });
        }
        auto last_report = std::chrono::steady_clock::now() - config.report_period;
        std::uint64_t reported_transition = player.snapshot().chain_transitions;
        bool ended_reported = false;
        while (!stop.stop_requested()) {
            auto line = read_line(control.get(), buffered, 200);
            if (line) {
                if (auto sent = send_all(control.get(), handle(*line)); !sent) {
                    return sent;
                }
            } else if (line.error().message != "Melody read timed out") {
                return std::unexpected(line.error());
            }
            const auto playback = player.snapshot();
            if (playback.chain_transitions != reported_transition) {
                reported_transition = playback.chain_transitions;
                const auto old = get_snapshot().current_position;
                const auto next = get_snapshot().preloaded_position;
                const auto next_item = queue_item(next);
                publish([&](auto& state) {
                    state.current_position = state.preloaded_position;
                    state.preloaded_position = -1;
                    state.received_track_gain_db =
                        next_item ? next_item->track_gain_db : std::nullopt;
                    state.received_album_gain_db =
                        next_item ? next_item->album_gain_db : std::nullopt;
                });
                if (auto sent =
                        send_all(control.get(), "agent_advance " + std::to_string(old) + "\n");
                    !sent) {
                    return sent;
                }
                ended_reported = false;
            } else if (playback.state == LocalAuditionState::ended && !ended_reported) {
                ended_reported = true;
                const auto old = get_snapshot().current_position;
                if (auto sent =
                        send_all(control.get(), "agent_advance " + std::to_string(old) + "\n");
                    !sent) {
                    return sent;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= config.report_period) {
                last_report = now;
                const auto state = get_snapshot();
                const auto rate = playback.format ? playback.format->sample_rate : 1;
                const auto elapsed = static_cast<double>(playback.position_sample) / rate;
                const auto duration =
                    playback.end_sample ? static_cast<double>(*playback.end_sample) / rate : 0.0;
                const auto report = "agent_state " + state_name(playback.state) + " " +
                                    std::to_string(state.current_position) + " " +
                                    std::to_string(elapsed) + " " + std::to_string(duration) + " " +
                                    std::to_string(playback.volume_percent) + "\n";
                if (auto sent = send_all(control.get(), report); !sent) {
                    return sent;
                }
            }
        }
        return {};
    }

    void run(const std::stop_token stop) {
        while (!stop.stop_requested()) {
            publish([](auto& state) {
                ++state.session_generation;
                state.connected = false;
                state.registered = false;
                state.issue.reset();
            });
            auto result = session(stop);
            control.close();
            static_cast<void>(player.stop());
            publish([&](auto& state) {
                state.connected = false;
                state.registered = false;
                if (!result && !stop.stop_requested()) {
                    state.issue = result.error();
                }
            });
            if (!stop.stop_requested()) {
                const auto retry_at = std::chrono::steady_clock::now() + config.reconnect_delay;
                while (!stop.stop_requested() && std::chrono::steady_clock::now() < retry_at) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }
            }
        }
    }

    MelodyAgentConfig config;
    LocalAuditionService& player;
    std::string instance_id;
    mutable std::mutex snapshot_mutex;
    MelodyAgentSnapshot published;
    mutable std::mutex queue_mutex;
    std::vector<MelodyAgentQueueItem> queue;
    Socket control;
    std::jthread worker;
};

core::Result<std::unique_ptr<MelodyAgentService>>
MelodyAgentService::create(MelodyAgentConfig config, LocalAuditionService& player) {
    if (config.name.empty() || config.host.empty() || config.port == 0U || config.port > 65'535U) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "invalid Melody agent configuration",
                                           .context = {}});
    }
    return std::unique_ptr<MelodyAgentService>{
        new MelodyAgentService{std::make_unique<Impl>(std::move(config), player)}};
}

MelodyAgentService::MelodyAgentService(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

MelodyAgentService::~MelodyAgentService() = default;

MelodyAgentSnapshot MelodyAgentService::snapshot() const { return implementation_->get_snapshot(); }

} // namespace trackknife::audio
