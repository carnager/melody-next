// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/mpd/client.hpp"

#include "trackknife/mpd/projection.hpp"

#include <mpd/client.h>

#include <poll.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

namespace trackknife::mpd {
namespace {
// libmpdclient's send_command takes a fixed variadic argument list, so every
// list-carrying melody_context subcommand composes its own quoted line.
std::string quoted_argument(const std::string& value) {
    std::string quoted = "\"";
    for (const auto character : value) {
        if (character == '"' || character == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

struct ConnectionDeleter {
    void operator()(mpd_connection* connection) const noexcept {
        if (connection != nullptr) {
            mpd_connection_free(connection);
        }
    }
};

using ConnectionPtr = std::unique_ptr<mpd_connection, ConnectionDeleter>;

[[nodiscard]] core::ErrorCode map_error_code(enum mpd_error error) noexcept {
    switch (error) {
    case MPD_ERROR_ARGUMENT:
    case MPD_ERROR_STATE:
        return core::ErrorCode::invalid_argument;
    case MPD_ERROR_TIMEOUT:
    case MPD_ERROR_SYSTEM:
    case MPD_ERROR_RESOLVER:
    case MPD_ERROR_CLOSED:
        return core::ErrorCode::io;
    case MPD_ERROR_OOM:
    case MPD_ERROR_MALFORMED:
    case MPD_ERROR_SERVER:
    case MPD_ERROR_SUCCESS:
        return core::ErrorCode::backend;
    }
    return core::ErrorCode::backend;
}

[[nodiscard]] core::ErrorCode map_server_error_code(enum mpd_server_error error) noexcept {
    switch (error) {
    case MPD_SERVER_ERROR_ARG:
        return core::ErrorCode::invalid_argument;
    case MPD_SERVER_ERROR_NO_EXIST:
        return core::ErrorCode::not_found;
    case MPD_SERVER_ERROR_EXIST:
        return core::ErrorCode::conflict;
    case MPD_SERVER_ERROR_UNKNOWN_CMD:
        return core::ErrorCode::unsupported;
    case MPD_SERVER_ERROR_UNK:
    case MPD_SERVER_ERROR_NOT_LIST:
    case MPD_SERVER_ERROR_PASSWORD:
    case MPD_SERVER_ERROR_PERMISSION:
    case MPD_SERVER_ERROR_PLAYLIST_MAX:
    case MPD_SERVER_ERROR_SYSTEM:
    case MPD_SERVER_ERROR_PLAYLIST_LOAD:
    case MPD_SERVER_ERROR_UPDATE_ALREADY:
    case MPD_SERVER_ERROR_PLAYER_SYNC:
        return core::ErrorCode::backend;
    }
    return core::ErrorCode::backend;
}

[[nodiscard]] unsigned bounded_timeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        return 1U;
    }
    const auto maximum =
        static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<unsigned>::max());
    return static_cast<unsigned>(std::min(timeout.count(), maximum));
}

[[nodiscard]] IdleEvents project_idle_events(enum mpd_idle events) noexcept {
    IdleEvents result;
    const auto add = [&result, events](enum mpd_idle backend, IdleEvent domain) {
        if ((events & backend) != 0) {
            result.mask |= static_cast<std::uint32_t>(domain);
        }
    };
    add(MPD_IDLE_DATABASE, IdleEvent::database);
    add(MPD_IDLE_STORED_PLAYLIST, IdleEvent::stored_playlist);
    add(MPD_IDLE_QUEUE, IdleEvent::queue);
    add(MPD_IDLE_PLAYER, IdleEvent::player);
    add(MPD_IDLE_MIXER, IdleEvent::mixer);
    add(MPD_IDLE_OUTPUT, IdleEvent::output);
    add(MPD_IDLE_OPTIONS, IdleEvent::options);
    add(MPD_IDLE_UPDATE, IdleEvent::update);
    add(MPD_IDLE_STICKER, IdleEvent::sticker);
    add(MPD_IDLE_PARTITION, IdleEvent::partition);
    return result;
}

[[nodiscard]] std::optional<enum mpd_single_state>
to_mpd_single_state(const PlaybackModeState state) noexcept {
    switch (state) {
    case PlaybackModeState::off:
        return MPD_SINGLE_OFF;
    case PlaybackModeState::on:
        return MPD_SINGLE_ON;
    case PlaybackModeState::oneshot:
        return MPD_SINGLE_ONESHOT;
    case PlaybackModeState::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<enum mpd_consume_state>
to_mpd_consume_state(const PlaybackModeState state) noexcept {
    switch (state) {
    case PlaybackModeState::off:
        return MPD_CONSUME_OFF;
    case PlaybackModeState::on:
        return MPD_CONSUME_ON;
    case PlaybackModeState::oneshot:
        return MPD_CONSUME_ONESHOT;
    case PlaybackModeState::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<enum mpd_replay_gain_mode>
to_mpd_replay_gain_mode(const ReplayGainMode mode) noexcept {
    switch (mode) {
    case ReplayGainMode::off:
        return MPD_REPLAY_OFF;
    case ReplayGainMode::track:
        return MPD_REPLAY_TRACK;
    case ReplayGainMode::album:
        return MPD_REPLAY_ALBUM;
    case ReplayGainMode::automatic:
        return MPD_REPLAY_AUTO;
    case ReplayGainMode::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

struct Client::Impl {
    ConnectionPtr connection;

    // Sends a caller-composed command line and drains its response.
    [[nodiscard]] core::Result<void> run_composed(const std::string& line, const char* stage) {
        if (!mpd_send_command(connection.get(), line.c_str(), nullptr) ||
            !mpd_response_finish(connection.get())) {
            return std::unexpected(take_error(stage));
        }
        return {};
    }

    [[nodiscard]] core::Error take_error(std::string_view stage) {
        const auto backend_error = mpd_connection_get_error(connection.get());
        const auto server_error = backend_error == MPD_ERROR_SERVER
                                      ? mpd_connection_get_server_error(connection.get())
                                      : MPD_SERVER_ERROR_UNK;
        const char* backend_message = backend_error == MPD_ERROR_SUCCESS
                                          ? nullptr
                                          : mpd_connection_get_error_message(connection.get());
        core::Error error{
            .code = backend_error == MPD_ERROR_SERVER ? map_server_error_code(server_error)
                                                      : map_error_code(backend_error),
            .message =
                backend_message == nullptr ? "MPD operation failed" : std::string{backend_message},
            .context = {{"stage", std::string{stage}},
                        {"libmpdclient_error", std::to_string(backend_error)}},
        };
        if (backend_error == MPD_ERROR_SERVER) {
            error.context.push_back({"mpd_server_error", std::to_string(server_error)});
        }
        // A failure raised by libmpdclient itself rather than by the server
        // leaves the connection object in a state we did not choose — the
        // command may have been rejected before or after bytes went out. The
        // session reconnects on these; a plain server ACK stays cheap.
        if (backend_error != MPD_ERROR_SUCCESS && backend_error != MPD_ERROR_SERVER) {
            error.context.push_back({"local_protocol_error", "true"});
        }
        // Server ACK/argument failures do not necessarily poison the socket.
        // Expose libmpdclient's recovery verdict so the session does not turn a
        // definitive command rejection into a needless reconnect.
        const auto recoverable = mpd_connection_clear_error(connection.get());
        error.context.push_back({"connection_recoverable", recoverable ? "true" : "false"});
        return error;
    }

    [[nodiscard]] core::Result<std::vector<Pair>>
    receive_pairs(std::string_view stage,
                  const std::size_t maximum_pairs = std::numeric_limits<std::size_t>::max(),
                  const std::size_t maximum_bytes = std::numeric_limits<std::size_t>::max()) {
        std::vector<Pair> result;
        std::size_t bytes = 0U;
        bool exceeded = false;
        while (auto* pair = mpd_recv_pair(connection.get())) {
            const std::string_view name = pair->name == nullptr ? "" : pair->name;
            const std::string_view value = pair->value == nullptr ? "" : pair->value;
            if (result.size() >= maximum_pairs ||
                name.size() + value.size() > maximum_bytes - bytes)
                exceeded = true;
            if (!exceeded) {
                bytes += name.size() + value.size();
                result.push_back({std::string{name}, std::string{value}});
            }
            mpd_return_pair(connection.get(), pair);
        }
        if (!mpd_response_finish(connection.get())) {
            return std::unexpected(take_error(stage));
        }
        if (exceeded)
            return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                               .message = "MPD response exceeds the query bounds",
                                               .context = {}});
        return result;
    }
};

Client::Client(std::unique_ptr<Impl> implementation) : implementation_(std::move(implementation)) {}

Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;
Client::~Client() = default;

core::Result<Client> Client::connect(const Profile& profile) {
    auto implementation = std::make_unique<Impl>();
    implementation->connection.reset(
        mpd_connection_new(profile.host.empty() ? nullptr : profile.host.c_str(), profile.port,
                           bounded_timeout(profile.connect_timeout)));
    if (!implementation->connection) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::backend,
                        .message = "libmpdclient could not allocate a connection",
                        .context = {}});
    }
    if (mpd_connection_get_error(implementation->connection.get()) != MPD_ERROR_SUCCESS) {
        return std::unexpected(implementation->take_error("connect"));
    }

    static_cast<void>(mpd_connection_set_keepalive(implementation->connection.get(), true));
    mpd_connection_set_timeout(implementation->connection.get(),
                               bounded_timeout(profile.command_timeout));

    if (profile.password &&
        !mpd_run_password(implementation->connection.get(), profile.password->c_str())) {
        return std::unexpected(implementation->take_error("authenticate"));
    }
    return Client{std::move(implementation)};
}

ProtocolVersion Client::protocol_version() const noexcept {
    const auto* version = mpd_connection_get_server_version(implementation_->connection.get());
    if (version == nullptr) {
        return {};
    }
    return {.major = version[0], .minor = version[1], .patch = version[2]};
}

core::Result<Capabilities> Client::capabilities() {
    Capabilities result;
    result.protocol = protocol_version();

    if (!mpd_send_allowed_commands(implementation_->connection.get())) {
        return std::unexpected(implementation_->take_error("send commands"));
    }
    auto command_pairs = implementation_->receive_pairs("receive commands");
    if (!command_pairs) {
        return std::unexpected(std::move(command_pairs.error()));
    }
    for (auto& pair : *command_pairs) {
        if (ascii_case_equal(pair.name, "command")) {
            result.commands.push_back(std::move(pair.value));
        }
    }

    if (!mpd_send_list_tag_types(implementation_->connection.get())) {
        return std::unexpected(implementation_->take_error("send tagtypes"));
    }
    auto tag_pairs = implementation_->receive_pairs("receive tagtypes");
    if (!tag_pairs) {
        return std::unexpected(std::move(tag_pairs.error()));
    }
    for (auto& pair : *tag_pairs) {
        if (ascii_case_equal(pair.name, "tagtype")) {
            result.tag_types.push_back(std::move(pair.value));
        }
    }
    return result;
}

core::Result<std::vector<Pair>> Client::command_pairs(std::string_view command) {
    const std::string command_text{command};
    if (command_text.empty()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD command name cannot be empty",
                                           .context = {}});
    }
    if (!mpd_send_command(implementation_->connection.get(), command_text.c_str(), nullptr)) {
        return std::unexpected(implementation_->take_error("send command"));
    }
    return implementation_->receive_pairs(command_text);
}

core::Result<void> Client::shuffle_albums(const std::uint32_t revision) {
    return implementation_->run_composed("melody_shuffle_albums " + std::to_string(revision),
                                         "melody_shuffle_albums");
}

core::Result<void> Client::set_album_random(const bool enabled) {
    return implementation_->run_composed(
        std::string{"melody_album_random "} + (enabled ? "1" : "0"), "melody_album_random");
}

core::Result<void> Client::shuffle_list_albums(std::string_view name) {
    if (name.find_first_of("\r\n") != std::string_view::npos ||
        name.find('\0') != std::string_view::npos)
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Invalid Melody list name",
                                           .context = {}});
    const auto command = "melody_list_shuffle_albums " + quoted_argument(std::string{name});
    auto pairs = command_pairs(command);
    if (!pairs)
        return std::unexpected(std::move(pairs.error()));
    for (const auto& pair : *pairs) {
        if (pair.name == "revision" && !pair.value.empty() &&
            pair.value.find_first_not_of("0123456789abcdef") == std::string::npos)
            return implementation_->run_composed(command + " " + quoted_argument(pair.value),
                                                 "melody_list_shuffle_albums");
    }
    return std::unexpected(core::Error{.code = core::ErrorCode::backend,
                                       .message = "Melody omitted the list revision",
                                       .context = {}});
}

