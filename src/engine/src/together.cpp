// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/engine/together.hpp"

#include "trackknife/protocol/message.hpp"
#include "trackknife/query/tkq.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace trackknife::engine {
namespace {

using protocol::Json;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

// How far apart a listener and the room may drift before it seeks back, and
// how long it lets a seek settle before judging it.
constexpr std::int64_t drift_ms = 250;
constexpr auto settle = std::chrono::seconds{2};
// A seek lands a moment after it is asked: aimed that much ahead.
constexpr std::int64_t seek_lead_ms = 120;
// The host's anchor moves this far before it is a seek, not jitter.
constexpr std::int64_t moved_ms = 750;
constexpr auto clock_again = std::chrono::minutes{5};
constexpr auto presence_every = std::chrono::seconds{5};

[[nodiscard]] core::Error together_error(std::string message,
                                         const core::ErrorCode code = core::ErrorCode::backend) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

std::size_t collect(char* data, const std::size_t size, const std::size_t count, void* into) {
    static_cast<std::string*>(into)->append(data, size * count);
    return size * count;
}

struct Answer final {
    long status{0};
    Json body;
};

// One request to the server, its JSON answer.
[[nodiscard]] core::Result<Answer> request(const std::string& method, const std::string& url,
                                           const std::string& key, const std::string& body,
                                           const long timeout_ms) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
    if (!curl) {
        return std::unexpected(together_error("could not start an HTTP request"));
    }
    std::string response;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!key.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
    }
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> held{headers,
                                                                           curl_slist_free_all};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    if (method != "GET") {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5'000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "melodyd");
    if (const auto performed = curl_easy_perform(curl.get()); performed != CURLE_OK) {
        return std::unexpected(
            together_error(std::string{"the server is not reached: "} +
                               curl_easy_strerror(performed),
                           core::ErrorCode::io));
    }
    Answer answer;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &answer.status);
    answer.body = Json::parse(response, nullptr, false);
    if (answer.body.is_discarded()) {
        answer.body = Json::object();
    }
    return answer;
}

[[nodiscard]] std::string escaped(const std::string& value) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
    if (!curl) {
        return {};
    }
    auto* done = curl_easy_escape(curl.get(), value.c_str(), static_cast<int>(value.size()));
    std::string out{done != nullptr ? done : ""};
    curl_free(done);
    return out;
}

[[nodiscard]] std::int64_t local_ms() {
    return std::chrono::duration_cast<milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::string tkq_quoted(const std::string& value) {
    std::string out{"\""};
    for (const auto character : value) {
        if (character == '"') {
            out += "\"\"";
        } else {
            out += character;
        }
    }
    return out + "\"";
}

[[nodiscard]] std::string first(const persistence::TkqRowFacts& facts, const char* name) {
    const auto found = facts.fields.find(name);
    return found != facts.fields.end() && !found->second.empty() ? found->second.front().first
                                                                  : std::string{};
}

[[nodiscard]] std::string text(const Json& object, const char* name) {
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

[[nodiscard]] std::int64_t number(const Json& object, const char* name) {
    const auto found = object.find(name);
    return found != object.end() && found->is_number() ? found->get<std::int64_t>() : 0;
}

struct CloseFile final {
    void operator()(std::FILE* file) const { std::fclose(file); }
};
using File = std::unique_ptr<std::FILE, CloseFile>;

// Long, and stopped when the session ends: a track's copy, up or down.
// Aborted when `stop` is set; slower than a kilobyte a second for half a
// minute is a line that has gone.
int still_wanted(void* stop, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic_bool*>(stop)->load() ? 1 : 0;
}

void long_transfer(CURL* curl, const std::atomic_bool& stop) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, still_wanted);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic_bool*>(&stop));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1'000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5'000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "melodyd");
}

[[nodiscard]] bool put_file(const std::string& url, const std::string& key,
                            const std::filesystem::path& file, const std::atomic_bool& stop) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
    const File input{std::fopen(file.c_str(), "rb")};
    std::error_code unsized;
    const auto size = std::filesystem::file_size(file, unsized);
    if (!curl || !input || unsized) {
        return false;
    }
    curl_slist* headers = curl_slist_append(nullptr, ("Authorization: Bearer " + key).c_str());
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> held{headers,
                                                                           curl_slist_free_all};
    std::string ignored;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, input.get());
    curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ignored);
    long_transfer(curl.get(), stop);
    long status = 0;
    return curl_easy_perform(curl.get()) == CURLE_OK &&
           curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
           status == 200;
}

