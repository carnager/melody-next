// SPDX-License-Identifier: GPL-3.0-only

#include "engine_access.hpp"

#include "trackknife/core/environment.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/protocol/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace trackknife::cli {
namespace {

using protocol::Client;
using protocol::Endpoint;

// A string field of an answer: empty when absent, null or not a string.
[[nodiscard]] std::string text_of(const Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

[[nodiscard]] std::string first_text(const Json& object, const char* key, const char* otherwise) {
    auto text = text_of(object, key);
    return text.empty() ? text_of(object, otherwise) : text;
}

[[nodiscard]] core::Error failure(std::string message,
                                  const core::ErrorCode code = core::ErrorCode::not_found) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] core::Result<Json> call(Client& client, const std::string& method,
                                      const Json& params = Json::object()) {
    auto answer = client.call(method, params);
    if (!answer) {
        return std::unexpected(
            failure(method + ": " + answer.error().message, answer.error().code));
    }
    return answer;
}

[[nodiscard]] core::Result<std::unique_ptr<Client>>
open(const Endpoint& endpoint, const std::string_view program, std::string& notice) {
    auto client = Client::connect(endpoint);
    if (!client) {
        return std::unexpected(failure("cannot reach the engine at " + endpoint.describe() + ": " +
                                           client.error().message,
                                       client.error().code));
    }
    // ADR-0260: another protocol is not used; an older engine is, and said
    // so.
    if (auto info = (*client)->call("engine.info")) {
        const auto peer = protocol::peer_version(*info);
        auto said =
            protocol::compatibility_message(peer, text_of(*info, "name"), std::string{program});
        if (protocol::compatibility(peer) == protocol::Compatibility::incompatible) {
            return std::unexpected(failure(std::move(said), core::ErrorCode::unsupported));
        }
        notice = std::move(said);
    }
    return client;
}

} // namespace

std::filesystem::path local_socket() {
    if (const char* runtime = core::environment_variable("XDG_RUNTIME_DIR");
        runtime != nullptr && *runtime != '\0') {
        return std::filesystem::path{runtime} / "melodyd.sock";
    }
    return std::filesystem::temp_directory_path() / "melodyd.sock";
}

std::vector<discovery::Found> look_around(const std::string& wanted) {
    auto browser = discovery::Browser::start();
    if (!browser) {
        return {};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1'200};
    while (std::chrono::steady_clock::now() < deadline) {
        if (!wanted.empty() &&
            std::ranges::any_of((*browser)->found(), [&wanted](const auto& engine) {
                return engine.instance == wanted;
            })) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return (*browser)->found();
}

core::Result<std::unique_ptr<protocol::Client>>
connect_engine(const EngineChoice& choice, const std::string_view program, std::string& notice) {
    const auto local = Endpoint{.socket = local_socket(), .host = {}, .port = 0, .token = {}};
    if (choice.local) {
        if (!std::filesystem::exists(local.socket)) {
            return std::unexpected(failure("this computer's engine is not running (no " +
                                           local.socket.string() + ")"));
        }
        return open(local, program, notice);
    }
    if (!choice.server.empty()) {
        const auto endpoint = Endpoint::parse(choice.server, choice.password);
        if (!endpoint) {
            return std::unexpected(failure("--server wants HOST:PORT or a socket path",
                                           core::ErrorCode::invalid_argument));
        }
        return open(*endpoint, program, notice);
    }
    if (choice.engine.empty() && std::filesystem::exists(local.socket)) {
        return open(local, program, notice);
    }
    auto found = look_around(choice.engine);
    if (!choice.engine.empty()) {
        std::erase_if(found,
                      [&choice](const auto& engine) { return engine.instance != choice.engine; });
    }
    if (found.empty()) {
        return std::unexpected(failure(
            choice.engine.empty() ? "no engine here or on the network; name one with --server"
                                  : "no engine called \"" + choice.engine + "\" was found"));
    }
    if (found.size() > 1U) {
        std::string names;
        for (const auto& engine : found) {
            names += "\n  " + engine.instance;
        }
        return std::unexpected(
            failure("several engines were found; choose one with --engine:" + names,
                    core::ErrorCode::conflict));
    }
    return open(Endpoint{.socket = {},
                         .host = found.front().address,
                         .port = found.front().port,
                         .token = choice.password},
                program, notice);
}

std::vector<Json> queue_entries(const std::vector<Json>& tracks) {
    std::vector<Json> entries;
    entries.reserve(tracks.size());
    for (const auto& track : tracks) {
        entries.push_back(Json{{"entry", core::StableId::random().to_string()},
                               {"path", text_of(track, "key")},
                               {"title", first_text(track, "title", "label")},
                               {"group", Json{{"album_artist", text_of(track, "artist")},
                                              {"artist", text_of(track, "artist")},
                                              {"album", text_of(track, "album")},
                                              {"date", text_of(track, "date")}}}});
        // Known to the library, so the engine can show how long it is.
        if (const auto duration = track.value("duration_ms", std::int64_t{-1}); duration >= 0) {
            entries.back()["duration_ms"] = duration;
        }
    }
    return entries;
}

core::Result<std::vector<Json>> keyed_tracks(protocol::Client& client, const bool album,
                                             const std::string& key) {
    auto params =
        album ? Json{{"kind", 2}, {"text", ""}, {"album_key", key}, {"offset", 0}, {"limit", 5'000}}
              : Json{{"kind", 2}, {"text", ""}, {"path", key}, {"limit", 1}};
    auto page = call(client, "catalogue.query", params);
    if (!page) {
        return std::unexpected(std::move(page.error()));
    }
    auto tracks = page->value("entries", std::vector<Json>{});
    if (tracks.empty()) {
        return std::unexpected(
            failure((album ? "no album has the key " : "no track has the key ") + key));
    }
    return tracks;
}

core::Result<Json> place(protocol::Client& client, const Placement placement,
                         const std::vector<Json>& entries) {
    if (placement == Placement::play) {
        if (auto replaced = call(client, "playback.replace_queue", Json{{"entries", entries}});
            !replaced) {
            return replaced;
        }
        return call(client, "playback.play", Json{{"entry", text_of(entries.front(), "entry")}});
    }
    if (placement == Placement::add) {
        // After what is there, which keeps its identities: what plays now
        // plays on.
        auto queue = call(client, "playback.queue");
        if (!queue) {
            return queue;
        }
        auto held = queue->value("entries", std::vector<Json>{});
        held.insert(held.end(), entries.begin(), entries.end());
        return call(client, "playback.replace_queue", Json{{"entries", held}});
    }
    // Up Next: handed to the engine to hold, then asked for in order.
    if (auto enqueued = call(client, "playback.enqueue", Json{{"entries", entries}}); !enqueued) {
        return enqueued;
    }
    auto requested = call(client, "playback.requests");
    if (!requested) {
        return requested;
    }
    auto requests = requested->value("entries", std::vector<Json>{});
    std::vector<Json> asked;
    asked.reserve(entries.size());
    for (const auto& entry : entries) {
        asked.push_back(text_of(entry, "entry"));
    }
    requests.insert(placement == Placement::insert ? requests.begin() : requests.end(),
                    asked.begin(), asked.end());
    return call(client, "playback.set_requests", Json{{"entries", requests}});
}

} // namespace trackknife::cli
