// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/lastfm.hpp"

#include <curl/curl.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <fstream>
#include <map>
#include <utility>

namespace trackknife::engine {
namespace {

using protocol::Json;

constexpr std::size_t pending_limit = 1'000U;
constexpr std::size_t response_limit = 1024U * 1024U;
constexpr long request_timeout_ms = 8'000;

[[nodiscard]] core::Error lastfm_error(std::string message) {
    return core::Error{.code = core::ErrorCode::backend, .message = std::move(message), .context = {}};
}

// Last.fm's request signature: every parameter but the format, sorted by
// name, name and value run together, the shared secret after, as MD5 hex.
[[nodiscard]] std::string signature(const std::map<std::string, std::string>& params,
                                    const std::string& secret) {
    std::string text;
    for (const auto& [name, value] : params) {
        if (name == "format" || name == "callback") {
            continue;
        }
        text += name;
        text += value;
    }
    text += secret;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_Digest(text.data(), text.size(), digest.data(), &length, EVP_md5(), nullptr);
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned int index = 0; index < length; ++index) {
        hex.push_back(digits[digest[index] >> 4U]);
        hex.push_back(digits[digest[index] & 0x0FU]);
    }
    return hex;
}

std::size_t collect(char* data, const std::size_t size, const std::size_t count, void* target) {
    auto& body = *static_cast<std::string*>(target);
    const auto bytes = size * count;
    if (body.size() + bytes > response_limit) {
        return 0; // aborts the transfer
    }
    body.append(data, bytes);
    return bytes;
}

// Whether a failure is worth retrying later: the network, or Last.fm busy
// or down -- as opposed to a request it will never accept.
[[nodiscard]] bool retryable(const long code) {
    return code == -1 || code == 11 || code == 16 || code == 29 || code == 429 || code >= 500;
}

[[nodiscard]] Json track_json(const std::string& artist, const std::string& title,
                              const std::string& album, const double duration) {
    return Json{{"artist", artist}, {"title", title}, {"album", album}, {"duration", duration}};
}

} // namespace

LastFm::LastFm(Player& player, Catalogue& catalogue, std::filesystem::path state_file,
               std::string endpoint)
    : player_(&player), catalogue_(&catalogue), state_file_(std::move(state_file)),
      endpoint_(std::move(endpoint)) {
    static const auto initialised = curl_global_init(CURL_GLOBAL_DEFAULT);
    static_cast<void>(initialised);
    load();
}

LastFm::~LastFm() { stop(); }

