// SPDX-License-Identifier: GPL-3.0-only

#include "bench/desktop_notifier.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>

#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString markup_escaped(QString text) {
    // The notification body is markup per the freedesktop specification.
    text.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    text.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    text.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return text;
}

} // namespace

DesktopNotifier::DesktopNotifier(QObject* parent) : QObject(parent) {}

bool DesktopNotifier::publish(const MprisPlaybackState& state, const bool window_active) {
    const auto playing = state.status == QStringLiteral("Playing");
    const auto changed =
        playing && !state.track_key.isEmpty() && state.track_key != last_track_key_;
    // Track identity follows every snapshot so enabling, unfocusing, or
    // resuming never retro-notifies an old transition.
    if (playing) {
        last_track_key_ = state.track_key;
    }
    if (!changed || !enabled_ || (background_only_ && window_active)) {
        return false;
    }
    const auto summary = state.title.isEmpty() ? state.track_key : state.title;
    QString body;
    if (!state.artist.isEmpty()) {
        body = state.artist;
    }
    if (!state.album.isEmpty()) {
        body += body.isEmpty() ? state.album : QStringLiteral(" — ") + state.album;
    }
    last_summary_ = summary;
    last_body_ = body;
    ++sent_count_;
    send(summary, markup_escaped(body));
    return true;
}

void DesktopNotifier::sendTest() {
    send(QStringLiteral("Trackknife"), QStringLiteral("Track-change notifications are working."));
}

void DesktopNotifier::send(const QString& summary, const QString& body) {
    if (send_override_) {
        send_override_(summary, body);
        emit deliveryFinished({});
        return;
    }
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit deliveryFinished(QStringLiteral("No desktop session bus is available"));
        return;
    }
    auto call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                               QStringLiteral("/org/freedesktop/Notifications"),
                                               QStringLiteral("org.freedesktop.Notifications"),
                                               QStringLiteral("Notify"));
    QVariantMap hints;
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue(static_cast<uchar>(1U)));
    hints.insert(QStringLiteral("transient"), true);
    hints.insert(QStringLiteral("suppress-sound"), true);
    hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("trackknife"));
    call << QStringLiteral("Trackknife") << replace_id_ << QStringLiteral("audio-x-generic")
         << summary << body << QStringList{} << hints << -1;
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* finished) {
                const QDBusPendingReply<quint32> reply{*finished};
                if (reply.isValid()) {
                    replace_id_ = reply.value();
                }
                emit deliveryFinished(reply.isError() ? reply.error().message() : QString{});
                finished->deleteLater();
            });
}

} // namespace trackknife::bench
