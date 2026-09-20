// SPDX-License-Identifier: GPL-3.0-only

#include "quick/mpd_probe_controller.hpp"

#include "uicommon/debug_log.hpp"

#include "quick/mpd_search_result_model.hpp"

#include "trackknife/core/stable_id.hpp"

#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::quick {
namespace {

[[nodiscard]] QString from_utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString track_label(const mpd::Track& track) {
    const auto artist = track.metadata.first("Artist").value_or("Unknown artist");
    const auto title = track.metadata.first("Title").value_or(track.uri);
    return QStringLiteral("%1 — %2").arg(from_utf8(artist), from_utf8(title));
}

[[nodiscard]] QString track_title(const mpd::Track& track) {
    return from_utf8(track.metadata.first("Title").value_or(track.uri));
}

[[nodiscard]] QString track_detail(const mpd::Track& track) {
    const auto artist = track.metadata.first("Artist").value_or("Unknown artist");
    const auto album = track.metadata.first("Album");
    const auto date = track.metadata.first("Date");
    auto detail = from_utf8(artist);
    if (album) {
        detail += QStringLiteral(" — ") + from_utf8(*album);
    }
    if (date) {
        detail += QStringLiteral(" (%1)").arg(from_utf8(*date));
    }
    return detail;
}

[[nodiscard]] QString describe_outputs(const std::vector<mpd::Output>& outputs) {
    QString summary;
    for (const auto& output : outputs) {
        summary += QStringLiteral("• %1 — %2")
                       .arg(from_utf8(output.name), output.enabled ? QStringLiteral("enabled")
                                                                   : QStringLiteral("disabled"));
        if (output.online) {
            summary += *output.online ? QStringLiteral(", online") : QStringLiteral(", offline");
        }
        if (output.primary && *output.primary) {
            summary += QStringLiteral(", primary");
        }
        summary += QLatin1Char('\n');
    }
    return summary;
}

[[nodiscard]] QString active_output_name(const std::vector<mpd::Output>& outputs) {
    const auto primary = std::ranges::find_if(
        outputs, [](const mpd::Output& output) { return output.primary.value_or(false); });
    if (primary != outputs.end()) {
        return from_utf8(primary->name);
    }
    const auto enabled =
        std::ranges::find_if(outputs, [](const mpd::Output& output) { return output.enabled; });
    return enabled == outputs.end() ? QStringLiteral("No output") : from_utf8(enabled->name);
}

[[nodiscard]] int mode_index(const mpd::PlaybackModeState mode) noexcept {
    switch (mode) {
    case mpd::PlaybackModeState::off:
        return 0;
    case mpd::PlaybackModeState::on:
        return 1;
    case mpd::PlaybackModeState::oneshot:
        return 2;
    case mpd::PlaybackModeState::unknown:
        return -1;
    }
    return -1;
}

[[nodiscard]] std::optional<mpd::PlaybackModeState> playback_mode(const int index) noexcept {
    switch (index) {
    case 0:
        return mpd::PlaybackModeState::off;
    case 1:
        return mpd::PlaybackModeState::on;
    case 2:
        return mpd::PlaybackModeState::oneshot;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] QString replay_gain_name(const mpd::ReplayGainMode mode) {
    switch (mode) {
    case mpd::ReplayGainMode::off:
        return QStringLiteral("off");
    case mpd::ReplayGainMode::track:
        return QStringLiteral("track");
    case mpd::ReplayGainMode::album:
        return QStringLiteral("album");
    case mpd::ReplayGainMode::automatic:
        return QStringLiteral("auto");
    case mpd::ReplayGainMode::unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

[[nodiscard]] std::optional<mpd::ReplayGainMode> replay_gain_mode(const QString& name) {
    if (name == QStringLiteral("off")) {
        return mpd::ReplayGainMode::off;
    }
    if (name == QStringLiteral("track")) {
        return mpd::ReplayGainMode::track;
    }
    if (name == QStringLiteral("album")) {
        return mpd::ReplayGainMode::album;
    }
    if (name == QStringLiteral("auto")) {
        return mpd::ReplayGainMode::automatic;
    }
    return std::nullopt;
}

} // namespace

MpdProbeController::MpdProbeController(QObject* parent)
    : QObject(parent), queue_model_(this), library_model_(this), browser_model_(this),
      browser_playlist_model_(this), output_model_(this), elapsed_timer_(new QTimer(this)),
      playback_confirmation_timer_(new QTimer(this)) {
    elapsed_timer_->setInterval(250);
    connect(elapsed_timer_, &QTimer::timeout, this, [this] {
        if (!playing()) {
            return;
        }
        elapsed_ms_ += elapsed_timer_->interval();
        if (duration_ms_ > 0) {
            elapsed_ms_ = std::min(elapsed_ms_, duration_ms_);
        }
        emit stateChanged();
    });
    playback_confirmation_timer_->setSingleShot(true);
    playback_confirmation_timer_->setInterval(3'000);
    connect(playback_confirmation_timer_, &QTimer::timeout, this, [this] {
        if (!optimistic_playback_state_) {
            return;
        }
        optimistic_playback_state_.reset();
        status_ = QStringLiteral("Playback command was not confirmed by MPD");
        updateElapsedTimer();
        emit notificationRequested(status_);
        emit stateChanged();
    });
}

MpdProbeController::~MpdProbeController() {
    ++connection_token_;
    session_.reset();
}

int MpdProbeController::singleMode() const noexcept {
    return mode_index(optimistic_single_.value_or(single_mode_));
}

int MpdProbeController::consumeMode() const noexcept {
    return mode_index(optimistic_consume_.value_or(consume_mode_));
}

QString MpdProbeController::replayGainMode() const {
    return replay_gain_name(optimistic_replay_gain_.value_or(replay_gain_mode_));
}

void MpdProbeController::lastFm(const QString& operation, const QStringList& arguments) {
    if (!session_ || !connected() || !supportsCommand(QStringLiteral("melody_lastfm"))) {
        emit lastFmCompleted(operation, {}, QStringLiteral("This server does not support Last.fm"));
        return;
    }
    mpd::LastFmCommand command{operation.toStdString(), {}};
    for (const auto& argument : arguments)
        command.arguments.push_back(argument.toStdString());
    const auto id = session_->lastfm(std::move(command));
    lastfm_commands_.insert(id, operation);
    pending_commands_.insert(id);
}

void MpdProbeController::editRequestQueue(mpd::RequestQueueCommand command) {
    if (!session_ || !connected() || !supportsCommand(QStringLiteral("melody_upnext")) ||
        !request_queue_)
        return;
    command.revision = request_queue_->revision;
    pending_commands_.insert(session_->edit_request_queue(std::move(command)));
    emit stateChanged();
}

void MpdProbeController::probe(const QString& host, const int port, const QString& password,
                               const QString& music_root) {
    probeProfile(QString::fromStdString(core::StableId::random().to_string()), host, port, password,
                 music_root);
}

void MpdProbeController::probeProfile(const QString& profile_id, const QString& host,
                                      const int port, const QString& password,
                                      const QString& music_root) {
    auto parsed_profile_id = core::StableId::parse(profile_id.toStdString());
    if (!parsed_profile_id) {
        emit notificationRequested(QStringLiteral("Connection profile has an invalid identity"));
        return;
    }
    ++connection_token_;
    const auto token = connection_token_;
    session_.reset();
    clearSessionState();
    busy_ = true;
    status_ = QStringLiteral("Connecting…");
    emit stateChanged();

    std::optional<std::string> password_value;
    if (!password.isEmpty()) {
        password_value = password.toUtf8().toStdString();
    }
    std::optional<std::filesystem::path> root_value;
    if (!music_root.isEmpty()) {
        root_value = std::filesystem::path{QFile::encodeName(music_root).toStdString()};
    }
    mpd::Profile profile{
        .id = *parsed_profile_id,
        .name = "desktop session",
        .host = host.toUtf8().toStdString(),
        .port = static_cast<unsigned>(port),
        .password = std::move(password_value),
        .local_music_root = std::move(root_value),
        .connect_timeout = std::chrono::milliseconds{5'000},
        .command_timeout = std::chrono::milliseconds{10'000},
    };
    profile_id_ = profile_id;

    const QPointer<MpdProbeController> self{this};
    mpd::SessionCallbacks callbacks{
        .state_changed =
            [self, token](const mpd::SessionState& state) {
                if (!self) {
                    return;
                }
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, token, state] {
                        if (self) {
                            self->applyState(token, state);
                        }
                    },
                    Qt::QueuedConnection);
            },
        .snapshot_changed =
            [self, token](const mpd::SessionSnapshot& snapshot) {
                if (!self) {
                    return;
                }
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, token, snapshot] {
                        if (self) {
                            self->applySnapshot(token, snapshot);
                        }
                    },
                    Qt::QueuedConnection);
            },
        .idle_received =
            [self, token](const mpd::IdleEvents events) {
                if (!self) {
                    return;
                }
                const auto database_changed = events.contains(mpd::IdleEvent::database) ||
                                              events.contains(mpd::IdleEvent::update);
                const auto playlists_changed = events.contains(mpd::IdleEvent::stored_playlist);
                if (!database_changed && !playlists_changed) {
                    return;
                }
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, token, database_changed, playlists_changed] {
                        auto* controller = self.data();
                        if (controller == nullptr || token != controller->connection_token_) {
                            return;
                        }
                        if (database_changed) {
                            controller->newest_order_cache_.clear();
                            controller->pending_newest_order_.reset();
                            controller->pending_newest_tag_.clear();
                            emit controller->serverDatabaseChanged();
                        }
                        if (playlists_changed) {
                            emit controller->storedPlaylistsChanged();
                        }
                    },
                    Qt::QueuedConnection);
            },
        .command_finished =
            [self, token](const mpd::SessionCommandResult& result) {
                if (!self) {
                    return;
                }
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, token, result] {
                        if (self) {
                            self->applyCommandResult(token, result);
                        }
                    },
                    Qt::QueuedConnection);
            },
    };
    session_ = std::make_unique<mpd::Session>(std::move(profile), std::move(callbacks));
}

void MpdProbeController::disconnectFromServer() {
    ++connection_token_;
    session_.reset();
    clearSessionState();
    emit stateChanged();
}

void MpdProbeController::playPause() {
    const auto state = presentedPlaybackState();
    if (state == mpd::PlaybackState::playing) {
        submitTransport(mpd::TransportAction::pause);
    } else if (state == mpd::PlaybackState::paused) {
        submitTransport(mpd::TransportAction::resume);
    } else {
        submitTransport(mpd::TransportAction::play);
    }
}

void MpdProbeController::stop() { submitTransport(mpd::TransportAction::stop); }

void MpdProbeController::next() { submitTransport(mpd::TransportAction::next); }

void MpdProbeController::previous() { submitTransport(mpd::TransportAction::previous); }

void MpdProbeController::playQueueItem(const int row) {
    if (!session_ || !connected_) {
        return;
    }
    const auto song_id = queue_model_.queueIdAt(row);
    if (!song_id) {
        return;
    }
    const auto command_id = session_->play_queue_id(*song_id);
    pending_commands_.insert(command_id);
    beginOptimisticPlayback(command_id, mpd::PlaybackState::playing);
    emit stateChanged();
}

void MpdProbeController::removeQueueItem(const int row) {
    if (!session_ || !connected_) {
        return;
    }
    if (queue_stashed_) {
        removeQueueContextRows({row});
        return;
    }
    const auto song_id = queue_model_.queueIdAt(row);
    if (!song_id) {
        return;
    }
    pending_commands_.insert(session_->delete_queue_id(*song_id));
    emit stateChanged();
}