void LastFm::start() {
    if (running_.exchange(true)) {
        return;
    }
    pause_.reset();
    worker_ = std::thread{[this] {
        while (running_.load()) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            const auto wall = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
            tick(now, wall);
            static_cast<void>(pause_.wait(std::chrono::milliseconds{1'000}));
        }
    }};
}

void LastFm::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    pause_.interrupt();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void LastFm::load() {
    std::error_code missing;
    if (!std::filesystem::exists(state_file_, missing)) {
        return;
    }
    std::ifstream input{state_file_};
    const auto document = Json::parse(input, nullptr, false);
    if (document.is_discarded() || !document.is_object() || document.value("version", 0) != 1) {
        // Kept as it is rather than overwritten: it may hold scrobbles.
        blocked_ = true;
        message_ = "Unreadable Last.fm state file";
        return;
    }
    if (!document.value("session", std::string{}).empty()) {
        session_ = Session{.api_key = document.value("key", std::string{}),
                           .secret = document.value("secret", std::string{}),
                           .session_key = document.value("session", std::string{}),
                           .user = document.value("user", std::string{})};
    }
    enabled_ = document.value("enabled", false);
    if (const auto pending = document.find("pending");
        pending != document.end() && pending->is_array()) {
        for (const auto& item : *pending) {
            pending_.push_back(Pending{
                .track = Track{.artist = item.value("artist", std::string{}),
                               .title = item.value("title", std::string{}),
                               .album = item.value("album", std::string{}),
                               .duration = item.value("duration", 0.0)},
                .timestamp = item.value("timestamp", std::int64_t{0})});
        }
    }
}

bool LastFm::save_locked() {
    if (blocked_) {
        return false;
    }
    Json document = Json::object();
    document["version"] = 1;
    document["enabled"] = enabled_;
    if (session_) {
        document["key"] = session_->api_key;
        document["secret"] = session_->secret;
        document["session"] = session_->session_key;
        document["user"] = session_->user;
    }
    auto pending = Json::array();
    for (const auto& item : pending_) {
        auto rendered = track_json(item.track.artist, item.track.title, item.track.album,
                                   item.track.duration);
        rendered["timestamp"] = item.timestamp;
        pending.push_back(std::move(rendered));
    }
    document["pending"] = std::move(pending);
    // Private from the first byte, and whole or not at all.
    std::error_code ignored;
    std::filesystem::create_directories(state_file_.parent_path(), ignored);
    const auto temporary = state_file_.string() + ".new";
    const auto descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    const auto text = document.dump();
    const bool written =
        ::write(descriptor, text.data(), text.size()) == static_cast<ssize_t>(text.size()) &&
        ::fsync(descriptor) == 0;
    ::close(descriptor);
    return written && ::rename(temporary.c_str(), state_file_.c_str()) == 0;
}

core::Result<void> LastFm::set_session(Session session) {
    if (session.api_key.size() != 32U || session.secret.size() != 32U ||
        session.session_key.empty() || session.user.empty()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "a Last.fm session needs the 32-character "
                                                      "API key and secret, the session key and "
                                                      "the user",
                                           .context = {}});
    }
    const std::lock_guard guard{mutex_};
    if (blocked_) {
        return std::unexpected(lastfm_error(message_));
    }
    // Another account's unsent scrobbles are not this one's to send.
    if (!session_ || session_->user != session.user) {
        pending_.clear();
    }
    session_ = std::move(session);
    enabled_ = true;
    auth_failed_ = false;
    failures_ = 0;
    retry_at_ms_ = 0;
    listen_.reset();
    listen_identity_.clear();
    now_playing_.reset();
    message_ = "Connected";
    if (!save_locked()) {
        return std::unexpected(lastfm_error("could not save the Last.fm session"));
    }
    return {};
}

core::Result<void> LastFm::clear() {
    const std::lock_guard guard{mutex_};
    if (blocked_) {
        return std::unexpected(lastfm_error(message_));
    }
    session_.reset();
    enabled_ = false;
    pending_.clear();
    now_playing_.reset();
    listen_.reset();
    message_ = "Signed out; unsent scrobbles dropped";
    if (!save_locked()) {
        return std::unexpected(lastfm_error("could not save the Last.fm state"));
    }
    return {};
}

LastFm::Status LastFm::status() const {
    const std::lock_guard guard{mutex_};
    return Status{.user = session_ ? session_->user : std::string{},
                  .enabled = enabled_ && session_.has_value(),
                  .pending = pending_.size(),
                  .message = message_};
}

std::optional<LastFm::Track> LastFm::playing_track(const Player::State& state) {
    if (state.entry.is_nil()) {
        return std::nullopt;
    }
    if (state.entry == cached_entry_) {
        return cached_track_;
    }
    cached_entry_ = state.entry;
    cached_track_.reset();
    const auto duration =
        state.duration_ms > 0 ? static_cast<double>(state.duration_ms) / 1000.0 : 0.0;
    // The library's tags when the file is in it -- what every client shows --
    // else what the client that queued it said.
    if (!state.source.raw_path.empty()) {
        if (auto known = catalogue_->cached_tracks({state.source.raw_path});
            known && !known->empty() && !known->front().facts.title.empty()) {
            const auto& facts = known->front().facts;
            cached_track_ = Track{.artist = facts.artist,
                                  .title = facts.title,
                                  .album = facts.album,
                                  .duration = duration};
            return cached_track_;
        }
    }
    if (const auto entry = player_->entry(state.entry); entry && !entry->title.empty()) {
        cached_track_ = Track{
            .artist = entry->group.artist.empty() ? entry->group.album_artist : entry->group.artist,
            .title = entry->title,
            .album = entry->group.album,
            .duration = duration};
    }
    return cached_track_;
}

