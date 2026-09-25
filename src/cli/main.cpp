// SPDX-License-Identifier: GPL-3.0-only

// melody-cli: an engine from the shell. Playback, the queue and Up Next, the
// library by words, outputs -- what a script, a key binding or a rofi menu
// wants, over the same protocol Trackknife speaks.

#include "trackknife/core/stable_id.hpp"
#include "trackknife/discovery/mdns.hpp"
#include "trackknife/protocol/client.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using trackknife::protocol::Client;
using trackknife::protocol::Endpoint;
using Json = nlohmann::json;

struct Options final {
    std::string server;
    // Every --server given, for watch --all: those engines and no others.
    std::vector<std::string> servers;
    std::string password;
    std::string engine;
    bool json{false};
    // --fields: what albums and tracks list, besides the key; empty, all.
    std::vector<std::string> fields;
    // --keys: each listed line ends in a tab and its key, for a picker to
    // hand back to add/play/next/queue --key.
    bool keys{false};
    std::vector<std::string> words;
};

void usage(std::ostream& out) {
    out << "usage: melody-cli [--server HOST:PORT] [--password PASS] [--engine NAME] [--json]\n"
           "                  [--fields artist,album,title,date] [--keys]\n"
           "                  COMMAND [ARGUMENTS]\n"
           "\n"
           "  status                      what plays, where and how far in\n"
           "  play | pause | toggle | stop | next | prev\n"
           "  seek [+|-]SECONDS           to a place, or forward and back\n"
           "  volume [[+|-]PERCENT]       say or set how loud\n"
           "\n"
           "  play  album|track WORDS...  replace the queue with it and play it\n"
           "  add   album|track WORDS...  append it to the queue\n"
           "  next  album|track WORDS...  play it next (Up Next, first)\n"
           "  queue album|track WORDS...  add it to Up Next, last\n"
           "      Every word must appear in its artist, title, album or year:\n"
           "      melody-cli play album doors 1967\n"
           "      Or name one exactly by the key albums or tracks --json gives it:\n"
           "      melody-cli add album --key KEY\n"
           "\n"
           "  albums [WORDS...]           list albums: every one, or those the words find\n"
           "  tracks [WORDS...]           list tracks: every one, or those the words find\n"
           "  latest [COUNT]              the albums added most recently (default 20)\n"
           "  outputs                     the speakers this engine can play on\n"
           "  output NAME                 play on those instead\n"
           "  engines                     the engines announcing themselves nearby\n"
           "\n"
           "  watch [--all]               print the state each time it changes; with\n"
           "                              --all, of whichever engine here or nearby plays\n"
           "                              (only those named, with --server given for each)\n"
           "  rate [0-5]                  say or set the playing track's stars\n"
           "  love | unlove               the playing track, on Last.fm\n"
           "\n"
           "The engine: --server (or $MELODY_SERVER), else this machine's engine, else\n"
           "one found on the network -- by name with --engine when there are several.\n"
           "--password (or $MELODY_PASSWORD) for an engine that wants one. --json prints\n"
           "the engine's own answers, for scripts.\n";
}

// A string field of an answer: empty when absent, null or not a string, so
// an engine that leaves one out is shown as nothing rather than a crash.
[[nodiscard]] std::string text_of(const Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

[[nodiscard]] std::string first_text(const Json& object, const char* key, const char* otherwise) {
    auto text = text_of(object, key);
    return text.empty() ? text_of(object, otherwise) : text;
}

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "melody-cli: " << message << "\n";
    std::exit(EXIT_FAILURE);
}

[[nodiscard]] std::filesystem::path local_socket() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0') {
        return std::filesystem::path{runtime} / "melodyd.sock";
    }
    return std::filesystem::temp_directory_path() / "melodyd.sock";
}