void MpdProbeController::removeQueueItems(const QVariantList& rows) {
    if (!session_ || !connected_) {
        return;
    }
    if (queue_stashed_) {
        std::vector<int> stash_rows;
        stash_rows.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto& value : rows) {
            bool valid = false;
            const auto row = value.toInt(&valid);
            if (valid) {
                stash_rows.push_back(row);
            }
        }
        removeQueueContextRows(stash_rows);
        return;
    }
    std::vector<std::uint32_t> song_ids;
    song_ids.reserve(static_cast<std::size_t>(rows.size()));
    QSet<int> unique_rows;
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (!valid || unique_rows.contains(row)) {
            continue;
        }
        unique_rows.insert(row);
        if (const auto song_id = queue_model_.queueIdAt(row)) {
            song_ids.push_back(*song_id);
        }
    }
    if (song_ids.empty()) {
        return;
    }
    if (song_ids.size() == static_cast<std::size_t>(queue_model_.rowCount())) {
        pending_commands_.insert(session_->clear_queue());
        emit stateChanged();
        return;
    }
    if (song_ids.size() == 1U) {
        pending_commands_.insert(session_->delete_queue_id(song_ids.front()));
    } else {
        pending_commands_.insert(session_->delete_queue_ids(std::move(song_ids)));
    }
    emit stateChanged();
}

void MpdProbeController::cropQueueToItems(const QVariantList& rows) {
    if (!session_ || !connected_ || rows.isEmpty()) {
        return;
    }
    QSet<int> kept_rows;
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (valid && row >= 0 && row < queue_model_.rowCount()) {
            kept_rows.insert(row);
        }
    }
    if (kept_rows.isEmpty()) {
        return;
    }
    if (queue_stashed_) {
        std::vector<int> dropped;
        for (int row = 0; row < queue_model_.rowCount(); ++row) {
            if (!kept_rows.contains(row)) {
                dropped.push_back(row);
            }
        }
        removeQueueContextRows(dropped);
        return;
    }
    std::vector<std::uint32_t> removed_ids;
    removed_ids.reserve(static_cast<std::size_t>(queue_model_.rowCount() - kept_rows.size()));
    for (int row = 0; row < queue_model_.rowCount(); ++row) {
        if (!kept_rows.contains(row)) {
            if (const auto song_id = queue_model_.queueIdAt(row)) {
                removed_ids.push_back(*song_id);
            }
        }
    }
    if (removed_ids.empty()) {
        return;
    }
    if (removed_ids.size() == 1U) {
        pending_commands_.insert(session_->delete_queue_id(removed_ids.front()));
    } else {
        pending_commands_.insert(session_->delete_queue_ids(std::move(removed_ids)));
    }
    emit stateChanged();
}

void MpdProbeController::loadNewestRootOrder(const QString& tag) {
    if (!session_ || !connected_) {
        emit newestRootOrderLoaded({}, QStringLiteral("Not connected"));
        return;
    }
    if (newest_order_cache_.contains(tag)) {
        emit newestRootOrderLoaded(newest_order_cache_.value(tag), {});
        return;
    }
    if (pending_newest_order_ && pending_newest_tag_ == tag)
        return;
    pending_newest_tag_ = tag;
    constexpr unsigned newest_track_window = 2'000U;
    pending_newest_order_ =
        session_->newest_root_values(tag.toUtf8().toStdString(), newest_track_window);
    emit stateChanged();
}

void MpdProbeController::updateDatabase(const QString& uri) {
    if (!session_ || !connected_) {
        return;
    }
    pending_commands_.insert(session_->update_database(uri.toUtf8().toStdString()));
    emit stateChanged();
}

void MpdProbeController::clearQueue() {
    if (!session_ || !connected_ || queue_model_.rowCount() == 0) {
        return;
    }
    if (queue_stashed_) {
        std::vector<int> all_rows(static_cast<std::size_t>(queue_model_.rowCount()));
        std::iota(all_rows.begin(), all_rows.end(), 0);
        removeQueueContextRows(all_rows);
        return;
    }
    pending_commands_.insert(session_->clear_queue());
    emit stateChanged();
}

void MpdProbeController::moveQueueItem(const int row, const int target_row) {
    if (!session_ || !connected_ || row == target_row || target_row < 0 ||
        target_row >= queue_model_.rowCount()) {
        return;
    }
    if (queue_stashed_) {
        if (row >= 0 && row < queue_model_.rowCount()) {
            pending_commands_.insert(session_->melody_context_queue_move(
                static_cast<unsigned>(row), static_cast<unsigned>(target_row)));
            emit stateChanged();
        }
        return;
    }
    const auto song_id = queue_model_.queueIdAt(row);
    if (!song_id) {
        return;
    }
    pending_commands_.insert(session_->move_queue_id(*song_id, static_cast<unsigned>(target_row)));
    emit stateChanged();
}

void MpdProbeController::moveQueueItems(const QVariantList& rows, const int insertion_row) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (!session_ || !connected_ || rows.isEmpty() || insertion_row < 0 ||
        insertion_row > queue_model_.rowCount()) {
        return;
    }
    if (queue_stashed_) {
        // The stash reorders by row, as a sequence of single moves: each one
        // shifts the indices the next addresses, so the block is walked in
        // order with the running offset applied.
        std::vector<int> sources;
        sources.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto& value : rows) {
            bool valid = false;
            const auto row = value.toInt(&valid);
            if (valid && row >= 0 && row < queue_model_.rowCount()) {
                sources.push_back(row);
            }
        }
        std::ranges::sort(sources);
        sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
        if (sources.empty()) {
            return;
        }
        auto target = insertion_row;
        for (const auto source : sources) {
            if (source < target) {
                --target;
            }
        }
        target = std::clamp(target, 0, queue_model_.rowCount() - 1);
        for (std::size_t index = 0U; index < sources.size(); ++index) {
            auto from = sources[index];
            for (std::size_t seen = 0U; seen < index; ++seen) {
                if (sources[seen] < sources[index]) {
                    --from;
                }
            }
            const auto to = std::min(target + static_cast<int>(index), queue_model_.rowCount() - 1);
            if (from != to) {
                pending_commands_.insert(session_->melody_context_queue_move(
                    static_cast<unsigned>(from), static_cast<unsigned>(to)));
            }
        }
        emit stateChanged();
        return;
    }
    const auto tracks = queue_model_.tracksSnapshot();
    if (tracks.size() > maximum_batch_size) {
        emit notificationRequested(QStringLiteral("Drag reorder is limited to 4096 queue items"));
        return;
    }

    QSet<int> selected_rows;
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (valid && row >= 0 && row < queue_model_.rowCount()) {
            selected_rows.insert(row);
        }
    }
    if (selected_rows.isEmpty()) {
        return;
    }

    std::vector<std::uint32_t> current;
    std::vector<std::uint32_t> selected;
    std::vector<std::uint32_t> remaining;
    current.reserve(tracks.size());
    selected.reserve(static_cast<std::size_t>(selected_rows.size()));
    remaining.reserve(tracks.size() - static_cast<std::size_t>(selected_rows.size()));
    std::size_t destination = 0U;
    for (std::size_t row = 0U; row < tracks.size(); ++row) {
        if (!tracks[row].queue_id) {
            emit notificationRequested(QStringLiteral("Queue reorder requires stable song IDs"));
            return;
        }
        const auto id = *tracks[row].queue_id;
        current.push_back(id);
        if (selected_rows.contains(static_cast<int>(row))) {
            selected.push_back(id);
        } else {
            if (row < static_cast<std::size_t>(insertion_row)) {
                ++destination;
            }
            remaining.push_back(id);
        }
    }

    auto desired = remaining;
    desired.insert(desired.begin() + static_cast<std::ptrdiff_t>(destination), selected.begin(),
                   selected.end());
    if (desired == current) {
        return;
    }

    std::vector<mpd::QueueMove> moves;
    auto working = current;
    for (std::size_t target = 0U; target < desired.size(); ++target) {
        if (working[target] == desired[target]) {
            continue;
        }
        const auto source = std::ranges::find(working.begin() + static_cast<std::ptrdiff_t>(target),
                                              working.end(), desired[target]);
        if (source == working.end()) {
            emit notificationRequested(QStringLiteral("Queue changed before reorder was planned"));
            return;
        }
        moves.push_back(
            mpd::QueueMove{.song_id = desired[target], .position = static_cast<unsigned>(target)});
        const auto id = *source;
        working.erase(source);
        working.insert(working.begin() + static_cast<std::ptrdiff_t>(target), id);
    }
    if (moves.empty()) {
        return;
    }
    pending_commands_.insert(session_->move_queue_ids(std::move(moves)));
    emit stateChanged();
}

const mpd::Track* MpdProbeController::listPriorityTrack(const QString& context, int row,
                                                        const QString& uri) const {
    if (context != active_context_ || row < 0)
        return nullptr;
    const auto* track = context.isEmpty()
                            ? queue_model_.trackAt(row)
                            : (static_cast<std::size_t>(row) < active_context_tracks_.size()
                                   ? &active_context_tracks_[static_cast<std::size_t>(row)]
                                   : nullptr);
    return track && from_utf8(track->uri) == uri ? track : nullptr;
}

void MpdProbeController::setQueuePriority(const QVariantList& rows, const int priority) {
    QList<QPair<int, QString>> selection;
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (const auto* track = valid ? queue_model_.trackAt(row) : nullptr)
            selection.push_back({row, from_utf8(track->uri)});
    }
    setListPriority({}, selection, priority);
}

void MpdProbeController::setListPriority(const QString& context,
                                         const QList<QPair<int, QString>>& selection,
                                         int priority) {
    if (!session_ || !connected_ || !supportsCommand(QStringLiteral("prioid")) || priority < 0 ||
        priority > 255)
        return;
    std::vector<std::uint32_t> ids;
    for (const auto& [row, uri] : selection) {
        const auto* track = listPriorityTrack(context, row, uri);
        if (!track || !track->queue_id)
            return; // Stale or inactive list: never guess another occurrence.
        ids.push_back(*track->queue_id);
    }
    if (ids.empty())
        return;
    pending_commands_.insert(
        session_->set_queue_priority(std::move(ids), static_cast<unsigned>(priority)));
    emit stateChanged();
}

void MpdProbeController::setTrackRating(const QVariantList& rows, const int rating) {
    std::vector<mpd::Track> tracks;
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (const auto* track = valid ? queue_model_.trackAt(row) : nullptr)
            tracks.push_back(*track);
    }
    setTracksRating(tracks, rating);
}

void MpdProbeController::setTracksRating(const std::vector<mpd::Track>& tracks, const int rating,
                                         const QString& list_name) {
    if (!session_ || !connected_ || !supportsRatings() || rating < 0 || rating > 10) {
        return;
    }
    const auto melody = supportsCommand(QStringLiteral("getrating"));
    std::vector<std::uint64_t> song_ids;
    std::vector<std::string> uris;
    QSet<QString> unique_uris;
    QSet<quint64> unique_ids;
    for (const auto& track : tracks) {
        if (melody) {
            if (track.melody_song_id && !unique_ids.contains(*track.melody_song_id)) {
                unique_ids.insert(*track.melody_song_id);
                song_ids.push_back(*track.melody_song_id);
            }
        } else if (!track.uri.empty()) {
            const auto key = from_utf8(track.uri);
            if (!unique_uris.contains(key)) {
                unique_uris.insert(key);
                uris.push_back(track.uri);
            }
        }
    }
    std::uint64_t id{};
    if (melody && !song_ids.empty()) {
        id = session_->set_melody_track_ratings(std::move(song_ids), static_cast<unsigned>(rating));
    } else if (!melody && !uris.empty()) {
        id = session_->set_sticker_ratings(std::move(uris), static_cast<unsigned>(rating));
    } else
        return;
    pending_commands_.insert(id);
    if (!list_name.isEmpty())
        pending_rating_lists_.insert(id, list_name);
    emit stateChanged();
}