core::Result<PlaybackStatus> Client::status() {
    auto pairs = command_pairs("status");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_status(*pairs);
}

core::Result<std::vector<Track>> Client::current_song() {
    auto pairs = command_pairs("currentsong");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

core::Result<std::vector<Track>> Client::queue_snapshot() {
    auto pairs = command_pairs("playlistinfo");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

core::Result<std::vector<Track>> Client::queue_changes(const std::uint32_t from_version) {
    const auto version = std::to_string(from_version);
    if (!mpd_send_command(implementation_->connection.get(), "plchanges", version.c_str(),
                          nullptr)) {
        return std::unexpected(implementation_->take_error("send plchanges"));
    }
    auto pairs = implementation_->receive_pairs("receive plchanges");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

core::Result<std::vector<DatabaseEntry>> Client::browse(const std::string_view uri) {
    if (uri.contains('\0')) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD browse URI cannot contain NUL",
                                           .context = {}});
    }
    const std::string uri_text{uri};
    if (!mpd_send_list_meta(implementation_->connection.get(),
                            uri_text.empty() ? nullptr : uri_text.c_str())) {
        return std::unexpected(implementation_->take_error("send lsinfo"));
    }
    auto pairs = implementation_->receive_pairs("receive lsinfo");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_database_entries(*pairs);
}

core::Result<std::vector<ArtistAlbumCount>>
Client::album_counts(const std::string_view artist_tag) {
    const auto type = artist_tag == "AlbumArtist" ? MPD_TAG_ALBUM_ARTIST
                      : artist_tag == "Artist"    ? MPD_TAG_ARTIST
                                                  : MPD_TAG_UNKNOWN;
    if (type == MPD_TAG_UNKNOWN) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Album counts require Artist or AlbumArtist",
                                           .context = {}});
    }
    auto* connection = implementation_->connection.get();
    if (!mpd_search_db_tags(connection, MPD_TAG_ALBUM) ||
        !mpd_search_add_group_tag(connection, type) ||
        !mpd_search_add_group_tag(connection, MPD_TAG_DATE) ||
        !mpd_search_add_group_tag(connection, MPD_TAG_MUSICBRAINZ_ALBUMID) ||
        !mpd_search_commit(connection)) {
        mpd_search_cancel(connection);
        return std::unexpected(implementation_->take_error("send artist album counts"));
    }
    auto pairs = implementation_->receive_pairs("receive artist album counts", 500'000U,
                                                32U * 1024U * 1024U);
    if (!pairs)
        return std::unexpected(std::move(pairs.error()));
    std::map<std::string, std::set<std::string>> albums;
    std::string artist, date, release;
    bool have_artist = false;
    std::size_t count = 0U;
    for (const auto& pair : *pairs) {
        const auto pair_type = mpd_tag_name_iparse(pair.name.c_str());
        if (pair_type == type) {
            artist = pair.value;
            have_artist = true;
        } else if (pair_type == MPD_TAG_DATE) {
            date = pair.value;
        } else if (pair_type == MPD_TAG_MUSICBRAINZ_ALBUMID) {
            release = pair.value;
        } else if (pair_type == MPD_TAG_ALBUM) {
            if (!have_artist) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "Album count response omitted artist grouping",
                                .context = {}});
            }
            auto key = release.empty() ? "album:" + pair.value + std::string(1, '\0') + date
                                       : "mbid:" + release;
            if (albums[artist].insert(std::move(key)).second && ++count > 100'000U) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::limit_exceeded,
                                .message = "Album count response exceeds 100000 releases",
                                .context = {}});
            }
        }
    }
    std::vector<ArtistAlbumCount> result;
    result.reserve(albums.size());
    for (const auto& [name, releases] : albums)
        result.push_back({name, releases.size()});
    return result;
}

core::Result<std::vector<std::string>> Client::list_tag(const std::string_view tag) {
    if (tag.empty() || tag.contains('\0')) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD tag name is invalid",
                                           .context = {}});
    }
    const std::string tag_name{tag};
    const auto type = mpd_tag_name_iparse(tag_name.c_str());
    if (type == MPD_TAG_UNKNOWN) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD tag name is unknown",
                                           .context = {{"tag", tag_name}}});
    }
    auto* connection = implementation_->connection.get();
    if (!mpd_search_db_tags(connection, type) || !mpd_search_commit(connection)) {
        mpd_search_cancel(connection);
        return std::unexpected(implementation_->take_error("send list tag"));
    }
    auto pairs = implementation_->receive_pairs("receive list tag");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    std::vector<std::string> values;
    values.reserve(pairs->size());
    for (auto& pair : *pairs) {
        if (ascii_case_equal(pair.name, tag_name) && !pair.value.empty()) {
            values.push_back(std::move(pair.value));
        }
    }
    return values;
}

core::Result<std::vector<Track>> Client::find_tag_tracks(const std::string_view tag,
                                                         const std::string_view value,
                                                         const unsigned limit) {
    constexpr unsigned maximum_tracks = 20'000U;
    if (tag.empty() || tag.contains('\0') || value.contains('\0') || limit == 0U ||
        limit > maximum_tracks) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD tag-track lookup is invalid",
                                           .context = {}});
    }
    const std::string tag_name{tag};
    const std::string tag_value{value};
    const auto type = mpd_tag_name_iparse(tag_name.c_str());
    if (type == MPD_TAG_UNKNOWN) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD tag name is unknown",
                                           .context = {{"tag", tag_name}}});
    }

    auto* connection = implementation_->connection.get();
    if (!mpd_search_db_songs(connection, true)) {
        return std::unexpected(implementation_->take_error("begin tag-track lookup"));
    }
    if (!mpd_search_add_tag_constraint(connection, MPD_OPERATOR_DEFAULT, type, tag_value.c_str()) ||
        !mpd_search_add_window(connection, 0U, limit)) {
        mpd_search_cancel(connection);
        return std::unexpected(implementation_->take_error("build tag-track lookup"));
    }
    if (!mpd_search_commit(connection)) {
        return std::unexpected(implementation_->take_error("send tag-track lookup"));
    }
    auto pairs = implementation_->receive_pairs("receive tag-track lookup");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

core::Result<std::vector<std::byte>> Client::artwork(const std::string_view uri,
                                                     const bool embedded) {
    constexpr std::size_t chunk_size = 1024U * 1024U;
    constexpr std::size_t maximum_artwork_size = 16U * 1024U * 1024U;
    if (uri.empty() || uri.contains('\0')) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD artwork URI is invalid",
                                           .context = {}});
    }
    const std::string uri_text{uri};
    std::vector<std::byte> result;
    std::vector<std::byte> chunk(chunk_size);
    while (true) {
        const auto offset = static_cast<unsigned>(result.size());
        const auto received =
            embedded ? mpd_run_readpicture(implementation_->connection.get(), uri_text.c_str(),
                                           offset, chunk.data(), chunk.size())
                     : mpd_run_albumart(implementation_->connection.get(), uri_text.c_str(), offset,
                                        chunk.data(), chunk.size());
        if (received < 0) {
            return std::unexpected(
                implementation_->take_error(embedded ? "receive readpicture" : "receive albumart"));
        }
        if (received == 0) {
            break;
        }
        const auto count = static_cast<std::size_t>(received);
        if (count > chunk.size() || count > maximum_artwork_size - result.size()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                               .message = "MPD artwork exceeds 16 MiB",
                                               .context = {}});
        }
        result.insert(result.end(), chunk.begin(), chunk.begin() + received);
        // MPD's negotiated binary-response limit is independent of this
        // receive buffer's capacity. Only an empty response means EOF.
    }
    return result;
}

