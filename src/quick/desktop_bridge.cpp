// SPDX-License-Identifier: GPL-3.0-only

#include "quick/desktop_bridge.hpp"

#include "quick/engine.hpp"

#include <QGuiApplication>

namespace trackknife::quick {

DesktopBridge::DesktopBridge(Engine& engine, QObject* parent)
    : QObject(parent), engine_(engine), mpris_(this), notifier_(this) {
    settle_.setSingleShot(true);
    settle_.setInterval(50);
    connect(&settle_, &QTimer::timeout, this, &DesktopBridge::publish);

    const auto player = [this] { return engine_.current()->player(); };
    connect(&mpris_, &bench::MprisService::playPauseRequested, this, [player] { player()->toggle(); });
    connect(&mpris_, &bench::MprisService::playRequested, this, [player] {
        if (!player()->playing()) {
            player()->toggle();
        }
    });
    connect(&mpris_, &bench::MprisService::pauseRequested, this, [player] {
        if (player()->playing()) {
            player()->toggle();
        }
    });
    connect(&mpris_, &bench::MprisService::stopRequested, this, [player] { player()->stop(); });
    connect(&mpris_, &bench::MprisService::nextRequested, this, [player] { player()->next(); });
    connect(&mpris_, &bench::MprisService::previousRequested, this, [player] { player()->previous(); });
    connect(&mpris_, &bench::MprisService::positionRequested, this,
            [player](const qlonglong position_ms) { player()->seek(static_cast<qreal>(position_ms) / 1000.0); });
    connect(&mpris_, &bench::MprisService::volumeRequested, this,
            [player](const int percent) { player()->setVolume(percent); });
    connect(&mpris_, &bench::MprisService::raiseRequested, this, &DesktopBridge::raiseRequested);

    connect(&engine_, &Engine::currentChanged, this, &DesktopBridge::follow);
    follow();
}

void DesktopBridge::setNotifications(const bool on) {
    if (on == notifier_.isEnabled()) {
        return;
    }
    notifier_.setEnabled(on);
    emit settingsChanged();
}

void DesktopBridge::setBackgroundOnly(const bool on) {
    if (on == background_only_) {
        return;
    }
    background_only_ = on;
    notifier_.setBackgroundOnly(on);
    emit settingsChanged();
}

void DesktopBridge::follow() {
    disconnect(changed_);
    disconnect(metadata_);
    auto* player = engine_.current()->player();
    changed_ = connect(player, &PlayerState::changed, &settle_, qOverload<>(&QTimer::start));
    metadata_ = connect(player, &PlayerState::metadataChanged, &settle_, qOverload<>(&QTimer::start));
    settle_.start();
}

void DesktopBridge::publish() {
    const auto* player = engine_.current()->player();
    bench::MprisPlaybackState state;
    state.status = player->status() == QStringLiteral("playing")  ? QStringLiteral("Playing")
                   : player->status() == QStringLiteral("paused") ? QStringLiteral("Paused")
                                                                  : QStringLiteral("Stopped");
    if (!player->entry().isEmpty()) {
        // The entry, not the path: the same file queued twice is two tracks
        // to the desktop, and a notification for each.
        state.track_key = player->entry();
        state.title = player->title();
        state.artist = player->artist();
        state.album = player->album();
    }
    state.position_us = static_cast<qlonglong>(player->position() * 1'000'000.0);
    state.length_us = player->duration() > 0 ? static_cast<qlonglong>(player->duration() * 1'000'000.0) : -1;
    state.volume_percent = player->volume();
    const bool has_queue = player->queueSize() > 0;
    state.can_next = player->queueSize() > 1 || player->requests() > 0;
    state.can_previous = player->queueSize() > 1;
    state.can_play = has_queue;
    state.can_pause = has_queue;
    state.can_seek = !player->entry().isEmpty() && player->duration() > 0;
    mpris_.publish(state);
    // Only once the title has arrived: a notification naming nothing, and
    // then none for the real title, is worse than one a moment later.
    if (state.track_key.isEmpty() || !state.title.isEmpty()) {
        notifier_.publish(state, QGuiApplication::applicationState() == Qt::ApplicationActive);
    }
}

} // namespace trackknife::quick