void MpdProbeController::searchServerExpression(const QString& expression, const QString& sort,
                                                ServerQueryCompletion completion) {
    if (!completion) {
        return;
    }
    if (!session_ || !connected_ || !supportsCommand(QStringLiteral("search"))) {
        completion(std::unexpected(
            core::Error{.code = core::ErrorCode::unsupported,
                        .message = "the connected server does not support structured queries",
                        .context = {}}));
        return;
    }
    const auto id =
        session_->search_expression(expression.toUtf8().toStdString(), sort.toUtf8().toStdString());
    pending_commands_.insert(id);
    pending_expression_searches_.insert(id, std::move(completion));
    emit stateChanged();
}

void MpdProbeController::requestMelodyAlbumRatings(const std::vector<mpd::Track>& queue) {
    if (!session_ || !supportsAlbumRatings()) {
        return;
    }
    constexpr qsizetype maximum_album_queries = 128;
    QSet<QString> in_flight;
    for (const auto& key : pending_album_rating_queries_) {
        in_flight.insert(key);
    }
    QSet<QString> seen;
    for (const auto& track : queue) {
        const auto group_key = MpdQueueModel::albumGroupKey(track);
        if (seen.contains(group_key) || in_flight.contains(group_key) ||
            melody_album_ratings_.contains(group_key)) {
            continue;
        }
        const std::string album_artist{
            track.metadata.first("AlbumArtist")
                .value_or(track.metadata.first("Artist").value_or(std::string_view{}))};
        const std::string album{track.metadata.first("Album").value_or(std::string_view{})};
        const std::string date{track.metadata.first("Date").value_or(std::string_view{})};
        if (album_artist.empty() || album.empty()) {
            continue;
        }
        seen.insert(group_key);
        if (seen.size() > maximum_album_queries) {
            break;
        }
        const auto id = session_->melody_album_rating(
            mpd::MelodyAlbumKey{.album_artist = album_artist, .album = album, .date = date});
        pending_commands_.insert(id);
        pending_album_rating_queries_.insert(id, group_key);
    }
}

void MpdProbeController::setMelodyAlbumRating(const QString& album_artist, const QString& album,
                                              const QString& date, const int rating,
                                              const QString& list_name) {
    if (!session_ || !connected_ || !supportsAlbumRatings() || rating < 0 || rating > 10 ||
        album_artist.isEmpty() || album.isEmpty()) {
        return;
    }
    const auto id = session_->set_melody_album_rating(
        mpd::MelodyAlbumKey{.album_artist = album_artist.toStdString(),
                            .album = album.toStdString(),
                            .date = date.toStdString()},
        static_cast<unsigned>(rating));
    pending_commands_.insert(id);
    if (!list_name.isEmpty())
        pending_rating_lists_.insert(id, list_name);
    emit stateChanged();
}

void MpdProbeController::searchLibrary(const QString& query) {
    const auto normalized = query.trimmed();
    if (normalized.isEmpty()) {
        if (session_ && pending_library_query_) {
            session_->cancel_pending(*pending_library_query_);
        }
        pending_library_query_.reset();
        pending_library_query_text_.clear();
        last_library_query_.clear();
        pending_search_append_ = false;
        search_has_more_ = false;
        library_model_.replaceTracks({});
        library_status_ = QStringLiteral("Search albums and tracks");
        emit stateChanged();
        return;
    }
    if (!session_ || !connected_) {
        emit notificationRequested(QStringLiteral("Connect to search the server library"));
        emit searchFinished(normalized, false);
        emit stateChanged();
        return;
    }
    if (pending_library_query_) {
        session_->cancel_pending(*pending_library_query_);
    }
    pending_library_query_ = session_->search_any(normalized.toUtf8().toStdString(), 0U, 200U);
    pending_library_query_text_ = normalized;
    last_library_query_ = normalized;
    pending_search_append_ = false;
    search_has_more_ = false;
    library_status_ = QStringLiteral("Searching…");
    emit stateChanged();
}

void MpdProbeController::continueSearch() {
    if (!session_ || !connected_ || pending_library_query_ || !search_has_more_ ||
        last_library_query_.isEmpty()) {
        return;
    }
    const auto offset = static_cast<unsigned>(library_model_.rowCount());
    pending_library_query_ =
        session_->search_any(last_library_query_.toUtf8().toStdString(), offset, 200U);
    pending_library_query_text_ = last_library_query_;
    pending_search_append_ = true;
    library_status_ = QStringLiteral("Loading more results…");
    emit stateChanged();
}

void MpdProbeController::addLibraryItem(const int row) {
    if (!session_ || !connected_) {
        return;
    }
    const auto uri = library_model_.uriAt(row);
    if (!uri) {
        return;
    }
    enqueueUri(*uri, false);
}

void MpdProbeController::addLibraryItemNext(const int row) {
    if (!session_ || !connected_) {
        return;
    }
    const auto uri = library_model_.uriAt(row);
    if (!uri) {
        return;
    }
    enqueueUri(*uri, true);
}

void MpdProbeController::addLibraryItems(const QVariantList& rows, const bool next) {
    if (!session_ || !connected_) {
        return;
    }
    QSet<int> unique_rows;
    std::vector<int> ordered_rows;
    ordered_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (valid && row >= 0 && row < library_model_.rowCount() && !unique_rows.contains(row)) {
            unique_rows.insert(row);
            ordered_rows.push_back(row);
        }
    }
    std::ranges::sort(ordered_rows);
    std::vector<std::string> uris;
    uris.reserve(ordered_rows.size());
    for (const auto row : ordered_rows) {
        if (auto uri = library_model_.uriAt(row)) {
            uris.push_back(std::move(*uri));
        }
    }
    enqueueUris(std::move(uris), next);
}

void MpdProbeController::addUris(const QStringList& uris, const bool next) {
    std::vector<std::string> encoded;
    encoded.reserve(static_cast<std::size_t>(uris.size()));
    for (const auto& uri : uris) {
        encoded.push_back(uri.toUtf8().toStdString());
    }
    enqueueUris(std::move(encoded), next);
}

void MpdProbeController::addUrisAt(const QStringList& uris, const int position) {
    if (!connected_ || position < 0 || position > queue_model_.rowCount()) {
        return;
    }
    std::vector<std::string> encoded;
    encoded.reserve(static_cast<std::size_t>(uris.size()));
    for (const auto& uri : uris) {
        encoded.push_back(uri.toUtf8().toStdString());
    }
    enqueueUrisAt(std::move(encoded), static_cast<unsigned>(position));
}

void MpdProbeController::replaceQueueWithUris(const QStringList& uris) {
    constexpr qsizetype maximum_batch_size = 4'096;
    if (!session_ || !connected_ || uris.isEmpty()) {
        return;
    }
    if (uris.size() > maximum_batch_size) {
        emit notificationRequested(QStringLiteral("At most 4096 tracks can replace the queue"));
        emit stateChanged();
        return;
    }
    std::vector<std::string> encoded;
    encoded.reserve(static_cast<std::size_t>(uris.size()));
    for (const auto& uri : uris) {
        encoded.push_back(uri.toUtf8().toStdString());
    }

    if (queue_stashed_) {
        // Replacing the queue means the queue is this list now, so the
        // server also switches back to it and plays.
        const auto command_id = session_->melody_context_queue_replace(std::move(encoded), 0U);
        pending_commands_.insert(command_id);
        beginOptimisticPlayback(command_id, mpd::PlaybackState::playing);
        emit stateChanged();
        return;
    }
    pending_commands_.insert(session_->clear_queue());
    enqueueUris(std::move(encoded), false);
    submitTransport(mpd::TransportAction::play);
}

// ADR-0181: the server list tabs' play gesture — the whole list becomes the
// queue and playback starts at the activated row, mirroring how a local
// list plays from the clicked track.
void MpdProbeController::replaceQueueWithUrisAndPlayAt(const QStringList& uris, const int row) {
    constexpr qsizetype maximum_batch_size = 4'096;
    if (!session_ || !connected_ || uris.isEmpty()) {
        return;
    }
    if (uris.size() > maximum_batch_size) {
        emit notificationRequested(QStringLiteral("At most 4096 tracks can replace the queue"));
        emit stateChanged();
        return;
    }
    std::vector<std::string> encoded;
    encoded.reserve(static_cast<std::size_t>(uris.size()));
    for (const auto& uri : uris) {
        encoded.push_back(uri.toUtf8().toStdString());
    }

    const auto start_row = std::clamp(row, 0, static_cast<int>(uris.size()) - 1);
    if (queue_stashed_) {
        const auto replace_id = session_->melody_context_queue_replace(
            std::move(encoded), static_cast<unsigned>(start_row));
        pending_commands_.insert(replace_id);
        beginOptimisticPlayback(replace_id, mpd::PlaybackState::playing);
        emit stateChanged();
        return;
    }
    pending_commands_.insert(session_->clear_queue());
    enqueueUris(std::move(encoded), false);
    const auto position = std::clamp(row, 0, static_cast<int>(uris.size()) - 1);
    const auto command_id = session_->play_queue_position(static_cast<unsigned>(position));
    pending_commands_.insert(command_id);
    beginOptimisticPlayback(command_id, mpd::PlaybackState::playing);
    emit stateChanged();
}

// ADR-0187: play a stored playlist as the active context. The server
// stashes the queue it displaces and remembers where each list was left, so
// switching between lists resumes rather than destroys.
void MpdProbeController::playStoredPlaylistContext(const QString& name, const int row) {
    if (!session_ || !connected_ || name.isEmpty()) {
        return;
    }
    const auto position =
        row >= 0 ? std::optional<unsigned>{static_cast<unsigned>(row)} : std::nullopt;
    const auto command_id = session_->melody_context_play(name.toStdString(), position);
    pending_commands_.insert(command_id);
    beginOptimisticPlayback(command_id, mpd::PlaybackState::playing);
    emit stateChanged();
}

// Deleting from the queue context addresses rows, not song ids: the stash is
// a list the server holds, not the queue whose ids the model carries.
void MpdProbeController::removeQueueContextRows(const std::vector<int>& rows) {
    std::vector<unsigned> positions;
    positions.reserve(rows.size());
    for (const auto row : rows) {
        if (row >= 0 && row < queue_model_.rowCount()) {
            positions.push_back(static_cast<unsigned>(row));
        }
    }
    if (positions.empty()) {
        return;
    }
    pending_commands_.insert(session_->melody_context_queue_delete(std::move(positions)));
    emit stateChanged();
}

// Scratch lists (ADR-0191): stored playlists the client shows as working
// tabs. The flag lives on the server with the list itself.
void MpdProbeController::browseScratchLists() {
    if (!session_ || !connected_ || !supportsCommand(QStringLiteral("melody_scratch"))) {
        emit scratchListsLoaded({});
        return;
    }
    if (pending_scratch_query_) {
        session_->cancel_pending(*pending_scratch_query_);
    }
    pending_scratch_query_ = session_->melody_scratch_lists();
    emit stateChanged();
}

