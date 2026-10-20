// SPDX-License-Identifier: GPL-3.0-only

// melody-cli: an engine from the shell. Playback, the queue and Up Next, the
// library by words, outputs -- what a script, a key binding or a rofi menu
// wants, over the same protocol Trackknife speaks.

#include "config_command.hpp"
#include "convert_command.hpp"
#include "trackknife/core/environment.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/discovery/mdns.hpp"
#include "trackknife/protocol/client.hpp"
#include "trackknife/protocol/message.hpp"
#include "trackknife/protocol/version.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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
    // --password-file or $MELODY_PASSWORD_FILE: read once the line is
    // parsed, and only if no --password came after it.
    std::string password_file;
    std::string engine;
    bool json{false};
    // --fields: what albums and tracks list, besides the key; empty, all.
    std::vector<std::string> fields;
    // --keys: each listed line ends in a tab and its key, for a picker to
    // hand back to add/play/next/queue --key.
    bool keys{false};
    // --format: how `current` and `find` print a track, in tkfmt-1.
    std::string format{"$if(%artist%,%artist% - )%title%"};
    std::vector<std::string> words;
};

void usage(std::ostream& out) {
    out << "usage: melody-cli [--server HOST:PORT] [--password PASS | --password-file FILE]\n"
           "                  [--engine NAME] [--json]\n"
           "                  [--fields artist,album,title,date] [--keys] [--format EXPR]\n"
           "                  COMMAND [ARGUMENTS]\n"
           "\n"
           "  status                      what plays, where and how far in\n"
           "  current [--format EXPR]     the playing track on one line; nothing when\n"
           "                              nothing plays. EXPR is tkfmt-1, as in\n"
           "                              Melody (default \"$if(%artist%,%artist% - "
           ")%title%\"):\n"
           "                              every tag as %name% (%replaygain_track_gain%...),\n"
           "                              $info(samplerate) $info(bitspersample)\n"
           "                              $info(codec) $info(channels), %rating%\n"
           "                              %albumrating% (1-10)\n"
           "                              %length% %path% %playback_state% %playback_time%\n"
           "                              %playback_remaining%; a field not known is\n"
           "                              empty. Literal ( ) , $ % are escaped with a\n"
           "                              backslash.\n"
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
           "      Or let the engine choose: random album (an album of a random album\n"
           "      artist), random tracks [COUNT] (default 25):\n"
           "      melody-cli play random tracks 50\n"
           "\n"
           "  play list NAME...           play one of the engine's lists: by its name,\n"
           "                              or every word found in one name\n"
           "\n"
           "  lists                       the engine's saved playlists\n"
           "  playlists                   where they are: the engine's playlist folder,\n"
           "                              each file there a playlist (ADR-0296)\n"
           "  playlists folder DIR|default [--style root|playlist|absolute]\n"
           "                              keep them in DIR -- MPD's playlist_directory,\n"
           "                              say -- or Playlists in the music folder; the\n"
           "                              style is how new files name their tracks\n"
           "  albums [WORDS...]           list albums: every one, or those the words find\n"
           "  tracks [WORDS...]           list tracks: every one, or those the words find\n"
           "  find QUERY [--format EXPR]  tracks a query finds, one line each, formatted\n"
           "                              by the engine from its whole library row, with\n"
           "                              the fields current has, bar playback_*.\n"
           "                              QUERY is a Melody query:\n"
           "                              melody-cli find 'genre IS jazz SORT BY %date%' \\\n"
           "                                --format '%date% %artist% - %title%'\n"
           "  latest [COUNT]              the albums added most recently (default 20)\n"
           "  latest --since WHEN         every album added after WHEN: Unix seconds,\n"
           "                              2026-10-17, 2026-10-17T18:30, or ago -- 90m,\n"
           "                              12h, 3d, 2w. Nothing new is no error. For a\n"
           "                              cron job: keep when it ran, and pass that\n"
           "         --folders            each album's folder, one a line, instead of\n"
           "                              its name; with --json, a \"folder\" each\n"
           "  convert --to FOLDER [--layout NAME] [--preset ID] (--since WHEN | PATH...)\n"
           "                              the engine converts, where the files are: tags\n"
           "                              and cover kept, named by its naming layout or\n"
           "                              kept below the music folder, ReplayGain measured\n"
           "                              afresh; what is there is passed over (--help)\n"
           "  revision                    changes when the library does, for caches\n"
           "  outputs                     the speakers this engine can play on\n"
           "  output NAME                 play on those instead\n"
           "  engines                     the engines announcing themselves nearby\n"
           "\n"
           "  config [KEY [VALUE...]]     this machine's melodyd.conf: all of it, one\n"
           "                              setting, or that setting changed -- checked as\n"
           "                              melodyd reads it, comments kept. --engine NAME\n"
           "                              for an [engine NAME] section, --unset KEY,\n"
           "                              --config FILE (default $MELODYD_CONFIG, else\n"
           "                              ~/.config/melody/melodyd.conf). A new file\n"
           "                              starts from the commented skeleton\n"
           "  config --edit               open it in $EDITOR, and take it only when\n"
           "                              melodyd would\n"
           "\n"
           "  watch albums [--folders | --run CMD]\n"
           "                              each album new to the library, once it is\n"
           "                              whole: a line, its folder, or CMD run for it --\n"
           "                              {} its folder, quoted; MELODY_ALBUM_FOLDER,\n"
           "                              _ARTIST, _TITLE and _DATE set. Runs until the\n"
           "                              engine goes, then fails\n"
           "  watch [--all]               print the state each time it changes; with\n"
           "                              --all, of whichever engine here or nearby plays\n"
           "                              (only those named, with --server given for each)\n"
           "  rating [0-10]               say or set the playing track's rating, 1-10\n"
           "                              as Melody keeps it; 0 clears it, and is\n"
           "                              what an unrated track says (also: rate)\n"
           "  albumrating [0-10]          the same, for the playing track's album\n"
           "  cover FILE [SIZE]           write the playing track's cover to FILE,\n"
           "                              SIZE pixels across at most if given\n"
           "  together host SERVER [ROOM [NAME]]\n"
           "                              a room on a melody-together server, named ROOM\n"
           "                              (- or none: a name is made), this\n"
           "                              engine its host (ADR-0302); prints the invite\n"
           "                              to send to friends. The server's password is\n"
           "                              $MELODY_TOGETHER_PASSWORD, else the first line\n"
           "                              of ~/.config/melody/together-password\n"
           "  together join INVITE [NAME] listen along, as NAME (default: $USER)\n"
           "  together leave              out of the room; as its host, closing it\n"
           "  together status             the room: who listens, what plays, and at a\n"
           "                              party what is suggested\n"
           "  together party on|off       as host: friends may suggest tracks and vote\n"
           "  together suggest FILE...    at a party: tracks of your library, suggested\n"
           "  together vote N             for suggestion N, or the vote taken back\n"
           "  together remove N           as host: suggestion N, gone\n"
           "  love | unlove               the playing track, on Last.fm\n"
           "  fav | unfav                 the playing track into the Favorites playlist,\n"
           "                              or out of it (ADR-0297)\n"
           "\n"
           "  replaygain album|track WORDS... [--track-gain] [--true-peak] [--sidecar]\n"
           "                              measure its loudness and write ReplayGain, as\n"
           "                              Melody does: an album gets track and album\n"
           "                              gain, a track or --track-gain only track gain.\n"
           "                              Found as play finds it (also --key KEY). The\n"
           "                              engine holding the files measures and writes;\n"
           "                              --sidecar keeps the values out of the files\n"
           "\n"
           "The engine: --server (or $MELODY_SERVER), else this machine's engine, else\n"
           "one found on the network -- by name with --engine when there are several.\n"
           "--password (or $MELODY_PASSWORD) for an engine on the network, or\n"
           "--password-file (or $MELODY_PASSWORD_FILE) to read it from the first line\n"
           "of a file. --json prints the engine's own answers, for scripts.\n";
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
    // On the main thread, the one that ends the process either way.
    std::exit(EXIT_FAILURE); // NOLINT(concurrency-mt-unsafe)
}