namespace {

// ADR-0179: search-box rating terms for Melody's filter dialect. A term is
// one token — rating/albumrating, an operator, and a 0-10 integer; every
// other token stays ordinary search text.
struct MelodyRatingSearch {
    std::string words;
    std::vector<std::string> conditions;
};

[[nodiscard]] std::optional<std::string> melody_rating_condition(const std::string_view token) {
    static constexpr std::string_view track_name = "rating";
    static constexpr std::string_view album_name = "albumrating";
    std::string lowered;
    lowered.reserve(token.size());
    for (const auto character : token) {
        lowered.push_back(character >= 'A' && character <= 'Z'
                              ? static_cast<char>(character - 'A' + 'a')
                              : character);
    }
    const std::string_view text = lowered;
    std::string_view name;
    if (text.starts_with(album_name)) {
        name = album_name;
    } else if (text.starts_with(track_name)) {
        name = track_name;
    } else {
        return std::nullopt;
    }
    auto rest = text.substr(name.size());
    std::string_view comparator;
    for (const std::string_view candidate : {">=", "<=", "==", "=", ">", "<"}) {
        if (rest.starts_with(candidate)) {
            comparator = candidate == "=" ? "==" : candidate;
            rest = rest.substr(candidate.size());
            break;
        }
    }
    if (comparator.empty() || rest.empty()) {
        return std::nullopt;
    }
    unsigned value = 0U;
    const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != rest.data() + rest.size() ||
        value > maximum_rating) {
        return std::nullopt;
    }
    return "(" + std::string{name} + " " + std::string{comparator} + " " + std::to_string(value) +
           ")";
}

[[nodiscard]] MelodyRatingSearch parse_melody_rating_search(const std::string_view query) {
    MelodyRatingSearch parsed;
    std::size_t position = 0U;
    while (position < query.size()) {
        while (position < query.size() && query[position] == ' ') {
            ++position;
        }
        auto end = query.find(' ', position);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        const auto token = query.substr(position, end - position);
        position = end;
        if (token.empty()) {
            continue;
        }
        if (auto condition = melody_rating_condition(token)) {
            parsed.conditions.push_back(std::move(*condition));
            continue;
        }
        if (!parsed.words.empty()) {
            parsed.words += ' ';
        }
        parsed.words += token;
    }
    return parsed;
}

[[nodiscard]] std::string melody_filter_expression(const MelodyRatingSearch& parsed) {
    std::vector<std::string> parts;
    if (!parsed.words.empty()) {
        std::string escaped;
        escaped.reserve(parsed.words.size());
        for (const auto character : parsed.words) {
            if (character == '"' || character == '\\') {
                escaped.push_back('\\');
            }
            escaped.push_back(character);
        }
        parts.push_back("(any contains \"" + escaped + "\")");
    }
    parts.insert(parts.end(), parsed.conditions.begin(), parsed.conditions.end());
    if (parts.size() == 1U) {
        return parts.front();
    }
    std::string joined;
    for (const auto& part : parts) {
        if (!joined.empty()) {
            joined += " AND ";
        }
        joined += part;
    }
    return "(" + joined + ")";
}

} // namespace

core::Result<std::vector<Track>> Client::search_any(const std::string_view query,
                                                    const unsigned offset, const unsigned limit,
                                                    const bool melody_rating_filters) {
    constexpr unsigned maximum_page_size = 500U;
    if (query.empty() || query.contains('\0') || limit == 0U || limit > maximum_page_size ||
        offset > std::numeric_limits<unsigned>::max() - limit) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD search needs non-empty text and a page size between 1 and 500",
            .context = {},
        });
    }
    const std::string query_text{query};
    const auto parsed = melody_rating_filters
                            ? parse_melody_rating_search(query_text)
                            : MelodyRatingSearch{.words = query_text, .conditions = {}};
    auto* connection = implementation_->connection.get();
    if (!mpd_search_db_songs(connection, false)) {
        return std::unexpected(implementation_->take_error("begin search"));
    }
    const auto constrained =
        parsed.conditions.empty()
            ? mpd_search_add_any_tag_constraint(connection, MPD_OPERATOR_DEFAULT,
                                                query_text.c_str())
            : mpd_search_add_expression(connection, melody_filter_expression(parsed).c_str());
    if (!constrained || !mpd_search_add_window(connection, offset, offset + limit)) {
        mpd_search_cancel(connection);
        return std::unexpected(implementation_->take_error("build search"));
    }
    if (!mpd_search_commit(connection)) {
        return std::unexpected(implementation_->take_error("send search"));
    }
    auto pairs = implementation_->receive_pairs("receive search");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

core::Result<std::vector<Track>> Client::search_expression(const std::string_view filter_expression,
                                                           const std::string_view sort,
                                                           const unsigned limit,
                                                           std::optional<std::string> list) {
    // A query like "date IS 1992" legitimately matches thousands of tracks;
    // the window bounds one response, it does not decide what a search may
    // return.
    constexpr unsigned maximum_page_size = 20'000U;
    const std::string expression{filter_expression};
    if (expression.empty() || expression.contains('\0') || limit > maximum_page_size) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "expression search needs a filter and a "
                                                      "page size of at most 20000",
                                           .context = {}});
    }
    auto* connection = implementation_->connection.get();
    if (list) {
        if (list->contains('\0') || sort.find('\0') != std::string_view::npos)
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = "Invalid list search argument",
                                               .context = {}});
        const std::string sort_text{sort};
        const bool sent =
            sort.empty() ? mpd_send_command(connection, "melody_list_search", list->c_str(),
                                            expression.c_str(), nullptr)
                         : mpd_send_command(connection, "melody_list_search", list->c_str(),
                                            expression.c_str(), "sort", sort_text.c_str(), nullptr);
        if (!sent)
            return std::unexpected(implementation_->take_error("send list search"));
        auto pairs = implementation_->receive_pairs("receive list search");
        if (!pairs)
            return std::unexpected(pairs.error());
        return project_tracks(*pairs);
    }
    if (!mpd_search_db_songs(connection, false)) {
        return std::unexpected(implementation_->take_error("begin expression search"));
    }
    auto built = mpd_search_add_expression(connection, expression.c_str());
    if (built && !sort.empty()) {
        const auto descending = sort.front() == '-';
        const std::string name{descending ? sort.substr(1) : sort};
        built = mpd_search_add_sort_name(connection, name.c_str(), descending);
    }
    if (built && limit > 0U) {
        built = mpd_search_add_window(connection, 0U, limit);
    }
    if (!built) {
        mpd_search_cancel(connection);
        return std::unexpected(implementation_->take_error("build expression search"));
    }
    if (!mpd_search_commit(connection)) {
        return std::unexpected(implementation_->take_error("send expression search"));
    }
    auto pairs = implementation_->receive_pairs("receive expression search");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

core::Result<std::vector<MelodyAlbum>>
Client::search_melody_albums(const std::string_view filter_expression, const std::string_view sort,
                             const unsigned limit) {
    const std::string expression{filter_expression};
    if (expression.empty() || expression.contains('\0')) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Melody album search needs a filter "
                                                      "expression",
                                           .context = {}});
    }
    auto* connection = implementation_->connection.get();
    bool sent = false;
    const std::string sort_text{sort};
    const auto window = "0:" + std::to_string(limit);
    if (!sort.empty() && limit > 0U) {
        sent = mpd_send_command(connection, "searchalbums", expression.c_str(), "sort",
                                sort_text.c_str(), "window", window.c_str(), nullptr);
    } else if (!sort.empty()) {
        sent = mpd_send_command(connection, "searchalbums", expression.c_str(), "sort",
                                sort_text.c_str(), nullptr);
    } else if (limit > 0U) {
        sent = mpd_send_command(connection, "searchalbums", expression.c_str(), "window",
                                window.c_str(), nullptr);
    } else {
        sent = mpd_send_command(connection, "searchalbums", expression.c_str(), nullptr);
    }
    if (!sent) {
        return std::unexpected(implementation_->take_error("send searchalbums"));
    }
    auto pairs = implementation_->receive_pairs("receive searchalbums");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_melody_albums(*pairs);
}