std::size_t write_file(char* data, const std::size_t size, const std::size_t count, void* into) {
    return std::fwrite(data, size, count, static_cast<std::FILE*>(into)) * size;
}

[[nodiscard]] bool get_file(const std::string& url, const std::string& key,
                            const std::filesystem::path& target, const std::atomic_bool& stop) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
    auto partial = target;
    partial += ".partial";
    File output{std::fopen(partial.c_str(), "wb")};
    if (!curl || !output) {
        return false;
    }
    curl_slist* headers = curl_slist_append(nullptr, ("Authorization: Bearer " + key).c_str());
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> held{headers,
                                                                           curl_slist_free_all};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, output.get());
    long_transfer(curl.get(), stop);
    long status = 0;
    const bool fetched = curl_easy_perform(curl.get()) == CURLE_OK &&
                         curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) ==
                             CURLE_OK &&
                         status == 200;
    output.reset();
    std::error_code ignored;
    if (!fetched) {
        std::filesystem::remove(partial, ignored);
        return false;
    }
    std::filesystem::rename(partial, target, ignored);
    return !ignored;
}

// Errands done one at a time, apart from the session's own round: a track's
// copy, up or down, takes as long as the line, and keeping in step does not
// wait for it.
class Errands final {
  public:
    Errands() : worker_{[this] { work(); }} {}
    Errands(const Errands&) = delete;
    Errands& operator=(const Errands&) = delete;
    ~Errands() { stop(); }
    void post(std::function<void()> errand) {
        {
            const std::lock_guard guard{mutex_};
            errands_.push_back(std::move(errand));
        }
        posted_.notify_one();
    }
    void stop() {
        {
            const std::lock_guard guard{mutex_};
            stopping_ = true;
            errands_.clear();
        }
        posted_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

  private:
    void work() {
        for (;;) {
            std::function<void()> errand;
            {
                std::unique_lock lock{mutex_};
                posted_.wait(lock, [this] { return stopping_ || !errands_.empty(); });
                if (stopping_) {
                    return;
                }
                errand = std::move(errands_.front());
                errands_.pop_front();
            }
            errand();
        }
    }
    std::mutex mutex_;
    std::condition_variable posted_;
    std::deque<std::function<void()>> errands_;
    bool stopping_{false};
    std::thread worker_;
};

} // namespace

struct Together::Session final {
    Invite invite;
    bool hosting{false};
    std::string name;
    InterruptiblePause pause;
    std::atomic_bool ended{false};
    // The server's clock, as this one's plus this.
    std::int64_t offset_ms{0};
    steady_clock::time_point measured;
    std::uint64_t revision{0};
    // Hosting: what was last said, to say only what changed.
    std::string said_key;
    std::int64_t said_anchor{0};
    std::int64_t said_position{0};
    Json described_track;
    core::StableId described_entry;
    steady_clock::time_point presence_asked;
    // Listening: what the room plays, and what of it this engine has.
    Json desired = Json::object();
    std::string applied_key;
    core::StableId ours;
    bool found{false};
    steady_clock::time_point last_seek;
    // Phase 2. The host: tracks sent, or on their way. A listener: what the
    // room has copies of, those fetched (by entry, the file), those on their
    // way, and where its library has each entry -- empty when it has not.
    std::atomic_bool stopping{false};
    std::mutex copies;
    std::set<std::string> sent;
    std::set<std::string> sending;
    std::set<std::string> files;
    std::map<std::string, std::string> fetched;
    std::set<std::string> fetching;
    std::map<std::string, std::string> library;
    std::map<std::string, Json> descriptions;
    bool waiting{false};
    std::filesystem::path downloads;
    // Last: stopped first, before anything its errands use is gone.
    Errands errands;

    [[nodiscard]] std::int64_t server_now() const { return local_ms() + offset_ms; }
    [[nodiscard]] std::string url(const std::string& path) const {
        return invite.server + "/rooms/" + invite.room + path;
    }
};