void LastFm::sample_locked(const std::int64_t now_ms, const std::int64_t wall_s) {
    if (blocked_ || !session_ || !enabled_) {
        listen_.reset();
        return;
    }
    const auto state = player_->state();
    auto track = playing_track(state);
    const bool valid = track && !track->artist.empty() && !track->title.empty();
    const bool playing = state.status == "playing";
    // Which playback, not which track: the same track played twice is two
    // listens.
    const auto identity = valid ? std::to_string(state.instance) : std::string{};
    const bool eligible =
        listen_.observe(identity, valid ? track->duration : 0.0,
                        static_cast<double>(state.position_ms) / 1000.0, playing, now_ms);
    if (listen_.changed()) {
        started_ = 0;
        now_playing_.reset();
        listen_track_ = valid ? track : std::nullopt;
    }
    if (started_ == 0 && valid && playing) {
        started_ = wall_s;
        now_playing_ = track;
    }
    if (eligible && listen_track_) {
        if (pending_.size() >= pending_limit) {
            message_ = "Scrobble outbox is full";
            return;
        }
        pending_.push_back(Pending{.track = *listen_track_, .timestamp = started_});
        if (!save_locked()) {
            message_ = "Could not save a pending scrobble";
        }
    }
}

void LastFm::tick(const std::int64_t now_ms, const std::int64_t wall_s) {
    {
        const std::lock_guard guard{mutex_};
        sample_locked(now_ms, wall_s);
    }
    flush(now_ms);
}

void LastFm::flush(const std::int64_t now_ms) {
    Session session;
    Track track;
    std::int64_t timestamp = 0;
    bool now_playing = false;
    {
        const std::lock_guard guard{mutex_};
        if (blocked_ || auth_failed_ || !session_ || !enabled_ || now_ms < retry_at_ms_) {
            return;
        }
        if (now_playing_) {
            track = *std::exchange(now_playing_, std::nullopt);
            now_playing = true;
        } else if (!pending_.empty()) {
            track = pending_.front().track;
            timestamp = pending_.front().timestamp;
        } else {
            return;
        }
        session = *session_;
    }
    std::vector<std::pair<std::string, std::string>> params{
        {"artist", track.artist},
        {"track", track.title},
        {"album", track.album},
        {"duration", std::to_string(static_cast<int>(track.duration))}};
    if (!now_playing) {
        params.emplace_back("timestamp", std::to_string(timestamp));
    }
    auto answer = call(now_playing ? "track.updateNowPlaying" : "track.scrobble",
                       std::move(params), session);
    if (now_playing) {
        return; // Best effort: a missed "now playing" is not worth a retry.
    }
    const std::lock_guard guard{mutex_};
    if (!answer) {
        const auto& context = answer.error().context;
        long reported = -1;
        for (const auto& [key, value] : context) {
            if (key == "code") {
                reported = std::stol(value);
            }
        }
        if (reported == 9) {
            auth_failed_ = true;
            message_ = "Last.fm session expired; sign in again";
            return;
        }
        if (retryable(reported)) {
            failures_ = std::min(failures_ + 1, 8);
            retry_at_ms_ = now_ms + (std::int64_t{1} << failures_) * 1'000;
            message_ = "Last.fm unavailable; scrobble queued";
            return;
        }
        // Refused for good: dropped, so it does not hold up the rest.
        if (!pending_.empty()) {
            pending_.pop_front();
        }
        message_ = "Scrobble rejected (code " + std::to_string(reported) + ")";
        static_cast<void>(save_locked());
        return;
    }
    const auto attributes = answer->value("scrobbles", Json::object()).value("@attr", Json::object());
    const auto count = [&attributes](const char* key) {
        const auto value = attributes.find(key);
        if (value == attributes.end()) {
            return 0;
        }
        return value->is_string() ? std::stoi(value->get<std::string>()) : value->get<int>();
    };
    if (count("accepted") + count("ignored") != 1) {
        retry_at_ms_ = now_ms + 60'000;
        message_ = "Invalid scrobble acknowledgement";
        return;
    }
    if (!pending_.empty()) {
        pending_.pop_front();
    }
    failures_ = 0;
    message_ = count("ignored") > 0 ? "Last.fm ignored the scrobble (metadata or timestamp)"
                                    : "Scrobble submitted";
    if (!save_locked()) {
        message_ = "Could not save a scrobble acknowledgement";
    }
}

