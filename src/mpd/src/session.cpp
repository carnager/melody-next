// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/mpd/session.hpp"

#include "trackknife/core/cancellation.hpp"
#include "trackknife/mpd/projection.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace trackknife::mpd {
namespace {

constexpr std::uint32_t full_refresh = 1U << 31U;
// Melody rating mutations change listing contents without bumping the queue
// version, so their refresh must bypass the version-compatible shortcut.
constexpr std::uint32_t rating_refresh = 1U << 30U;

void invoke_safely(const std::function<void(const SessionState&)>& callback,
                   const SessionState& value) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(value);
    } catch (...) {
        // Observers cannot be allowed to terminate a protocol worker.
    }
}

void invoke_safely(const std::function<void(const SessionSnapshot&)>& callback,
                   const SessionSnapshot& value) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(value);
    } catch (...) {
    }
}

void invoke_safely(const std::function<void(IdleEvents)>& callback, IdleEvents value) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(value);
    } catch (...) {
    }
}

void invoke_safely(const std::function<void(const SessionCommandResult&)>& callback,
                   const SessionCommandResult& value) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(value);
    } catch (...) {
    }
}

[[nodiscard]] bool requires_reconnect(const core::Error& error) {
    // A client-side protocol failure is not a definitive server rejection:
    // reconnect rather than keep talking over a connection whose state we no
    // longer know.
    if (std::ranges::any_of(error.context, [](const core::ErrorContext& context) {
            return context.key == "local_protocol_error" && context.value == "true";
        })) {
        return true;
    }
    switch (error.code) {
    case core::ErrorCode::invalid_argument:
    case core::ErrorCode::not_found:
    case core::ErrorCode::conflict:
    case core::ErrorCode::unsupported:
    case core::ErrorCode::limit_exceeded:
        return false;
    case core::ErrorCode::cancelled:
    case core::ErrorCode::io:
    case core::ErrorCode::backend:
    case core::ErrorCode::database:
    case core::ErrorCode::invariant:
        break;
    }
    return std::ranges::none_of(error.context, [](const core::ErrorContext& context) {
        return context.key == "connection_recoverable" && context.value == "true";
    });
}

[[nodiscard]] bool targets_live_queue_occurrence(const SessionCommandKind kind) noexcept {
    switch (kind) {
    case SessionCommandKind::queue_play:
    case SessionCommandKind::queue_delete:
    case SessionCommandKind::queue_delete_batch:
    case SessionCommandKind::queue_move:
    case SessionCommandKind::queue_move_batch:
    case SessionCommandKind::queue_priority:
    case SessionCommandKind::seek:
        return true;
    default:
        return false;
    }
}

} // namespace

struct Session::Impl {
    struct PendingCommand {
        std::uint64_t id{0U};
        SessionCommandKind kind{SessionCommandKind::transport};
        TransportAction action{TransportAction::play};
        std::uint32_t object_id{0U};
        std::vector<std::uint32_t> object_ids;
        std::vector<unsigned> positions;
        std::chrono::milliseconds position{0};
        unsigned volume{0U};
        bool enabled{false};
        bool embedded{false};
        PlaybackModeState playback_mode{PlaybackModeState::unknown};
        ReplayGainMode replay_gain_mode{ReplayGainMode::unknown};
        std::string uri;
        std::string secondary_uri;
        std::vector<std::string> uris;
        AlbumFilter album;
        std::vector<QueueAddition> additions;
        std::vector<QueueMove> moves;
        unsigned priority{0U};
        std::optional<unsigned> queue_position;
        unsigned query_offset{0U};
        unsigned query_limit{200U};
        unsigned rating{0U};
        std::vector<std::uint64_t> melody_song_ids;
        MelodyAlbumKey album_rating_key;
        RequestQueueCommand request;
        LastFmCommand lastfm;
    };

    static constexpr std::size_t maximum_pending_commands = 64U;

    [[nodiscard]] static bool is_query(const SessionCommandKind kind) noexcept {
        return kind == SessionCommandKind::database_newest ||
               kind == SessionCommandKind::database_browse ||
               kind == SessionCommandKind::database_tag ||
               kind == SessionCommandKind::database_album_counts ||
               kind == SessionCommandKind::database_tag_tracks ||
               kind == SessionCommandKind::artwork || kind == SessionCommandKind::database_search ||
               kind == SessionCommandKind::database_album ||
               kind == SessionCommandKind::stored_playlists ||
               kind == SessionCommandKind::stored_playlist ||
               kind == SessionCommandKind::melody_album_rating ||
               kind == SessionCommandKind::database_expression_search;
    }

    Profile profile;
    SessionCallbacks callbacks;
    core::CancellationSource cancellation;
    std::atomic_bool stopping{false};
    // Set from the advertised commands on each full refresh; read by the
    // command worker when it builds search constraints.
    std::atomic_bool melody_rating_search{false};
    std::atomic_bool melody_album_search{false};
    std::atomic_bool melody_latest_albums{false};
    std::atomic_bool melody_contexts{false};
    // Melody takes a whole batch of playlist additions as one write.
    std::atomic_bool melody_playlist_batch{false};
    std::atomic_uint32_t pending_refresh{full_refresh};
    std::atomic_uint64_t next_command_id{1U};
    std::atomic_uint64_t active_generation{0U};
    std::mutex wake_mutex;
    std::condition_variable wake;
    bool accepting_commands{false};
    std::deque<PendingCommand> pending_commands;
    std::jthread command_worker;
    std::jthread idle_worker;