[[nodiscard]] std::filesystem::path local_socket() {
    if (const char* runtime = trackknife::core::environment_variable("XDG_RUNTIME_DIR");
        runtime != nullptr && *runtime != '\0') {
        return std::filesystem::path{runtime} / "melodyd.sock";
    }
    return std::filesystem::temp_directory_path() / "melodyd.sock";
}

// The engines on the network. Their answers come within a second, so that
// long is waited for -- unless one is wanted by name, which ends the wait
// as soon as it has answered.
[[nodiscard]] std::vector<trackknife::discovery::Found>
look_around(const std::string& wanted = {}) {
    auto browser = trackknife::discovery::Browser::start();
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

[[nodiscard]] std::unique_ptr<Client> connect(const Options& options) {
    const auto open = [](const Endpoint& endpoint) {
        auto client = Client::connect(endpoint);
        if (!client) {
            fail("cannot reach the engine at " + endpoint.describe() + ": " +
                 client.error().message);
        }
        // ADR-0260: another protocol is not used; an older engine is, and
        // said so.
        if (auto info = (*client)->call("engine.info")) {
            const auto peer = trackknife::protocol::peer_version(*info);
            const auto said = trackknife::protocol::compatibility_message(
                peer, text_of(*info, "name"), "melody-cli");
            if (trackknife::protocol::compatibility(peer) ==
                trackknife::protocol::Compatibility::incompatible) {
                fail(said);
            }
            if (!said.empty()) {
                std::cerr << "melody-cli: " << said << "\n";
            }
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
        std::erase_if(found,
                      [&options](const auto& engine) { return engine.instance != options.engine; });
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

[[nodiscard]] Json call(Client& client, const std::string& method,
                        const Json& params = Json::object()) {
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
                      static_cast<long long>(seconds / 60 % 60),
                      static_cast<long long>(seconds % 60));
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

// The folder every track of an album is in, from their encoded paths.
[[nodiscard]] std::string album_folder(Client& client, const std::string& album_key) {
    std::optional<std::filesystem::path> common;
    for (const auto& raw : trackknife::cli::album_track_paths(client, album_key)) {
        const auto folder = std::filesystem::path{raw}.parent_path();
        if (!common) {
            common = folder;
            continue;
        }
        std::filesystem::path shared;
        for (auto mine = common->begin(), theirs = folder.begin();
             mine != common->end() && theirs != folder.end() && *mine == *theirs;
             ++mine, ++theirs) {
            shared /= *mine;
        }
        common = shared;
    }
    return common ? common->string() : std::string{};
}

[[nodiscard]] std::string describe_found(const Json& entry, bool album);

// The shell's quoting of one word.
[[nodiscard]] std::string shell_quoted(const std::string& word) {
    std::string quoted{"'"};
    for (const auto character : word) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

// Each album new to the library, as the engine says it once it is whole:
// a line, its folder, or a command run for it.
int watch_albums(Client& client, const Options& options, const std::vector<std::string>& words) {
    bool folders = false;
    std::string run;
    for (std::size_t at = 2; at < words.size(); ++at) {
        if (words[at] == "--folders") {
            folders = true;
        } else if (words[at] == "--run" && at + 1 < words.size()) {
            run = words[++at];
        } else {
            fail("watch albums takes --folders or --run CMD, not " + words[at]);
        }
    }
    std::mutex lock;
    std::condition_variable woken;
    std::vector<Json> arrived;
    bool gone = false;
    client.on_event([&](const trackknife::protocol::Event& event) {
        if (event.name != "catalogue.albums_added") {
            return;
        }
        const std::scoped_lock held{lock};
        for (const auto& album : event.data.value("albums", Json::array())) {
            arrived.push_back(album);
        }
        woken.notify_one();
    });
    client.on_closed([&] {
        const std::scoped_lock held{lock};
        gone = true;
        woken.notify_one();
    });
    const auto decoded = [](const Json& album, const char* key) {
        auto raw = trackknife::protocol::decode_raw_path(text_of(album, key));
        return raw ? *raw : std::string{};
    };
    while (true) {
        std::vector<Json> now;
        {
            std::unique_lock held{lock};
            woken.wait(held, [&] { return gone || !arrived.empty(); });
            if (arrived.empty() && gone) {
                fail("the engine went away");
            }
            now.swap(arrived);
        }
        for (const auto& album : now) {
            const auto folder = decoded(album, "folder");
            if (!run.empty()) {
                auto command = run;
                const auto quoted = shell_quoted(folder);
                if (const auto at = command.find("{}"); at != std::string::npos) {
                    for (auto place = at; place != std::string::npos;
                         place = command.find("{}", place + quoted.size())) {
                        command.replace(place, 2, quoted);
                    }
                } else {
                    command += " " + quoted;
                }
                // One at a time, on this thread, the only one that runs
                // commands: what the environment says is this album's.
                // NOLINTBEGIN(concurrency-mt-unsafe,cert-env33-c)
                ::setenv("MELODY_ALBUM_FOLDER", folder.c_str(), 1);
                ::setenv("MELODY_ALBUM_ARTIST", text_of(album, "artist").c_str(), 1);
                ::setenv("MELODY_ALBUM_TITLE", text_of(album, "album").c_str(), 1);
                ::setenv("MELODY_ALBUM_DATE", text_of(album, "date").c_str(), 1);
                const auto status = std::system(command.c_str());
                // NOLINTEND(concurrency-mt-unsafe,cert-env33-c)
                if (status != 0) {
                    std::cerr << "melody-cli: " << command << " ended with " << status << "\n";
                }
            } else if (options.json) {
                std::cout << album.dump() << std::endl;
            } else if (folders) {
                std::cout << folder << std::endl;
            } else {
                std::cout << describe_found(album, true) << std::endl;
            }
        }
    }
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
        Json params{
            {"kind", albums ? 1 : 2}, {"text", words}, {"offset", offset}, {"limit", wanted}};
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
        if (page.contains("entries") && page["entries"].is_array()) {
            auto& entries = page["entries"].get_ref<Json::array_t&>();
            taken = entries.size();
            for (auto& entry : entries) {
                found.push_back(std::move(entry));
            }
        }
        offset += taken;
        if (!page.value("more", false) || taken == 0U || (limit != 0U && found.size() >= limit)) {
            return found;
        }
    }
}

[[nodiscard]] std::string describe_found(const Json& entry, const bool album) {
    const auto artist = text_of(entry, "artist");
    const auto name = album ? text_of(entry, "album") : first_text(entry, "title", "label");
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
        entries.push_back(Json{{"entry", trackknife::core::StableId::random().to_string()},
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

// What the engine is given to hold: the tracks of the album or the track
// these words find first, each with an identity of its own.
[[nodiscard]] std::vector<Json> entries_for(Client& client, const std::string& kind,
                                            const std::string& words, std::string& chosen) {
    // Chosen by the engine: an album of a random album artist -- so an artist
    // with forty albums is no likelier than one with a single one -- or as
    // many random tracks as asked for.
    if (kind == "random") {
        std::istringstream parts{words};
        std::string what;
        std::string count_text;
        parts >> what >> count_text;
        const auto random = [&client](Json params) {
            params["text"] = "";
            params["random"] = true;
            params["offset"] = 0;
            return call(client, "catalogue.query", params).value("entries", std::vector<Json>{});
        };
        if (what == "album") {
            const auto artists = random(Json{{"kind", 0}, {"limit", 1}});
            if (artists.empty()) {
                fail("the library has no albums");
            }
            const auto albums = random(
                Json{{"kind", 1}, {"artist", text_of(artists.front(), "key")}, {"limit", 1}});
            if (albums.empty()) {
                fail("no album of " + text_of(artists.front(), "label") + " was found");
            }
            chosen = describe_found(albums.front(), true);
            return queue_entries(call(client, "catalogue.query",
                                      Json{{"kind", 2},
                                           {"text", ""},
                                           {"album_key", text_of(albums.front(), "key")},
                                           {"offset", 0},
                                           {"limit", 5'000}})
                                     .value("entries", std::vector<Json>{}));
        }
        if (what == "tracks") {
            const auto count = count_text.empty() ? 25 : std::atoi(count_text.c_str());
            if (count < 1) {
                fail("random tracks wants a count of at least one");
            }
            auto tracks = random(Json{{"kind", 2}, {"limit", count}});
            if (tracks.empty()) {
                fail("the library has no tracks");
            }
            chosen = std::to_string(tracks.size()) + " random tracks";
            return queue_entries(tracks);
        }
        fail("say random album, or random tracks [COUNT]");
    }
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
        const auto page = call(
            client, "catalogue.query",
            Json{{"kind", 2}, {"text", ""}, {"album_key", key}, {"offset", 0}, {"limit", 5'000}});
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
        if (!text_of(known.front(), "album_rating_hash").empty()) {
            track["album_rating_hash"] = text_of(known.front(), "album_rating_hash");
            track["album_rating"] = known.front().value("album_rating", 0);
        }
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
              << state.value("volume_percent", 0) << "% · queue " << state.value("queue_size", 0)
              << " · up next " << state.value("requests", 0);
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
            if (!announced_id.empty() && std::ranges::any_of(engines, [&](const auto& known) {
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
                return !known->gone &&
                       (!engine->id.empty() && !known->id.empty() ? known->id == engine->id
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
    auto browser = options.servers.empty() ? trackknife::discovery::Browser::start()
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
                return std::ranges::any_of(
                    engines, [](const auto& engine) { return engine->fresh || engine->gone; });
            });
            std::erase_if(engines, [](const auto& engine) { return engine->gone; });
            // Playing beats paused beats stopped; then the latest.
            const auto rank = [](const Watched& engine) {
                const auto status = text_of(engine.state, "status");
                return std::tuple{status == "playing"                       ? 2
                                  : !text_of(engine.state, "entry").empty() ? 1
                                                                            : 0,
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
    // ADR-0286: this machine's file; no engine is asked.
    if (command == "config") {
        return trackknife::cli::config_command({words.begin() + 1, words.end()});
    }
    if (command == "engines") {
        const auto found = look_around();
        if (options.json) {
            auto listed = Json::array();
            for (const auto& engine : found) {
                listed.push_back(Json{
                    {"name", engine.instance},
                    {"address", engine.address},
                    {"port", engine.port},
                    {"password", engine.txt.contains("auth") && engine.txt.at("auth") == "1"}});
            }
            std::cout << listed.dump() << "\n";
            return EXIT_SUCCESS;
        }
        for (const auto& engine : found) {
            std::cout << engine.instance << "\t" << engine.address << ":" << engine.port
                      << (engine.txt.contains("auth") && engine.txt.at("auth") == "1" ? "\tpassword"
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
    } else if (command == "current") {
        if (words.size() > 1U) {
            fail("current takes no words; its format is given with --format");
        }
        // The engine formats it: it has the library's whole row for the
        // track -- every tag, ReplayGain, the technicals -- where this has a
        // summary.
        const auto answer = call(*client, "playback.format", Json{{"format", options.format}});
        // Nothing playing is nothing to print, as mpc does: a status bar
        // shows an empty line, a script tests for it.
        if (const auto text = answer.find("text"); text != answer.end() && text->is_string()) {
            std::cout << text->get<std::string>() << "\n";
        }
    } else if ((command == "play" || command == "next") && words.size() == 1U) {
        // Plain play resumes; plain next skips.
        const auto current = state();
        if (command == "next") {
            show(moved_on(call(*client, "playback.next"), text_of(current, "entry")));
        } else if (text_of(current, "status") == "paused") {
            show(started(call(*client, "playback.resume")));
        } else if (text_of(current, "status") == "stopped" && current.value("queue_size", 0) > 0) {
            const auto queue =
                call(*client, "playback.queue").value("entries", std::vector<Json>{});
            show(started(
                call(*client, "playback.play", Json{{"entry", text_of(queue.front(), "entry")}})));
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
        show(call(*client, "playback.seek",
                  Json{{"position_ms", std::max<std::int64_t>(0, target)}}));
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
    } else if (command == "play" && words.size() >= 3U && words[1] == "list") {
        // ADR-0233: the engine's list, played there as the queue.
        const auto wanted = joined(words, 2);
        const auto lists = call(*client, "list.all").value("lists", std::vector<Json>{});
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        };
        std::vector<Json> matches;
        for (const auto& list : lists) {
            if (lower(text_of(list, "name")) == lower(wanted)) {
                matches = {list};
                break;
            }
            bool every = true;
            for (std::size_t index = 2; index < words.size() && every; ++index) {
                every = lower(text_of(list, "name")).find(lower(words[index])) != std::string::npos;
            }
            if (every) {
                matches.push_back(list);
            }
        }
        if (matches.empty()) {
            fail("no list is called " + wanted);
        }
        if (matches.size() > 1U) {
            std::string names;
            for (const auto& list : matches) {
                names += "\n  " + text_of(list, "name");
            }
            fail("more than one list matches " + wanted + ":" + names);
        }
        static_cast<void>(call(*client, "list.play", Json{{"id", text_of(matches.front(), "id")}}));
        show(started(call(*client, "playback.state")));
        if (!options.json) {
            std::cerr << "melody-cli: " << text_of(matches.front(), "name") << "\n";
        }
    } else if (command == "playlists") {
        // ADR-0296: the playlist folder, said or chosen.
        Json answer;
        if (words.size() == 1U) {
            answer = call(*client, "playlists.folder");
        } else if (words[1] == "folder" && words.size() >= 3U) {
            Json params = Json::object();
            const auto& where = words[2];
            if (where == "default") {
                params["folder"] = std::string{};
            } else {
                std::error_code error;
                const auto absolute = std::filesystem::absolute(where, error);
                params["folder"] =
                    trackknife::protocol::encode_raw_path(error ? where : absolute.string());
            }
            for (std::size_t at = 3; at < words.size(); ++at) {
                if (words[at] == "--style" && at + 1 < words.size()) {
                    params["style"] = words[++at];
                } else {
                    fail("playlists folder takes DIR|default [--style root|playlist|absolute]");
                }
            }
            answer = call(*client, "playlists.set_folder", params);
        } else {
            fail("playlists takes nothing, or folder DIR|default [--style STYLE]");
        }
        if (options.json) {
            std::cout << answer.dump() << "\n";
            return EXIT_SUCCESS;
        }
        const auto shown = [&answer](const char* key) {
            auto raw = trackknife::protocol::decode_raw_path(text_of(answer, key));
            return raw ? *raw : std::string{};
        };
        const auto folder = shown("folder");
        std::cout << (folder.empty() ? std::string{"no playlist folder: the engine has no music "
                                                   "folder, and none was chosen"}
                                     : folder)
                  << (shown("chosen").empty() ? " (the default)" : "") << "\n"
                  << "new playlists name their tracks: "
                  << (text_of(answer, "style") == "playlist"   ? "relative to the playlist"
                      : text_of(answer, "style") == "absolute" ? "whole paths"
                                                               : "relative to the music folder")
                  << "\n";
        return EXIT_SUCCESS;
    } else if (command == "lists") {
        const auto lists = call(*client, "list.all").value("lists", std::vector<Json>{});
        if (options.json) {
            std::cout << Json(lists).dump() << "\n";
            return lists.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
        }
        std::string lines;
        for (const auto& list : lists) {
            lines += text_of(list, "name");
            lines += " (" + std::to_string(list.value("tracks", 0)) + " tracks";
            lines += text_of(list, "kind") == "working" ? ", working)" : ")";
            if (options.keys) {
                lines += '\t';
                lines += text_of(list, "id");
            }
            lines += '\n';
        }
        std::cout << lines;
        return lists.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
    } else if ((command == "play" || command == "add" || command == "next" || command == "queue") &&
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
            auto requests =
                call(*client, "playback.requests").value("entries", std::vector<Json>{});
            std::vector<Json> asked;
            asked.reserve(entries.size());
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
    } else if (command == "convert") {
        return trackknife::cli::convert_command(*client, options.json, words);
    } else if (command == "latest" && std::ranges::any_of(words, [](const std::string& word) {
                   return word == "--since" || word == "--folders";
               })) {
        // Every album added after a moment, newest first, its folder if
        // asked: what a cron job wants of what came in since it last ran.
        std::optional<std::int64_t> since;
        bool folders = false;
        std::size_t count = 0;
        for (std::size_t at = 1; at < words.size(); ++at) {
            if (words[at] == "--folders") {
                folders = true;
            } else if (words[at] == "--since" && at + 1 < words.size()) {
                since = trackknife::cli::since_seconds(words[++at]);
                if (!since) {
                    fail("--since wants Unix seconds, a date (2026-10-17, 2026-10-17T18:30) or "
                         "a time ago (90m, 12h, 3d, 2w), not " +
                         words[at]);
                }
            } else if (words[at] == "--since") {
                fail("--since wants a moment: melody-cli latest --since 1d");
            } else {
                count = static_cast<std::size_t>(std::max(1, std::atoi(words[at].c_str())));
            }
        }
        if (!since && count == 0U) {
            count = 20U;
        }
        std::vector<Json> found;
        constexpr std::size_t page = 200;
        for (std::size_t offset = 0;;) {
            Json params{{"kind", 1},
                        {"text", ""},
                        {"offset", offset},
                        {"limit", page},
                        {"newest_first", true}};
            if (!options.json) {
                params["fields"] = Json::array({"artist", "album", "date", "added"});
            }
            auto answer = call(*client, "catalogue.query", params);
            const auto entries = answer.value("entries", std::vector<Json>{});
            bool older = false;
            for (const auto& entry : entries) {
                if (since && entry.value("added", std::int64_t{0}) <= *since) {
                    older = true;
                    break;
                }
                found.push_back(entry);
                if (count != 0U && found.size() >= count) {
                    break;
                }
            }
            offset += entries.size();
            if (older || entries.empty() || !answer.value("more", false) ||
                (count != 0U && found.size() >= count)) {
                break;
            }
        }
        if (folders) {
            for (auto& entry : found) {
                entry["folder"] = album_folder(*client, text_of(entry, "key"));
            }
        }
        if (options.json) {
            std::cout << Json(found).dump() << "\n";
        } else {
            std::string lines;
            for (const auto& entry : found) {
                lines += folders ? text_of(entry, "folder") : describe_found(entry, true);
                if (options.keys) {
                    lines += '\t';
                    lines += text_of(entry, "key");
                }
                lines += '\n';
            }
            std::cout << lines;
        }
        // Nothing new since is no failure: a cron job runs on a quiet day too.
        return since || !found.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
    } else if (command == "albums" || command == "tracks" || command == "latest") {
        const bool albums = command != "tracks";
        const auto text = command == "latest" ? std::string{} : joined(words, 1);
        // Everything that matches; only latest is a count.
        std::size_t limit = 0;
        if (command == "latest") {
            limit = words.size() > 1U
                        ? static_cast<std::size_t>(std::max(1, std::atoi(words[1].c_str())))
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
            for (const auto& entry : found) {
                listed.push_back(entry);
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
    } else if (command == "find") {
        if (words.size() < 2U) {
            fail("find wants a query: melody-cli find 'artist HAS doors'");
        }
        // The engine runs the query and formats each track with its whole
        // library row; this prints what it says.
        const auto answer = call(*client, "catalogue.find",
                                 Json{{"query", joined(words, 1)}, {"format", options.format}});
        const auto tracks = answer.value("tracks", std::vector<Json>{});
        if (options.json) {
            std::cout << Json(tracks).dump() << "\n";
            return tracks.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
        }
        std::string lines;
        for (const auto& track : tracks) {
            lines += text_of(track, "text");
            if (options.keys) {
                lines += '\t';
                lines += text_of(track, "key");
            }
            lines += '\n';
        }
        std::cout << lines;
        return tracks.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
    } else if (command == "revision") {
        // The library's revision: a picker keeps its list while it stays
        // the same. An engine too old to say fails, and it lists afresh.
        std::cout << text_of(call(*client, "catalogue.revision"), "revision") << "\n";
    } else if (command == "outputs") {
        const auto outputs = call(*client, "outputs.list").value("outputs", std::vector<Json>{});
        if (options.json) {
            std::cout << Json(outputs).dump() << "\n";
            return EXIT_SUCCESS;
        }
        for (const auto& output : outputs) {
            std::cout << (output.value("selected", false) ? "* " : "  ") << text_of(output, "name")
                      << (output.value("online", true) ? "" : " (offline)") << "\n";
        }
    } else if (command == "output") {
        const auto name = joined(words, 1);
        if (name.empty()) {
            fail("output wants the name of the speakers, as outputs lists them");
        }
        const auto outputs = call(*client, "outputs.list").value("outputs", std::vector<Json>{});
        const auto found = std::ranges::find_if(outputs, [&name](const Json& output) {
            return text_of(output, "name") == name || text_of(output, "id") == name;
        });
        if (found == outputs.end()) {
            fail("no output called \"" + name + "\"; see melody-cli outputs");
        }
        static_cast<void>(call(*client, "outputs.select", Json{{"id", text_of(*found, "id")}}));
        show(state());
    } else if (command == "watch" && words.size() >= 2U && words[1] == "albums") {
        return watch_albums(*client, options, words);
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
    } else if (command == "rating" || command == "rate" || command == "albumrating") {
        // As the engine keeps them: 1-10, 0 for none. The album's is keyed
        // by the album, found through the track that plays.
        const bool album = command == "albumrating";
        const auto track = now_playing(*client, state());
        if (track.is_null()) {
            fail("nothing is playing");
        }
        const auto hash_field = album ? "album_rating_hash" : "rating_hash";
        const auto rating_field = album ? "album_rating" : "rating";
        if (!track.contains(hash_field)) {
            fail(album ? "what plays is not in the library, or its engine is older and does "
                         "not say its album's key, so the album has no rating here"
                       : "what plays is not in the library, so it has no rating");
        }
        if (words.size() == 1U) {
            const auto rating = track.value(rating_field, 0);
            if (options.json) {
                std::cout << Json{{"rating", rating}}.dump() << "\n";
            } else {
                std::cout << rating << "\n";
            }
            return EXIT_SUCCESS;
        }
        const auto& given = words[1];
        int rating = -1;
        const auto* const end_of = given.data() + given.size();
        if (const auto [rest, error] = std::from_chars(given.data(), end_of, rating);
            error != std::errc{} || rest != end_of || rating < 0 || rating > 10) {
            fail(command + " wants a whole number from 0 to 10 (0 clears it)");
        }
        static_cast<void>(
            call(*client, "catalogue.set_rating",
                 Json{{"hash", text_of(track, hash_field)}, {"album", album}, {"rating", rating}}));
        if (!options.json) {
            const auto what = album ? text_of(track, "album") : describe_track(track);
            std::cout << (rating == 0 ? std::string{"cleared: "}
                                      : "rated " + std::to_string(rating) + ": ")
                      << (what.empty() ? describe_track(track) : what) << "\n";
        }
    } else if (command == "replaygain") {
        // ADR-0237: measured and written by the engine that holds the files,
        // exactly as Trackknife's ReplayGain window does it.
        std::vector<std::string> rest;
        bool true_peak = false;
        bool sidecar = false;
        bool track_gains = false;
        for (std::size_t index = 1; index < words.size(); ++index) {
            if (words[index] == "--true-peak") {
                true_peak = true;
            } else if (words[index] == "--sidecar") {
                sidecar = true;
            } else if (words[index] == "--track-gain") {
                track_gains = true;
            } else {
                rest.push_back(words[index]);
            }
        }
        if (rest.size() < 2U) {
            fail("replaygain wants album or track, then the words to find it by");
        }
        std::string chosen;
        const auto entries = entries_for(*client, rest[0], joined(rest, 1), chosen);
        if (entries.empty()) {
            fail("nothing to measure in " + chosen);
        }
        auto paths = Json::array();
        for (const auto& entry : entries) {
            paths.push_back(text_of(entry, "path"));
        }
        // An album is one programme; a track, or --track-gain, has none.
        const auto* grouping = rest[0] == "track" || track_gains ? "track" : "album";
        const bool interactive = !options.json && ::isatty(STDERR_FILENO) == 1;
        if (!options.json) {
            std::cerr << "melody-cli: " << chosen << "\n";
        }
        auto outcome = client->run_job("replaygain.apply",
                                       Json{{"paths", std::move(paths)},
                                            {"grouping", grouping},
                                            {"true_peak", true_peak},
                                            {"sidecar", sidecar}},
                                       [interactive](const Json& progress) {
                                           if (!interactive) {
                                               return;
                                           }
                                           std::cerr << "\r\033[K" << text_of(progress, "phase")
                                                     << ' ' << progress.value("completed", 0) << '/'
                                                     << progress.value("total", 0) << std::flush;
                                       });
        if (interactive) {
            std::cerr << "\r\033[K";
        }
        if (!outcome) {
            fail(outcome.error().message);
        }
        if (const auto error = outcome->find("error"); error != outcome->end()) {
            fail(error->value("message", std::string{"the engine could not do it"}));
        }
        const auto result = outcome->value("result", Json::object());
        if (options.json) {
            std::cout << result.dump() << "\n";
            return result.contains("refused") ? EXIT_FAILURE : EXIT_SUCCESS;
        }
        // Tag text travels as a string, or as {"bytes"} when it is no UTF-8.
        const auto text = [](const Json& value) -> std::string {
            if (value.is_string()) {
                return value.get<std::string>();
            }
            auto raw = trackknife::protocol::decode_raw_path(value.value("bytes", std::string{}));
            return raw ? trackknife::core::display_raw_path(*raw) : std::string{};
        };
        const auto shown_path = [](const std::string& encoded) {
            auto raw = trackknife::protocol::decode_raw_path(encoded);
            return raw ? trackknife::core::display_raw_path(*raw) : std::string{"a file"};
        };
        std::string lines;
        for (const auto& track : result.value("tracks", Json::array())) {
            const auto gain = text_of(track, "track_gain");
            auto title = text(track.value("title", Json{}));
            if (title.empty()) {
                title = shown_path(text_of(track, "path"));
            }
            lines += gain.empty() ? std::string{"-"} : gain;
            if (const auto album_gain = text_of(track, "album_gain"); !album_gain.empty()) {
                lines += "  album " + album_gain;
            }
            lines += "  " + title + "\n";
        }
        std::cout << lines;
        for (const auto& problem : result.value("problems", Json::array())) {
            const auto path = problem.find("path");
            const auto where = path != problem.end() && path->is_string()
                                   ? shown_path(path->get<std::string>())
                                   : std::string{"the album"};
            std::cerr << "melody-cli: " << where << ": " << text(problem.value("message", Json{}))
                      << "\n";
        }
        if (result.contains("refused")) {
            fail("nothing was written: not every file could take its gain");
        }
        const auto written = result.value("written", 0);
        const auto not_written = result.value("failed", 0);
        std::cerr << "melody-cli: wrote ReplayGain to " << written
                  << (written == 1 ? " file" : " files");
        if (not_written > 0) {
            std::cerr << ", " << not_written << " failed";
        }
        std::cerr << "\n";
        return not_written > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    } else if (command == "cover") {
        // The playing track's cover, into a file -- for a bar widget, which
        // shows a file. Written whole, then renamed into place, so what
        // reads it never reads half of one.
        if (words.size() < 2U || words.size() > 3U) {
            fail("cover wants a file to write it to, and a size in pixels to have it smaller");
        }
        int size = 0;
        if (words.size() == 3U) {
            const auto& given = words[2];
            const auto* const end_of = given.data() + given.size();
            if (const auto [rest, error] = std::from_chars(given.data(), end_of, size);
                error != std::errc{} || rest != end_of || size <= 0) {
                fail("cover's size is a number of pixels");
            }
        }
        const auto path = text_of(state(), "path");
        if (path.empty()) {
            fail("nothing is playing");
        }
        Json params{{"path", path}};
        if (size > 0) {
            params["size"] = size;
        }
        const auto answer = call(*client, "catalogue.artwork", params);
        const auto image = answer.find("image");
        if (image == answer.end() || !image->is_string()) {
            fail("what plays has no cover");
        }
        auto bytes = trackknife::protocol::decode_raw_path(image->get<std::string>());
        if (!bytes) {
            fail("the engine sent a cover that does not decode");
        }
        const std::filesystem::path target{words[1]};
        auto written = target;
        written += ".part";
        {
            std::ofstream file{written, std::ios::binary | std::ios::trunc};
            file.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
            if (!file.good()) {
                fail("cannot write " + written.string());
            }
        }
        std::error_code error;
        std::filesystem::rename(written, target, error);
        if (error) {
            std::filesystem::remove(written, error);
            fail("cannot write " + target.string());
        }
        if (!options.json) {
            std::cout << target.string() << "\n";
        }
    } else if (command == "together") {
        // ADR-0302: the engine hosts or follows; this only asks it to.
        const auto what = words.size() > 1U ? words[1] : std::string{"status"};
        const auto show_room = [&options](const Json& room) {
            if (options.json) {
                std::cout << room.dump() << "\n";
                return;
            }
            const auto role = text_of(room, "role");
            std::cout << (role.empty() ? "not in a room" : role == "host" ? "hosting" : "listening")
                      << "\n";
            if (!text_of(room, "invite").empty() && role == "host") {
                std::cout << "invite: " << text_of(room, "invite") << "\n";
            }
            if (!text_of(room, "track").empty()) {
                std::cout << "playing: " << text_of(room, "track") << "\n";
            }
            std::string listeners;
            for (const auto& name : room.value("listeners", Json::array())) {
                if (name.is_string()) {
                    listeners += (listeners.empty() ? "" : ", ") + name.get<std::string>();
                }
            }
            if (!listeners.empty()) {
                std::cout << "listening: " << listeners << "\n";
            }
            if (room.value("party", false)) {
                std::cout << "a party\n";
                for (const auto& each : room.value("suggestions", Json::array())) {
                    std::cout << "  " << each.value("id", 0) << ". " << text_of(each, "artist")
                              << " \u2014 " << text_of(each, "title") << " (" << text_of(each, "name")
                              << ", " << each.value("votes", 0) << " votes"
                              << (each.value("voted", false) ? ", yours" : "")
                              << (each.value("ready", false) ? "" : ", its copy on its way")
                              << ")\n";
                }
            }
            if (!text_of(room, "message").empty()) {
                std::cout << text_of(room, "message") << "\n";
            }
        };
        if (what == "host") {
            if (words.size() < 3U || words.size() > 5U) {
                fail("together host wants the server's address: https://together.example.org, "
                     "a name for the room (- for one made), and yours at a party");
            }
            std::string password;
            if (const char* given = trackknife::core::environment_variable("MELODY_TOGETHER_PASSWORD");
                given != nullptr) {
                password = given;
            } else if (const char* home = trackknife::core::environment_variable("HOME");
                       home != nullptr) {
                const char* config = trackknife::core::environment_variable("XDG_CONFIG_HOME");
                const std::filesystem::path base =
                    config != nullptr && *config != '\0'
                        ? std::filesystem::path{config}
                        : std::filesystem::path{home} / ".config";
                if (auto read = trackknife::protocol::read_password_file(
                        base / "melody" / "together-password")) {
                    password = *read;
                }
            }
            if (password.empty()) {
                fail("the server's password: $MELODY_TOGETHER_PASSWORD, or "
                     "~/.config/melody/together-password");
            }
            const std::string room =
                words.size() >= 4U && words[3] != "-" ? words[3] : std::string{};
            std::string name = words.size() == 5U ? words[4] : std::string{};
            if (const char* user = trackknife::core::environment_variable("USER");
                name.empty() && user != nullptr) {
                name = user;
            }
            const auto answer = call(*client, "together.host",
                                     Json{{"server", words[2]},
                                          {"password", password},
                                          {"name", name},
                                          {"room", room}});
            if (options.json) {
                std::cout << answer.dump() << "\n";
            } else {
                std::cout << text_of(answer, "invite") << "\n";
            }
        } else if (what == "join") {
            if (words.size() < 3U || words.size() > 4U) {
                fail("together join wants the invite a friend sent, and a name to be known by");
            }
            std::string name = words.size() == 4U ? words[3] : std::string{};
            if (const char* user = trackknife::core::environment_variable("USER");
                name.empty() && user != nullptr) {
                name = user;
            }
            static_cast<void>(
                call(*client, "together.join", Json{{"invite", words[2]}, {"name", name}}));
            show_room(call(*client, "together.state"));
        } else if (what == "leave") {
            static_cast<void>(call(*client, "together.leave"));
            if (!options.json) {
                std::cout << "left the room\n";
            }
        } else if (what == "status") {
            show_room(call(*client, "together.state"));
        } else if (what == "party") {
            if (words.size() != 3U || (words[2] != "on" && words[2] != "off")) {
                fail("together party is on or off");
            }
            show_room(call(*client, "together.party", Json{{"party", words[2] == "on"}}));
        } else if (what == "suggest") {
            if (words.size() < 3U) {
                fail("together suggest wants the files to suggest");
            }
            for (std::size_t index = 2; index < words.size(); ++index) {
                // As the shell gave it, a path of this machine's.
                const auto absolute = std::filesystem::absolute(words[index]).string();
                const auto answer = call(*client, "together.suggest",
                                         Json{{"path", trackknife::protocol::encode_raw_path(absolute)}});
                if (options.json) {
                    std::cout << answer.dump() << "\n";
                } else {
                    std::cout << "suggested " << answer.value("id", 0) << ": " << words[index]
                              << "\n";
                }
            }
        } else if (what == "vote" || what == "remove") {
            std::uint64_t id = 0;
            if (words.size() != 3U ||
                std::from_chars(words[2].data(), words[2].data() + words[2].size(), id).ec !=
                    std::errc{}) {
                fail("together " + what + " wants a suggestion's number");
            }
            static_cast<void>(call(*client, what == "vote" ? "together.vote" : "together.remove",
                                   Json{{"id", id}}));
            if (!options.json) {
                std::cout << (what == "vote" ? "voted\n" : "removed\n");
            }
        } else {
            fail("together is host, join, leave, status, party, suggest, vote or remove");
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
    } else if (command == "fav" || command == "unfav") {
        const auto track = now_playing(*client, state());
        if (track.is_null()) {
            fail("nothing is playing");
        }
        // ADR-0297: what the engine plays, in its Favorites playlist or not.
        const auto answer = call(*client, "favorites.toggle", Json{{"favorite", command == "fav"}});
        if (options.json) {
            std::cout << answer.dump() << "\n";
        } else {
            std::cout << (answer.value("favorite", false) ? "a favourite: " : "not a favourite: ")
                      << describe_track(track) << "\n";
        }
    } else {
        usage(std::cerr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace

namespace {

int run(int argc, char** argv) {
    Options options;
    if (const char* server = trackknife::core::environment_variable("MELODY_SERVER");
        server != nullptr) {
        options.server = server;
    }
    if (const char* password = trackknife::core::environment_variable("MELODY_PASSWORD");
        password != nullptr) {
        options.password = password;
    } else if (const char* file = trackknife::core::environment_variable("MELODY_PASSWORD_FILE");
               file != nullptr) {
        options.password_file = file;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto value = [&]() -> std::string {
            if (index + 1 >= argc) {
                fail(std::string{argument} + " wants a value");
            }
            return argv[++index];
        };
        // Anywhere on the line: `find QUERY --format ... --keys` reads
        // naturally.
        if (argument == "--format") {
            options.format = value();
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--keys") {
            options.keys = true;
        } else if (!options.words.empty()) {
            options.words.emplace_back(argument);
        } else if (argument == "--server") {
            options.server = value();
            options.servers.push_back(options.server);
        } else if (argument == "--password") {
            options.password = value();
            options.password_file.clear();
        } else if (argument == "--password-file") {
            options.password_file = value();
        } else if (argument == "--engine") {
            options.engine = value();
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
    if (!options.password_file.empty()) {
        auto read = trackknife::protocol::read_password_file(options.password_file);
        if (!read) {
            fail(read.error().message);
        }
        options.password = std::move(*read);
    }
    return run(options);
}

} // namespace

int main(int argc, char** argv) {
    // Anything thrown is said, and the program fails, rather than ending in
    // terminate().
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "melody-cli: " << error.what() << "\n";
    } catch (...) {
        std::cerr << "melody-cli: an unknown error\n";
    }
    return EXIT_FAILURE;
}