core::Result<LibrarySearchResult> Client::search_library(const std::string_view query,
                                                         const unsigned track_limit,
                                                         const unsigned album_limit,
                                                         const unsigned offset,
                                                         const MelodySearchFeatures melody) {
    constexpr unsigned maximum_track_results = 500U;
    constexpr unsigned maximum_album_results = 2'000U;
    if (query.empty() || query.contains('\0') || track_limit == 0U ||
        track_limit > maximum_track_results || album_limit == 0U ||
        album_limit > maximum_album_results ||
        offset > std::numeric_limits<unsigned>::max() - track_limit) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD library search has invalid text or result limits",
            .context = {},
        });
    }

    if (offset > 0U) {
        auto tracks = search_any(query, offset, track_limit, melody.rating_filters);
        if (!tracks) {
            return std::unexpected(std::move(tracks.error()));
        }
        return LibrarySearchResult{.albums = {}, .tracks = std::move(*tracks)};
    }

    const std::string query_text{query};
    const auto parsed = melody.rating_filters
                            ? parse_melody_rating_search(query_text)
                            : MelodyRatingSearch{.words = query_text, .conditions = {}};
    auto* connection = implementation_->connection.get();
    if (!mpd_search_db_songs(connection, false)) {
        return std::unexpected(implementation_->take_error("begin library search"));
    }
    const auto constrained =
        parsed.conditions.empty()
            ? mpd_search_add_any_tag_constraint(connection, MPD_OPERATOR_DEFAULT,
                                                query_text.c_str())
            : mpd_search_add_expression(connection, melody_filter_expression(parsed).c_str());
    if (!constrained) {
        mpd_search_cancel(connection);
        return std::unexpected(implementation_->take_error("build library search"));
    }
    if (!mpd_search_commit(connection)) {
        return std::unexpected(implementation_->take_error("send library search"));
    }
    auto pairs = implementation_->receive_pairs("receive library search");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    auto tracks = project_tracks(*pairs);
    if (!tracks) {
        return std::unexpected(std::move(tracks.error()));
    }

    // The server's album records carry ratings, counts, and artwork
    // identities that derived summaries cannot; songs-only servers (or a
    // failed album search) keep the client-side derivation below.
    if (melody.album_search) {
        auto melody_albums =
            search_melody_albums(melody_filter_expression(parsed), {}, album_limit);
        if (melody_albums) {
            std::vector<AlbumSummary> albums;
            albums.reserve(melody_albums->size());
            for (auto& record : *melody_albums) {
                const auto dated = record.date != "0000" && !record.date.empty();
                albums.push_back(AlbumSummary{
                    .filter =
                        AlbumFilter{
                            .release_id = std::nullopt,
                            .artist = record.album_artist,
                            .album = record.album,
                            .date = dated ? std::optional{record.date} : std::nullopt,
                            .artist_is_album_artist = true,
                        },
                    .artist = std::move(record.album_artist),
                    .album = std::move(record.album),
                    .date = dated ? record.date : std::string{},
                    .artwork_uri = std::move(record.artwork_uri),
                    .rating = record.rating,
                    .computed_rating = record.computed_rating,
                    .track_count = record.track_count,
                    .duration_seconds = record.duration_seconds,
                });
            }
            if (tracks->size() > static_cast<std::size_t>(track_limit)) {
                tracks->resize(static_cast<std::size_t>(track_limit));
            }
            return LibrarySearchResult{.albums = std::move(albums), .tracks = std::move(*tracks)};
        }
    }

    std::vector<AlbumSummary> albums;
    albums.reserve(std::min<std::size_t>(tracks->size(), album_limit));
    std::unordered_set<std::string> album_keys;
    for (const auto& track : *tracks) {
        const auto album_value = track.metadata.first("Album");
        if (!album_value || album_value->empty()) {
            continue;
        }

        const auto album_artist_value = track.metadata.first("AlbumArtist");
        const auto track_artist_value = track.metadata.first("Artist");
        const bool has_album_artist = album_artist_value && !album_artist_value->empty();
        const std::string artist = has_album_artist     ? std::string{*album_artist_value}
                                   : track_artist_value ? std::string{*track_artist_value}
                                                        : std::string{};
        const std::string album{*album_value};
        const auto date_value = track.metadata.first("Date");
        const std::string date = date_value ? std::string{*date_value} : std::string{};
        const auto release_id =
            track.musicbrainz.release_ids.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{track.musicbrainz.release_ids.front()};

        std::string key;
        if (release_id) {
            key = "mbid:" + *release_id;
        } else {
            key.reserve(artist.size() + album.size() + date.size() + 2U);
            key.append(artist).push_back('\0');
            key.append(album).push_back('\0');
            key.append(date);
        }
        if (!album_keys.insert(std::move(key)).second || albums.size() >= album_limit) {
            continue;
        }

        albums.push_back(AlbumSummary{
            .filter =
                AlbumFilter{
                    .release_id = release_id,
                    .artist = artist,
                    .album = album,
                    .date = date.empty() ? std::nullopt : std::optional<std::string>{date},
                    .artist_is_album_artist = has_album_artist,
                },
            .artist = artist,
            .album = album,
            .date = date,
            .artwork_uri = track.uri,
        });
    }

    if (tracks->size() > static_cast<std::size_t>(track_limit)) {
        tracks->resize(static_cast<std::size_t>(track_limit));
    }
    return LibrarySearchResult{.albums = std::move(albums), .tracks = std::move(*tracks)};
}

core::Result<std::vector<Track>> Client::find_album(const AlbumFilter& album) {
    constexpr unsigned maximum_album_tracks = 4'096U;
    const auto invalid = [](const std::string& value) { return value.contains('\0'); };
    if ((!album.release_id && album.album.empty()) ||
        (album.release_id && (album.release_id->empty() || invalid(*album.release_id))) ||
        invalid(album.artist) || invalid(album.album) || (album.date && invalid(*album.date))) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD album lookup has invalid identity",
                                           .context = {}});
    }

    auto* connection = implementation_->connection.get();
    if (!mpd_search_db_songs(connection, true)) {
        return std::unexpected(implementation_->take_error("begin album lookup"));
    }
    const auto add_constraint = [connection](const mpd_tag_type tag, const std::string& value) {
        return value.empty() ||
               mpd_search_add_tag_constraint(connection, MPD_OPERATOR_DEFAULT, tag, value.c_str());
    };
    auto valid = true;
    if (album.release_id) {
        valid = add_constraint(MPD_TAG_MUSICBRAINZ_ALBUMID, *album.release_id);
    } else {
        valid = add_constraint(MPD_TAG_ALBUM, album.album) &&
                add_constraint(album.artist_is_album_artist ? MPD_TAG_ALBUM_ARTIST : MPD_TAG_ARTIST,
                               album.artist) &&
                (!album.date || add_constraint(MPD_TAG_DATE, *album.date));
    }
    if (!valid || !mpd_search_add_window(connection, 0U, maximum_album_tracks)) {
        mpd_search_cancel(connection);
        return std::unexpected(implementation_->take_error("build album lookup"));
    }
    if (!mpd_search_commit(connection)) {
        return std::unexpected(implementation_->take_error("send album lookup"));
    }
    auto pairs = implementation_->receive_pairs("receive album lookup");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

core::Result<std::vector<StoredPlaylist>> Client::stored_playlists() {
    if (!mpd_send_list_playlists(implementation_->connection.get())) {
        return std::unexpected(implementation_->take_error("send listplaylists"));
    }
    auto pairs = implementation_->receive_pairs("receive listplaylists");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    auto entries = project_database_entries(*pairs);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }
    std::vector<StoredPlaylist> playlists;
    playlists.reserve(entries->size());
    for (auto& entry : *entries) {
        auto* playlist = std::get_if<StoredPlaylist>(&entry);
        if (playlist == nullptr) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::backend,
                            .message = "MPD listplaylists returned a non-playlist entry",
                            .context = {}});
        }
        playlists.push_back(std::move(*playlist));
    }
    return playlists;
}

core::Result<std::vector<Track>> Client::stored_playlist(const std::string_view name) {
    if (name.empty() || name.contains('\0')) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "MPD stored playlist name cannot be empty or contain NUL",
                        .context = {}});
    }
    const std::string name_text{name};
    if (!mpd_send_list_playlist_meta(implementation_->connection.get(), name_text.c_str())) {
        return std::unexpected(implementation_->take_error("send listplaylistinfo"));
    }
    auto pairs = implementation_->receive_pairs("receive listplaylistinfo");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

core::Result<void> Client::save_queue_as_playlist(const std::string_view name) {
    if (name.empty() || name.contains('\0')) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist name cannot be empty or contain NUL",
            .context = {},
        });
    }
    const std::string name_text{name};
    if (!mpd_run_save(implementation_->connection.get(), name_text.c_str())) {
        return std::unexpected(implementation_->take_error("save stored playlist"));
    }
    return {};
}

core::Result<void> Client::load_stored_playlist_into_queue(const std::string_view name) {
    if (name.empty() || name.contains('\0')) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist name cannot be empty or contain NUL",
            .context = {},
        });
    }
    const std::string name_text{name};
    if (!mpd_run_load(implementation_->connection.get(), name_text.c_str())) {
        return std::unexpected(implementation_->take_error("load stored playlist"));
    }
    return {};
}

core::Result<void> Client::add_to_stored_playlist(const std::string_view name,
                                                  const std::string_view uri,
                                                  const std::optional<unsigned> position) {
    if (name.empty() || name.contains('\0') || uri.empty() || uri.contains('\0')) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist name and URI cannot be empty or contain NUL",
            .context = {},
        });
    }
    const std::string name_text{name};
    const std::string uri_text{uri};
    const auto succeeded =
        position ? mpd_run_playlist_add_to(implementation_->connection.get(), name_text.c_str(),
                                           uri_text.c_str(), *position)
                 : mpd_run_playlist_add(implementation_->connection.get(), name_text.c_str(),
                                        uri_text.c_str());
    if (!succeeded) {
        return std::unexpected(implementation_->take_error("add to stored playlist"));
    }
    return {};
}

core::Result<void> Client::add_to_stored_playlist(const std::string_view name,
                                                  const std::span<const std::string> uris,
                                                  const std::optional<unsigned> first_position,
                                                  const bool allow_melody_batch) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (name.empty() || name.contains('\0') || uris.empty() || uris.size() > maximum_batch_size) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist addition requires a valid name and 1 to 4096 URIs",
            .context = {},
        });
    }
    for (const auto& uri : uris) {
        if (uri.empty() || uri.contains('\0')) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "MPD stored playlist URIs cannot be empty or contain NUL",
                .context = {},
            });
        }
    }
    if (uris.size() == 1U) {
        return add_to_stored_playlist(name, uris.front(), first_position);
    }

    const std::string name_text{name};
    // Melody can take the whole batch as one write; plain MPD needs one
    // playlistadd per track, which costs a commit each on the server.
    if (!first_position && allow_melody_batch) {
        if (auto staged = stage_context_uris({uris.begin(), uris.end()}); !staged) {
            return staged;
        }
        return implementation_->run_composed("melody_playlistadd " + quoted_argument(name_text),
                                             "melody_playlistadd");
    }
    auto* connection = implementation_->connection.get();
    if (!mpd_command_list_begin(connection, false)) {
        return std::unexpected(
            implementation_->take_error("begin stored playlist add command list"));
    }
    unsigned position = first_position.value_or(0U);
    for (const auto& uri : uris) {
        const auto sent =
            first_position
                ? mpd_send_playlist_add_to(connection, name_text.c_str(), uri.c_str(), position++)
                : mpd_send_playlist_add(connection, name_text.c_str(), uri.c_str());
        if (!sent) {
            return std::unexpected(
                implementation_->take_error("send stored playlist add command list"));
        }
    }
    if (!mpd_command_list_end(connection) || !mpd_response_finish(connection)) {
        return std::unexpected(
            implementation_->take_error("finish stored playlist add command list"));
    }
    return {};
}

