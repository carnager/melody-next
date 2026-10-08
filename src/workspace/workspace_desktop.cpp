// SPDX-License-Identifier: GPL-3.0-only

// What plays, told to the desktop -- MPRIS and track-change notifications --
// and to Last.fm.

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "workspace/workspace_view.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

namespace trackknife::bench {

void Workspace::startLastFm() {
    lastfm_ = new LastFmService(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                    QStringLiteral("/lastfm-v1.json"),
                                this);
    lastfm_clock_.start();
    auto feedback = [this](const QString& op, const QJsonObject&, const QString& error) {
        if (!error.isEmpty() && op != QStringLiteral("info") && op != QStringLiteral("status"))
            view_->showMessage(error, 7000);
        else if (op == QStringLiteral("love") || op == QStringLiteral("unlove"))
            view_->showMessage(
                QStringLiteral("Last.fm updated. Refresh loved-track playlists to see the change."),
                5000);
    };
    connect(lastfm_, &LastFmService::completed, this, feedback);
    // Who this window is signed in as, to tell an engine already using the
    // same account from one that is not.
    connect(lastfm_, &LastFmService::completed, this,
            [this](const QString&, const QJsonObject& state, const QString&) {
                if (state.contains(QStringLiteral("user"))) {
                    lastfm_user_ = state.value(QStringLiteral("user")).toString();
                }
            });
    lastfm_->execute(QStringLiteral("status"));
}

// ADR-0220: an interim, and named as one. Scrobbling belongs to whoever owns
// playback, so its eventual home is the engine -- which would also scrobble
// with no window open. It lives here for now because the engine knows paths
// and durations while the tags a scrobble needs are in this process's rows.
// The accounting itself is core::ListenAccounting either way, so the move
// when it comes is of the network client, not of the rules.
void Workspace::sampleLastFm(const EnginePlayback::State& state) {
    if (!lastfm_ || lastfm_clock_.elapsed() - lastfm_sample_time_ < 500) {
        return;
    }
    lastfm_sample_time_ = lastfm_clock_.elapsed();
    // An engine with its own Last.fm session scrobbles what it plays; this
    // window crediting it too would count every listen twice.
    if (transport_ != nullptr && transport_->scrobblesItself()) {
        setProperty("trackknife-lastfm-sample", QStringLiteral("engine"));
        return;
    }
    // Wherever this window holds the entry: the list it was played from, Up
    // Next, or another list. Looked for only in the first, a track from Up
    // Next was credited with no artist and no title.
    LocalTrackRow track;
    if (const auto* row = playingRow(state.entry)) {
        track = *row;
    }
    // Observable for offscreen tests and diagnostics: "is anything being
    // credited, and for which track" is otherwise only answerable by watching
    // the network.
    setProperty("trackknife-lastfm-sample",
                QStringLiteral("%1|%2|%3")
                    .arg(QString::fromStdString(track.artist), QString::fromStdString(track.title),
                         state.status == QStringLiteral("playing") ? QStringLiteral("playing")
                                                                   : state.status));
    lastfm_->observe(
        {// The engine's playback instance, so the same track played twice is
         // two listens rather than one long one.
         {"identity", state.instance == 0U ? QString{} : QString::number(state.instance)},
         {"artist", QString::fromStdString(track.artist)},
         {"title", QString::fromStdString(track.title)},
         {"album", QString::fromStdString(track.album)},
         {"duration",
          state.duration_ms > 0 ? static_cast<double>(state.duration_ms) / 1000.0 : 0.0},
         {"position", static_cast<double>(state.position_ms) / 1000.0},
         {"playing", state.status == QStringLiteral("playing")},
         {"monotonic", lastfm_sample_time_}});
}

QString Workspace::loopStatus() const {
    if (!playback_.modes.repeat) {
        return QStringLiteral("None");
    }
    return playback_.modes.single == audio::ModeState::on ? QStringLiteral("Track")
                                                          : QStringLiteral("Playlist");
}

void Workspace::setLoopStatus(const QString& status) {
    const bool track = status == QStringLiteral("Track");
    playback_.modes.repeat = track || status == QStringLiteral("Playlist");
    // Single on is what loops the track; set for "Track", taken off for the
    // others -- a one-shot single, stopping after the current, is left as is.
    if (track) {
        playback_.modes.single = audio::ModeState::on;
    } else if (playback_.modes.single == audio::ModeState::on) {
        playback_.modes.single = audio::ModeState::off;
    }
    applyLocalPlaybackModes();
}

bool Workspace::shuffled() const { return playback_.modes.random || playback_.modes.album_random; }

void Workspace::setShuffled(const bool shuffled) {
    if (shuffled == this->shuffled()) {
        return;
    }
    // Shuffled from the desktop is track shuffle; album shuffle, chosen
    // here, stays what it is until the desktop turns shuffling off.
    playback_.modes.random = shuffled;
    if (!shuffled) {
        playback_.modes.album_random = false;
    }
    applyLocalPlaybackModes();
}

QString Workspace::desktopCoverPath(const LocalTrackRow& track, const EngineKey& engine) {
    const auto key = LocalListModel::groupKeyOf(track);
    if (key == desktop_cover_key_ && !desktop_cover_path_.isEmpty() &&
        QFileInfo::exists(desktop_cover_path_)) {
        return desktop_cover_path_;
    }
    const auto cover = coverFor(track, engine);
    if (cover.isNull()) {
        return {};
    }
    const QDir directory{QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                         QStringLiteral("/now-playing")};
    if (!directory.mkpath(QStringLiteral("."))) {
        return {};
    }
    const auto name = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    const auto path = directory.filePath(QStringLiteral("cover-%1.png").arg(name));
    // Large enough for any panel or notification, small enough to write at
    // every album change.
    const auto image = cover.width() > 512 || cover.height() > 512
                           ? cover.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           : cover;
    if (!image.save(path, "PNG")) {
        return {};
    }
    if (!desktop_cover_path_.isEmpty() && desktop_cover_path_ != path) {
        QFile::remove(desktop_cover_path_);
    }
    desktop_cover_key_ = key;
    desktop_cover_path_ = path;
    return path;
}

MprisPlaybackState Workspace::desktopStateOf(const EnginePlayback::State& engine) {
    // What the desktop sees is what the engine is doing. Reading the local
    // player here would publish an idle player while music plays, so media
    // keys and the notification would describe nothing.
    MprisPlaybackState state;
    state.status = engine.status == QStringLiteral("playing")  ? QStringLiteral("Playing")
                   : engine.status == QStringLiteral("paused") ? QStringLiteral("Paused")
                                                               : QStringLiteral("Stopped");
    if (!engine.entry.isEmpty()) {
        // The entry, not the path: the same file queued twice is two tracks
        // to the desktop, and a notification per occurrence.
        state.track_key = engine.entry;
        state.title = QFileInfo{engine.path}.fileName();
        // Named as the header names it: the entry looked for in the list
        // played from, Up Next and every other open list. The list played
        // from alone is unknown after a restart until something plays, and
        // the desktop then saw only a file name.
        const auto* track = playingRow(engine.entry);
        desktop_track_unknown_ = track == nullptr;
        if (track != nullptr) {
            if (!track->title.empty()) {
                state.title = displayText(track->title);
            }
            state.artist = displayText(track->artist);
            state.album = displayText(track->album);
            const auto* playing = transport_ != nullptr ? linkOf(transport_) : nullptr;
            state.art_path =
                desktopCoverPath(*track, playing != nullptr ? playing->key : EngineKey::local());
            // Asked for just now, or on its way: worth a moment's wait.
            const auto key = LocalListModel::groupKeyOf(*track);
            desktop_cover_waiting_ =
                state.art_path.isEmpty() && artwork_pending_.contains(key) ? key : QString{};
        } else {
            desktop_cover_waiting_.clear();
        }
    } else {
        desktop_track_unknown_ = false;
        desktop_cover_waiting_.clear();
    }
    state.position_us = engine.position_ms * 1'000;
    state.length_us = engine.duration_ms > 0 ? engine.duration_ms * 1'000 : -1;
    state.volume_percent = engine.volume_percent;
    const bool has_queue = engine.queue_size > 0U;
    state.can_next = engine.queue_size > 1U || engine.requests > 0U;
    state.can_previous = engine.queue_size > 1U;
    state.can_play = has_queue;
    state.can_pause = has_queue;
    state.can_seek = !engine.entry.isEmpty() && engine.duration_ms > 0;
    return state;
}

void Workspace::publishDesktopState() {
    if (mpris_ == nullptr && notifier_ == nullptr) {
        return;
    }
    MprisPlaybackState state;
    if (playingOnEngine()) {
        state = desktopStateOf(transport_->state());
    }
    state.loop_status = loopStatus();
    state.shuffle = shuffled();
    if (mpris_ != nullptr) {
        mpris_->publish(state);
    }
    if (notifier_ == nullptr) {
        return;
    }
    // A new track this window cannot name yet -- the engine's queue on its
    // way -- or whose cover is being fetched: its notification waits, at most
    // a moment, rather than going out as a file name with no cover. Looked at
    // again every little while, and at once when the cover comes.
    constexpr qint64 wait_ms = 1'500;
    const bool waiting = notifier_->isNewTrack(state) &&
                         (desktop_track_unknown_ || !desktop_cover_waiting_.isEmpty()) &&
                         notification_cover_given_up_ != state.track_key;
    if (waiting) {
        if (notification_waiting_track_ != state.track_key) {
            notification_waiting_track_ = state.track_key;
            notification_waited_.start();
        }
        if (notification_wait_ == nullptr) {
            notification_wait_ = new QTimer(this);
            notification_wait_->setInterval(150);
            connect(notification_wait_, &QTimer::timeout, this, [this] {
                if (notification_waited_.elapsed() >= wait_ms) {
                    notification_cover_given_up_ = notification_waiting_track_;
                }
                publishDesktopState();
            });
        }
        notification_wait_->start();
        return;
    }
    if (notification_wait_ != nullptr) {
        notification_wait_->stop();
    }
    notification_waiting_track_.clear();
    if (notifier_->publish(state, QGuiApplication::focusWindow() != nullptr)) {
        setProperty("trackknife-notifications-sent",
                    static_cast<qulonglong>(notifier_->sentCount()));
        setProperty("trackknife-notification-summary", notifier_->lastSummary());
        setProperty("trackknife-notification-body", notifier_->lastBody());
        setProperty("trackknife-notification-image", notifier_->lastImage());
    }
}

} // namespace trackknife::bench