    Impl(Profile session_profile, SessionCallbacks session_callbacks)
        : profile(std::move(session_profile)), callbacks(std::move(session_callbacks)) {
        command_worker = std::jthread([this](std::stop_token stop) { run_commands(stop); });
        idle_worker = std::jthread([this](std::stop_token stop) { run_idle(stop); });
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        cancellation.request_cancellation();
        command_worker.request_stop();
        idle_worker.request_stop();
        wake.notify_all();
    }

    void publish_state(SessionPhase phase, std::uint64_t generation,
                       std::optional<core::Error> error = std::nullopt) const noexcept {
        invoke_safely(
            callbacks.state_changed,
            SessionState{.phase = phase, .generation = generation, .error = std::move(error)});
    }

    [[nodiscard]] bool wait_before_reconnect(std::stop_token stop, unsigned& attempt) {
        constexpr auto initial_delay = std::chrono::milliseconds{500};
        constexpr auto maximum_delay = std::chrono::milliseconds{10'000};
        constexpr unsigned maximum_shift = 4U;
        const auto shift = std::min(attempt, maximum_shift);
        const auto base = std::min(initial_delay * (1U << shift), maximum_delay);
        thread_local std::minstd_rand generator{std::random_device{}()};
        std::uniform_int_distribution<std::chrono::milliseconds::rep> jitter{
            0, std::max<std::chrono::milliseconds::rep>(1, base.count() / 4)};
        const auto delay = base + std::chrono::milliseconds{jitter(generator)};
        attempt = std::min(attempt + 1U, maximum_shift);
        std::unique_lock lock{wake_mutex};
        return wake.wait_for(lock, delay, [this, stop] {
            return stop.stop_requested() || stopping.load(std::memory_order_acquire);
        });
    }

    // The command worker and the idle worker's reconnect backoff share one
    // condition variable, so a single notification can be consumed by the
    // wrong waiter. Always notify every waiter; the loser re-checks its
    // predicate and sleeps again.
    void request(std::uint32_t mask) {
        pending_refresh.fetch_or(mask, std::memory_order_release);
        wake.notify_all();
    }

    void finish_command(PendingCommand command, SessionCommandPayload payload,
                        std::optional<core::Error> error) const noexcept {
        invoke_safely(
            callbacks.command_finished,
            SessionCommandResult{.id = command.id,
                                 .generation = active_generation.load(std::memory_order_acquire),
                                 .kind = command.kind,
                                 .action = command.action,
                                 .payload = std::move(payload),
                                 .error = std::move(error)});
    }

    void reject_pending(const core::Error& error) {
        std::deque<PendingCommand> rejected;
        {
            std::lock_guard lock{wake_mutex};
            accepting_commands = false;
            rejected.swap(pending_commands);
        }
        for (auto& command : rejected) {
            finish_command(command, std::monostate{}, error);
        }
    }

    [[nodiscard]] std::uint64_t enqueue(PendingCommand command) {
        command.id = next_command_id.fetch_add(1U, std::memory_order_relaxed);
        std::optional<core::Error> rejection;
        {
            std::lock_guard lock{wake_mutex};
            if (!accepting_commands) {
                rejection = core::Error{.code = core::ErrorCode::backend,
                                        .message = "MPD transport is not connected",
                                        .context = {}};
            } else if (pending_commands.size() >= maximum_pending_commands) {
                rejection = core::Error{.code = core::ErrorCode::backend,
                                        .message = "MPD command queue is full",
                                        .context = {}};
            } else {
                if (is_query(command.kind)) {
                    pending_commands.push_back(command);
                } else {
                    const auto first_query =
                        std::ranges::find_if(pending_commands, [](const PendingCommand& queued) {
                            return is_query(queued.kind);
                        });
                    pending_commands.insert(first_query, command);
                }
            }
        }
        if (rejection) {
            finish_command(command, std::monostate{}, std::move(rejection));
        } else {
            wake.notify_all();
        }
        return command.id;
    }

    void cancel_pending(const std::uint64_t command_id) {
        std::optional<PendingCommand> cancelled;
        {
            std::lock_guard lock{wake_mutex};
            const auto found = std::ranges::find(pending_commands, command_id, &PendingCommand::id);
            if (found != pending_commands.end()) {
                cancelled = std::move(*found);
                pending_commands.erase(found);
            }
        }
        if (cancelled) {
            finish_command(std::move(*cancelled), std::monostate{},
                           core::Error{.code = core::ErrorCode::cancelled,
                                       .message = "MPD request was superseded before it started",
                                       .context = {}});
        }
    }

    [[nodiscard]] core::Result<SessionCommandPayload> execute(Client& client,
                                                              const PendingCommand& command) {
        const auto without_payload =
            [](core::Result<void> result) -> core::Result<SessionCommandPayload> {
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return std::monostate{};
        };
        switch (command.kind) {
        case SessionCommandKind::transport:
            return without_payload(client.run_transport(command.action));
        case SessionCommandKind::lastfm: {
            auto result = client.lastfm(command.lastfm);
            if (!result)
                return std::unexpected(result.error());
            return std::move(*result);
        }
        case SessionCommandKind::request_queue_edit:
            return without_payload(client.edit_request_queue(command.request));
        case SessionCommandKind::melody_context_play:
            return without_payload(client.melody_context_play(command.uri, command.queue_position));
        case SessionCommandKind::melody_context_queue:
            return without_payload(client.melody_context_queue(command.queue_position));
        case SessionCommandKind::melody_scratch_lists: {
            auto names = client.melody_scratch_lists();
            if (!names) {
                return std::unexpected(std::move(names.error()));
            }
            return SessionCommandPayload{std::move(*names)};
        }
        case SessionCommandKind::melody_set_scratch:
            return without_payload(client.melody_set_scratch(command.uri, command.enabled));
        case SessionCommandKind::melody_context_queue_add:
            return without_payload(client.melody_context_queue_write(
                false, command.uris, static_cast<int>(command.object_id) - 1));
        case SessionCommandKind::melody_context_queue_replace:
            return without_payload(client.melody_context_queue_write(
                true, command.uris, static_cast<int>(command.queue_position.value_or(0U))));
        case SessionCommandKind::melody_context_queue_delete:
            return without_payload(client.melody_context_queue_delete(command.positions));
        case SessionCommandKind::melody_context_queue_move:
            return without_payload(client.melody_context_queue_move(
                command.object_id, command.queue_position.value_or(0U)));
        case SessionCommandKind::queue_play:
            return without_payload(command.queue_position
                                       ? client.play_position(*command.queue_position)
                                       : client.play_id(command.object_id));
        case SessionCommandKind::queue_add: {
            auto added = client.add_id(command.uri, command.queue_position);
            if (!added) {
                return std::unexpected(std::move(added.error()));
            }
            return std::monostate{};
        }
        case SessionCommandKind::queue_add_batch:
            return without_payload(client.add_ids(command.additions));
        case SessionCommandKind::queue_delete:
            return without_payload(client.delete_id(command.object_id));
        case SessionCommandKind::queue_delete_batch:
            return without_payload(client.delete_ids(command.object_ids));
        case SessionCommandKind::queue_clear:
            return without_payload(client.clear_queue());
        case SessionCommandKind::database_update:
            return without_payload(client.update_database(command.uri));
        case SessionCommandKind::database_newest: {
            auto result =
                client.newest_tag_values(command.uri, command.priority,
                                         melody_latest_albums.load(std::memory_order_acquire));
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::queue_move:
            if (auto result =
                    client.move_id(command.object_id, command.queue_position.value_or(0U));
                !result) {
                return std::unexpected(std::move(result.error()));
            }
            return std::monostate{};
        case SessionCommandKind::queue_move_batch:
            return without_payload(client.move_ids(command.moves));
        case SessionCommandKind::queue_priority:
            return without_payload(client.set_priority_ids(command.object_ids, command.priority));
        case SessionCommandKind::database_browse: {
            auto result = client.browse(command.uri);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::database_album_counts: {
            auto result = client.album_counts(command.uri);
            if (!result)
                return std::unexpected(std::move(result.error()));
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::database_tag: {
            auto result = client.list_tag(command.uri);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::database_tag_tracks: {
            auto result =
                client.find_tag_tracks(command.uri, command.secondary_uri, command.query_limit);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::artwork: {
            auto result = client.artwork(command.uri, command.embedded);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::database_search: {
            auto result = client.search_library(
                command.uri, command.query_limit, 1'000U, command.query_offset,
                {.rating_filters = melody_rating_search.load(std::memory_order_acquire),
                 .album_search = melody_album_search.load(std::memory_order_acquire)});
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::database_album: {
            auto result = client.find_album(command.album);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::stored_playlists: {
            auto result = client.stored_playlists();
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::stored_playlist: {
            auto result = client.stored_playlist(command.uri);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        case SessionCommandKind::stored_playlist_save:
            return without_payload(client.save_queue_as_playlist(command.uri));
        case SessionCommandKind::stored_playlist_load:
            return without_payload(client.load_stored_playlist_into_queue(command.uri));
        case SessionCommandKind::stored_playlist_add:
            return without_payload(client.add_to_stored_playlist(command.uri, command.secondary_uri,
                                                                 command.queue_position));
        case SessionCommandKind::stored_playlist_add_batch:
            return without_payload(client.add_to_stored_playlist(
                command.uri, command.uris, command.queue_position,
                melody_playlist_batch.load(std::memory_order_acquire)));
        case SessionCommandKind::stored_playlist_delete_item:
            return without_payload(
                client.delete_from_stored_playlist(command.uri, command.object_id));
        case SessionCommandKind::stored_playlist_delete_batch:
            return without_payload(
                client.delete_from_stored_playlist(command.uri, command.positions));
        case SessionCommandKind::stored_playlist_move_item:
            return without_payload(client.move_in_stored_playlist(
                command.uri, command.object_id, command.queue_position.value_or(0U)));
        case SessionCommandKind::stored_playlist_clear:
            return without_payload(client.clear_stored_playlist(command.uri));
        case SessionCommandKind::stored_playlist_rename:
            return without_payload(
                client.rename_stored_playlist(command.uri, command.secondary_uri));
        case SessionCommandKind::stored_playlist_remove:
            return without_payload(client.delete_stored_playlist(command.uri));
        case SessionCommandKind::repeat:
            return without_payload(client.set_repeat(command.enabled));
        case SessionCommandKind::random:
            return without_payload(client.set_random(command.enabled));
        case SessionCommandKind::single:
            return without_payload(client.set_single(command.playback_mode));
        case SessionCommandKind::consume:
            return without_payload(client.set_consume(command.playback_mode));
        case SessionCommandKind::replay_gain:
            return without_payload(client.set_replay_gain_mode(command.replay_gain_mode));
        case SessionCommandKind::seek:
            return without_payload(client.seek_id(command.object_id, command.position));
        case SessionCommandKind::volume:
            return without_payload(client.set_volume(command.volume));
        case SessionCommandKind::output_enabled:
            return without_payload(client.set_output_enabled(command.object_id, command.enabled));
        case SessionCommandKind::sticker_rating:
            return without_payload(client.set_sticker_ratings(command.uris, command.rating));
        case SessionCommandKind::melody_rating:
            return without_payload(
                client.set_melody_track_ratings(command.melody_song_ids, command.rating));
        case SessionCommandKind::melody_album_rate:
            return without_payload(
                client.set_melody_album_rating(command.album_rating_key, command.rating));
        case SessionCommandKind::melody_album_rating: {
            auto result = client.melody_album_rating(command.album_rating_key);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{*result};
        }
        case SessionCommandKind::database_expression_search: {
            auto result =
                client.search_expression(command.uri, command.secondary_uri, command.query_limit);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            return SessionCommandPayload{std::move(*result)};
        }
        }
        return std::unexpected(core::Error{.code = core::ErrorCode::backend,
                                           .message = "Unknown MPD session command",
                                           .context = {}});
    }

    [[nodiscard]] static std::uint32_t refresh_after(const PendingCommand& command) noexcept {
        switch (command.kind) {
        case SessionCommandKind::transport:
        case SessionCommandKind::seek:
            return static_cast<std::uint32_t>(IdleEvent::player);
        case SessionCommandKind::request_queue_edit:
        case SessionCommandKind::melody_context_play:
        case SessionCommandKind::melody_context_queue:
        case SessionCommandKind::melody_context_queue_add:
        case SessionCommandKind::melody_context_queue_replace:
        case SessionCommandKind::melody_context_queue_delete:
        case SessionCommandKind::melody_context_queue_move:
            return static_cast<std::uint32_t>(IdleEvent::queue) |
                   static_cast<std::uint32_t>(IdleEvent::player);
        case SessionCommandKind::lastfm:
        case SessionCommandKind::melody_scratch_lists:
            return 0U;
        case SessionCommandKind::melody_set_scratch:
            return static_cast<std::uint32_t>(IdleEvent::stored_playlist);
        case SessionCommandKind::queue_play:
            return static_cast<std::uint32_t>(IdleEvent::player);
        case SessionCommandKind::queue_delete:
        case SessionCommandKind::queue_delete_batch:
        case SessionCommandKind::queue_clear:
        case SessionCommandKind::queue_add:
        case SessionCommandKind::queue_add_batch:
        case SessionCommandKind::queue_move:
        case SessionCommandKind::queue_move_batch:
        case SessionCommandKind::queue_priority:
            return static_cast<std::uint32_t>(IdleEvent::queue) |
                   static_cast<std::uint32_t>(IdleEvent::player);
        case SessionCommandKind::database_update:
        case SessionCommandKind::database_newest:
        case SessionCommandKind::database_browse:
        case SessionCommandKind::database_tag:
        case SessionCommandKind::database_album_counts:
        case SessionCommandKind::database_tag_tracks:
        case SessionCommandKind::artwork:
        case SessionCommandKind::database_search:
        case SessionCommandKind::database_album:
        case SessionCommandKind::stored_playlists:
        case SessionCommandKind::stored_playlist:
        case SessionCommandKind::stored_playlist_save:
        case SessionCommandKind::stored_playlist_add:
        case SessionCommandKind::stored_playlist_add_batch:
        case SessionCommandKind::stored_playlist_delete_item:
        case SessionCommandKind::stored_playlist_delete_batch:
        case SessionCommandKind::stored_playlist_move_item:
        case SessionCommandKind::stored_playlist_clear:
        case SessionCommandKind::stored_playlist_rename:
        case SessionCommandKind::stored_playlist_remove:
            return 0U;
        case SessionCommandKind::stored_playlist_load:
            return static_cast<std::uint32_t>(IdleEvent::queue) |
                   static_cast<std::uint32_t>(IdleEvent::player);
        case SessionCommandKind::repeat:
        case SessionCommandKind::random:
        case SessionCommandKind::single:
        case SessionCommandKind::consume:
        case SessionCommandKind::replay_gain:
            return static_cast<std::uint32_t>(IdleEvent::options);
        case SessionCommandKind::volume:
            return static_cast<std::uint32_t>(IdleEvent::mixer);
        case SessionCommandKind::output_enabled:
            return static_cast<std::uint32_t>(IdleEvent::output);
        case SessionCommandKind::sticker_rating:
            return static_cast<std::uint32_t>(IdleEvent::sticker);
        case SessionCommandKind::melody_rating:
        case SessionCommandKind::melody_album_rate:
            return rating_refresh;
        case SessionCommandKind::melody_album_rating:
        case SessionCommandKind::database_expression_search:
            return 0U;
        }
        return full_refresh;
    }

    [[nodiscard]] core::Result<void> refresh(Client& client, SessionSnapshot& snapshot,
                                             std::uint32_t requested) {
        const bool all = (requested & full_refresh) != 0U;
        const auto previous_queue_version = snapshot.status.queue_version;
        if (all) {
            auto capabilities = client.capabilities();
            if (!capabilities) {
                return std::unexpected(std::move(capabilities.error()));
            }
            snapshot.capabilities = std::move(*capabilities);
            melody_rating_search.store(snapshot.capabilities.supports_command("getrating"),
                                       std::memory_order_release);
            melody_playlist_batch.store(
                snapshot.capabilities.supports_command("melody_playlistadd"),
                std::memory_order_release);
            melody_contexts.store(snapshot.capabilities.supports_command("melody_context"),
                                  std::memory_order_release);
            melody_latest_albums.store(
                snapshot.capabilities.supports_command("melody_albums_latest"),
                std::memory_order_release);
            melody_album_search.store(snapshot.capabilities.supports_command("searchalbums"),
                                      std::memory_order_release);
        }
        constexpr auto status_events = static_cast<std::uint32_t>(IdleEvent::player) |
                                       static_cast<std::uint32_t>(IdleEvent::queue) |
                                       static_cast<std::uint32_t>(IdleEvent::mixer) |
                                       static_cast<std::uint32_t>(IdleEvent::options) |
                                       static_cast<std::uint32_t>(IdleEvent::update);
        if (all || (requested & status_events) != 0U) {
            auto status = client.status();
            if (!status) {
                return std::unexpected(std::move(status.error()));
            }
            snapshot.status = std::move(*status);
            snapshot.status_sample_time = std::chrono::steady_clock::now();
        }
        if (all || (requested & static_cast<std::uint32_t>(IdleEvent::player)) != 0U) {
            auto current = client.current_song();
            if (!current) {
                return std::unexpected(std::move(current.error()));
            }
            snapshot.current_song = std::move(*current);
        }
        if (snapshot.capabilities.supports_command("melody_upnext") &&
            (all || (requested & (static_cast<std::uint32_t>(IdleEvent::queue) |
                                  static_cast<std::uint32_t>(IdleEvent::player))) != 0U)) {
            auto state = client.request_queue();
            if (!state)
                return std::unexpected(state.error());
            snapshot.requests = std::move(*state);
        }
        // ADR-0187: which stored playlist is materialized as the playback
        // context. Refreshed with the queue, since a switch replaces it.
        if (snapshot.capabilities.supports_command("melody_context") &&
            (all || (requested & static_cast<std::uint32_t>(IdleEvent::queue)) != 0U)) {
            auto context = client.melody_active_context();
            if (!context) {
                return std::unexpected(std::move(context.error()));
            }
            snapshot.context = *context;
            if (context->queue_stashed) {
                auto stashed = client.melody_context_queue_tracks();
                if (!stashed) {
                    return std::unexpected(std::move(stashed.error()));
                }
                snapshot.queue_context_tracks = std::move(*stashed);
            } else {
                snapshot.queue_context_tracks.clear();
            }
        }
        // Melody ratings ride on listing lines without bumping the queue
        // version, so a rating refresh must bypass the reconcile shortcuts.
        const bool rating_forced = (requested & rating_refresh) != 0U &&
                                   snapshot.capabilities.supports_command("getrating");
        if (all || rating_forced ||
            (requested & static_cast<std::uint32_t>(IdleEvent::queue)) != 0U) {
            std::optional<std::vector<Track>> reconciled;
            const auto current_queue_version = snapshot.status.queue_version;
            const auto current_queue_length = snapshot.status.queue_length;
            const bool same_compatible_version =
                !all && !rating_forced && previous_queue_version && current_queue_version &&
                current_queue_length && *previous_queue_version == *current_queue_version &&
                snapshot.queue.size() == *current_queue_length;
            const bool can_diff = !all && !rating_forced && previous_queue_version &&
                                  current_queue_version && current_queue_length &&
                                  *current_queue_version > *previous_queue_version &&
                                  snapshot.capabilities.supports_command("plchanges");
            if (can_diff) {
                auto changes = client.queue_changes(*previous_queue_version);
                if (changes) {
                    auto applied =
                        apply_queue_changes(snapshot.queue, *changes, *current_queue_length);
                    if (applied) {
                        reconciled = std::move(*applied);
                    }
                }
            } else if (same_compatible_version) {
                reconciled = snapshot.queue;
            }
            if (!reconciled) {
                auto queue = client.queue_snapshot();
                if (!queue) {
                    return std::unexpected(std::move(queue.error()));
                }
                reconciled = std::move(*queue);
            }
            snapshot.queue = std::move(*reconciled);
        }
        if (all || (requested & static_cast<std::uint32_t>(IdleEvent::output)) != 0U) {
            auto outputs = client.outputs();
            if (!outputs) {
                return std::unexpected(std::move(outputs.error()));
            }
            snapshot.outputs = std::move(*outputs);
        }
        if ((all || rating_forced ||
             (requested & static_cast<std::uint32_t>(IdleEvent::sticker)) != 0U) &&
            snapshot.capabilities.supports_command("sticker")) {
            auto ratings = client.sticker_ratings();
            if (!ratings) {
                return std::unexpected(std::move(ratings.error()));
            }
            snapshot.sticker_ratings = std::move(*ratings);
        }
        if ((all || (requested & static_cast<std::uint32_t>(IdleEvent::options)) != 0U) &&
            snapshot.capabilities.supports_command("replay_gain_status")) {
            auto replay_gain = client.replay_gain_mode();
            if (!replay_gain) {
                return std::unexpected(std::move(replay_gain.error()));
            }
            snapshot.replay_gain_mode = *replay_gain;
        }
        return {};
    }

    void run_commands(std::stop_token stop) {
        std::uint64_t generation = 0U;
        unsigned reconnect_attempt = 0U;
        while (!stop.stop_requested() && !stopping.load(std::memory_order_acquire)) {
            ++generation;
            active_generation.store(generation, std::memory_order_release);
            publish_state(generation == 1U ? SessionPhase::connecting : SessionPhase::reconnecting,
                          generation);
            auto connected = Client::connect(profile);
            if (!connected) {
                publish_state(SessionPhase::reconnecting, generation, std::move(connected.error()));
                if (wait_before_reconnect(stop, reconnect_attempt)) {
                    break;
                }
                continue;
            }

            auto client = std::move(*connected);
            SessionSnapshot snapshot;
            snapshot.generation = generation;
            pending_refresh.fetch_or(full_refresh, std::memory_order_release);
            bool reconnect = false;
            publish_state(SessionPhase::connected, generation);

            while (!stop.stop_requested() && !stopping.load(std::memory_order_acquire)) {
                const auto requested = pending_refresh.exchange(0U, std::memory_order_acq_rel);
                if (requested != 0U) {
                    auto refreshed = refresh(client, snapshot, requested);
                    if (!refreshed) {
                        publish_state(SessionPhase::reconnecting, generation,
                                      std::move(refreshed.error()));
                        reject_pending(core::Error{
                            .code = core::ErrorCode::io,
                            .message = "MPD connection was lost before queued commands ran",
                            .context = {},
                        });
                        reconnect = true;
                        break;
                    }
                    // Accept commands before publishing: an observer reacting
                    // to the authoritative snapshot may immediately issue a
                    // command, and a healthy connection must not reject it.
                    reconnect_attempt = 0U;
                    {
                        std::lock_guard lock{wake_mutex};
                        accepting_commands = true;
                    }
                    invoke_safely(callbacks.snapshot_changed, snapshot);
                }

                std::optional<PendingCommand> command;
                {
                    std::lock_guard lock{wake_mutex};
                    if (!pending_commands.empty()) {
                        command = pending_commands.front();
                        pending_commands.pop_front();
                    }
                }
                if (command) {
                    auto result = execute(client, *command);
                    if (!result) {
                        auto error = std::move(result.error());
                        if (targets_live_queue_occurrence(command->kind) &&
                            (error.code == core::ErrorCode::not_found ||
                             error.code == core::ErrorCode::conflict)) {
                            error.context.push_back({"queue_conflict", "true"});
                            error.context.push_back({"server_message", error.message});
                            error.code = core::ErrorCode::conflict;
                            error.message =
                                "Live queue changed before the command could be applied";
                            pending_refresh.fetch_or(
                                static_cast<std::uint32_t>(IdleEvent::queue) |
                                    static_cast<std::uint32_t>(IdleEvent::player),
                                std::memory_order_release);
                        }
                        if (command->kind == SessionCommandKind::request_queue_edit) {
                            pending_refresh.fetch_or(refresh_after(*command),
                                                     std::memory_order_release);
                        }
                        finish_command(*command, std::monostate{}, error);
                        if (!requires_reconnect(error)) {
                            continue;
                        }
                        publish_state(SessionPhase::reconnecting, generation, error);
                        reject_pending(core::Error{
                            .code = core::ErrorCode::io,
                            .message = "MPD connection failed; queued commands were not sent",
                            .context = {},
                        });
                        reconnect = true;
                        break;
                    }
                    finish_command(*command, std::move(*result), std::nullopt);
                    pending_refresh.fetch_or(refresh_after(*command), std::memory_order_release);
                    continue;
                }

                std::unique_lock lock{wake_mutex};
                wake.wait(lock, [this, stop] {
                    return stop.stop_requested() || stopping.load(std::memory_order_acquire) ||
                           pending_refresh.load(std::memory_order_acquire) != 0U ||
                           !pending_commands.empty();
                });
            }
            if (!reconnect) {
                break;
            }
            if (wait_before_reconnect(stop, reconnect_attempt)) {
                break;
            }
        }
        reject_pending(core::Error{.code = core::ErrorCode::cancelled,
                                   .message = "MPD session stopped before queued commands ran",
                                   .context = {}});
        publish_state(SessionPhase::stopped, generation);
    }

    void run_idle(std::stop_token stop) {
        unsigned reconnect_attempt = 0U;
        while (!stop.stop_requested() && !stopping.load(std::memory_order_acquire)) {
            auto connected = Client::connect(profile);
            if (!connected) {
                if (wait_before_reconnect(stop, reconnect_attempt)) {
                    break;
                }
                continue;
            }
            auto client = std::move(*connected);
            reconnect_attempt = 0U;
            while (!stop.stop_requested() && !stopping.load(std::memory_order_acquire)) {
                auto events = client.wait_for_idle(cancellation.token());
                if (!events) {
                    // The command connection may otherwise remain asleep indefinitely on a
                    // socket closed by a restarted daemon.  Waking it for an authoritative
                    // refresh makes that connection observe the failure and enter its normal
                    // generation-guarded reconnect path without waiting for user input.
                    request(full_refresh);
                    break;
                }
                invoke_safely(callbacks.idle_received, *events);
                request(events->mask);
            }
            if (!stop.stop_requested() && !stopping.load(std::memory_order_acquire) &&
                wait_before_reconnect(stop, reconnect_attempt)) {
                break;
            }
        }
    }
};

Session::Session(Profile profile, SessionCallbacks callbacks)
    : implementation_(std::make_unique<Impl>(std::move(profile), std::move(callbacks))) {}

Session::~Session() = default;

void Session::request_full_refresh() { implementation_->request(full_refresh); }

void Session::cancel_pending(const std::uint64_t command_id) {
    implementation_->cancel_pending(command_id);
}

std::uint64_t Session::run_transport(const TransportAction action) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::transport;
    command.action = action;
    return implementation_->enqueue(command);
}

std::uint64_t Session::play_queue_id(const std::uint32_t song_id) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_play;
    command.object_id = song_id;
    return implementation_->enqueue(command);
}

std::uint64_t Session::lastfm(LastFmCommand request) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::lastfm;
    command.lastfm = std::move(request);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::edit_request_queue(RequestQueueCommand request) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::request_queue_edit;
    command.request = std::move(request);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::melody_context_play(std::string name, const std::optional<unsigned> row) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_context_play;
    command.uri = std::move(name);
    command.queue_position = row;
    return implementation_->enqueue(std::move(command));
}

// The insert position rides in object_id biased by one, so -1 (append) stays
// representable in the unsigned field every command shares.
std::uint64_t Session::melody_context_queue_add(std::vector<std::string> uris, const int position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_context_queue_add;
    command.uris = std::move(uris);
    command.object_id = static_cast<std::uint32_t>(position + 1);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::melody_context_queue_replace(std::vector<std::string> uris,
                                                    const unsigned position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_context_queue_replace;
    command.uris = std::move(uris);
    command.queue_position = position;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::melody_context_queue_delete(std::vector<unsigned> rows) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_context_queue_delete;
    command.positions = std::move(rows);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::melody_context_queue_move(const unsigned from, const unsigned to) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_context_queue_move;
    command.object_id = from;
    command.queue_position = to;
    return implementation_->enqueue(command);
}

std::uint64_t Session::melody_scratch_lists() {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_scratch_lists;
    return implementation_->enqueue(command);
}

std::uint64_t Session::melody_set_scratch(std::string name, const bool scratch) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_set_scratch;
    command.uri = std::move(name);
    command.enabled = scratch;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::melody_context_queue(const std::optional<unsigned> row) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_context_queue;
    command.queue_position = row;
    return implementation_->enqueue(command);
}

std::uint64_t Session::play_queue_position(const unsigned position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_play;
    command.queue_position = position;
    return implementation_->enqueue(command);
}

std::uint64_t Session::add_queue_uri(std::string uri, const std::optional<unsigned> position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_add;
    command.uri = std::move(uri);
    command.queue_position = position;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::add_queue_uris(std::vector<QueueAddition> additions) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_add_batch;
    command.additions = std::move(additions);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::delete_queue_id(const std::uint32_t song_id) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_delete;
    command.object_id = song_id;
    return implementation_->enqueue(command);
}

std::uint64_t Session::delete_queue_ids(std::vector<std::uint32_t> song_ids) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_delete_batch;
    command.object_ids = std::move(song_ids);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::newest_root_values(std::string tag, const unsigned track_limit) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_newest;
    command.uri = std::move(tag);
    command.priority = track_limit;
    return implementation_->enqueue(command);
}

std::uint64_t Session::update_database(std::string uri) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_update;
    command.uri = std::move(uri);
    return implementation_->enqueue(command);
}

std::uint64_t Session::clear_queue() {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_clear;
    return implementation_->enqueue(command);
}

std::uint64_t Session::move_queue_id(const std::uint32_t song_id, const unsigned position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_move;
    command.object_id = song_id;
    command.queue_position = position;
    return implementation_->enqueue(command);
}

std::uint64_t Session::move_queue_ids(std::vector<QueueMove> moves) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_move_batch;
    command.moves = std::move(moves);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::set_queue_priority(std::vector<std::uint32_t> song_ids,
                                          const unsigned priority) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::queue_priority;
    command.object_ids = std::move(song_ids);
    command.priority = priority;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::set_sticker_ratings(std::vector<std::string> uris, const unsigned rating) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::sticker_rating;
    command.uris = std::move(uris);
    command.rating = rating;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::set_melody_track_ratings(std::vector<std::uint64_t> song_ids,
                                                const unsigned rating) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_rating;
    command.melody_song_ids = std::move(song_ids);
    command.rating = rating;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::set_melody_album_rating(MelodyAlbumKey key, const unsigned rating) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_album_rate;
    command.album_rating_key = std::move(key);
    command.rating = rating;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::search_expression(std::string filter_expression, std::string sort,
                                         const unsigned limit) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_expression_search;
    command.uri = std::move(filter_expression);
    command.secondary_uri = std::move(sort);
    command.query_limit = limit;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::melody_album_rating(MelodyAlbumKey key) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::melody_album_rating;
    command.album_rating_key = std::move(key);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::browse(std::string uri) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_browse;
    command.uri = std::move(uri);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::album_counts(std::string artist_tag) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_album_counts;
    command.uri = std::move(artist_tag);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::list_tag(std::string tag) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_tag;
    command.uri = std::move(tag);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::find_tag_tracks(std::string tag, std::string value, const unsigned limit) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_tag_tracks;
    command.uri = std::move(tag);
    command.secondary_uri = std::move(value);
    command.query_limit = limit;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::load_artwork(std::string uri, const bool embedded) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::artwork;
    command.uri = std::move(uri);
    command.embedded = embedded;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::search_any(std::string query, const unsigned offset, const unsigned limit) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_search;
    command.uri = std::move(query);
    command.query_offset = offset;
    command.query_limit = limit;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::find_album(AlbumFilter album) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::database_album;
    command.album = std::move(album);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::list_stored_playlists() {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlists;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::load_stored_playlist(std::string name) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist;
    command.uri = std::move(name);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::save_queue_as_playlist(std::string name) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_save;
    command.uri = std::move(name);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::load_stored_playlist_into_queue(std::string name) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_load;
    command.uri = std::move(name);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::add_to_stored_playlist(std::string name, std::string uri,
                                              const std::optional<unsigned> position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_add;
    command.uri = std::move(name);
    command.secondary_uri = std::move(uri);
    command.queue_position = position;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::add_to_stored_playlist(std::string name, std::vector<std::string> uris,
                                              const std::optional<unsigned> first_position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_add_batch;
    command.uri = std::move(name);
    command.uris = std::move(uris);
    command.queue_position = first_position;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::delete_from_stored_playlist(std::string name, const unsigned position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_delete_item;
    command.uri = std::move(name);
    command.object_id = position;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::delete_from_stored_playlist(std::string name,
                                                   std::vector<unsigned> positions) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_delete_batch;
    command.uri = std::move(name);
    command.positions = std::move(positions);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::move_in_stored_playlist(std::string name, const unsigned from,
                                               const unsigned to) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_move_item;
    command.uri = std::move(name);
    command.object_id = from;
    command.queue_position = to;
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::clear_stored_playlist(std::string name) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_clear;
    command.uri = std::move(name);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::rename_stored_playlist(std::string from, std::string to) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_rename;
    command.uri = std::move(from);
    command.secondary_uri = std::move(to);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::delete_stored_playlist(std::string name) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::stored_playlist_remove;
    command.uri = std::move(name);
    return implementation_->enqueue(std::move(command));
}

std::uint64_t Session::seek(const std::uint32_t song_id, const std::chrono::milliseconds position) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::seek;
    command.object_id = song_id;
    command.position = position;
    return implementation_->enqueue(command);
}

std::uint64_t Session::set_volume(const unsigned volume) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::volume;
    command.volume = volume;
    return implementation_->enqueue(command);
}

std::uint64_t Session::set_repeat(const bool enabled) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::repeat;
    command.enabled = enabled;
    return implementation_->enqueue(command);
}

std::uint64_t Session::set_random(const bool enabled) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::random;
    command.enabled = enabled;
    return implementation_->enqueue(command);
}

std::uint64_t Session::set_single(const PlaybackModeState state) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::single;
    command.playback_mode = state;
    return implementation_->enqueue(command);
}

std::uint64_t Session::set_consume(const PlaybackModeState state) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::consume;
    command.playback_mode = state;
    return implementation_->enqueue(command);
}

std::uint64_t Session::set_replay_gain_mode(const ReplayGainMode mode) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::replay_gain;
    command.replay_gain_mode = mode;
    return implementation_->enqueue(command);
}

std::uint64_t Session::set_output_enabled(const std::uint32_t output_id, const bool enabled) {
    Impl::PendingCommand command;
    command.kind = SessionCommandKind::output_enabled;
    command.object_id = output_id;
    command.enabled = enabled;
    return implementation_->enqueue(command);
}

} // namespace trackknife::mpd