core::Result<void> Client::delete_from_stored_playlist(const std::string_view name,
                                                       const unsigned position) {
    if (name.empty() || name.contains('\0')) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist name cannot be empty or contain NUL",
            .context = {},
        });
    }
    const std::string name_text{name};
    if (!mpd_run_playlist_delete(implementation_->connection.get(), name_text.c_str(), position)) {
        return std::unexpected(implementation_->take_error("delete from stored playlist"));
    }
    return {};
}

core::Result<void> Client::delete_from_stored_playlist(const std::string_view name,
                                                       const std::span<const unsigned> positions) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (name.empty() || name.contains('\0') || positions.empty() ||
        positions.size() > maximum_batch_size) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist deletion requires a valid name and 1 to 4096 rows",
            .context = {},
        });
    }
    std::vector<unsigned> descending{positions.begin(), positions.end()};
    std::ranges::sort(descending, std::greater{});
    if (std::ranges::adjacent_find(descending) != descending.end()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist deletion positions must be unique",
            .context = {},
        });
    }
    if (descending.size() == 1U) {
        return delete_from_stored_playlist(name, descending.front());
    }

    const std::string name_text{name};
    auto* connection = implementation_->connection.get();
    if (!mpd_command_list_begin(connection, false)) {
        return std::unexpected(
            implementation_->take_error("begin stored playlist delete command list"));
    }
    for (const auto position : descending) {
        if (!mpd_send_playlist_delete(connection, name_text.c_str(), position)) {
            return std::unexpected(
                implementation_->take_error("send stored playlist delete command list"));
        }
    }
    if (!mpd_command_list_end(connection) || !mpd_response_finish(connection)) {
        return std::unexpected(
            implementation_->take_error("finish stored playlist delete command list"));
    }
    return {};
}

core::Result<void> Client::move_in_stored_playlist(const std::string_view name, const unsigned from,
                                                   const unsigned to) {
    if (name.empty() || name.contains('\0') || from == to) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist move requires a valid name and distinct positions",
            .context = {},
        });
    }
    const std::string name_text{name};
    if (!mpd_run_playlist_move(implementation_->connection.get(), name_text.c_str(), from, to)) {
        return std::unexpected(implementation_->take_error("move in stored playlist"));
    }
    return {};
}

core::Result<void> Client::clear_stored_playlist(const std::string_view name) {
    if (name.empty() || name.contains('\0')) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist name cannot be empty or contain NUL",
            .context = {},
        });
    }
    const std::string name_text{name};
    if (!mpd_run_playlist_clear(implementation_->connection.get(), name_text.c_str())) {
        return std::unexpected(implementation_->take_error("clear stored playlist"));
    }
    return {};
}

core::Result<void> Client::rename_stored_playlist(const std::string_view from,
                                                  const std::string_view to) {
    if (from.empty() || from.contains('\0') || to.empty() || to.contains('\0') || from == to) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist rename requires distinct valid names",
            .context = {},
        });
    }
    const std::string from_text{from};
    const std::string to_text{to};
    if (!mpd_run_rename(implementation_->connection.get(), from_text.c_str(), to_text.c_str())) {
        return std::unexpected(implementation_->take_error("rename stored playlist"));
    }
    return {};
}

core::Result<void> Client::delete_stored_playlist(const std::string_view name) {
    if (name.empty() || name.contains('\0')) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "MPD stored playlist name cannot be empty or contain NUL",
            .context = {},
        });
    }
    const std::string name_text{name};
    if (!mpd_run_rm(implementation_->connection.get(), name_text.c_str())) {
        return std::unexpected(implementation_->take_error("delete stored playlist"));
    }
    return {};
}

core::Result<std::vector<Output>> Client::outputs() {
    auto pairs = command_pairs("outputs");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_outputs(*pairs);
}

core::Result<void> Client::run_transport(const TransportAction action) {
    bool succeeded = false;
    std::string_view stage;
    switch (action) {
    case TransportAction::play:
        stage = "play";
        succeeded = mpd_run_play(implementation_->connection.get());
        break;
    case TransportAction::pause:
        stage = "pause";
        succeeded = mpd_run_pause(implementation_->connection.get(), true);
        break;
    case TransportAction::resume:
        stage = "resume";
        succeeded = mpd_run_pause(implementation_->connection.get(), false);
        break;
    case TransportAction::stop:
        stage = "stop";
        succeeded = mpd_run_stop(implementation_->connection.get());
        break;
    case TransportAction::next:
        stage = "next";
        succeeded = mpd_run_next(implementation_->connection.get());
        break;
    case TransportAction::previous:
        stage = "previous";
        succeeded = mpd_run_previous(implementation_->connection.get());
        break;
    }
    if (!succeeded) {
        return std::unexpected(implementation_->take_error(stage));
    }
    return {};
}

core::Result<void> Client::play_id(const std::uint32_t song_id) {
    if (!mpd_run_play_id(implementation_->connection.get(), song_id)) {
        return std::unexpected(implementation_->take_error("playid"));
    }
    return {};
}

core::Result<void> Client::play_position(const unsigned position) {
    if (!mpd_run_play_pos(implementation_->connection.get(), position)) {
        return std::unexpected(implementation_->take_error("play"));
    }
    return {};
}

core::Result<std::uint32_t> Client::add_id(const std::string_view uri,
                                           const std::optional<unsigned> position) {
    if (uri.empty() || uri.contains('\0')) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "MPD queue URI cannot be empty or contain NUL",
                        .context = {}});
    }
    const std::string uri_text{uri};
    const auto song_id =
        position ? mpd_run_add_id_to(implementation_->connection.get(), uri_text.c_str(), *position)
                 : mpd_run_add_id(implementation_->connection.get(), uri_text.c_str());
    if (song_id < 0) {
        return std::unexpected(implementation_->take_error("addid"));
    }
    return static_cast<std::uint32_t>(song_id);
}

core::Result<void> Client::add_ids(const std::span<const QueueAddition> additions) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (additions.empty() || additions.size() > maximum_batch_size) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "MPD queue addition batches must contain between 1 and 4096 items",
            .context = {},
        });
    }
    for (const auto& addition : additions) {
        if (addition.uri.empty() || addition.uri.contains('\0')) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "MPD queue addition URI cannot be empty or contain NUL",
                .context = {},
            });
        }
    }
    if (additions.size() == 1U) {
        auto added = add_id(additions.front().uri, additions.front().position);
        if (!added) {
            return std::unexpected(std::move(added.error()));
        }
        return {};
    }

    auto* connection = implementation_->connection.get();
    if (!mpd_command_list_begin(connection, false)) {
        return std::unexpected(implementation_->take_error("begin addid command list"));
    }
    for (const auto& addition : additions) {
        const auto sent = addition.position ? mpd_send_add_id_to(connection, addition.uri.c_str(),
                                                                 *addition.position)
                                            : mpd_send_add_id(connection, addition.uri.c_str());
        if (!sent) {
            return std::unexpected(implementation_->take_error("send addid command list"));
        }
    }
    if (!mpd_command_list_end(connection) || !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("finish addid command list"));
    }
    return {};
}

core::Result<void> Client::delete_id(const std::uint32_t song_id) {
    if (!mpd_run_delete_id(implementation_->connection.get(), song_id)) {
        return std::unexpected(implementation_->take_error("deleteid"));
    }
    return {};
}

core::Result<void> Client::delete_ids(const std::span<const std::uint32_t> song_ids) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (song_ids.empty() || song_ids.size() > maximum_batch_size) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "MPD queue deletion batches must contain between 1 and 4096 items",
            .context = {},
        });
    }
    std::unordered_set<std::uint32_t> unique_ids;
    unique_ids.reserve(song_ids.size());
    for (const auto song_id : song_ids) {
        if (!unique_ids.insert(song_id).second) {
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = "MPD queue deletion IDs must be unique",
                                               .context = {}});
        }
    }
    if (song_ids.size() == 1U) {
        return delete_id(song_ids.front());
    }

    auto* connection = implementation_->connection.get();
    if (!mpd_command_list_begin(connection, false)) {
        return std::unexpected(implementation_->take_error("begin deleteid command list"));
    }
    for (const auto song_id : song_ids) {
        if (!mpd_send_delete_id(connection, song_id)) {
            return std::unexpected(implementation_->take_error("send deleteid command list"));
        }
    }
    if (!mpd_command_list_end(connection) || !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("finish deleteid command list"));
    }
    return {};
}