void MpdProbeController::setPlaylistScratch(const QString& name, const bool scratch) {
    if (!session_ || !connected_ || name.isEmpty() ||
        !supportsCommand(QStringLiteral("melody_scratch"))) {
        return;
    }
    pending_commands_.insert(session_->melody_set_scratch(name.toStdString(), scratch));
    emit stateChanged();
}

void MpdProbeController::playListContext(const QString& name, int row) {
    if (name.isEmpty()) {
        if (queue_stashed_)
            playQueueContext(row);
        else
            playQueueItem(row);
    } else if (supportsPlaybackContexts()) {
        playStoredPlaylistContext(name, row);
    } else if (session_ && connected_) {
        const auto id = session_->load_stored_playlist(name.toStdString());
        pending_list_playbacks_.insert(id, row);
        pending_commands_.insert(id);
        emit stateChanged();
    }
}
void MpdProbeController::removeListItems(const QString& name, const QVariantList& rows) {
    if (rows.isEmpty())
        return;
    if (name.isEmpty())
        removeQueueItems(rows);
    else
        removeStoredPlaylistItems(name, rows);
}
void MpdProbeController::cropListToItems(const QString& name, const QVariantList& rows,
                                         int row_count) {
    if (rows.isEmpty())
        return;
    if (name.isEmpty()) {
        cropQueueToItems(rows);
        return;
    }
    QSet<int> keep;
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (!valid || row < 0 || row >= row_count)
            return;
        keep.insert(row);
    }
    QVariantList remove;
    for (int row = 0; row < row_count; ++row)
        if (!keep.contains(row))
            remove.push_back(row);
    removeListItems(name, remove);
}

void MpdProbeController::playQueueContext(const int row) {
    if (!session_ || !connected_) {
        return;
    }
    const auto position =
        row >= 0 ? std::optional<unsigned>{static_cast<unsigned>(row)} : std::nullopt;
    const auto command_id = session_->melody_context_queue(position);
    pending_commands_.insert(command_id);
    if (position) {
        beginOptimisticPlayback(command_id, mpd::PlaybackState::playing);
    }
    emit stateChanged();
}

void MpdProbeController::addAlbum(mpd::AlbumFilter album, const QueueAddMode mode) {
    if (!session_ || !connected_) {
        emit notificationRequested(QStringLiteral("Connect to add an album"));
        emit stateChanged();
        return;
    }
    const auto command_id = session_->find_album(std::move(album));
    pending_album_adds_.insert(command_id, mode);
    pending_commands_.insert(command_id);
    emit stateChanged();
}

void MpdProbeController::loadSearchAlbum(const quint64 token, const mpd::AlbumFilter& album) {
    if (!session_ || !connected_) {
        emit searchAlbumLoaded(token, {}, QStringLiteral("Not connected"));
        return;
    }
    pending_search_albums_.insert(session_->find_album(album), token);
}

void MpdProbeController::browseDirectory(const QString& uri) {
    if (!session_ || !connected_) {
        emit notificationRequested(QStringLiteral("Connect to browse the server library"));
        emit stateChanged();
        return;
    }
    if (pending_browser_query_) {
        session_->cancel_pending(*pending_browser_query_);
    }
    pending_browser_path_ = uri;
    pending_browser_query_ = session_->browse(uri.toUtf8().toStdString());
    browser_showing_playlist_ = false;
    browser_status_ = QStringLiteral("Loading folder…");
    emit stateChanged();
}

void MpdProbeController::browseTag(const QString& tag) {
    if (!session_ || !connected_) {
        emit notificationRequested(QStringLiteral("Connect to browse the server library"));
        emit stateChanged();
        return;
    }
    if (pending_tag_query_) {
        session_->cancel_pending(*pending_tag_query_);
    }
    pending_tag_name_ = tag;
    pending_tag_query_ = session_->list_tag(tag.toUtf8().toStdString());
    browser_status_ = QStringLiteral("Loading %1 values…").arg(tag);
    emit stateChanged();
}

void MpdProbeController::loadServerLibraryRoot(const quint64 token, const QString& preferred_tag) {
    if (session_) {
        for (auto request = pending_library_album_counts_.cbegin();
             request != pending_library_album_counts_.cend(); ++request)
            session_->cancel_pending(request.key());
    }
    pending_library_album_counts_.clear();
    if (!session_ || !connected_) {
        emit serverLibraryRootLoaded(token, {}, {}, QStringLiteral("Not connected"));
        return;
    }
    auto tag = preferred_tag;
    if (!supportsTag(tag)) {
        if (tag.compare(QStringLiteral("AlbumArtist"), Qt::CaseInsensitive) == 0 &&
            supportsTag(QStringLiteral("Artist"))) {
            tag = QStringLiteral("Artist");
        } else {
            emit serverLibraryRootLoaded(
                token, {}, {},
                QStringLiteral("The server does not expose the configured %1 tag").arg(tag));
            return;
        }
    }
    const auto command_id = session_->list_tag(tag.toUtf8().toStdString());
    pending_library_tree_roots_.insert(command_id,
                                       PendingLibraryTreeRoot{.token = token, .tag = tag});
}

void MpdProbeController::loadServerLibraryBranch(const quint64 token, const QString& tag,
                                                 const QString& value) {
    if (!session_ || !connected_) {
        emit serverLibraryBranchLoaded(token, {}, QStringLiteral("Not connected"));
        return;
    }
    // Ask for one sentinel row so an oversized branch is reported rather than
    // presented as a complete but silently truncated artist.
    constexpr unsigned branch_track_limit = 10'001U;
    const auto command_id = session_->find_tag_tracks(
        tag.toUtf8().toStdString(), value.toUtf8().toStdString(), branch_track_limit);
    pending_library_tree_branches_.insert(command_id, token);
}

void MpdProbeController::searchServerLibraryFilter(const quint64 token,
                                                   const QString& preferred_tag,
                                                   const QString& query) {
    const auto normalized = query.trimmed();
    if (!session_ || !connected_ || normalized.isEmpty()) {
        emit serverLibraryFilterLoaded(token, {}, {});
        return;
    }
    auto tag = preferred_tag;
    if (!supportsTag(tag)) {
        if (tag.compare(QStringLiteral("AlbumArtist"), Qt::CaseInsensitive) == 0 &&
            supportsTag(QStringLiteral("Artist"))) {
            tag = QStringLiteral("Artist");
        } else {
            emit serverLibraryFilterLoaded(token, {}, {});
            return;
        }
    }
    // One bounded any-tag page is enough to reveal which unloaded roots hold
    // matching descendants; it never becomes tree membership by itself.
    constexpr unsigned filter_result_limit = 200U;
    const auto command_id =
        session_->search_any(normalized.toUtf8().toStdString(), 0U, filter_result_limit);
    pending_library_tree_filters_.insert(command_id,
                                         PendingLibraryTreeRoot{.token = token, .tag = tag});
}

void MpdProbeController::loadServerLibraryArtwork(const quint64 token, const QString& uri) {
    if (!session_ || !connected_ || uri.isEmpty()) {
        emit serverLibraryArtworkLoaded(token, {});
        return;
    }
    const auto embedded = !supportsCommand(QStringLiteral("albumart")) &&
                          supportsCommand(QStringLiteral("readpicture"));
    if (!supportsCommand(QStringLiteral("albumart")) && !embedded) {
        emit serverLibraryArtworkLoaded(token, {});
        return;
    }
    const auto command_id = session_->load_artwork(uri.toUtf8().toStdString(), embedded);
    pending_library_tree_artwork_.insert(command_id, token);
}

void MpdProbeController::browseStoredPlaylists() {
    if (!session_ || !connected_ || !supportsCommand("listplaylists")) {
        emit notificationRequested(QStringLiteral("This server cannot list stored playlists"));
        emit stateChanged();
        return;
    }
    if (pending_stored_playlists_query_) {
        session_->cancel_pending(*pending_stored_playlists_query_);
    }
    pending_stored_playlists_query_ = session_->list_stored_playlists();
    browser_status_ = QStringLiteral("Loading stored playlists…");
    emit stateChanged();
}

void MpdProbeController::browseParentDirectory() {
    if (browser_showing_playlist_) {
        browser_showing_playlist_ = false;
        browser_status_ =
            QStringLiteral("%1 item%2")
                .arg(browser_model_.rowCount())
                .arg(browser_model_.rowCount() == 1 ? QString{} : QStringLiteral("s"));
        emit stateChanged();
        return;
    }
    const auto slash = browser_path_.lastIndexOf(QLatin1Char('/'));
    browseDirectory(slash < 0 ? QString{} : browser_path_.left(slash));
}

void MpdProbeController::activateBrowserItem(const int row) {
    if (!session_ || !connected_) {
        return;
    }
    const auto kind = browser_model_.kindAt(row);
    const auto uri = browser_model_.uriAt(row);
    if (!kind || !uri) {
        return;
    }
    switch (*kind) {
    case MpdBrowserModel::EntryKind::directory:
        browseDirectory(QString::fromUtf8(uri->data(), static_cast<qsizetype>(uri->size())));
        break;
    case MpdBrowserModel::EntryKind::track:
        enqueueUri(*uri, false);
        browser_status_ = QStringLiteral("Adding track to queue…");
        emit stateChanged();
        break;
    case MpdBrowserModel::EntryKind::playlist:
        openStoredPlaylist(QString::fromUtf8(uri->data(), static_cast<qsizetype>(uri->size())));
        break;
    }
}

void MpdProbeController::addBrowserItemNext(const int row) {
    if (!session_ || !connected_ ||
        browser_model_.kindAt(row) != MpdBrowserModel::EntryKind::track) {
        return;
    }
    const auto uri = browser_model_.uriAt(row);
    if (!uri) {
        return;
    }
    enqueueUri(*uri, true);
    browser_status_ = QStringLiteral("Adding track next…");
    emit stateChanged();
}

void MpdProbeController::addBrowserItems(const QVariantList& rows, const bool next) {
    if (!session_ || !connected_) {
        return;
    }
    QSet<int> unique_rows;
    std::vector<int> ordered_rows;
    ordered_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (valid && row >= 0 && row < browser_model_.rowCount() && !unique_rows.contains(row)) {
            unique_rows.insert(row);
            ordered_rows.push_back(row);
        }
    }
    std::ranges::sort(ordered_rows);
    std::vector<std::string> uris;
    uris.reserve(ordered_rows.size());
    for (const auto row : ordered_rows) {
        if (browser_model_.kindAt(row) == MpdBrowserModel::EntryKind::track) {
            if (auto uri = browser_model_.uriAt(row)) {
                uris.push_back(std::move(*uri));
            }
        }
    }
    if (uris.empty()) {
        emit notificationRequested(QStringLiteral("Select one or more tracks to add"));
        emit stateChanged();
        return;
    }
    const auto count = uris.size();
    enqueueUris(std::move(uris), next);
    browser_status_ = QStringLiteral("Adding %1 track%2%3…")
                          .arg(count)
                          .arg(count == 1U ? QString{} : QStringLiteral("s"))
                          .arg(next ? QStringLiteral(" next") : QString{});
    emit stateChanged();
}

