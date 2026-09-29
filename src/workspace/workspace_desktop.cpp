// SPDX-License-Identifier: GPL-3.0-only

// What plays, told to the desktop -- MPRIS and track-change notifications --
// and to Last.fm.

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "workspace/workspace_view.hpp"

#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonObject>
#include <QStandardPaths>

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

void Workspace::publishDesktopState() {
    if (mpris_ == nullptr && notifier_ == nullptr) {
        return;
    }
    MprisPlaybackState state;
    if (playingOnEngine()) {
        // What the desktop sees is what the engine is doing. Reading the
        // local player here would publish an idle player while music plays,
        // so media keys and the notification would describe nothing.
        const auto engine = transport_->state();
        state.status = engine.status == QStringLiteral("playing")  ? QStringLiteral("Playing")
                       : engine.status == QStringLiteral("paused") ? QStringLiteral("Paused")
                                                                   : QStringLiteral("Stopped");
        if (!engine.entry.isEmpty()) {
            // The entry, not the path: the same file queued twice is two
            // tracks to the desktop, and a notification per occurrence.
            state.track_key = engine.entry;
            state.title = QFileInfo{engine.path}.fileName();
            if (const auto entry = core::StableId::parse(engine.entry.toStdString())) {
                if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
                    if (const auto row = tab->model->rowOfEntry(*entry, playback_.row); row >= 0) {
                        const auto& track = tab->model->rows()[static_cast<std::size_t>(row)];
                        if (!track.title.empty()) {
                            state.title = displayText(track.title);
                        }
                        state.artist = displayText(track.artist);
                        state.album = displayText(track.album);
                    }
                }
            }
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
    }
    if (mpris_ != nullptr) {
        mpris_->publish(state);
    }
    if (notifier_ != nullptr &&
        notifier_->publish(state, QGuiApplication::focusWindow() != nullptr)) {
        setProperty("trackknife-notifications-sent",
                    static_cast<qulonglong>(notifier_->sentCount()));
        setProperty("trackknife-notification-summary", notifier_->lastSummary());
        setProperty("trackknife-notification-body", notifier_->lastBody());
    }
}

} // namespace trackknife::bench