core::Result<std::vector<std::string>> Client::newest_tag_values(const std::string_view tag,
                                                                 const unsigned track_limit,
                                                                 const bool melody_album_order) {
    if (melody_album_order && tag == "AlbumArtist") {
        auto pairs = command_pairs("melody_albums_latest");
        if (!pairs)
            return std::unexpected(std::move(pairs.error()));
        std::vector<std::string> ordered;
        std::unordered_set<std::string> seen;
        for (const auto& pair : *pairs) {
            if (pair.name != "X-Album")
                continue;
            const auto first = pair.value.find('\t');
            const auto second =
                first == std::string::npos ? first : pair.value.find('\t', first + 1);
            if (first == std::string::npos || second == std::string::npos) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "Melody returned an invalid latest-album record",
                                .context = {}});
            }
            auto artist = pair.value.substr(first + 1, second - first - 1);
            if (!artist.empty() && seen.insert(artist).second)
                ordered.push_back(std::move(artist));
        }
        return ordered;
    }
    const std::string tag_name{tag};
    auto* connection = implementation_->connection.get();
    // "Added" is the database insertion time (MPD 0.24) — the honest
    // "recently added" signal. File mtime is only an approximation for
    // older servers: copies that preserve timestamps sort by their
    // original encode dates there.
    const auto* const sort_key =
        mpd_connection_cmp_server_version(connection, 0, 24, 0) >= 0 ? "Added" : "Last-Modified";
    if (!mpd_search_db_songs(connection, false)) {
        return std::unexpected(implementation_->take_error("begin newest lookup"));
    }
    // Sorted searches need an expression; every track modified since the
    // epoch is every track.
    if (!mpd_search_add_expression(connection, "(modified-since '1970-01-01T00:00:00Z')") ||
        !mpd_search_add_sort_name(connection, sort_key, true) ||
        !mpd_search_add_window(connection, 0U, track_limit)) {
        mpd_search_cancel(connection);
        return std::unexpected(implementation_->take_error("build newest lookup"));
    }
    if (!mpd_search_commit(connection)) {
        return std::unexpected(implementation_->take_error("send newest lookup"));
    }
    auto pairs = implementation_->receive_pairs("receive newest lookup");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    auto tracks = project_tracks(*pairs);
    if (!tracks) {
        return std::unexpected(std::move(tracks.error()));
    }
    std::vector<std::string> ordered;
    std::unordered_set<std::string> seen;
    for (const auto& track : *tracks) {
        auto values = track.metadata.values(tag_name);
        if (values.empty() && tag_name == "AlbumArtist") {
            values = track.metadata.values("Artist");
        }
        for (const auto value : values) {
            std::string owned{value};
            if (!owned.empty() && seen.insert(owned).second) {
                ordered.push_back(std::move(owned));
            }
        }
    }
    return ordered;
}

core::Result<void> Client::update_database(const std::string& uri) {
    if (mpd_run_update(implementation_->connection.get(), uri.empty() ? nullptr : uri.c_str()) ==
        0U) {
        return std::unexpected(implementation_->take_error("update"));
    }
    return {};
}

core::Result<void> Client::clear_queue() {
    if (!mpd_run_clear(implementation_->connection.get())) {
        return std::unexpected(implementation_->take_error("clear"));
    }
    return {};
}

core::Result<void> Client::move_id(const std::uint32_t song_id, const unsigned position) {
    if (!mpd_run_move_id(implementation_->connection.get(), song_id, position)) {
        return std::unexpected(implementation_->take_error("moveid"));
    }
    return {};
}

core::Result<void> Client::move_ids(const std::span<const QueueMove> moves) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (moves.empty() || moves.size() > maximum_batch_size) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "MPD queue move batches must contain between 1 and 4096 moves",
            .context = {},
        });
    }
    std::unordered_set<std::uint32_t> unique_ids;
    unique_ids.reserve(moves.size());
    for (const auto& move : moves) {
        if (!unique_ids.insert(move.song_id).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "MPD queue move batches must address each song ID at most once",
                .context = {},
            });
        }
    }
    if (moves.size() == 1U) {
        return move_id(moves.front().song_id, moves.front().position);
    }

    auto* connection = implementation_->connection.get();
    if (!mpd_command_list_begin(connection, false)) {
        return std::unexpected(implementation_->take_error("begin moveid command list"));
    }
    for (const auto& move : moves) {
        if (!mpd_send_move_id(connection, move.song_id, move.position)) {
            return std::unexpected(implementation_->take_error("send moveid command list"));
        }
    }
    if (!mpd_command_list_end(connection) || !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("finish moveid command list"));
    }
    return {};
}

core::Result<void> Client::set_priority_id(const std::uint32_t song_id, const unsigned priority) {
    if (priority > 255U) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "MPD queue priority must be between 0 and 255",
                        .context = {}});
    }
    if (!mpd_run_prio_id(implementation_->connection.get(), priority, song_id)) {
        return std::unexpected(implementation_->take_error("prioid"));
    }
    return {};
}

core::Result<void> Client::set_priority_ids(const std::span<const std::uint32_t> song_ids,
                                            const unsigned priority) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (song_ids.empty() || song_ids.size() > maximum_batch_size) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "MPD queue priority batches must contain between 1 and 4096 items",
            .context = {},
        });
    }
    if (priority > 255U) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "MPD queue priority must be between 0 and 255",
                        .context = {}});
    }
    std::unordered_set<std::uint32_t> unique_ids;
    unique_ids.reserve(song_ids.size());
    for (const auto song_id : song_ids) {
        if (!unique_ids.insert(song_id).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "MPD queue priority batches must address each song ID at most once",
                .context = {},
            });
        }
    }
    if (song_ids.size() == 1U) {
        return set_priority_id(song_ids.front(), priority);
    }

    auto* connection = implementation_->connection.get();
    if (!mpd_command_list_begin(connection, false)) {
        return std::unexpected(implementation_->take_error("begin prioid command list"));
    }
    for (const auto song_id : song_ids) {
        if (!mpd_send_prio_id(connection, priority, song_id)) {
            return std::unexpected(implementation_->take_error("send prioid command list"));
        }
    }
    if (!mpd_command_list_end(connection) || !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("finish prioid command list"));
    }
    return {};
}

namespace {

[[nodiscard]] std::optional<core::Error> invalid_rating(const unsigned rating) {
    if (rating > maximum_rating) {
        return core::Error{.code = core::ErrorCode::invalid_argument,
                           .message = "track and album ratings must be between 0 and 10",
                           .context = {}};
    }
    return std::nullopt;
}

} // namespace

core::Result<void> Client::set_sticker_rating(const std::string_view uri, const unsigned rating) {
    if (auto error = invalid_rating(rating)) {
        return std::unexpected(std::move(*error));
    }
    if (uri.empty() || uri.contains('\0')) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD sticker URI cannot be empty or "
                                                      "contain NUL",
                                           .context = {}});
    }
    const std::string uri_text{uri};
    auto* connection = implementation_->connection.get();
    if (rating == 0U) {
        if (!mpd_run_sticker_delete(connection, "song", uri_text.c_str(), "rating")) {
            auto error = implementation_->take_error("sticker delete");
            // Deleting a sticker that never existed is already the requested
            // outcome, not a failure.
            if (error.code == core::ErrorCode::not_found) {
                return {};
            }
            return std::unexpected(std::move(error));
        }
        return {};
    }
    const auto value = std::to_string(rating);
    if (!mpd_run_sticker_set(connection, "song", uri_text.c_str(), "rating", value.c_str())) {
        return std::unexpected(implementation_->take_error("sticker set"));
    }
    return {};
}

core::Result<void> Client::set_sticker_ratings(const std::span<const std::string> uris,
                                               const unsigned rating) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (uris.empty() || uris.size() > maximum_batch_size) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "MPD sticker rating batches must contain between 1 and 4096 items",
            .context = {},
        });
    }
    if (auto error = invalid_rating(rating)) {
        return std::unexpected(std::move(*error));
    }
    std::unordered_set<std::string_view> unique_uris;
    unique_uris.reserve(uris.size());
    for (const auto& uri : uris) {
        if (uri.empty() || uri.contains('\0')) {
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = "MPD sticker URIs cannot be empty or "
                                                          "contain NUL",
                                               .context = {}});
        }
        if (!unique_uris.insert(uri).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "MPD sticker rating batches must address each URI at most once",
                .context = {},
            });
        }
    }
    if (rating == 0U) {
        // A command list would abort at the first song that has no sticker,
        // leaving the rest rated; unrate one by one and tolerate not-found.
        for (const auto& uri : uris) {
            if (auto result = set_sticker_rating(uri, 0U); !result) {
                return result;
            }
        }
        return {};
    }
    if (uris.size() == 1U) {
        return set_sticker_rating(uris.front(), rating);
    }

    const auto value = std::to_string(rating);
    auto* connection = implementation_->connection.get();
    if (!mpd_command_list_begin(connection, false)) {
        return std::unexpected(implementation_->take_error("begin sticker command list"));
    }
    for (const auto& uri : uris) {
        if (!mpd_send_sticker_set(connection, "song", uri.c_str(), "rating", value.c_str())) {
            return std::unexpected(implementation_->take_error("send sticker command list"));
        }
    }
    if (!mpd_command_list_end(connection) || !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("finish sticker command list"));
    }
    return {};
}

core::Result<std::vector<TrackRating>> Client::sticker_ratings() {
    if (!mpd_send_command(implementation_->connection.get(), "sticker", "find", "song", "",
                          "rating", nullptr)) {
        return std::unexpected(implementation_->take_error("send sticker find"));
    }
    auto pairs = implementation_->receive_pairs("receive sticker find");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_sticker_ratings(*pairs);
}

core::Result<void> Client::set_melody_track_rating(const std::uint64_t song_id,
                                                   const unsigned rating) {
    if (auto error = invalid_rating(rating)) {
        return std::unexpected(std::move(*error));
    }
    auto* connection = implementation_->connection.get();
    const auto id_text = std::to_string(song_id);
    const auto rating_text = std::to_string(rating);
    if (!mpd_send_command(connection, "rate", id_text.c_str(), rating_text.c_str(), nullptr) ||
        !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("rate"));
    }
    return {};
}