// The engines on the network. Their answers come within a second, so that
// long is waited for -- unless one is wanted by name, which ends the wait
// as soon as it has answered.
[[nodiscard]] std::vector<trackknife::discovery::Found> look_around(const std::string& wanted = {}) {
    auto browser = trackknife::discovery::Browser::start();
    if (!browser) {
        return {};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1'200};
    while (std::chrono::steady_clock::now() < deadline) {
        if (!wanted.empty() && std::ranges::any_of((*browser)->found(), [&wanted](const auto& engine) {
                return engine.instance == wanted;
            })) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return (*browser)->found();
}

[[nodiscard]] std::unique_ptr<Client> connect(const Options& options) {
    const auto open = [&options](const Endpoint& endpoint) {
        auto client = Client::connect(endpoint);
        if (!client) {
            fail("cannot reach the engine at " + endpoint.describe() + ": " +
                 client.error().message);
        }
        return std::move(*client);
    };
    if (!options.server.empty()) {
        const auto endpoint = Endpoint::parse(options.server, options.password);
        if (!endpoint) {
            fail("--server wants HOST:PORT or a socket path");
        }
        return open(*endpoint);
    }
    if (options.engine.empty()) {
        if (const auto socket = local_socket(); std::filesystem::exists(socket)) {
            return open(Endpoint{.socket = socket, .host = {}, .port = 0, .token = {}});
        }
    }
    auto found = look_around(options.engine);
    if (!options.engine.empty()) {
        std::erase_if(found, [&options](const auto& engine) {
            return engine.instance != options.engine;
        });
    }
    if (found.empty()) {
        fail(options.engine.empty() ? "no engine here or on the network; name one with --server"
                                    : "no engine called \"" + options.engine + "\" was found");
    }
    if (found.size() > 1U) {
        std::string names;
        for (const auto& engine : found) {
            names += "\n  " + engine.instance;
        }
        fail("several engines were found; choose one with --engine:" + names);
    }
    return open(Endpoint{.socket = {},
                         .host = found.front().address,
                         .port = found.front().port,
                         .token = options.password});
}

[[nodiscard]] Json call(Client& client, const std::string& method, const Json& params = Json::object()) {
    auto answer = client.call(method, params);
    if (!answer) {
        fail(method + ": " + answer.error().message);
    }
    return std::move(*answer);
}

[[nodiscard]] std::string clock(const std::int64_t milliseconds) {
    if (milliseconds < 0) {
        return "?";
    }
    const auto seconds = milliseconds / 1'000;
    const auto hours = seconds / 3'600;
    char text[32];
    if (hours > 0) {
        std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", static_cast<long long>(hours),
                      static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60));
    } else {
        std::snprintf(text, sizeof(text), "%lld:%02lld", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    }
    return text;
}

[[nodiscard]] std::string joined(const std::vector<std::string>& words, const std::size_t from) {
    std::string text;
    for (auto index = from; index < words.size(); ++index) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += words[index];
    }
    return text;
}