void MpdProbeController::addBrowserPlaylistItems(const QVariantList& rows, const bool next) {
    if (!session_ || !connected_) {
        return;
    }
    QSet<int> unique_rows;
    std::vector<int> ordered_rows;
    ordered_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (valid && row >= 0 && row < browser_playlist_model_.rowCount() &&
            !unique_rows.contains(row)) {
            unique_rows.insert(row);
            ordered_rows.push_back(row);
        }
    }
    std::ranges::sort(ordered_rows);
    std::vector<std::string> uris;
    uris.reserve(ordered_rows.size());
    for (const auto row : ordered_rows) {
        if (auto uri = browser_playlist_model_.uriAt(row)) {
            uris.push_back(std::move(*uri));
        }
    }
    enqueueUris(std::move(uris), next);
}

void MpdProbeController::openStoredPlaylist(const QString& name) { reloadStoredPlaylist(name); }

void MpdProbeController::saveQueueAsPlaylist(const QString& name) {
    if (!session_ || !connected_ || name.trimmed().isEmpty() || !supportsCommand("save")) {
        emit notificationRequested(QStringLiteral("This server cannot save the live queue"));
        return;
    }
    const auto id = session_->save_queue_as_playlist(name.toUtf8().toStdString());
    pending_commands_.insert(id);
    pending_playlist_mutations_.insert(
        id, PendingPlaylistMutation{
                .kind = PlaylistMutationKind::save, .name = name, .target_name = {}});
    emit stateChanged();
}

void MpdProbeController::loadStoredPlaylistIntoQueue(const QString& name) {
    if (!session_ || !connected_ || name.isEmpty() || !supportsCommand("load")) {
        emit notificationRequested(QStringLiteral("This server cannot load stored playlists"));
        return;
    }
    const auto id = session_->load_stored_playlist_into_queue(name.toUtf8().toStdString());
    pending_commands_.insert(id);
    pending_playlist_mutations_.insert(
        id, PendingPlaylistMutation{
                .kind = PlaylistMutationKind::load, .name = name, .target_name = {}});
    emit stateChanged();
}

void MpdProbeController::addToStoredPlaylist(const QString& name, const QStringList& uris,
                                             const int insertion_row) {
    if (!session_ || !connected_ || name.isEmpty() || !supportsCommand("playlistadd")) {
        emit notificationRequested(QStringLiteral("This server cannot edit stored playlists"));
        return;
    }
    std::vector<std::string> unique_uris;
    unique_uris.reserve(static_cast<std::size_t>(uris.size()));
    QSet<QString> seen;
    for (const auto& uri : uris) {
        if (!uri.isEmpty() && !seen.contains(uri)) {
            seen.insert(uri);
            unique_uris.push_back(uri.toUtf8().toStdString());
        }
    }
    if (unique_uris.empty()) {
        return;
    }
    const auto position = insertion_row >= 0
                              ? std::optional<unsigned>{static_cast<unsigned>(insertion_row)}
                              : std::nullopt;
    const auto id = session_->add_to_stored_playlist(name.toUtf8().toStdString(),
                                                     std::move(unique_uris), position);
    pending_commands_.insert(id);
    pending_playlist_mutations_.insert(
        id, PendingPlaylistMutation{
                .kind = PlaylistMutationKind::edit, .name = name, .target_name = {}});
    emit stateChanged();
}

void MpdProbeController::removeStoredPlaylistItems(const QString& name, const QVariantList& rows) {
    if (!session_ || !connected_ || name.isEmpty() || !supportsCommand("playlistdelete")) {
        emit notificationRequested(QStringLiteral("This server cannot edit stored playlists"));
        return;
    }
    QSet<unsigned> unique_rows;
    std::vector<unsigned> positions;
    positions.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& value : rows) {
        bool valid = false;
        const auto row = value.toInt(&valid);
        if (valid && row >= 0 && !unique_rows.contains(static_cast<unsigned>(row))) {
            unique_rows.insert(static_cast<unsigned>(row));
            positions.push_back(static_cast<unsigned>(row));
        }
    }
    if (positions.empty()) {
        return;
    }
    const auto id =
        session_->delete_from_stored_playlist(name.toUtf8().toStdString(), std::move(positions));
    pending_commands_.insert(id);
    pending_playlist_mutations_.insert(
        id, PendingPlaylistMutation{
                .kind = PlaylistMutationKind::edit, .name = name, .target_name = {}});
    emit stateChanged();
}

void MpdProbeController::moveStoredPlaylistItem(const QString& name, const int row,
                                                const int target_row) {
    if (!session_ || !connected_ || name.isEmpty() || row < 0 || target_row < 0 ||
        row == target_row || !supportsCommand("playlistmove")) {
        return;
    }
    const auto id = session_->move_in_stored_playlist(
        name.toUtf8().toStdString(), static_cast<unsigned>(row), static_cast<unsigned>(target_row));
    pending_commands_.insert(id);
    pending_playlist_mutations_.insert(
        id, PendingPlaylistMutation{
                .kind = PlaylistMutationKind::edit, .name = name, .target_name = {}});
    emit stateChanged();
}

void MpdProbeController::clearStoredPlaylist(const QString& name) {
    if (!session_ || !connected_ || name.isEmpty() || !supportsCommand("playlistclear")) {
        return;
    }
    const auto id = session_->clear_stored_playlist(name.toUtf8().toStdString());
    pending_commands_.insert(id);
    pending_playlist_mutations_.insert(
        id, PendingPlaylistMutation{
                .kind = PlaylistMutationKind::edit, .name = name, .target_name = {}});
    emit stateChanged();
}

void MpdProbeController::renameStoredPlaylist(const QString& from, const QString& to) {
    if (!session_ || !connected_ || from.isEmpty() || to.trimmed().isEmpty() || from == to ||
        !supportsCommand("rename")) {
        return;
    }
    const auto id =
        session_->rename_stored_playlist(from.toUtf8().toStdString(), to.toUtf8().toStdString());
    pending_commands_.insert(id);
    pending_playlist_mutations_.insert(
        id, PendingPlaylistMutation{
                .kind = PlaylistMutationKind::rename, .name = from, .target_name = to});
    emit stateChanged();
}

void MpdProbeController::deleteStoredPlaylist(const QString& name) {
    if (!session_ || !connected_ || name.isEmpty() || !supportsCommand("rm")) {
        return;
    }
    const auto id = session_->delete_stored_playlist(name.toUtf8().toStdString());
    pending_commands_.insert(id);
    pending_playlist_mutations_.insert(
        id, PendingPlaylistMutation{
                .kind = PlaylistMutationKind::remove, .name = name, .target_name = {}});
    emit stateChanged();
}

void MpdProbeController::reloadStoredPlaylist(const QString& name) {
    if (!session_ || !connected_ || name.isEmpty() || !supportsCommand("listplaylistinfo")) {
        emit notificationRequested(QStringLiteral("This server cannot read stored playlists"));
        return;
    }
    const auto id = session_->load_stored_playlist(name.toUtf8().toStdString());
    pending_playlist_queries_.insert(id, name);
    pending_commands_.insert(id);
    browser_status_ = QStringLiteral("Loading stored playlist…");
    emit stateChanged();
}

void MpdProbeController::setRepeatEnabled(const bool enabled) {
    if (!session_ || !connected_) {
        return;
    }
    optimistic_repeat_ = enabled;
    pending_commands_.insert(session_->set_repeat(enabled));
    emit stateChanged();
}

void MpdProbeController::setRandomEnabled(const bool enabled) {
    if (!session_ || !connected_) {
        return;
    }
    optimistic_random_ = enabled;
    pending_commands_.insert(session_->set_random(enabled));
    emit stateChanged();
}

void MpdProbeController::setSingleMode(const int mode) {
    if (!session_ || !connected_) {
        return;
    }
    const auto value = playback_mode(mode);
    if (!value) {
        return;
    }
    optimistic_single_ = *value;
    pending_commands_.insert(session_->set_single(*value));
    emit stateChanged();
}

void MpdProbeController::setConsumeMode(const int mode) {
    if (!session_ || !connected_) {
        return;
    }
    const auto value = playback_mode(mode);
    if (!value) {
        return;
    }
    optimistic_consume_ = *value;
    pending_commands_.insert(session_->set_consume(*value));
    emit stateChanged();
}

void MpdProbeController::setReplayGainMode(const QString& mode) {
    if (!session_ || !connected_ || !supports_replay_gain_) {
        return;
    }
    const auto value = replay_gain_mode(mode);
    if (!value) {
        return;
    }
    optimistic_replay_gain_ = *value;
    pending_commands_.insert(session_->set_replay_gain_mode(*value));
    emit stateChanged();
}

void MpdProbeController::seekTo(const qint64 position_ms) {
    if (!session_ || !connected_ || !current_song_id_ || position_ms < 0) {
        return;
    }
    const auto bounded = std::min(position_ms, duration_ms_);
    pending_commands_.insert(session_->seek(*current_song_id_, std::chrono::milliseconds{bounded}));
    emit stateChanged();
}

void MpdProbeController::setVolume(const int volume) {
    if (!session_ || !connected_ || volume < 0 || volume > 100) {
        return;
    }
    pending_commands_.insert(session_->set_volume(static_cast<unsigned>(volume)));
    emit stateChanged();
}

void MpdProbeController::setOutputEnabled(const quint32 output_id, const bool enabled) {
    if (!session_ || !connected_) {
        return;
    }
    pending_commands_.insert(session_->set_output_enabled(output_id, enabled));
    emit stateChanged();
}

void MpdProbeController::applyState(const std::uint64_t token, mpd::SessionState state) {
    if (token != connection_token_ || state.generation < generation_) {
        return;
    }
    if (state.generation != generation_) {
        newest_order_cache_.clear();
        pending_newest_order_.reset();
        pending_newest_tag_.clear();
    }
    generation_ = state.generation;
    switch (state.phase) {
    case mpd::SessionPhase::connecting:
        busy_ = true;
        connected_ = false;
        status_ = QStringLiteral("Connecting…");
        break;
    case mpd::SessionPhase::connected:
        busy_ = true;
        connected_ = false;
        status_ = QStringLiteral("Connected · synchronizing…");
        break;
    case mpd::SessionPhase::reconnecting:
        busy_ = true;
        connected_ = false;
        status_ = QStringLiteral("Connection lost · reconnecting…");
        if (state.error) {
            details_ = QString::fromStdString(state.error->message);
            for (const auto& context : state.error->context) {
                details_ += QStringLiteral("\n%1: %2")
                                .arg(QString::fromStdString(context.key),
                                     QString::fromStdString(context.value));
            }
            emit notificationRequested(
                QStringLiteral("Connection lost: %1").arg(from_utf8(state.error->message)));
        }
        break;
    case mpd::SessionPhase::stopped:
        busy_ = false;
        connected_ = false;
        status_ = QStringLiteral("Disconnected");
        if (state.error) {
            emit notificationRequested(
                QStringLiteral("Connection failed: %1").arg(from_utf8(state.error->message)));
        }
        break;
    }
    emit stateChanged();
}