core::Result<void> Client::set_melody_track_ratings(const std::span<const std::uint64_t> song_ids,
                                                    const unsigned rating) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (song_ids.empty() || song_ids.size() > maximum_batch_size) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "Melody rating batches must contain between 1 and 4096 items",
            .context = {},
        });
    }
    if (auto error = invalid_rating(rating)) {
        return std::unexpected(std::move(*error));
    }
    std::unordered_set<std::uint64_t> unique_ids;
    unique_ids.reserve(song_ids.size());
    for (const auto song_id : song_ids) {
        if (!unique_ids.insert(song_id).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "Melody rating batches must address each song ID at most once",
                .context = {},
            });
        }
    }
    if (song_ids.size() == 1U) {
        return set_melody_track_rating(song_ids.front(), rating);
    }

    auto* connection = implementation_->connection.get();
    const auto rating_text = std::to_string(rating);
    if (!mpd_command_list_begin(connection, false)) {
        return std::unexpected(implementation_->take_error("begin rate command list"));
    }
    for (const auto song_id : song_ids) {
        const auto id_text = std::to_string(song_id);
        if (!mpd_send_command(connection, "rate", id_text.c_str(), rating_text.c_str(), nullptr)) {
            return std::unexpected(implementation_->take_error("send rate command list"));
        }
    }
    if (!mpd_command_list_end(connection) || !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("finish rate command list"));
    }
    return {};
}

namespace {

[[nodiscard]] std::optional<core::Error> invalid_album_key(const MelodyAlbumKey& key) {
    if (key.album_artist.empty() || key.album.empty()) {
        return core::Error{.code = core::ErrorCode::invalid_argument,
                           .message = "Melody album ratings need a non-empty album artist "
                                      "and album",
                           .context = {}};
    }
    for (const auto* part : {&key.album_artist, &key.album, &key.date}) {
        if (part->contains('\0')) {
            return core::Error{.code = core::ErrorCode::invalid_argument,
                               .message = "Melody album identity cannot contain NUL",
                               .context = {}};
        }
    }
    return std::nullopt;
}

} // namespace

core::Result<void> Client::set_melody_album_rating(const MelodyAlbumKey& key,
                                                   const unsigned rating) {
    if (auto error = invalid_rating(rating)) {
        return std::unexpected(std::move(*error));
    }
    if (auto error = invalid_album_key(key)) {
        return std::unexpected(std::move(*error));
    }
    auto* connection = implementation_->connection.get();
    const auto rating_text = std::to_string(rating);
    if (!mpd_send_command(connection, "albumrate", key.album_artist.c_str(), key.album.c_str(),
                          key.date.c_str(), rating_text.c_str(), nullptr) ||
        !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("albumrate"));
    }
    return {};
}
namespace {
// libmpdclient writes a command through a fixed 4 KiB buffer, so a list of
// any real size cannot travel on one line: it is staged across several
// "melody_context stage" lines and the command that consumes it carries no
// URIs. This bound leaves room for the verb and quoting.
constexpr std::size_t maximum_command_line = 3'000U;

} // namespace

// Stages a track list across as many lines as it takes. The leading bare
// "stage" discards anything a failed earlier command left behind, so a list
// is never silently prefixed with someone else's tracks.
core::Result<void> Client::stage_context_uris(const std::vector<std::string>& uris) {
    if (auto cleared =
            implementation_->run_composed("melody_context stage", "melody_context stage");
        !cleared) {
        return cleared;
    }
    std::string line = "melody_context stage";
    for (const auto& uri : uris) {
        auto argument = quoted_argument(uri);
        if (line.size() + argument.size() + 1U > maximum_command_line) {
            if (auto sent = implementation_->run_composed(line, "melody_context stage"); !sent) {
                return sent;
            }
            line = "melody_context stage";
        }
        line += ' ';
        line += argument;
    }
    return implementation_->run_composed(line, "melody_context stage");
}

// Queue-context edits (docs/protocol.md). While another list is the active
// queue the queue context is the server's stash — the list the Queue tab is
// showing — so edits aimed at that tab have to address it there.
core::Result<void> Client::melody_context_play(const std::string_view name,
                                               const std::optional<unsigned> row) {
    auto* connection = implementation_->connection.get();
    const std::string playlist{name};
    const auto row_text = row ? std::to_string(*row) : std::string{};
    const auto sent =
        row ? mpd_send_command(connection, "melody_context", "play", playlist.c_str(),
                               row_text.c_str(), nullptr)
            : mpd_send_command(connection, "melody_context", "play", playlist.c_str(), nullptr);
    if (!sent || !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("melody_context play"));
    }
    return {};
}

// Queue-context edits (docs/protocol.md).
core::Result<void> Client::melody_context_queue_write(const bool replace,
                                                      const std::vector<std::string>& uris,
                                                      const int position) {
    if (uris.empty()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A queue edit needs at least one track",
                                           .context = {}});
    }
    if (auto staged = stage_context_uris(uris); !staged) {
        return staged;
    }
    const std::string line = (replace ? std::string{"melody_context queuereplace "}
                                      : std::string{"melody_context queueadd "}) +
                             std::to_string(position);
    return implementation_->run_composed(line, "melody_context queue write");
}

core::Result<void> Client::melody_context_queue_delete(const std::vector<unsigned>& rows) {
    if (rows.empty()) {
        return {};
    }
    // Rows shift as earlier ones go, so a chunked delete walks from the back.
    std::vector<unsigned> descending{rows};
    std::ranges::sort(descending, std::greater{});
    std::string line = "melody_context queuedelete";
    for (const auto row : descending) {
        const auto argument = std::to_string(row);
        if (line.size() + argument.size() + 1U > maximum_command_line) {
            if (auto sent = implementation_->run_composed(line, "melody_context queuedelete");
                !sent) {
                return sent;
            }
            line = "melody_context queuedelete";
        }
        line += ' ';
        line += argument;
    }
    return implementation_->run_composed(line, "melody_context queuedelete");
}
core::Result<void> Client::melody_context_queue_move(const unsigned from, const unsigned to) {
    return implementation_->run_composed("melody_context queuemove " + std::to_string(from) + " " +
                                             std::to_string(to),
                                         "melody_context queuemove");
}

core::Result<void> Client::melody_context_queue(const std::optional<unsigned> row) {
    auto* connection = implementation_->connection.get();
    const auto row_text = row ? std::to_string(*row) : std::string{};
    const auto sent =
        row ? mpd_send_command(connection, "melody_context", "queue", row_text.c_str(), nullptr)
            : mpd_send_command(connection, "melody_context", "queue", nullptr);
    if (!sent || !mpd_response_finish(connection)) {
        return std::unexpected(implementation_->take_error("melody_context queue"));
    }
    return {};
}

core::Result<LastFmReply> Client::lastfm(const LastFmCommand& command) {
    const auto& op = command.operation;
    if ((op != "status" && op != "begin" && op != "finish" && op != "disconnect" &&
         op != "enable" && op != "love" && op != "unlove" && op != "info") ||
        command.arguments.size() > 2)
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Invalid Last.fm command",
                                           .context = {}});
    std::string line = "melody_lastfm " + op;
    for (const auto& arg : command.arguments) {
        if (arg.size() > 1024 || arg.find_first_of("\r\n") != std::string::npos ||
            arg.find('\0') != std::string::npos)
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = "Invalid Last.fm argument",
                                               .context = {}});
        line += " " + quoted_argument(arg);
    }
    if (!mpd_send_command(implementation_->connection.get(), line.c_str(), nullptr))
        return std::unexpected(implementation_->take_error("melody_lastfm"));
    auto pairs = implementation_->receive_pairs("melody_lastfm");
    if (!pairs)
        return std::unexpected(pairs.error());
    for (const auto& pair : *pairs)
        if (pair.name == "lastfm")
            return LastFmReply{pair.value};
    return std::unexpected(core::Error{
        .code = core::ErrorCode::backend, .message = "Missing Last.fm response", .context = {}});
}

core::Result<RequestQueueState> Client::request_queue() {
    if (!mpd_send_command(implementation_->connection.get(), "melody_upnext", nullptr))
        return std::unexpected(implementation_->take_error("melody_upnext"));
    auto pairs = implementation_->receive_pairs("melody_upnext");
    if (!pairs)
        return std::unexpected(pairs.error());
    RequestQueueState state;
    bool has_revision = false;
    const auto first_track = std::find_if(pairs->begin(), pairs->end(), [](const Pair& pair) {
        return ascii_case_equal(pair.name, "file");
    });
    for (auto it = pairs->begin(); it != first_track; ++it) {
        const auto& pair = *it;
        if (pair.name == "undo")
            state.can_undo = pair.value == "1";
        if (pair.name == "context")
            state.context = pair.value;
        if (pair.name == "revision" || pair.name == "active") {
            unsigned value = 0;
            auto parsed =
                std::from_chars(pair.value.data(), pair.value.data() + pair.value.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != pair.value.data() + pair.value.size())
                return std::unexpected(core::Error{.code = core::ErrorCode::backend,
                                                   .message = "Malformed Up Next response",
                                                   .context = {}});
            if (pair.name == "revision") {
                state.revision = value;
                has_revision = true;
            } else
                state.active_id = value;
        }
    }
    if (!has_revision)
        return std::unexpected(core::Error{.code = core::ErrorCode::backend,
                                           .message = "Up Next response has no revision",
                                           .context = {}});
    auto tracks = project_tracks(std::span<const Pair>{first_track, pairs->end()});
    if (!tracks)
        return std::unexpected(tracks.error());
    state.pending = std::move(*tracks);
    return state;
}