std::optional<Together::Invite> Together::parse_invite(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    const auto rooms = text.rfind("/rooms/");
    const auto hash = text.find('#', rooms == std::string_view::npos ? 0 : rooms);
    if (rooms == std::string_view::npos || hash == std::string_view::npos ||
        !(text.starts_with("http://") || text.starts_with("https://"))) {
        return std::nullopt;
    }
    Invite invite{.server = std::string{text.substr(0, rooms)},
                  .room = std::string{text.substr(rooms + 7U, hash - rooms - 7U)},
                  .key = std::string{text.substr(hash + 1U)}};
    if (invite.room.empty() || invite.key.empty() ||
        invite.room.find('/') != std::string::npos) {
        return std::nullopt;
    }
    return invite;
}

std::string Together::invite_text(const Invite& invite) {
    return invite.server + "/rooms/" + invite.room + "#" + invite.key;
}

std::vector<std::string> Together::track_queries(const Json& track) {
    std::vector<std::string> queries;
    if (const auto id = text(track, "musicbrainztrackid"); !id.empty()) {
        queries.push_back("musicbrainztrackid IS " + tkq_quoted(id));
    }
    const auto title = text(track, "title");
    auto artist = text(track, "artist");
    if (artist.empty()) {
        artist = text(track, "albumartist");
    }
    if (title.empty() || artist.empty()) {
        return queries;
    }
    const auto by = "title IS " + tkq_quoted(title) + " AND (artist IS " + tkq_quoted(artist) +
                    " OR albumartist IS " + tkq_quoted(artist) + ")";
    if (const auto album = text(track, "album"); !album.empty()) {
        queries.push_back(by + " AND album IS " + tkq_quoted(album));
    }
    queries.push_back(by);
    return queries;
}

Together::Together(Player& player, Catalogue& catalogue, EventSink sink,
                   std::filesystem::path directory, TranscodeCache* transcodes)
    : player_(&player), catalogue_(&catalogue), sink_(std::move(sink)),
      directory_(std::move(directory)), transcodes_(transcodes) {
    // An engine that streams to no agent keeps no transcodes; the host's
    // copies are made in a cache of their own.
    if (transcodes_ == nullptr) {
        own_transcodes_ =
            std::make_unique<TranscodeCache>(directory_ / "transcodes", 1024ULL * 1024U * 1024U);
        transcodes_ = own_transcodes_.get();
    }
}

Together::~Together() { leave(); }

void Together::leave() {
    const std::lock_guard control{control_};
    end();
}

core::Result<void> Together::measure_clock(Session& session) {
    // The best of a few: the shortest round trip says the most about when the
    // server read its clock.
    std::optional<std::pair<std::int64_t, std::int64_t>> best; // round trip, offset
    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto sent = local_ms();
        auto answered = request("GET", session.invite.server + "/time", {}, {}, 5'000L);
        const auto received = local_ms();
        if (!answered || answered->status != 200) {
            continue;
        }
        const auto trip = received - sent;
        const auto offset = number(answered->body, "ms") - (sent + received) / 2;
        if (!best || trip < best->first) {
            best = std::pair{trip, offset};
        }
    }
    if (!best) {
        return std::unexpected(together_error("the server is not reached", core::ErrorCode::io));
    }
    session.offset_ms = best->second;
    session.measured = steady_clock::now();
    return {};
}