// The file name in an encoded path (base64 of its bytes), for a track the
// library does not know.
[[nodiscard]] std::string decoded_name(const std::string& encoded) {
    static constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    std::string raw;
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
            raw.push_back(static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return std::filesystem::path{raw}.filename().string();
}

// The library's albums or tracks for these words -- every one, page by
// page -- or, with none and newest set, the most recently added.
[[nodiscard]] std::vector<Json> find(Client& client, const bool albums, const std::string& words,
                                     const std::size_t limit, const bool newest = false,
                                     const std::vector<std::string>& fields = {}) {
    std::vector<Json> found;
    // The engine's own cap: every page re-sorts the whole library, so one
    // page is the fastest way to list all of it.
    constexpr std::size_t page_size = 100'000;
    for (std::size_t offset = 0;;) {
        const auto wanted = limit == 0U ? page_size : std::min(page_size, limit - found.size());
        Json params{{"kind", albums ? 1 : 2}, {"text", words}, {"offset", offset}, {"limit", wanted}};
        if (newest) {
            params["newest_first"] = true;
        }
        // Only these, besides the key -- a picker's whole library is then a
        // fraction of the size. An engine that predates fields sends all.
        if (!fields.empty()) {
            params["fields"] = fields;
        }
        auto page = call(client, "catalogue.query", params);
        // Moved out, not copied: the whole library can be one page.
        std::size_t taken = 0;
        if (const auto entries = page.find("entries");
            entries != page.end() && entries->is_array()) {
            taken = entries->size();
            for (auto& entry : *entries) {
                found.push_back(std::move(entry));
            }
        }
        offset += taken;
        if (!page.value("more", false) || taken == 0U ||
            (limit != 0U && found.size() >= limit)) {
            return found;
        }
    }
}

[[nodiscard]] std::string describe_found(const Json& entry, const bool album) {
    const auto artist = text_of(entry, "artist");
    const auto name = album ? text_of(entry, "album")
                            : first_text(entry, "title", "label");
    const auto date = text_of(entry, "date");
    std::string text = artist + " — " + name;
    if (!album && !text_of(entry, "album").empty()) {
        text += " (" + text_of(entry, "album") +
                (date.empty() ? std::string{} : ", " + date.substr(0, 4)) + ")";
    } else if (!date.empty()) {
        text += " (" + date.substr(0, 4) + ")";
    }
    return text;
}

// What the engine is given to hold for these library tracks, each with an
// identity of its own.
[[nodiscard]] std::vector<Json> queue_entries(const std::vector<Json>& tracks) {
    std::vector<Json> entries;
    for (const auto& track : tracks) {
        entries.push_back(Json{
            {"entry", trackknife::core::StableId::random().to_string()},
            {"path", text_of(track, "key")},
            {"title", first_text(track, "title", "label")},
            {"group",
             Json{{"album_artist", text_of(track, "artist")},
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

// What the engine is given to hold: the tracks of the album or the track
// these words find first, each with an identity of its own.
[[nodiscard]] std::vector<Json> entries_for(Client& client, const std::string& kind,
                                            const std::string& words, std::string& chosen) {
    const bool album = kind == "album";
    if (!album && kind != "track") {
        fail("say album or track, then the words to find it by");
    }
    if (words.empty()) {
        fail("which " + kind + "? Give words from its artist, title, album or year");
    }
    // One track by its key -- its path -- as a picker showed it.
    if (!album && words.starts_with("--key ")) {
        const auto key = words.substr(6);
        auto tracks = call(client, "catalogue.query",
                           Json{{"kind", 2}, {"text", ""}, {"path", key}, {"limit", 1}})
                          .value("entries", std::vector<Json>{});
        if (tracks.empty()) {
            fail("no track has the key " + key);
        }
        chosen = describe_found(tracks.front(), false);
        return queue_entries(tracks);
    }
    // One album by its key: exactly the one a picker showed, not the first
    // that the words happen to find.
    if (album && words.starts_with("--key ")) {
        const auto key = words.substr(6);
        const auto page = call(client, "catalogue.query",
                               Json{{"kind", 2},
                                    {"text", ""},
                                    {"album_key", key},
                                    {"offset", 0},
                                    {"limit", 5'000}});
        auto tracks = page.value("entries", std::vector<Json>{});
        if (tracks.empty()) {
            fail("no album has the key " + key);
        }
        chosen = describe_found(tracks.front(), true);
        return queue_entries(tracks);
    }
    const auto found = find(client, album, words, 1);
    if (found.empty()) {
        fail("no " + kind + " matches \"" + words + "\"");
    }
    chosen = describe_found(found.front(), album);
    std::vector<Json> tracks;
    if (album) {
        const auto page = call(client, "catalogue.query",
                               Json{{"kind", 2},
                                    {"text", ""},
                                    {"album_key", text_of(found.front(), "key")},
                                    {"offset", 0},
                                    {"limit", 5'000}});
        tracks = page.value("entries", std::vector<Json>{});
    } else {
        tracks = found;
    }
    return queue_entries(tracks);
}


// What plays, as a script wants it: its tags from the engine's library,
// which read them from the file -- the queue entry says only what the
// client that queued it knew then, which may be nothing but the path -- and
// from the entry for a file the library does not have. Its rating from the
// library. Null when nothing plays.
[[nodiscard]] Json now_playing(Client& client, const Json& state) {
    const auto entry = text_of(state, "entry");
    if (entry.empty()) {
        return nullptr;
    }
    const auto path = text_of(state, "path");
    const auto known = call(client, "catalogue.query",
                            Json{{"kind", 2}, {"text", ""}, {"path", path}, {"limit", 1}})
                           .value("entries", std::vector<Json>{});
    Json track{{"path", path}, {"artist", ""}, {"title", ""}, {"album", ""}, {"date", ""}};
    if (!known.empty()) {
        track["artist"] = text_of(known.front(), "artist");
        track["title"] = first_text(known.front(), "title", "label");
        track["album"] = text_of(known.front(), "album");
        track["date"] = text_of(known.front(), "date");
    } else {
        const auto queue = call(client, "playback.queue").value("entries", std::vector<Json>{});
        const auto queued = std::ranges::find_if(queue, [&entry](const Json& candidate) {
            return text_of(candidate, "entry") == entry;
        });
        if (queued != queue.end()) {
            const auto group = queued->value("group", Json::object());
            track["artist"] = first_text(group, "artist", "album_artist");
            track["title"] = text_of(*queued, "title");
            track["album"] = text_of(group, "album");
            track["date"] = text_of(group, "date");
        }
    }
    if (text_of(track, "title").empty()) {
        track["title"] = decoded_name(path);
    }
    // Only a track in the library has a rating to show or set.
    if (!known.empty() && !text_of(known.front(), "rating_hash").empty()) {
        track["rating_hash"] = text_of(known.front(), "rating_hash");
        track["rating"] = known.front().value("rating", 0);
    }
    return track;
}

// "Artist — Title (Album, Year)".
[[nodiscard]] std::string describe_track(const Json& track) {
    const auto artist = text_of(track, "artist");
    const auto title = text_of(track, "title");
    std::string text = artist.empty() ? title : artist + " — " + title;
    const auto album = text_of(track, "album");
    const auto date = text_of(track, "date");
    if (!album.empty()) {
        text += " (" + album + (date.empty() ? std::string{} : ", " + date.substr(0, 4)) + ")";
    }
    return text;
}

void print_state(const Json& state, Client& client) {
    const auto status = state.value("status", std::string{"stopped"});
    const auto track = now_playing(client, state);
    const auto what = track.is_null() ? std::string{"nothing"} : describe_track(track);
    std::cout << status << ": " << what << "\n";
    std::cout << clock(state.value("position_ms", std::int64_t{0})) << " / "
              << clock(state.value("duration_ms", std::int64_t{-1})) << " · volume "
              << state.value("volume_percent", 0) << "% · queue "
              << state.value("queue_size", 0) << " · up next " << state.value("requests", 0);
    const auto outputs = call(client, "outputs.list").value("outputs", std::vector<Json>{});
    for (const auto& output : outputs) {
        if (output.value("selected", false)) {
            std::cout << " · on " << text_of(output, "name");
        }
    }
    if (const auto taken = text_of(state.value("output", Json::object()), "taken_by");
        !taken.empty()) {
        std::cout << " · " << taken << " has the speakers";
    }
    std::cout << "\n";
    if (const auto error = text_of(state, "error"); !error.empty() && status != "playing") {
        std::cout << "error: " << error << "\n";
    }
}

// watch --all: every engine reachable -- this computer's and those on the
// network -- and a line for the one that matters: the one playing (the
// latest to start, if several are), else the one that changed last. Each
// line names the engine, so what acts on it reaches the same one.
int watch_all(const Options& options) {
    struct Watched final {
        std::string id;
        std::string name;
        // What --server takes to reach it again.
        std::string server;
        std::unique_ptr<Client> client;
        Json state;
        std::chrono::steady_clock::time_point changed{};
        std::chrono::steady_clock::time_point started{};
        bool fresh{false};
        bool gone{false};
    };
    std::mutex lock;
    std::condition_variable woken;
    std::vector<std::shared_ptr<Watched>> engines;

    // Under the lock: a state that came in, and when it began to play.
    const auto take = [](Watched& engine, Json state) {
        const auto now = std::chrono::steady_clock::now();
        const bool was = text_of(engine.state, "status") == "playing";
        const bool is = text_of(state, "status") == "playing";
        if (is && (!was || text_of(state, "entry") != text_of(engine.state, "entry"))) {
            engine.started = now;
        }
        engine.state = std::move(state);
        engine.changed = now;
        engine.fresh = true;
    };
    const auto attach = [&](const Endpoint& endpoint, std::string server,
                            const std::string& announced_id) {
        {
            const std::scoped_lock held{lock};
            if (!announced_id.empty() &&
                std::ranges::any_of(engines, [&](const auto& known) {
                    return known->id == announced_id && !known->gone;
                })) {
                return;
            }
        }
        auto connected = Client::connect(endpoint);
        if (!connected) {
            return;
        }
        auto engine = std::make_shared<Watched>();
        engine->server = std::move(server);
        engine->client = std::move(*connected);
        const std::weak_ptr<Watched> weak{engine};
        engine->client->on_event([&, weak](const trackknife::protocol::Event& event) {
            const auto watched = weak.lock();
            if (!watched) {
                return;
            }
            const std::scoped_lock held{lock};
            if (event.name == "playback.changed") {
                take(*watched, event.data);
            } else if (event.name == "catalogue.rating_changed") {
                watched->fresh = true;
            } else {
                return;
            }
            woken.notify_one();
        });
        engine->client->on_closed([&, weak] {
            if (const auto watched = weak.lock()) {
                const std::scoped_lock held{lock};
                watched->gone = true;
                woken.notify_one();
            }
        });
        const auto info = engine->client->call("engine.info");
        const auto state = engine->client->call("playback.state");
        if (!info || !state) {
            engine->client->close();
            return;
        }
        engine->id = text_of(*info, "id").empty() ? announced_id : text_of(*info, "id");
        engine->name = text_of(*info, "name");
        const std::scoped_lock held{lock};
        // Reached twice -- here and over the network -- it is one engine.
        if (std::ranges::any_of(engines, [&](const auto& known) {
                // By id where both say one; an engine too old to say is
                // known by its name.
                return !known->gone && (!engine->id.empty() && !known->id.empty()
                                            ? known->id == engine->id
                                            : known->name == engine->name);
            })) {
            engine->client->close();
            return;
        }
        engine->state = *state;
        engine->fresh = true;
        engines.push_back(std::move(engine));
        woken.notify_one();
    };

    // Named engines are all there is; otherwise this computer's, and the
    // network's as they come and go.
    const auto named = [&] {
        for (const auto& server : options.servers) {
            {
                const std::scoped_lock held{lock};
                if (std::ranges::any_of(engines, [&](const auto& known) {
                        return known->server == server && !known->gone;
                    })) {
                    continue;
                }
            }
            if (const auto endpoint = Endpoint::parse(server, options.password)) {
                attach(*endpoint, server, {});
            }
        }
    };
    if (options.servers.empty()) {
        if (const auto socket = local_socket(); std::filesystem::exists(socket)) {
            attach(Endpoint{.socket = socket, .host = {}, .port = 0, .token = {}}, socket.string(),
                   {});
        }
    }
    auto browser = options.servers.empty()
                       ? trackknife::discovery::Browser::start()
                       : std::unexpected(trackknife::core::Error{
                             .code = trackknife::core::ErrorCode::unsupported,
                             .message = "engines named",
                             .context = {}});
    const auto look = [&] {
        if (!options.servers.empty()) {
            named();
            return;
        }
        if (!browser) {
            return;
        }
        for (const auto& found : (*browser)->found()) {
            const auto id = found.txt.contains("id") ? found.txt.at("id") : std::string{};
            attach(Endpoint{.socket = {},
                            .host = found.address,
                            .port = found.port,
                            .token = options.password},
                   found.address + ":" + std::to_string(found.port), id);
        }
    };

    std::string last_line;
    // The network answers within a second; named engines are there at once.
    if (!options.servers.empty()) {
        named();
    }
    auto next_look = std::chrono::steady_clock::now() + (options.servers.empty()
                                                             ? std::chrono::milliseconds{1'200}
                                                             : std::chrono::milliseconds{5'000});
    while (true) {
        std::shared_ptr<Watched> chosen;
        Json state;
        bool tell = false;
        {
            std::unique_lock held{lock};
            woken.wait_until(held, next_look, [&] {
                return std::ranges::any_of(engines,
                                           [](const auto& engine) { return engine->fresh || engine->gone; });
            });
            std::erase_if(engines, [](const auto& engine) { return engine->gone; });
            // Playing beats paused beats stopped; then the latest.
            const auto rank = [](const Watched& engine) {
                const auto status = text_of(engine.state, "status");
                return std::tuple{status == "playing" ? 2 : !text_of(engine.state, "entry").empty() ? 1 : 0,
                                  status == "playing" ? engine.started : engine.changed};
            };
            for (const auto& engine : engines) {
                if (!chosen || rank(*engine) > rank(*chosen)) {
                    chosen = engine;
                }
            }
            tell = std::ranges::any_of(engines, [](const auto& engine) { return engine->fresh; });
            for (const auto& engine : engines) {
                engine->fresh = false;
            }
            if (chosen) {
                state = chosen->state;
            }
        }
        if (std::chrono::steady_clock::now() >= next_look) {
            look();
            next_look = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        }
        if (!chosen || !tell) {
            continue;
        }
        // Asked without the lock: its reader thread answers the call.
        auto track = now_playing(*chosen->client, state);
        std::string line;
        if (options.json) {
            state["track"] = std::move(track);
            state["engine"] = Json{{"name", chosen->name}, {"server", chosen->server}};
            line = state.dump();
        } else {
            line = chosen->name + ": " + text_of(state, "status") + ": " +
                   (track.is_null() ? std::string{"nothing"} : describe_track(track));
        }
        if (line != last_line) {
            std::cout << line << std::endl;
            last_line = std::move(line);
        }
    }
}

int run(const Options& options) {
    const auto& words = options.words;
    const auto command = words.front();
    if (command == "engines") {
        const auto found = look_around();
        if (options.json) {
            auto listed = Json::array();
            for (const auto& engine : found) {
                listed.push_back(Json{{"name", engine.instance},
                                      {"address", engine.address},
                                      {"port", engine.port},
                                      {"password", engine.txt.contains("auth") &&
                                                       engine.txt.at("auth") == "1"}});
            }
            std::cout << listed.dump() << "\n";
            return EXIT_SUCCESS;
        }
        for (const auto& engine : found) {
            std::cout << engine.instance << "\t" << engine.address << ":" << engine.port
                      << (engine.txt.contains("auth") && engine.txt.at("auth") == "1"
                              ? "\tpassword"
                              : "")
                      << "\n";
        }
        return found.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (command == "watch" && words.size() == 2U && words[1] == "--all") {
        return watch_all(options);
    }

    auto client = connect(options);
    const auto show = [&options, &client](const Json& answer) {
        if (options.json) {
            std::cout << answer.dump() << "\n";
        } else {
            print_state(answer, *client);
        }
    };
    const auto state = [&client] { return call(*client, "playback.state"); };
    // Asked to play and it did not: the engine's reason, and a failing exit,
    // so a script does not take a stopped engine for a playing one.
    const auto failed = [](const Json& answer) {
        return text_of(answer, "status") != "playing" && !text_of(answer, "error").empty();
    };
    const auto playing_or_fail = [&failed, &show](Json answer) {
        if (failed(answer)) {
            show(answer);
            fail("could not play: " + text_of(answer, "error"));
        }
        return answer;
    };
    // After asking to play: the engine opens the file before it plays, so
    // its answer is a moment early. What it says once it has started, within
    // two seconds -- or once it has given up.
    const auto settled = [&client, &failed](Json answer, const std::string& wanted) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (text_of(answer, "status") != wanted && !(wanted == "playing" && failed(answer)) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            answer = call(*client, "playback.state");
        }
        return answer;
    };
    const auto started = [&settled, &playing_or_fail](Json answer) {
        return playing_or_fail(settled(std::move(answer), "playing"));
    };
    // A skip is done when another entry plays, whatever the status was.
    const auto moved_on = [&client, &failed, &playing_or_fail](Json answer,
                                                                 const std::string& before) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while ((text_of(answer, "entry") == before || text_of(answer, "status") != "playing") &&
               !failed(answer) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            answer = call(*client, "playback.state");
        }
        return playing_or_fail(std::move(answer));
    };

    if (command == "status") {
        show(state());
    } else if ((command == "play" || command == "next") && words.size() == 1U) {
        // Plain play resumes; plain next skips.
        const auto current = state();
        if (command == "next") {
            show(moved_on(call(*client, "playback.next"), text_of(current, "entry")));
        } else if (text_of(current, "status") == "paused") {
            show(started(call(*client, "playback.resume")));
        } else if (text_of(current, "status") == "stopped" &&
                   current.value("queue_size", 0) > 0) {
            const auto queue = call(*client, "playback.queue").value("entries", std::vector<Json>{});
            show(started(call(*client, "playback.play",
                              Json{{"entry", text_of(queue.front(), "entry")}})));
        } else {
            show(current);
        }
    } else if (command == "pause") {
        show(settled(call(*client, "playback.pause"), "paused"));
    } else if (command == "toggle") {
        const bool playing = text_of(state(), "status") == "playing";
        const auto answer = call(*client, playing ? "playback.pause" : "playback.resume");
        show(playing ? settled(answer, "paused") : started(answer));
    } else if (command == "stop") {
        show(settled(call(*client, "playback.stop"), "stopped"));
    } else if (command == "prev" || command == "previous") {
        const auto before = text_of(state(), "entry");
        show(moved_on(call(*client, "playback.previous"), before));
    } else if (command == "seek") {
        if (words.size() != 2U) {
            fail("seek wants SECONDS, +SECONDS or -SECONDS");
        }
        const auto& amount = words[1];
        const auto seconds = std::strtod(amount.c_str(), nullptr);
        auto target = static_cast<std::int64_t>(seconds * 1'000.0);
        if (amount.front() == '+' || amount.front() == '-') {
            target = state().value("position_ms", std::int64_t{0}) + target;
        }
        show(call(*client, "playback.seek", Json{{"position_ms", std::max<std::int64_t>(0, target)}}));
    } else if (command == "volume") {
        if (words.size() == 1U) {
            const auto current = state();
            std::cout << current.value("volume_percent", 0) << "\n";
            return EXIT_SUCCESS;
        }
        const auto& amount = words[1];
        auto percent = std::atoi(amount.c_str());
        if (amount.front() == '+' || amount.front() == '-') {
            percent += state().value("volume_percent", 0);
        }
        show(call(*client, "playback.set_volume", Json{{"percent", std::clamp(percent, 0, 100)}}));
    } else if ((command == "play" || command == "add" || command == "next" ||
                command == "queue") &&
               words.size() >= 2U) {
        std::string chosen;
        auto entries = entries_for(*client, words[1], joined(words, 2), chosen);
        if (entries.empty()) {
            fail("nothing to play in " + chosen);
        }
        if (command == "play") {
            static_cast<void>(call(*client, "playback.replace_queue", Json{{"entries", entries}}));
            show(started(call(*client, "playback.play",
                              Json{{"entry", text_of(entries.front(), "entry")}})));
        } else if (command == "add") {
            // After what is there, which keeps its identities: what plays
            // now plays on.
            auto queue = call(*client, "playback.queue").value("entries", std::vector<Json>{});
            queue.insert(queue.end(), entries.begin(), entries.end());
            show(call(*client, "playback.replace_queue", Json{{"entries", queue}}));
        } else {
            // Up Next: handed to the engine to hold, then asked for in order.
            static_cast<void>(call(*client, "playback.enqueue", Json{{"entries", entries}}));
            auto requests = call(*client, "playback.requests").value("entries", std::vector<Json>{});
            std::vector<Json> asked;
            for (const auto& entry : entries) {
                asked.push_back(text_of(entry, "entry"));
            }
            requests.insert(command == "next" ? requests.begin() : requests.end(), asked.begin(),
                            asked.end());
            show(call(*client, "playback.set_requests", Json{{"entries", requests}}));
        }
        if (!options.json) {
            std::cerr << "melody-cli: " << chosen << "\n";
        }
    } else if (command == "albums" || command == "tracks" || command == "latest") {
        const bool albums = command != "tracks";
        const auto text = command == "latest" ? std::string{} : joined(words, 1);
        // Everything that matches; only latest is a count.
        std::size_t limit = 0;
        if (command == "latest") {
            limit = words.size() > 1U ? static_cast<std::size_t>(std::max(1, std::atoi(words[1].c_str())))
                                      : 20U;
        }
        // Lines are only ever artist, name, album and year: only those are
        // asked for, which for a whole library is most of the time saved.
        auto fields = options.fields;
        if (!options.json && fields.empty()) {
            fields = {"artist", "album", "title", "label", "date"};
        }
        const auto found = find(*client, albums, text, limit, command == "latest", fields);
        if (options.json) {
            const auto count = found.size();
            Json listed = Json::array();
            for (auto& entry : found) {
                listed.push_back(std::move(entry));
            }
            std::cout << listed.dump() << "\n";
            return count == 0U ? EXIT_FAILURE : EXIT_SUCCESS;
        }
        std::string lines;
        for (const auto& entry : found) {
            lines += describe_found(entry, albums);
            if (options.keys) {
                lines += '\t';
                lines += text_of(entry, "key");
            }
            lines += '\n';
        }
        std::cout << lines;
        return found.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
    } else if (command == "outputs") {
        const auto outputs = call(*client, "outputs.list").value("outputs", std::vector<Json>{});
        if (options.json) {
            std::cout << Json(outputs).dump() << "\n";
            return EXIT_SUCCESS;
        }
        for (const auto& output : outputs) {
            std::cout << (output.value("selected", false) ? "* " : "  ")
                      << text_of(output, "name")
                      << (output.value("online", true) ? "" : " (offline)") << "\n";
        }
    } else if (command == "output") {
        const auto name = joined(words, 1);
        if (name.empty()) {
            fail("output wants the name of the speakers, as outputs lists them");
        }
        const auto outputs = call(*client, "outputs.list").value("outputs", std::vector<Json>{});
        const auto found = std::ranges::find_if(outputs, [&name](const Json& output) {
            return text_of(output, "name") == name ||
                   text_of(output, "id") == name;
        });
        if (found == outputs.end()) {
            fail("no output called \"" + name + "\"; see melody-cli outputs");
        }
        static_cast<void>(call(*client, "outputs.select", Json{{"id", text_of(*found, "id")}}));
        show(state());
    } else if (command == "watch") {
        // A line per change, for a status bar: the engine tells every
        // client when the state changes (not as the position moves). The
        // reader thread only hands the newest state over; asking the
        // engine what it is happens here.
        std::mutex lock;
        std::condition_variable woken;
        std::optional<Json> latest;
        // A rating set anywhere: what plays is looked at again, in case it
        // is its own.
        bool rated = false;
        bool gone = false;
        client->on_event([&](const trackknife::protocol::Event& event) {
            const std::scoped_lock held{lock};
            if (event.name == "playback.changed") {
                latest = event.data;
            } else if (event.name == "catalogue.rating_changed") {
                rated = true;
            } else {
                return;
            }
            woken.notify_one();
        });
        client->on_closed([&] {
            const std::scoped_lock held{lock};
            gone = true;
            woken.notify_one();
        });
        // The state as it is, unless a change has come in meanwhile. Asked
        // without the lock: the reader thread, which answers the call, may
        // be waiting for it.
        auto first = state();
        {
            const std::scoped_lock held{lock};
            if (!latest) {
                latest = std::move(first);
            }
        }
        std::string last_line;
        Json last_state;
        while (true) {
            Json changed;
            {
                std::unique_lock held{lock};
                woken.wait(held, [&] { return latest.has_value() || rated || gone; });
                if (gone) {
                    fail("the engine went away");
                }
                if (latest) {
                    changed = std::move(*latest);
                    latest.reset();
                } else {
                    changed = last_state;
                }
                rated = false;
            }
            last_state = changed;
            auto track = now_playing(*client, changed);
            std::string line;
            if (options.json) {
                changed["track"] = std::move(track);
                line = changed.dump();
            } else {
                line = text_of(changed, "status") + ": " +
                       (track.is_null() ? std::string{"nothing"} : describe_track(track));
            }
            // Only what a reader would see as a change.
            if (line != last_line) {
                std::cout << line << std::endl;
                last_line = std::move(line);
            }
        }
    } else if (command == "rate") {
        const auto track = now_playing(*client, state());
        if (track.is_null()) {
            fail("nothing is playing");
        }
        if (!track.contains("rating_hash")) {
            fail("what plays is not in the library, so it has no rating");
        }
        // Stars, as people count them; the engine keeps 0-10.
        if (words.size() == 1U) {
            const auto rating = track.value("rating", 0);
            if (options.json) {
                std::cout << Json{{"stars", rating / 2}, {"rating", rating}}.dump() << "\n";
            } else {
                std::cout << rating / 2 << "\n";
            }
            return EXIT_SUCCESS;
        }
        const auto& given = words[1];
        if (given.size() != 1U || given.front() < '0' || given.front() > '5') {
            fail("rate wants stars from 0 to 5");
        }
        const auto stars = given.front() - '0';
        static_cast<void>(call(*client, "catalogue.set_rating",
                               Json{{"hash", text_of(track, "rating_hash")}, {"rating", stars * 2}}));
        if (!options.json) {
            std::cout << "rated " << stars << (stars == 1 ? " star: " : " stars: ")
                      << describe_track(track) << "\n";
        }
    } else if (command == "love" || command == "unlove") {
        const auto track = now_playing(*client, state());
        if (track.is_null()) {
            fail("nothing is playing");
        }
        if (text_of(track, "artist").empty()) {
            fail("what plays has no artist for Last.fm to know it by");
        }
        // The engine's Last.fm account, the one it scrobbles with.
        const bool loved = command == "love";
        static_cast<void>(call(*client, "lastfm.love",
                               Json{{"artist", text_of(track, "artist")},
                                    {"title", text_of(track, "title")},
                                    {"loved", loved}}));
        if (!options.json) {
            std::cout << (loved ? "loved: " : "unloved: ") << describe_track(track) << "\n";
        }
    } else {
        usage(std::cerr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (const char* server = std::getenv("MELODY_SERVER"); server != nullptr) {
        options.server = server;
    }
    if (const char* password = std::getenv("MELODY_PASSWORD"); password != nullptr) {
        options.password = password;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto value = [&]() -> std::string {
            if (index + 1 >= argc) {
                fail(std::string{argument} + " wants a value");
            }
            return argv[++index];
        };
        if (!options.words.empty()) {
            options.words.emplace_back(argument);
        } else if (argument == "--server") {
            options.server = value();
            options.servers.push_back(options.server);
        } else if (argument == "--password") {
            options.password = value();
        } else if (argument == "--engine") {
            options.engine = value();
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--keys") {
            options.keys = true;
        } else if (argument == "--fields") {
            std::stringstream list{value()};
            for (std::string field; std::getline(list, field, ',');) {
                if (!field.empty()) {
                    options.fields.push_back(field);
                }
            }
        } else if (argument == "--help" || argument == "-h") {
            usage(std::cout);
            return EXIT_SUCCESS;
        } else if (argument.starts_with("--")) {
            fail("unrecognised option " + std::string{argument});
        } else {
            options.words.emplace_back(argument);
        }
    }
    if (options.words.empty()) {
        usage(std::cerr);
        return EXIT_FAILURE;
    }
    return run(options);
}