core::Result<void> Client::edit_request_queue(const RequestQueueCommand& command) {
    if (command.operation == RequestQueueOperation::retain) {
        std::string line = "melody_upnext_edit " + std::to_string(command.revision);
        for (const auto id : command.ids)
            line += " " + std::to_string(id);
        return implementation_->run_composed(line, "melody_upnext_edit");
    }
    std::string operation;
    switch (command.operation) {
    case RequestQueueOperation::append:
        operation = "append";
        break;
    case RequestQueueOperation::prepend:
        operation = "prepend";
        break;
    case RequestQueueOperation::remove:
        operation = "remove";
        break;
    case RequestQueueOperation::move:
        operation = "move";
        break;
    case RequestQueueOperation::clear:
        operation = "clear";
        break;
    case RequestQueueOperation::resume:
        operation = "return";
        break;
    case RequestQueueOperation::insert:
        operation = "insert";
        break;
    case RequestQueueOperation::play:
        operation = "play";
        break;
    case RequestQueueOperation::retain:
        break;
    case RequestQueueOperation::undo:
        operation = "undo";
        break;
    }
    if (command.operation == RequestQueueOperation::append ||
        command.operation == RequestQueueOperation::prepend ||
        command.operation == RequestQueueOperation::insert) {
        if (auto staged = stage_context_uris(command.uris); !staged)
            return staged;
    }
    std::string line = "melody_upnext " + operation + " " + std::to_string(command.revision);
    if (command.operation == RequestQueueOperation::insert)
        line += " " + std::to_string(command.position);
    if (command.operation == RequestQueueOperation::remove ||
        command.operation == RequestQueueOperation::move ||
        command.operation == RequestQueueOperation::play)
        line += " " + std::to_string(command.id);
    if (command.operation == RequestQueueOperation::move)
        line += " " + std::to_string(command.position);
    return implementation_->run_composed(line, "melody_upnext");
}

core::Result<std::vector<Track>> Client::melody_context_queue_tracks() {
    if (!mpd_send_command(implementation_->connection.get(), "melody_context", "queueinfo",
                          nullptr)) {
        return std::unexpected(implementation_->take_error("melody_context queueinfo"));
    }
    auto pairs = implementation_->receive_pairs("receive melody_context queueinfo");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    return project_tracks(*pairs);
}

// Scratch lists (docs/protocol.md): stored playlists a client presents as
// working tabs rather than beside curated playlists.
core::Result<std::vector<std::string>> Client::melody_scratch_lists() {
    if (!mpd_send_command(implementation_->connection.get(), "melody_scratch", nullptr)) {
        return std::unexpected(implementation_->take_error("melody_scratch"));
    }
    auto pairs = implementation_->receive_pairs("receive melody_scratch");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    std::vector<std::string> names;
    for (const auto& pair : *pairs) {
        if (ascii_case_equal(pair.name, "scratch")) {
            names.push_back(pair.value);
        }
    }
    return names;
}

core::Result<void> Client::melody_set_scratch(const std::string& name, const bool scratch) {
    return implementation_->run_composed(
        "melody_scratch " + quoted_argument(name) + (scratch ? " 1" : " 0"), "melody_scratch set");
}

core::Result<MelodyContextState> Client::melody_active_context() {
    if (!mpd_send_command(implementation_->connection.get(), "melody_context", nullptr)) {
        return std::unexpected(implementation_->take_error("melody_context"));
    }
    auto pairs = implementation_->receive_pairs("receive melody_context");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    MelodyContextState state;
    for (const auto& pair : *pairs) {
        if (ascii_case_equal(pair.name, "context")) {
            state.name = pair.value;
        } else if (ascii_case_equal(pair.name, "stashed")) {
            state.queue_stashed = pair.value != "0";
        }
    }
    return state;
}

core::Result<MelodyAlbumRating> Client::melody_album_rating(const MelodyAlbumKey& key) {
    if (auto error = invalid_album_key(key)) {
        return std::unexpected(std::move(*error));
    }
    if (!mpd_send_command(implementation_->connection.get(), "getalbumrating",
                          key.album_artist.c_str(), key.album.c_str(), key.date.c_str(), nullptr)) {
        return std::unexpected(implementation_->take_error("send getalbumrating"));
    }
    auto pairs = implementation_->receive_pairs("receive getalbumrating");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    MelodyAlbumRating result;
    for (const auto& pair : *pairs) {
        if (ascii_case_equal(pair.name, "rating")) {
            unsigned value = 0U;
            const auto* begin = pair.value.data();
            const auto* end = begin + pair.value.size();
            const auto [parsed, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsed != end || value > maximum_rating) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::backend,
                                .message = "Melody returned an invalid album rating",
                                .context = {}});
            }
            result.rating = value;
        } else if (ascii_case_equal(pair.name, "computed")) {
            double value = 0.0;
            const auto* begin = pair.value.data();
            const auto* end = begin + pair.value.size();
            const auto [parsed, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsed != end || value < 0.0 ||
                value > static_cast<double>(maximum_rating)) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::backend,
                                .message = "Melody returned an invalid computed album rating",
                                .context = {}});
            }
            result.computed = value;
        }
    }
    return result;
}

core::Result<void> Client::seek_id(const std::uint32_t song_id,
                                   const std::chrono::milliseconds position) {
    if (position.count() < 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD seek position cannot be negative",
                                           .context = {}});
    }
    const auto seconds = std::chrono::duration<float>(position).count();
    if (!std::isfinite(seconds) ||
        !mpd_run_seek_id_float(implementation_->connection.get(), song_id, seconds)) {
        return std::unexpected(implementation_->take_error("seekid"));
    }
    return {};
}

core::Result<void> Client::set_volume(const unsigned volume) {
    if (volume > 100U) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD volume must be between 0 and 100",
                                           .context = {}});
    }
    if (!mpd_run_set_volume(implementation_->connection.get(), volume)) {
        return std::unexpected(implementation_->take_error("setvol"));
    }
    return {};
}

core::Result<void> Client::set_repeat(const bool enabled) {
    if (!mpd_run_repeat(implementation_->connection.get(), enabled)) {
        return std::unexpected(implementation_->take_error("repeat"));
    }
    return {};
}

core::Result<void> Client::set_random(const bool enabled) {
    if (!mpd_run_random(implementation_->connection.get(), enabled)) {
        return std::unexpected(implementation_->take_error("random"));
    }
    return {};
}

core::Result<void> Client::set_single(const PlaybackModeState state) {
    const auto backend_state = to_mpd_single_state(state);
    if (!backend_state) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD single mode cannot be unknown",
                                           .context = {}});
    }
    if (!mpd_run_single_state(implementation_->connection.get(), *backend_state)) {
        return std::unexpected(implementation_->take_error("single"));
    }
    return {};
}

core::Result<void> Client::set_consume(const PlaybackModeState state) {
    const auto backend_state = to_mpd_consume_state(state);
    if (!backend_state) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD consume mode cannot be unknown",
                                           .context = {}});
    }
    if (!mpd_run_consume_state(implementation_->connection.get(), *backend_state)) {
        return std::unexpected(implementation_->take_error("consume"));
    }
    return {};
}

core::Result<ReplayGainMode> Client::replay_gain_mode() {
    auto pairs = command_pairs("replay_gain_status");
    if (!pairs) {
        return std::unexpected(std::move(pairs.error()));
    }
    for (const auto& pair : *pairs) {
        if (!ascii_case_equal(pair.name, "replay_gain_mode")) {
            continue;
        }
        if (pair.value == "off") {
            return ReplayGainMode::off;
        }
        if (pair.value == "track") {
            return ReplayGainMode::track;
        }
        if (pair.value == "album") {
            return ReplayGainMode::album;
        }
        if (pair.value == "auto") {
            return ReplayGainMode::automatic;
        }
    }
    return std::unexpected(core::Error{.code = core::ErrorCode::backend,
                                       .message = "MPD returned an invalid ReplayGain mode",
                                       .context = {}});
}

core::Result<void> Client::set_replay_gain_mode(const ReplayGainMode mode) {
    const auto backend_mode = to_mpd_replay_gain_mode(mode);
    if (!backend_mode) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "MPD ReplayGain mode cannot be unknown",
                                           .context = {}});
    }
    if (!mpd_run_replay_gain_mode(implementation_->connection.get(), *backend_mode)) {
        return std::unexpected(implementation_->take_error("replay_gain_mode"));
    }
    return {};
}

core::Result<void> Client::set_output_enabled(const std::uint32_t output_id, const bool enabled) {
    const auto succeeded =
        enabled ? mpd_run_enable_output(implementation_->connection.get(), output_id)
                : mpd_run_disable_output(implementation_->connection.get(), output_id);
    if (!succeeded) {
        return std::unexpected(
            implementation_->take_error(enabled ? "enableoutput" : "disableoutput"));
    }
    return {};
}

core::Result<void> Client::ping() {
    auto response = command_pairs("ping");
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    return {};
}

core::Result<IdleEvents> Client::wait_for_idle(const core::CancellationToken& cancellation) {
    if (!mpd_send_idle(implementation_->connection.get())) {
        return std::unexpected(implementation_->take_error("send idle"));
    }

    pollfd descriptor{
        .fd = mpd_connection_get_fd(implementation_->connection.get()),
        .events = POLLIN,
        .revents = 0,
    };
    while (!cancellation.is_cancellation_requested()) {
        const auto result = ::poll(&descriptor, 1, 100);
        if (result > 0) {
            // Generic pairs retain advertised extensions that libmpdclient's
            // fixed idle enum cannot represent. Unknown names stay ignored.
            auto pairs = implementation_->receive_pairs("receive idle");
            if (!pairs) {
                return std::unexpected(pairs.error());
            }
            IdleEvents events;
            for (const auto& pair : *pairs) {
                if (pair.name != "changed")
                    continue;
                if (pair.value == "stats") {
                    events.mask |= static_cast<std::uint32_t>(IdleEvent::listening_statistics);
                } else {
                    events.mask |=
                        project_idle_events(mpd_idle_name_parse(pair.value.c_str())).mask;
                }
            }
            return events;
        }
        if (result < 0 && errno != EINTR) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::io,
                .message = "polling the MPD idle connection failed",
                .context = {{"errno", std::to_string(errno)}},
            });
        }
        descriptor.revents = 0;
    }

    if (!mpd_send_noidle(implementation_->connection.get())) {
        return std::unexpected(implementation_->take_error("cancel idle"));
    }
    static_cast<void>(mpd_recv_idle(implementation_->connection.get(), false));
    if (mpd_connection_get_error(implementation_->connection.get()) != MPD_ERROR_SUCCESS) {
        return std::unexpected(implementation_->take_error("finish cancelled idle"));
    }
    return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                       .message = "MPD idle wait was cancelled",
                                       .context = {}});
}

} // namespace trackknife::mpd