void MpdProbeController::applySnapshot(const std::uint64_t token, mpd::SessionSnapshot snapshot) {
    if (token != connection_token_ || snapshot.generation < generation_) {
        return;
    }
    generation_ = snapshot.generation;
    busy_ = false;
    connected_ = true;
    advertised_commands_.clear();
    advertised_commands_.reserve(static_cast<qsizetype>(snapshot.capabilities.commands.size()));
    for (const auto& command : snapshot.capabilities.commands) {
        advertised_commands_.insert(from_utf8(command).toLower());
    }
    advertised_tag_types_.clear();
    advertised_tag_types_.reserve(static_cast<qsizetype>(snapshot.capabilities.tag_types.size()));
    for (const auto& tag : snapshot.capabilities.tag_types) {
        advertised_tag_types_.insert(from_utf8(tag).toLower());
    }
    const auto artwork_uri =
        snapshot.current_song.empty() ? QString{} : from_utf8(snapshot.current_song.front().uri);
    if (artwork_uri != current_artwork_uri_) {
        if (pending_artwork_query_) {
            session_->cancel_pending(*pending_artwork_query_);
            pending_artwork_query_.reset();
        }
        current_artwork_uri_ = artwork_uri;
        emit artworkLoaded(current_artwork_uri_, {});
        const auto embedded = !supportsCommand(QStringLiteral("albumart")) &&
                              supportsCommand(QStringLiteral("readpicture"));
        if (!current_artwork_uri_.isEmpty() &&
            (supportsCommand(QStringLiteral("albumart")) || embedded)) {
            pending_artwork_query_ =
                session_->load_artwork(snapshot.current_song.front().uri, embedded);
            pending_commands_.insert(*pending_artwork_query_);
        }
    }
    const auto& version = snapshot.capabilities.protocol;
    status_ = QStringLiteral("Connected · MPD %1.%2.%3")
                  .arg(version.major)
                  .arg(version.minor)
                  .arg(version.patch);
    now_playing_ = snapshot.current_song.empty() ? QStringLiteral("Nothing playing")
                                                 : track_label(snapshot.current_song.front());
    now_playing_title_ = snapshot.current_song.empty() ? QStringLiteral("Nothing playing")
                                                       : track_title(snapshot.current_song.front());
    now_playing_detail_ = snapshot.current_song.empty()
                              ? QStringLiteral("Queue is idle")
                              : track_detail(snapshot.current_song.front());
    if (snapshot.current_song.empty()) {
        now_playing_artist_.clear();
        now_playing_album_.clear();
        now_playing_uri_.clear();
    } else {
        const auto& song = snapshot.current_song.front();
        now_playing_artist_ = from_utf8(song.metadata.first("Artist").value_or(""));
        now_playing_album_ = from_utf8(song.metadata.first("Album").value_or(""));
        now_playing_uri_ = from_utf8(song.uri);
    }
    playback_state_ = snapshot.status.state;
    if (optimistic_playback_state_ && playback_state_ == *optimistic_playback_state_) {
        optimistic_playback_state_.reset();
        playback_confirmation_timer_->stop();
    }
    elapsed_ms_ = snapshot.status.elapsed ? snapshot.status.elapsed->count() : 0;
    duration_ms_ = snapshot.status.duration ? snapshot.status.duration->count() : 0;
    // Output/library-only refreshes carry the original status sample. Project
    // it forward rather than resetting the advancing UI clock to stale elapsed.
    if (playback_state_ == mpd::PlaybackState::playing && snapshot.status_sample_time &&
        snapshot.status.elapsed) {
        elapsed_ms_ +=
            std::max<qint64>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - *snapshot.status_sample_time)
                                    .count());
        if (duration_ms_ > 0) {
            elapsed_ms_ = std::min(elapsed_ms_, duration_ms_);
        }
    }
    volume_ = snapshot.status.volume ? static_cast<int>(*snapshot.status.volume) : -1;
    song_position_ =
        snapshot.status.song_position ? static_cast<int>(*snapshot.status.song_position) : -1;
    request_queue_ = snapshot.requests;
    active_context_ = snapshot.context ? QString::fromStdString(snapshot.context->name) : QString{};
    queue_stashed_ = snapshot.context && snapshot.context->queue_stashed;
    qCDebug(tkDebug) << "snapshot: context"
                     << (active_context_.isEmpty() ? QStringLiteral("(queue)") : active_context_)
                     << "stashed" << queue_stashed_ << "queue rows"
                     << static_cast<int>(snapshot.queue.size()) << "queue-context rows"
                     << static_cast<int>(snapshot.queue_context_tracks.size()) << "song pos"
                     << song_position_;
    current_song_id_ = snapshot.status.song_id;
    queue_model_.setCurrentSongId(queue_stashed_ ? std::nullopt : current_song_id_);
    repeat_enabled_ = snapshot.status.repeat;
    random_enabled_ = snapshot.status.random;
    single_mode_ = snapshot.status.single;
    consume_mode_ = snapshot.status.consume;
    replay_gain_mode_ = snapshot.replay_gain_mode;
    if (optimistic_repeat_ == repeat_enabled_) {
        optimistic_repeat_.reset();
    }
    if (optimistic_random_ == random_enabled_) {
        optimistic_random_.reset();
    }
    if (optimistic_single_ == single_mode_) {
        optimistic_single_.reset();
    }
    if (optimistic_consume_ == consume_mode_) {
        optimistic_consume_.reset();
    }
    if (optimistic_replay_gain_ == replay_gain_mode_) {
        optimistic_replay_gain_.reset();
    }
    // melodyd removed exclusive output switching for MPD compatibility;
    // the advertised command only survives as the legacy-release marker for
    // the playback-restore quirk below.
    legacy_melody_output_restore_ = snapshot.capabilities.supports_command("switchoutput");
    supports_replay_gain_ = snapshot.capabilities.supports_command("replay_gain_status") &&
                            snapshot.capabilities.supports_command("replay_gain_mode");
    updateElapsedTimer();
    output_summary_ = describe_outputs(snapshot.outputs);
    active_output_name_ = active_output_name(snapshot.outputs);
    details_ = QStringLiteral("%1 commands · %2 tag types · %3 queue items\n%4\n")
                   .arg(snapshot.capabilities.commands.size())
                   .arg(snapshot.capabilities.tag_types.size())
                   .arg(snapshot.queue.size())
                   .arg(now_playing_);
    auto commands = advertised_commands_.values();
    commands.sort(Qt::CaseInsensitive);
    auto tags = advertised_tag_types_.values();
    tags.sort(Qt::CaseInsensitive);
    details_ += QStringLiteral("\nCommands\n%1\n\nTag types\n%2\n")
                    .arg(commands.join(QStringLiteral(", ")), tags.join(QStringLiteral(", ")));
    details_ += QStringLiteral("\nOutputs\n") + output_summary_;
    // ADR-0188: the Queue tab shows the queue context's own list. While
    // another tab is the active queue, the server holds this list stashed —
    // it must keep showing unchanged rather than the other tab's tracks.
    auto queue_rows =
        queue_stashed_ ? std::move(snapshot.queue_context_tracks) : std::move(snapshot.queue);
    active_context_tracks_ = queue_stashed_ ? std::move(snapshot.queue) : std::vector<mpd::Track>{};
    requestMelodyAlbumRatings(queue_rows);
    queue_model_.replaceTracks(std::move(queue_rows));
    queue_model_.setAlbumRatings(melody_album_ratings_);
    QHash<QString, unsigned> sticker_ratings;
    sticker_ratings.reserve(static_cast<qsizetype>(snapshot.sticker_ratings.size()));
    for (const auto& rating : snapshot.sticker_ratings) {
        sticker_ratings.insert(from_utf8(rating.uri), rating.rating);
    }
    queue_model_.setStickerRatings(std::move(sticker_ratings));
    output_model_.replaceOutputs(std::move(snapshot.outputs));
    emit stateChanged();
}