core::Result<Json> LastFm::call(const std::string& method,
                                std::vector<std::pair<std::string, std::string>> params,
                                const Session& session) {
    std::map<std::string, std::string> signed_params{params.begin(), params.end()};
    signed_params["method"] = method;
    signed_params["api_key"] = session.api_key;
    signed_params["sk"] = session.session_key;
    signed_params["api_sig"] = signature(signed_params, session.secret);
    signed_params["format"] = "json";

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
    if (!curl) {
        return std::unexpected(lastfm_error("could not start an HTTP request"));
    }
    std::string body;
    for (const auto& [name, value] : signed_params) {
        if (!body.empty()) {
            body += '&';
        }
        auto* escaped_name = curl_easy_escape(curl.get(), name.c_str(), static_cast<int>(name.size()));
        auto* escaped_value =
            curl_easy_escape(curl.get(), value.c_str(), static_cast<int>(value.size()));
        body += escaped_name;
        body += '=';
        body += escaped_value;
        curl_free(escaped_name);
        curl_free(escaped_value);
    }
    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, request_timeout_ms);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "melodyd");
    const auto performed = curl_easy_perform(curl.get());
    long http_status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);
    const auto parsed = Json::parse(response, nullptr, false);
    long code = 0;
    if (performed != CURLE_OK) {
        code = -1;
    } else if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("error")) {
        code = parsed.value("error", 0L);
    } else if (http_status >= 400) {
        code = http_status;
    } else if (parsed.is_discarded() || !parsed.is_object()) {
        code = -1;
    }
    if (code != 0) {
        return std::unexpected(core::Error{
            .code = code == -1 ? core::ErrorCode::io : core::ErrorCode::backend,
            .message = "Last.fm request failed (code " + std::to_string(code) + ")",
            .context = {{.key = "code", .value = std::to_string(code)}}});
    }
    return parsed;
}

core::Result<void> LastFm::love(std::optional<std::string> artist, std::optional<std::string> title,
                                const bool loved) {
    Session session;
    {
        const std::lock_guard guard{mutex_};
        if (!session_) {
            return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                               .message = "no Last.fm account on this engine",
                                               .context = {}});
        }
        session = *session_;
        if (!artist || !title) {
            // The one playing.
            const auto track = playing_track(player_->state());
            if (!track || track->artist.empty() || track->title.empty()) {
                return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                                   .message = "nothing is playing to love",
                                                   .context = {}});
            }
            artist = track->artist;
            title = track->title;
        }
    }
    auto answer = call(loved ? "track.love" : "track.unlove",
                       {{"artist", *artist}, {"track", *title}}, session);
    if (!answer) {
        return std::unexpected(std::move(answer.error()));
    }
    return {};
}

void register_lastfm_methods(protocol::Dispatcher& dispatcher, LastFm& lastfm) {
    const auto status_json = [&lastfm] {
        const auto status = lastfm.status();
        return Json{{"user", status.user.empty() ? Json(nullptr) : Json(status.user)},
                    {"enabled", status.enabled},
                    {"pending", status.pending},
                    {"message", status.message}};
    };
    // Write-only: the session goes in and never comes back out.
    dispatcher.on("lastfm.set_session", [&lastfm, status_json](const Json& params)
                                            -> core::Result<Json> {
        auto set = lastfm.set_session(
            LastFm::Session{.api_key = params.value("api_key", std::string{}),
                            .secret = params.value("secret", std::string{}),
                            .session_key = params.value("session_key", std::string{}),
                            .user = params.value("user", std::string{})});
        if (!set) {
            return std::unexpected(std::move(set.error()));
        }
        return status_json();
    });
    dispatcher.on("lastfm.clear", [&lastfm, status_json](const Json&) -> core::Result<Json> {
        if (auto cleared = lastfm.clear(); !cleared) {
            return std::unexpected(std::move(cleared.error()));
        }
        return status_json();
    });
    dispatcher.on("lastfm.status",
                  [status_json](const Json&) -> core::Result<Json> { return status_json(); });
    dispatcher.on("lastfm.love", [&lastfm](const Json& params) -> core::Result<Json> {
        const auto text = [&params](const char* key) -> std::optional<std::string> {
            const auto found = params.find(key);
            return found != params.end() && found->is_string() && !found->get<std::string>().empty()
                       ? std::optional{found->get<std::string>()}
                       : std::nullopt;
        };
        auto loved = lastfm.love(text("artist"), text("title"), params.value("loved", true));
        if (!loved) {
            return std::unexpected(std::move(loved.error()));
        }
        return Json::object();
    });
}

} // namespace trackknife::engine
