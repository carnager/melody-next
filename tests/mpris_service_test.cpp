// SPDX-License-Identifier: GPL-3.0-only

#include "bench/desktop_notifier.hpp"
#include "bench/mpris_service.hpp"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QSignalSpy>
#include <QVariantMap>
#include <QtTest>

namespace trackknife::bench {

class NotificationFixture final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
  public:
    QVariantMap hints;
    quint32 replaced{0};
    QString body;
  public slots:
    quint32 Notify(const QString&, quint32 replace_id, const QString&, const QString&,
                   const QString& message, const QStringList&, const QVariantMap& values, int) {
        replaced = replace_id;
        hints = values;
        body = message;
        return 42U;
    }
};

class MprisServiceTest final : public QObject {
    Q_OBJECT
  private slots:
    void publishesDiffedStateWithoutABus();
    void exportsThePlayerOverTheSessionBus();
    void notificationDeliveryReportsAcceptanceAndFailure();
};

void MprisServiceTest::notificationDeliveryReportsAcceptanceAndFailure() {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.registerService(QStringLiteral("org.freedesktop.Notifications")))
        QSKIP("run this case under dbus-run-session to use an isolated notification fixture");
    NotificationFixture fixture;
    QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &fixture,
                               QDBusConnection::ExportAllSlots));
    DesktopNotifier notifier;
    QSignalSpy delivered(&notifier, &DesktopNotifier::deliveryFinished);
    notifier.sendTest();
    QTRY_COMPARE(delivered.size(), 1);
    QVERIFY(delivered.front().front().toString().isEmpty());
    QCOMPARE(fixture.hints.value(QStringLiteral("urgency")).toUInt(), 1U);
    QCOMPARE(fixture.hints.value(QStringLiteral("suppress-sound")).toBool(), true);
    QCOMPARE(fixture.replaced, 0U);
    QVERIFY(fixture.body.contains(QStringLiteral("working")));
    notifier.sendTest();
    QTRY_COMPARE(delivered.size(), 2);
    QCOMPARE(fixture.replaced, 42U);
    bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
    notifier.sendTest();
    QTRY_COMPARE(delivered.size(), 3);
    QVERIFY(!delivered.back().front().toString().isEmpty());
    bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
}

// The state machine must work identically whether or not a session bus
// exists; without one the service is inert but fully observable.
void MprisServiceTest::publishesDiffedStateWithoutABus() {
    MprisService service;
    QCOMPARE(service.currentState().status, QStringLiteral("Stopped"));

    MprisPlaybackState playing;
    playing.status = QStringLiteral("Playing");
    playing.title = QStringLiteral("Song");
    playing.artist = QStringLiteral("Band");
    playing.album = QStringLiteral("Album");
    playing.track_key = QStringLiteral("/music/song.flac");
    playing.length_us = 180'000'000;
    playing.position_us = 5'000'000;
    playing.volume_percent = 60;
    playing.can_play = true;
    playing.can_pause = true;
    playing.can_seek = true;
    service.publish(playing);
    QCOMPARE(service.currentState().status, QStringLiteral("Playing"));
    QCOMPARE(service.currentState().title, QStringLiteral("Song"));
    QCOMPARE(service.currentState().length_us, 180'000'000);
    QVERIFY(service.currentState().sameExceptPosition(playing));

    auto paused = playing;
    paused.status = QStringLiteral("Paused");
    paused.position_us = 6'000'000;
    service.publish(paused);
    QCOMPARE(service.currentState().status, QStringLiteral("Paused"));
    QVERIFY(!service.currentState().sameExceptPosition(playing));
}

void MprisServiceTest::exportsThePlayerOverTheSessionBus() {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        QSKIP("no session bus in this environment");
    }
    MprisService service;
    QVERIFY(service.registered());
    QVERIFY(service.serviceName().startsWith(QStringLiteral("org.mpris.MediaPlayer2.trackknife")));

    QDBusInterface root{service.serviceName(), QStringLiteral("/org/mpris/MediaPlayer2"),
                        QStringLiteral("org.mpris.MediaPlayer2"), bus};
    QVERIFY(root.isValid());
    QCOMPARE(root.property("Identity").toString(), QStringLiteral("Trackknife"));
    QCOMPARE(root.property("CanQuit").toBool(), false);
    QCOMPARE(root.property("CanRaise").toBool(), true);

    MprisPlaybackState playing;
    playing.status = QStringLiteral("Playing");
    playing.title = QStringLiteral("Bus Song");
    playing.artist = QStringLiteral("Bus Band");
    playing.track_key = QStringLiteral("mpd/queue/1.flac");
    playing.length_us = 90'000'000;
    playing.position_us = 1'000'000;
    playing.can_next = true;
    playing.can_play = true;
    playing.can_pause = true;
    playing.can_seek = true;
    service.publish(playing);

    QDBusInterface player{service.serviceName(), QStringLiteral("/org/mpris/MediaPlayer2"),
                          QStringLiteral("org.mpris.MediaPlayer2.Player"), bus};
    QVERIFY(player.isValid());
    QCOMPARE(player.property("PlaybackStatus").toString(), QStringLiteral("Playing"));
    QCOMPARE(player.property("CanGoNext").toBool(), true);
    QCOMPARE(player.property("CanGoPrevious").toBool(), false);
    const auto metadata = qdbus_cast<QVariantMap>(player.property("Metadata"));
    QCOMPARE(metadata.value(QStringLiteral("xesam:title")).toString(), QStringLiteral("Bus Song"));
    QCOMPARE(metadata.value(QStringLiteral("mpris:length")).toLongLong(), 90'000'000);

    // Desktop commands surface as signals for the transport binding.
    QSignalSpy play_pause{&service, &MprisService::playPauseRequested};
    QSignalSpy next{&service, &MprisService::nextRequested};
    QSignalSpy position{&service, &MprisService::positionRequested};
    QSignalSpy volume{&service, &MprisService::volumeRequested};
    QVERIFY(QDBusReply<void>{player.call(QStringLiteral("PlayPause"))}.isValid());
    QVERIFY(QDBusReply<void>{player.call(QStringLiteral("Next"))}.isValid());
    QVERIFY(QDBusReply<void>{player.call(QStringLiteral("Seek"), 4'000'000LL)}.isValid());
    QVERIFY(player.setProperty("Volume", 0.4));
    QTRY_COMPARE(play_pause.count(), 1);
    QCOMPARE(next.count(), 1);
    QCOMPARE(position.count(), 1);
    QVERIFY(position.front().front().toLongLong() >= 5'000);
    QTRY_COMPARE(volume.count(), 1);
    QCOMPARE(volume.front().front().toInt(), 40);

    // A second service in the same process shares the pid-qualified fallback
    // and must stay inert instead of clobbering the first registration;
    // across processes the pid suffix keeps the names distinct.
    MprisService second;
    QVERIFY(!second.registered());
    QCOMPARE(player.property("PlaybackStatus").toString(), QStringLiteral("Playing"));
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::MprisServiceTest)
#include "mpris_service_test.moc"