void MpdProbeController::applyCommandResult(const std::uint64_t token,
                                            mpd::SessionCommandResult result) {
    if (token != connection_token_) {
        return;
    }
    pending_commands_.remove(result.id);
    qCDebug(tkDebug) << "command" << static_cast<int>(result.kind) << "id" << result.id
                     << (result.error ? QString::fromStdString(result.error->message)
                                      : QStringLiteral("ok"));
    if (result.kind == mpd::SessionCommandKind::lastfm) {
        const auto operation = lastfm_commands_.take(result.id);
        const auto* reply = std::get_if<mpd::LastFmReply>(&result.payload);
        emit lastFmCompleted(
            operation, reply ? QByteArray::fromStdString(reply->json) : QByteArray{},
            result.error ? QString::fromStdString(result.error->message) : QString{});
        return;
    }
    if (result.kind == mpd::SessionCommandKind::database_expression_search) {
        if (auto completion = pending_expression_searches_.take(result.id)) {
            if (result.error) {
                completion(std::unexpected(*result.error));
            } else if (const auto* tracks = std::get_if<std::vector<mpd::Track>>(&result.payload)) {
                completion(*tracks);
            } else {
                completion(std::unexpected(
                    core::Error{.code = core::ErrorCode::backend,
                                .message = "the server returned an invalid search response",
                                .context = {}}));
            }
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::melody_album_rating) {
        const auto group_key = pending_album_rating_queries_.take(result.id);
        if (!group_key.isEmpty() && !result.error) {
            if (const auto* rating = std::get_if<mpd::MelodyAlbumRating>(&result.payload)) {
                // Show the user-set album rating; fall back to the server's
                // computed track-rating mean when nothing was set explicitly.
                const auto computed =
                    std::min(10U, static_cast<unsigned>(std::lround(rating->computed)));
                melody_album_ratings_.insert(group_key,
                                             rating->rating != 0U ? rating->rating : computed);
                melody_album_stored_ratings_.insert(group_key, rating->rating);
                queue_model_.setAlbumRatings(melody_album_ratings_);
            }
        }
        emit stateChanged();
        return;
    }
    if (!result.error && (result.kind == mpd::SessionCommandKind::melody_rating ||
                          result.kind == mpd::SessionCommandKind::melody_album_rate)) {
        // The stored or computed album aggregates changed server-side; the
        // rating-forced snapshot that follows refetches them.
        melody_album_ratings_.clear();
        melody_album_stored_ratings_.clear();
    }
    const auto rated_list = pending_rating_lists_.take(result.id);
    if (!rated_list.isEmpty() && !result.error)
        reloadStoredPlaylist(rated_list);
    if (result.kind == mpd::SessionCommandKind::database_browse) {
        if (!pending_browser_query_ || *pending_browser_query_ != result.id) {
            emit stateChanged();
            return;
        }
        pending_browser_query_.reset();
        if (result.error) {
            emit notificationRequested(
                QStringLiteral("Folder failed: %1").arg(from_utf8(result.error->message)));
        } else if (const auto* entries =
                       std::get_if<std::vector<mpd::DatabaseEntry>>(&result.payload)) {
            browser_model_.replaceEntries(*entries);
            browser_path_ = pending_browser_path_;
            browser_showing_playlist_ = false;
            browser_status_ = QStringLiteral("%1 item%2")
                                  .arg(entries->size())
                                  .arg(entries->size() == 1U ? QString{} : QStringLiteral("s"));
        } else {
            emit notificationRequested(QStringLiteral("Folder returned an invalid response"));
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::database_newest) {
        if (!pending_newest_order_ || *pending_newest_order_ != result.id) {
            emit stateChanged();
            return;
        }
        pending_newest_order_.reset();
        QStringList values;
        QString error;
        if (result.error) {
            error = from_utf8(result.error->message);
        } else if (const auto* result_values =
                       std::get_if<std::vector<std::string>>(&result.payload)) {
            values.reserve(static_cast<qsizetype>(result_values->size()));
            for (const auto& value : *result_values) {
                values.push_back(from_utf8(value));
            }
        } else {
            error = QStringLiteral("Newest ordering returned an invalid response");
        }
        if (error.isEmpty())
            newest_order_cache_.insert(pending_newest_tag_, values);
        pending_newest_tag_.clear();
        emit newestRootOrderLoaded(values, error);
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::database_album_counts) {
        const auto pending = pending_library_album_counts_.find(result.id);
        if (pending == pending_library_album_counts_.end())
            return;
        const auto count_token = *pending;
        pending_library_album_counts_.erase(pending);
        if (result.error) {
            emit serverLibraryAlbumCountsLoaded(count_token, {}, from_utf8(result.error->message));
        } else if (const auto* counts =
                       std::get_if<std::vector<mpd::ArtistAlbumCount>>(&result.payload)) {
            emit serverLibraryAlbumCountsLoaded(count_token, *counts, {});
        } else {
            emit serverLibraryAlbumCountsLoaded(count_token, {},
                                                QStringLiteral("Invalid album count response"));
        }
        return;
    }
    if (result.kind == mpd::SessionCommandKind::database_tag) {
        const auto tree_query = pending_library_tree_roots_.find(result.id);
        if (tree_query != pending_library_tree_roots_.end()) {
            const auto request = std::move(*tree_query);
            pending_library_tree_roots_.erase(tree_query);
            QStringList values;
            QString error;
            if (result.error) {
                error = from_utf8(result.error->message);
            } else if (const auto* result_values =
                           std::get_if<std::vector<std::string>>(&result.payload)) {
                values.reserve(static_cast<qsizetype>(result_values->size()));
                for (const auto& value : *result_values) {
                    values.push_back(from_utf8(value));
                }
            } else {
                error = QStringLiteral("Library root returned an invalid response");
            }
            emit serverLibraryRootLoaded(request.token, request.tag, values, error);
            if (error.isEmpty() && session_ && connected_ &&
                (request.tag == QStringLiteral("AlbumArtist") ||
                 request.tag == QStringLiteral("Artist"))) {
                const auto count_id = session_->album_counts(request.tag.toStdString());
                pending_library_album_counts_.insert(count_id, request.token);
            }
            emit stateChanged();
            return;
        }
        if (!pending_tag_query_ || *pending_tag_query_ != result.id) {
            emit stateChanged();
            return;
        }
        pending_tag_query_.reset();
        const auto tag = std::exchange(pending_tag_name_, QString{});
        if (result.error) {
            emit notificationRequested(
                QStringLiteral("Library browse failed: %1").arg(from_utf8(result.error->message)));
        } else if (const auto* values = std::get_if<std::vector<std::string>>(&result.payload)) {
            QStringList display_values;
            display_values.reserve(static_cast<qsizetype>(values->size()));
            for (const auto& value : *values) {
                display_values.push_back(from_utf8(value));
            }
            display_values.sort(Qt::CaseInsensitive);
            browser_status_ =
                QStringLiteral("%1 %2 value%3")
                    .arg(display_values.size())
                    .arg(tag)
                    .arg(display_values.size() == 1 ? QString{} : QStringLiteral("s"));
            emit tagListLoaded(tag, display_values);
        } else {
            emit notificationRequested(
                QStringLiteral("Library browse returned an invalid response"));
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::database_tag_tracks) {
        const auto query = pending_library_tree_branches_.find(result.id);
        if (query == pending_library_tree_branches_.end()) {
            emit stateChanged();
            return;
        }
        const auto request_token = *query;
        pending_library_tree_branches_.erase(query);
        if (result.error) {
            emit serverLibraryBranchLoaded(request_token, {}, from_utf8(result.error->message));
        } else if (const auto* tracks = std::get_if<std::vector<mpd::Track>>(&result.payload)) {
            constexpr std::size_t maximum_complete_branch = 10'000U;
            if (tracks->size() > maximum_complete_branch) {
                emit serverLibraryBranchLoaded(
                    request_token, {},
                    QStringLiteral("This branch exceeds the 10,000-track safety limit"));
            } else {
                emit serverLibraryBranchLoaded(request_token, *tracks, {});
            }
        } else {
            emit serverLibraryBranchLoaded(
                request_token, {}, QStringLiteral("Library branch returned an invalid response"));
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::artwork) {
        const auto tree_query = pending_library_tree_artwork_.find(result.id);
        if (tree_query != pending_library_tree_artwork_.end()) {
            const auto request_token = *tree_query;
            pending_library_tree_artwork_.erase(tree_query);
            QByteArray data;
            if (!result.error) {
                if (const auto* bytes = std::get_if<std::vector<std::byte>>(&result.payload)) {
                    data = QByteArray{reinterpret_cast<const char*>(bytes->data()),
                                      static_cast<qsizetype>(bytes->size())};
                }
            }
            emit serverLibraryArtworkLoaded(request_token, data);
            emit stateChanged();
            return;
        }
        if (!pending_artwork_query_ || *pending_artwork_query_ != result.id) {
            emit stateChanged();
            return;
        }
        pending_artwork_query_.reset();
        if (!result.error) {
            if (const auto* bytes = std::get_if<std::vector<std::byte>>(&result.payload)) {
                const auto* data = reinterpret_cast<const char*>(bytes->data());
                emit artworkLoaded(current_artwork_uri_,
                                   QByteArray{data, static_cast<qsizetype>(bytes->size())});
            }
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::melody_scratch_lists) {
        if (pending_scratch_query_ && *pending_scratch_query_ == result.id) {
            pending_scratch_query_.reset();
            QStringList names;
            if (const auto* values = std::get_if<std::vector<std::string>>(&result.payload)) {
                for (const auto& value : *values) {
                    names.push_back(from_utf8(value));
                }
            }
            emit scratchListsLoaded(names);
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::stored_playlists) {
        if (!pending_stored_playlists_query_ || *pending_stored_playlists_query_ != result.id) {
            emit stateChanged();
            return;
        }
        pending_stored_playlists_query_.reset();
        if (result.error) {
            emit notificationRequested(
                QStringLiteral("Playlist browse failed: %1").arg(from_utf8(result.error->message)));
        } else if (const auto* playlists =
                       std::get_if<std::vector<mpd::StoredPlaylist>>(&result.payload)) {
            QStringList names;
            names.reserve(static_cast<qsizetype>(playlists->size()));
            for (const auto& playlist : *playlists) {
                names.push_back(from_utf8(playlist.name));
            }
            names.sort(Qt::CaseInsensitive);
            browser_status_ = QStringLiteral("%1 stored playlist%2")
                                  .arg(names.size())
                                  .arg(names.size() == 1 ? QString{} : QStringLiteral("s"));
            emit storedPlaylistListLoaded(names);
        } else {
            emit notificationRequested(
                QStringLiteral("Playlist browse returned an invalid response"));
        }
        emit stateChanged();
        return;
    }
    const auto playback = pending_list_playbacks_.find(result.id);
    if (playback != pending_list_playbacks_.end()) {
        const auto row = *playback;
        pending_list_playbacks_.erase(playback);
        if (result.error) {
            emit notificationRequested(
                QStringLiteral("Playlist failed: %1").arg(from_utf8(result.error->message)));
        } else if (const auto* tracks = std::get_if<std::vector<mpd::Track>>(&result.payload)) {
            QStringList uris;
            for (const auto& track : *tracks)
                uris.push_back(from_utf8(track.uri));
            if (!uris.isEmpty())
                replaceQueueWithUrisAndPlayAt(uris, row);
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::stored_playlist) {
        const auto query = pending_playlist_queries_.find(result.id);
        if (query == pending_playlist_queries_.end()) {
            emit stateChanged();
            return;
        }
        auto playlist_name = std::move(*query);
        pending_playlist_queries_.erase(query);
        if (result.error) {
            emit notificationRequested(
                QStringLiteral("Playlist failed: %1").arg(from_utf8(result.error->message)));
        } else if (const auto* tracks = std::get_if<std::vector<mpd::Track>>(&result.payload)) {
            requestMelodyAlbumRatings(*tracks);
            browser_playlist_model_.replaceTracks(*tracks);
            browser_showing_playlist_ = true;
            browser_status_ = QStringLiteral("%1 · %2 track%3")
                                  .arg(playlist_name)
                                  .arg(tracks->size())
                                  .arg(tracks->size() == 1U ? QString{} : QStringLiteral("s"));
            emit storedPlaylistLoaded(playlist_name);
        } else {
            emit notificationRequested(QStringLiteral("Playlist returned an invalid response"));
        }
        emit stateChanged();
        return;
    }
    const auto playlist_mutation = pending_playlist_mutations_.find(result.id);
    if (playlist_mutation != pending_playlist_mutations_.end()) {
        auto mutation = std::move(*playlist_mutation);
        pending_playlist_mutations_.erase(playlist_mutation);
        if (result.error) {
            emit notificationRequested(QStringLiteral("Playlist command failed: %1")
                                           .arg(from_utf8(result.error->message)));
        } else {
            switch (mutation.kind) {
            case PlaylistMutationKind::save:
                reloadStoredPlaylist(mutation.name);
                browseStoredPlaylists();
                break;
            case PlaylistMutationKind::edit:
                reloadStoredPlaylist(mutation.name);
                break;
            case PlaylistMutationKind::load:
                emit notificationRequested(
                    QStringLiteral("Loaded %1 into the live queue").arg(mutation.name));
                break;
            case PlaylistMutationKind::rename:
                emit storedPlaylistRenamed(mutation.name, mutation.target_name);
                reloadStoredPlaylist(mutation.target_name);
                browseStoredPlaylists();
                break;
            case PlaylistMutationKind::remove:
                emit storedPlaylistDeleted(mutation.name);
                browseStoredPlaylists();
                break;
            }
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::database_search) {
        if (const auto filter = pending_library_tree_filters_.find(result.id);
            filter != pending_library_tree_filters_.end()) {
            const auto pending_token = filter->token;
            const auto tag = filter->tag.toUtf8().toStdString();
            pending_library_tree_filters_.erase(filter);
            if (result.error) {
                emit serverLibraryFilterLoaded(pending_token, {}, from_utf8(result.error->message));
                return;
            }
            QStringList root_values;
            QSet<QString> seen;
            if (const auto* search = std::get_if<mpd::LibrarySearchResult>(&result.payload)) {
                for (const auto& track : search->tracks) {
                    for (const auto value : track.metadata.values(tag)) {
                        auto text = from_utf8(value);
                        if (!text.isEmpty() && !seen.contains(text)) {
                            seen.insert(text);
                            root_values.push_back(std::move(text));
                        }
                    }
                }
            }
            emit serverLibraryFilterLoaded(pending_token, root_values, {});
            return;
        }
        if (!pending_library_query_ || *pending_library_query_ != result.id) {
            emit stateChanged();
            return;
        }
        pending_library_query_.reset();
        const auto query = std::exchange(pending_library_query_text_, QString{});
        const auto append = std::exchange(pending_search_append_, false);
        if (result.error) {
            emit notificationRequested(
                QStringLiteral("Search failed: %1").arg(from_utf8(result.error->message)));
            emit searchFinished(query, false);
        } else if (const auto* search = std::get_if<mpd::LibrarySearchResult>(&result.payload)) {
            auto sorted_tracks = search->tracks;
            search_has_more_ = search->tracks.size() == 200U;
            if (append) {
                auto previous = library_model_.tracksSnapshot();
                previous.insert(previous.end(), std::make_move_iterator(sorted_tracks.begin()),
                                std::make_move_iterator(sorted_tracks.end()));
                sorted_tracks = std::move(previous);
            }
            mpd::sort_search_results(sorted_tracks);
            library_model_.replaceTracks(std::move(sorted_tracks));
            if (!append) {
                library_albums_ = filterAlbumSearchResults(search->albums, query);
            }
            library_status_ =
                search_has_more_
                    ? QStringLiteral("%1 results · more available").arg(library_model_.rowCount())
                    : QStringLiteral("%1 album%2 · %3 track%4")
                          .arg(library_albums_.size())
                          .arg(library_albums_.size() == 1U ? QString{} : QStringLiteral("s"))
                          .arg(library_model_.rowCount())
                          .arg(library_model_.rowCount() == 1 ? QString{} : QStringLiteral("s"));
            emit searchFinished(query, true);
        } else {
            emit notificationRequested(QStringLiteral("Search returned an invalid response"));
            emit searchFinished(query, false);
        }
        emit stateChanged();
        return;
    }
    if (result.kind == mpd::SessionCommandKind::database_album) {
        if (pending_search_albums_.contains(result.id)) {
            const auto request_token = pending_search_albums_.take(result.id);
            if (result.error) {
                emit searchAlbumLoaded(request_token, {}, from_utf8(result.error->message));
            } else if (const auto* tracks = std::get_if<std::vector<mpd::Track>>(&result.payload)) {
                emit searchAlbumLoaded(request_token, *tracks, {});
            } else {
                emit searchAlbumLoaded(request_token, {},
                                       QStringLiteral("Album returned an invalid response"));
            }
            return;
        }

        if (!pending_album_adds_.contains(result.id)) {
            emit stateChanged();
            return;
        }
        const auto mode = pending_album_adds_.take(result.id);
        if (result.error) {
            emit notificationRequested(
                QStringLiteral("Album failed: %1").arg(from_utf8(result.error->message)));
        } else if (const auto* result_tracks =
                       std::get_if<std::vector<mpd::Track>>(&result.payload)) {
            auto tracks = *result_tracks;
            mpd::sort_search_results(tracks);
            std::vector<std::string> uris;
            uris.reserve(tracks.size());
            for (auto& track : tracks) {
                uris.push_back(std::move(track.uri));
            }
            if (uris.empty()) {
                emit notificationRequested(QStringLiteral("Album contains no tracks"));
            } else if (mode == QueueAddMode::replace) {
                pending_commands_.insert(session_->clear_queue());
                enqueueUris(std::move(uris), false);
                submitTransport(mpd::TransportAction::play);
            } else {
                enqueueUris(std::move(uris), mode == QueueAddMode::next);
            }
        } else {
            emit notificationRequested(QStringLiteral("Album returned an invalid response"));
        }
        emit stateChanged();
        return;
    }
    if (pending_playback_command_ == result.id) {
        pending_playback_command_.reset();
        if (result.error) {
            optimistic_playback_state_.reset();
            playback_confirmation_timer_->stop();
            updateElapsedTimer();
        }
    }
    if (result.error) {
        switch (result.kind) {
        case mpd::SessionCommandKind::repeat:
            optimistic_repeat_.reset();
            break;
        case mpd::SessionCommandKind::random:
            optimistic_random_.reset();
            break;
        case mpd::SessionCommandKind::single:
            optimistic_single_.reset();
            break;
        case mpd::SessionCommandKind::consume:
            optimistic_consume_.reset();
            break;
        case mpd::SessionCommandKind::replay_gain:
            optimistic_replay_gain_.reset();
            break;
        default:
            break;
        }
        const auto failure =
            result.error->code == core::ErrorCode::conflict
                ? QStringLiteral("Queue changed on the server — refreshing current state")
                : QStringLiteral("Command failed: %1")
                      .arg(QString::fromStdString(result.error->message));
        emit notificationRequested(failure);
    }
    emit stateChanged();
}

void MpdProbeController::submitTransport(const mpd::TransportAction action) {
    if (!session_ || !connected_) {
        return;
    }
    const auto command_id = session_->run_transport(action);
    pending_commands_.insert(command_id);
    switch (action) {
    case mpd::TransportAction::play:
    case mpd::TransportAction::resume:
        beginOptimisticPlayback(command_id, mpd::PlaybackState::playing);
        break;
    case mpd::TransportAction::pause:
        beginOptimisticPlayback(command_id, mpd::PlaybackState::paused);
        break;
    case mpd::TransportAction::stop:
        beginOptimisticPlayback(command_id, mpd::PlaybackState::stopped);
        break;
    case mpd::TransportAction::next:
    case mpd::TransportAction::previous:
        break;
    }
    emit stateChanged();
}

void MpdProbeController::enqueueUri(std::string uri, const bool next) {
    std::vector<std::string> uris;
    uris.push_back(std::move(uri));
    enqueueUris(std::move(uris), next);
}

void MpdProbeController::enqueueUris(std::vector<std::string> uris, const bool next) {
    std::optional<unsigned> first_position;
    if (next) {
        if (!current_song_id_) {
            first_position = 0U;
        } else if (const auto current_row = queue_model_.rowForQueueId(*current_song_id_)) {
            first_position = static_cast<unsigned>(*current_row) + 1U;
        }
    }
    enqueueUrisAt(std::move(uris), first_position);
}

void MpdProbeController::enqueueUrisAt(std::vector<std::string> uris,
                                       const std::optional<unsigned> first_position) {
    constexpr std::size_t maximum_batch_size = 4'096U;
    if (!session_ || uris.empty()) {
        return;
    }
    // ADR-0188: while another list is the active queue, the Queue tab shows
    // the server's stash. An add aimed at that tab has to land there — a
    // plain add would go to the materialized list nothing is displaying.
    if (queue_stashed_) {
        pending_commands_.insert(session_->melody_context_queue_add(
            std::move(uris), first_position ? static_cast<int>(*first_position) : -1));
        emit stateChanged();
        return;
    }
    if (uris.size() > maximum_batch_size) {
        emit notificationRequested(QStringLiteral("At most 4096 tracks can be added at once"));
        emit stateChanged();
        return;
    }

    // Melody releases predating `melody_version` started the first item loaded into an
    // empty queue. MPD's add/addid contract does not imply playback, so restore the
    // pre-add state on those servers. Fixed releases advertise `melody_version` and
    // preserve playback state themselves. Queue replacement explicitly submits Play
    // afterward either way.
    std::optional<mpd::TransportAction> restore_playback;
    if (legacy_melody_output_restore_ && !supportsCommand(QStringLiteral("melody_version")) &&
        queue_model_.rowCount() == 0) {
        switch (presentedPlaybackState()) {
        case mpd::PlaybackState::stopped:
            restore_playback = mpd::TransportAction::stop;
            break;
        case mpd::PlaybackState::paused:
            restore_playback = mpd::TransportAction::pause;
            break;
        case mpd::PlaybackState::unknown:
        case mpd::PlaybackState::playing:
            break;
        }
    }
    if (uris.size() == 1U) {
        pending_commands_.insert(session_->add_queue_uri(std::move(uris.front()), first_position));
        if (restore_playback) {
            submitTransport(*restore_playback);
        }
        emit stateChanged();
        return;
    }
    std::vector<mpd::QueueAddition> additions;
    additions.reserve(uris.size());
    for (std::size_t index = 0U; index < uris.size(); ++index) {
        additions.push_back(mpd::QueueAddition{
            .uri = std::move(uris[index]),
            .position = first_position ? std::optional<unsigned>{*first_position +
                                                                 static_cast<unsigned>(index)}
                                       : std::nullopt,
        });
    }
    pending_commands_.insert(session_->add_queue_uris(std::move(additions)));
    if (restore_playback) {
        submitTransport(*restore_playback);
    }
    emit stateChanged();
}

void MpdProbeController::beginOptimisticPlayback(const std::uint64_t command_id,
                                                 const mpd::PlaybackState state) {
    pending_playback_command_ = command_id;
    optimistic_playback_state_ = state;
    playback_confirmation_timer_->start();
    updateElapsedTimer();
}

void MpdProbeController::updateElapsedTimer() {
    if (playing()) {
        elapsed_timer_->start();
    } else {
        elapsed_timer_->stop();
    }
}

mpd::PlaybackState MpdProbeController::presentedPlaybackState() const noexcept {
    return optimistic_playback_state_.value_or(playback_state_);
}

void MpdProbeController::clearSessionState() {
    lastfm_commands_.clear();
    pending_rating_lists_.clear();
    pending_list_playbacks_.clear();
    active_context_tracks_.clear();
    newest_order_cache_.clear();
    pending_newest_order_.reset();
    pending_newest_tag_.clear();
    busy_ = false;
    connected_ = false;
    generation_ = 0U;
    status_ = QStringLiteral("Not connected");
    details_.clear();
    output_summary_.clear();
    active_output_name_ = QStringLiteral("No output");
    now_playing_ = QStringLiteral("Nothing playing");
    now_playing_title_ = QStringLiteral("Nothing playing");
    now_playing_detail_ = QStringLiteral("Connect to MPD or Melody");
    now_playing_artist_.clear();
    now_playing_album_.clear();
    now_playing_uri_.clear();
    playback_state_ = mpd::PlaybackState::unknown;
    optimistic_playback_state_.reset();
    pending_playback_command_.reset();
    legacy_melody_output_restore_ = false;
    supports_replay_gain_ = false;
    advertised_commands_.clear();
    advertised_tag_types_.clear();
    repeat_enabled_ = false;
    random_enabled_ = false;
    single_mode_ = mpd::PlaybackModeState::unknown;
    consume_mode_ = mpd::PlaybackModeState::unknown;
    replay_gain_mode_ = mpd::ReplayGainMode::unknown;
    optimistic_repeat_.reset();
    optimistic_random_.reset();
    optimistic_single_.reset();
    optimistic_consume_.reset();
    optimistic_replay_gain_.reset();
    elapsed_ms_ = 0;
    duration_ms_ = 0;
    volume_ = -1;
    current_song_id_.reset();
    queue_model_.setCurrentSongId(std::nullopt);
    pending_commands_.clear();
    pending_album_adds_.clear();
    melody_album_ratings_.clear();
    melody_album_stored_ratings_.clear();
    pending_album_rating_queries_.clear();
    const auto orphaned = std::exchange(pending_expression_searches_, {});
    for (const auto& completion : orphaned) {
        completion(std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                               .message = "the server connection was closed",
                                               .context = {}}));
    }
    queue_model_.setStickerRatings({});
    queue_model_.setAlbumRatings({});
    const auto pending_albums = std::exchange(pending_search_albums_, {});
    for (const auto token : pending_albums) {
        emit searchAlbumLoaded(token, {}, QStringLiteral("Disconnected"));
    }
    pending_library_query_.reset();
    pending_library_query_text_.clear();
    last_library_query_.clear();
    pending_search_append_ = false;
    search_has_more_ = false;
    pending_browser_query_.reset();
    pending_tag_query_.reset();
    pending_library_tree_roots_.clear();
    pending_library_tree_branches_.clear();
    pending_library_album_counts_.clear();
    pending_library_tree_filters_.clear();
    pending_library_tree_artwork_.clear();
    pending_stored_playlists_query_.reset();
    pending_tag_name_.clear();
    pending_artwork_query_.reset();
    current_artwork_uri_.clear();
    emit artworkLoaded({}, {});
    pending_playlist_queries_.clear();
    pending_playlist_mutations_.clear();
    queue_model_.replaceTracks({});
    library_model_.replaceTracks({});
    library_albums_.clear();
    browser_model_.replaceEntries({});
    browser_playlist_model_.replaceTracks({});
    output_model_.replaceOutputs({});
    library_status_ = QStringLiteral("Search albums and tracks");
    browser_status_ = QStringLiteral("No folder loaded");
    browser_path_.clear();
    pending_browser_path_.clear();
    browser_showing_playlist_ = false;
    elapsed_timer_->stop();
    playback_confirmation_timer_->stop();
}

} // namespace trackknife::quick