core::Result<std::string> Together::host(const std::string& server, const std::string& password) {
    const std::lock_guard control{control_};
    end();
    auto session = std::make_shared<Session>();
    auto base = server;
    while (base.ends_with('/')) {
        base.pop_back();
    }
    session->invite.server = base;
    session->hosting = true;
    auto made = request("POST", base + "/rooms", password, {}, 10'000L);
    if (!made) {
        return std::unexpected(made.error());
    }
    if (made->status != 200) {
        return std::unexpected(together_error(
            made->status == 403 ? "the server's password is not that"
                                : "the server made no room (" + std::to_string(made->status) + ")",
            made->status == 403 ? core::ErrorCode::unauthorized : core::ErrorCode::backend));
    }
    session->invite.room = text(made->body, "room");
    session->invite.key = text(made->body, "host_key");
    const Invite shared{.server = base,
                        .room = session->invite.room,
                        .key = text(made->body, "invite")};
    if (auto measured = measure_clock(*session); !measured) {
        return std::unexpected(measured.error());
    }
    const auto invite = invite_text(shared);
    {
        const std::lock_guard guard{mutex_};
        session_ = session;
        status_ = Status{.role = "host", .invite = invite, .listeners = {}, .track = {},
                         .message = {}};
    }
    worker_ = std::thread{[this, session] { run(session); }};
    changed();
    return invite;
}

core::Result<void> Together::join(const std::string& invite, const std::string& name) {
    const std::lock_guard control{control_};
    end();
    auto parsed = parse_invite(invite);
    if (!parsed) {
        return std::unexpected(together_error(
            "an invite is the server's address, /rooms/ROOM and #KEY",
            core::ErrorCode::invalid_argument));
    }
    auto session = std::make_shared<Session>();
    session->invite = *parsed;
    session->name = name;
    session->downloads = directory_ / "downloads";
    std::error_code unmade;
    std::filesystem::remove_all(session->downloads, unmade);
    std::filesystem::create_directories(session->downloads, unmade);
    // Asked once now, so a wrong invite is said to whoever gave it.
    auto first_look = request("GET", session->url("/state?after=0&wait=0"), parsed->key, {},
                              10'000L);
    if (!first_look) {
        return std::unexpected(first_look.error());
    }
    if (first_look->status != 200) {
        return std::unexpected(together_error("the invite opens no room",
                                              core::ErrorCode::unauthorized));
    }
    if (auto measured = measure_clock(*session); !measured) {
        return std::unexpected(measured.error());
    }
    {
        const std::lock_guard guard{mutex_};
        session_ = session;
        status_ = Status{.role = "listener", .invite = invite, .listeners = {}, .track = {},
                         .message = {}};
    }
    worker_ = std::thread{[this, session] { run(session); }};
    changed();
    return {};
}

void Together::end() {
    std::shared_ptr<Session> session;
    {
        const std::lock_guard guard{mutex_};
        session = std::exchange(session_, nullptr);
    }
    if (session) {
        session->stopping = true;
        session->pause.interrupt();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (!session) {
        return;
    }
    session->errands.stop();
    // Said to the room on the way out, never waited on for long.
    if (session->hosting) {
        static_cast<void>(request("DELETE", session->url(""), session->invite.key, {}, 3'000L));
    } else if (!session->ended.load()) {
        static_cast<void>(request("POST", session->url("/leave?name=" + escaped(session->name)),
                                  session->invite.key, {}, 3'000L));
        if (session->found && player_->state().entry == session->ours) {
            static_cast<void>(player_->stop());
        }
    }
    if (!session->downloads.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(session->downloads, ignored);
    }
    {
        const std::lock_guard guard{mutex_};
        status_ = Status{};
    }
    changed();
}

Together::Status Together::status() const {
    const std::lock_guard guard{mutex_};
    return status_;
}

void Together::changed() {
    if (!sink_) {
        return;
    }
    const auto now = status();
    sink_(protocol::Event{.name = "together.changed",
                          .data = Json{{"role", now.role},
                                       {"invite", now.invite},
                                       {"listeners", now.listeners},
                                       {"track", now.track},
                                       {"message", now.message}}});
}

void Together::say(std::string message) {
    {
        const std::lock_guard guard{mutex_};
        if (status_.message == message) {
            return;
        }
        status_.message = std::move(message);
    }
    changed();
}

void Together::run(const std::shared_ptr<Session> session) {
    // A listener asks the room every second -- answered at once when it
    // changes -- and keeps in step between; a host looks four times a second.
    const auto interval = session->hosting ? milliseconds{250} : milliseconds{0};
    do {
        if (steady_clock::now() - session->measured > clock_again) {
            static_cast<void>(measure_clock(*session));
        }
        if (session->hosting) {
            host_round(*session);
        } else {
            listen_round(*session);
        }
    } while (!session->ended.load() && session->pause.wait(interval));
}

Json Together::describe(Session& session, const QueueEntry& entry) {
    const auto id = entry.entry_id.to_string();
    if (const auto known = session.descriptions.find(id); known != session.descriptions.end()) {
        return known->second;
    }
    // The track as a listener's library can find it, read once.
    Json described = Json::object();
    if (auto tracks = catalogue_->cached_tracks({entry.source.raw_path});
        tracks && !tracks->empty()) {
        const auto& facts = tracks->front().facts;
        for (const auto* name : {"title", "artist", "album", "albumartist", "musicbrainztrackid",
                                 "tracknumber", "discnumber"}) {
            if (auto value = first(facts, name); !value.empty()) {
                described[name] = protocol::displayable_text(value);
            }
        }
    }
    if (!described.contains("title")) {
        described["title"] = protocol::displayable_text(entry.title);
        described["artist"] = protocol::displayable_text(
            entry.group.artist.empty() ? entry.group.album_artist : entry.group.artist);
        described["album"] = protocol::displayable_text(entry.group.album);
    }
    if (entry.duration_ms) {
        described["duration_ms"] = *entry.duration_ms;
    }
    session.descriptions[id] = described;
    return described;
}

void Together::host_round(Session& session) {
    const auto state = player_->state();
    const bool playing = state.status == "playing";
    const bool paused = state.status == "paused";
    const auto current = playing || paused ? player_->entry(state.entry) : std::nullopt;
    // What follows: armed to play without a gap, else the next in the queue.
    std::optional<QueueEntry> following;
    if (current) {
        if (!state.gapless_entry.is_nil()) {
            following = player_->entry(state.gapless_entry);
        } else {
            const auto queue = player_->queue();
            const auto here = std::ranges::find(queue, state.entry, &QueueEntry::entry_id);
            if (here != queue.end() && std::next(here) != queue.end()) {
                following = *std::next(here);
            }
        }
    }
    const auto key = state.entry.to_string() + "/" + std::to_string(state.instance) + "/" +
                     (playing ? "playing" : paused ? "paused" : "stopped") + "/" +
                     (following ? following->entry_id.to_string() : std::string{});
    const auto now = session.server_now();
    const auto anchor = now - state.position_ms;
    const bool moved = key != session.said_key ||
                       (playing && std::abs(anchor - session.said_anchor) > moved_ms) ||
                       (paused && std::abs(state.position_ms - session.said_position) > drift_ms);
    if (moved) {
        Json said = Json::object();
        Json track = Json::object();
        if (current) {
            track = describe(session, *current);
            if (state.duration_ms > 0) {
                track["duration_ms"] = state.duration_ms;
            }
            said = Json{{"track", track},
                        {"entry", state.entry.to_string()},
                        {"instance", state.instance},
                        {"paused", paused},
                        {"anchor_ms", anchor},
                        {"position_ms", state.position_ms}};
            if (following) {
                said["next"] = Json{{"entry", following->entry_id.to_string()},
                                    {"track", describe(session, *following)}};
            }
        } else {
            said = Json{{"track", nullptr}};
        }
        if (auto put = request("PUT", session.url("/state"), session.invite.key, said.dump(),
                               5'000L);
            put && put->status == 200) {
            session.said_key = key;
            session.said_anchor = anchor;
            session.said_position = state.position_ms;
            say({});
            {
                const std::lock_guard guard{mutex_};
                status_.track = current ? text(track, "artist") + " — " + text(track, "title")
                                        : std::string{};
            }
            changed();
        } else {
            say(put ? "the server refused what plays (" + std::to_string(put->status) + ")"
                    : put.error().message);
        }
    }
    // Phase 2: while anyone listens, the host's copy of what plays and of
    // what follows, for whoever lacks it.
    bool anyone = false;
    {
        const std::lock_guard guard{mutex_};
        anyone = !status_.listeners.empty();
    }
    if (anyone && current) {
        send(session, *current);
        if (following) {
            send(session, *following);
        }
    }
    // Who listens, now and then.
    if (steady_clock::now() - session.presence_asked > presence_every) {
        session.presence_asked = steady_clock::now();
        if (auto seen = request("GET", session.url("/state?after=0&wait=0"), session.invite.key,
                                {}, 5'000L);
            seen && seen->status == 200) {
            std::vector<std::string> names;
            for (const auto& name : seen->body.value("listeners", Json::array())) {
                if (name.is_string()) {
                    names.push_back(name.get<std::string>());
                }
            }
            bool different = false;
            {
                const std::lock_guard guard{mutex_};
                different = status_.listeners != names;
                status_.listeners = std::move(names);
            }
            if (different) {
                changed();
            }
        }
    }
}

void Together::listen_round(Session& session) {
    auto seen = request("GET",
                        session.url("/state?after=" + std::to_string(session.revision) +
                                    "&wait=1000&name=" + escaped(session.name)),
                        session.invite.key, {}, 10'000L);
    if (!seen || seen->status != 200) {
        say(seen ? (seen->status == 404 ? "the room is gone" : "the room refused this listener")
                 : seen.error().message);
        // Not reached: a moment before asking again.
        static_cast<void>(session.pause.wait(milliseconds{2'000}));
        return;
    }
    const auto revision = seen->body.value("revision", std::uint64_t{0});
    session.files.clear();
    for (const auto& file : seen->body.value("files", Json::array())) {
        if (file.is_string()) {
            session.files.insert(file.get<std::string>());
        }
    }
    if (seen->body.value("closed", false)) {
        session.ended = true;
        if (session.found && player_->state().entry == session.ours) {
            static_cast<void>(player_->stop());
        }
        {
            const std::lock_guard guard{mutex_};
            status_.role.clear();
            status_.message = "the host closed the room";
        }
        changed();
        return;
    }
    if (revision > session.revision) {
        session.revision = revision;
        std::vector<std::string> names;
        for (const auto& name : seen->body.value("listeners", Json::array())) {
            if (name.is_string()) {
                names.push_back(name.get<std::string>());
            }
        }
        {
            const std::lock_guard guard{mutex_};
            status_.listeners = std::move(names);
        }
        follow(session, seen->body.value("state", Json::object()));
        changed();
    }
    if (session.desired.contains("track") && session.desired.at("track").is_object()) {
        play_what_is_due(session);
    }
    keep_in_step(session);
}

void Together::follow(Session& session, const Json& state) {
    session.desired = state;
    const auto track = state.find("track");
    if (track == state.end() || !track->is_object()) {
        if (session.found && player_->state().entry == session.ours) {
            static_cast<void>(player_->stop());
        }
        session.applied_key.clear();
        session.found = false;
        const std::lock_guard guard{mutex_};
        status_.track.clear();
        return;
    }
    const auto key = text(state, "entry") + "/" + std::to_string(number(state, "instance"));
    if (key == session.applied_key) {
        return;
    }
    session.applied_key = key;
    const auto shown = text(*track, "artist") + " — " + text(*track, "title");
    {
        const std::lock_guard guard{mutex_};
        status_.track = shown;
    }
    if (const auto path = library_copy(session, text(state, "entry"), *track); !path.empty()) {
        start(session, path);
        return;
    }
    // Not here: the host's copy, when there is one -- play_what_is_due.
    session.found = false;
    session.waiting = true;
    if (player_->state().entry == session.ours) {
        static_cast<void>(player_->stop());
    }
}

void Together::send(Session& session, const QueueEntry& entry) {
    const auto id = entry.entry_id.to_string();
    {
        const std::lock_guard guard{session.copies};
        if (session.sent.contains(id) || !session.sending.insert(id).second) {
            return;
        }
    }
    session.errands.post([this, &session, id, source = entry.source] {
        // The engine's Opus copy -- made once, kept -- a hundred and sixty
        // kilobits a second: as the phone hears it away from home.
        auto file = transcodes_->ensure(
            TranscodeSource{.raw_path = source.raw_path,
                            .selection = source.selection,
                            .segment = source.segment},
            StreamFormat{.bitrate_kbps = 160, .codec = output::StreamCodec::opus,
                         .sample_rate_cap = std::nullopt});
        const bool kept = file && put_file(session.url("/tracks/" + id), session.invite.key,
                                           *file, session.stopping);
        const std::lock_guard guard{session.copies};
        session.sending.erase(id);
        if (kept) {
            session.sent.insert(id);
        }
    });
}

void Together::fetch(Session& session, const std::string& entry) {
    {
        const std::lock_guard guard{session.copies};
        if (session.fetched.contains(entry) || !session.fetching.insert(entry).second) {
            return;
        }
    }
    session.errands.post([&session, entry] {
        const auto target = session.downloads / (entry + ".opus");
        const bool kept = get_file(session.url("/tracks/" + entry), session.invite.key, target,
                                   session.stopping);
        const std::lock_guard guard{session.copies};
        session.fetching.erase(entry);
        if (kept) {
            session.fetched[entry] = target.string();
        }
    });
}

std::string Together::library_copy(Session& session, const std::string& entry,
                                   const Json& track) {
    if (const auto known = session.library.find(entry); known != session.library.end()) {
        return known->second;
    }
    // This library's copy: by id, else by what it is called.
    std::string path;
    for (const auto& source : track_queries(track)) {
        auto compiled = query::compile_tkq(source);
        if (!compiled) {
            continue;
        }
        if (auto paths = catalogue_->filter_paths(*compiled); paths && !paths->empty()) {
            path = paths->front();
            break;
        }
    }
    session.library[entry] = path;
    return path;
}

void Together::start(Session& session, const std::string& path) {
    const auto& track = session.desired.at("track");
    QueueEntry entry;
    entry.source.raw_path = path;
    entry.title = text(track, "title");
    entry.group = audio::AlbumRowKey{.album_artist = text(track, "albumartist"),
                                     .artist = text(track, "artist"),
                                     .album = text(track, "album"),
                                     .date = {}};
    if (const auto duration = number(track, "duration_ms"); duration > 0) {
        entry.duration_ms = duration;
    }
    session.ours = entry.entry_id;
    session.found = true;
    session.waiting = false;
    session.last_seek = {};
    player_->replace_queue({entry});
    if (auto played = player_->play_entry(session.ours); !played) {
        say("could not play " + text(track, "title") + ": " + played.error().message);
        return;
    }
    say({});
}

void Together::play_what_is_due(Session& session) {
    const auto& desired = session.desired;
    // What follows, fetched ahead where this library lacks it.
    if (const auto next = desired.find("next"); next != desired.end() && next->is_object()) {
        const auto entry = text(*next, "entry");
        const auto track = next->find("track");
        if (!entry.empty() && track != next->end() && track->is_object() &&
            library_copy(session, entry, *track).empty() && session.files.contains(entry)) {
            fetch(session, entry);
        }
    }
    if (!session.waiting) {
        return;
    }
    const auto entry = text(desired, "entry");
    const auto shown =
        text(desired.at("track"), "artist") + " — " + text(desired.at("track"), "title");
    std::string fetched;
    {
        const std::lock_guard guard{session.copies};
        if (const auto found = session.fetched.find(entry); found != session.fetched.end()) {
            fetched = found->second;
        }
    }
    if (!fetched.empty()) {
        start(session, fetched);
    } else if (session.files.contains(entry)) {
        fetch(session, entry);
        say("fetching the host's copy of " + shown);
    } else {
        say("not in this library: " + shown + " -- waiting for the host's copy");
    }
}

void Together::keep_in_step(Session& session) {
    if (!session.found) {
        return;
    }
    const auto state = player_->state();
    // Something else plays -- chosen here, by hand -- and is left alone.
    if (state.entry != session.ours) {
        return;
    }
    const auto& desired = session.desired;
    const bool paused = desired.value("paused", false);
    const auto target = paused ? number(desired, "position_ms")
                               : session.server_now() - number(desired, "anchor_ms");
    if (paused && state.status == "playing") {
        static_cast<void>(player_->pause());
    } else if (!paused && state.status == "paused") {
        static_cast<void>(player_->resume());
    }
    if (state.status == "stopped" || target < 0 ||
        (state.duration_ms > 0 && target >= state.duration_ms)) {
        return;
    }
    if (std::abs(state.position_ms - target) <= drift_ms ||
        steady_clock::now() - session.last_seek < settle) {
        return;
    }
    if (player_->seek_ms(paused ? target : target + seek_lead_ms)) {
        session.last_seek = steady_clock::now();
    }
}

void register_together_methods(protocol::Dispatcher& dispatcher, Together& together) {
    const auto status_json = [&together] {
        const auto now = together.status();
        return Json{{"role", now.role},
                    {"invite", now.invite},
                    {"listeners", now.listeners},
                    {"track", now.track},
                    {"message", now.message}};
    };
    dispatcher.on("together.host",
                  [&together](const Json& params) -> core::Result<Json> {
                      const auto server = params.value("server", std::string{});
                      if (server.empty()) {
                          return std::unexpected(together_error(
                              "the server's address is needed", core::ErrorCode::invalid_argument));
                      }
                      auto invite =
                          together.host(server, params.value("password", std::string{}));
                      if (!invite) {
                          return std::unexpected(invite.error());
                      }
                      return Json{{"invite", *invite}};
                  });
    dispatcher.on("together.join", [&together](const Json& params) -> core::Result<Json> {
        auto name = params.value("name", std::string{});
        if (name.empty()) {
            name = "a friend";
        }
        auto joined = together.join(params.value("invite", std::string{}), name);
        if (!joined) {
            return std::unexpected(joined.error());
        }
        return Json{{"joined", true}};
    });
    dispatcher.on("together.leave", [&together](const Json&) -> core::Result<Json> {
        together.leave();
        return Json{{"left", true}};
    });
    dispatcher.on("together.state",
                  [status_json](const Json&) -> core::Result<Json> { return status_json(); });
}

} // namespace trackknife::engine
