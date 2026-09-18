// SPDX-License-Identifier: GPL-3.0-only

#include "bench/mpris_service.hpp"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QVariantMap>

#include <cstdlib>

namespace trackknife::bench {

namespace {
constexpr auto mpris_object_path = "/org/mpris/MediaPlayer2";
constexpr auto player_interface = "org.mpris.MediaPlayer2.Player";

// A track id must be a valid, per-track-stable object path; hashing the raw
// key keeps arbitrary path/URI bytes out of the D-Bus path grammar.
[[nodiscard]] QDBusObjectPath track_object_path(const QString& track_key) {
    if (track_key.isEmpty()) {
        return QDBusObjectPath{QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack")};
    }
    return QDBusObjectPath{QStringLiteral("/de/trackknife/track/%1").arg(qHash(track_key), 0, 16)};
}
} // namespace

// org.mpris.MediaPlayer2 — identity and window control only.
class MprisRootAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

  public:
    explicit MprisRootAdaptor(MprisService* service)
        : QDBusAbstractAdaptor(service), service_(service) {}

    [[nodiscard]] bool canQuit() const { return false; }
    [[nodiscard]] bool canRaise() const { return true; }
    [[nodiscard]] bool hasTrackList() const { return false; }
    [[nodiscard]] QString identity() const { return QStringLiteral("Trackknife"); }
    [[nodiscard]] QString desktopEntry() const { return QStringLiteral("trackknife"); }
    [[nodiscard]] QStringList supportedUriSchemes() const { return {}; }
    [[nodiscard]] QStringList supportedMimeTypes() const { return {}; }

  public slots:
    void Raise() { emit service_->raiseRequested(); }
    void Quit() {}

  private:
    MprisService* service_;
};

// org.mpris.MediaPlayer2.Player — transport mirror of the active authority.
class MprisPlayerAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

  public:
    explicit MprisPlayerAdaptor(MprisService* service)
        : QDBusAbstractAdaptor(service), service_(service) {}

    [[nodiscard]] QString playbackStatus() const { return service_->currentState().status; }
    [[nodiscard]] QString loopStatus() const { return QStringLiteral("None"); }
    [[nodiscard]] double rate() const { return 1.0; }
    void setRate(double) {}
    [[nodiscard]] bool shuffle() const { return false; }
    void setShuffle(bool) {}
    [[nodiscard]] QVariantMap metadata() const { return service_->metadataMap(); }
    [[nodiscard]] double volume() const {
        const auto percent = service_->currentState().volume_percent;
        return percent < 0 ? 1.0 : percent / 100.0;
    }
    void setVolume(const double value) {
        emit service_->volumeRequested(static_cast<int>(qBound(0.0, value, 1.0) * 100.0 + 0.5));
    }
    [[nodiscard]] qlonglong position() const {
        const auto& state = service_->currentState();
        if (state.status != QStringLiteral("Playing") || !service_->position_clock_.isValid()) {
            return state.position_us;
        }
        return state.position_us + service_->position_clock_.elapsed() * 1'000;
    }
    [[nodiscard]] double minimumRate() const { return 1.0; }
    [[nodiscard]] double maximumRate() const { return 1.0; }
    [[nodiscard]] bool canGoNext() const { return service_->currentState().can_next; }
    [[nodiscard]] bool canGoPrevious() const { return service_->currentState().can_previous; }
    [[nodiscard]] bool canPlay() const { return service_->currentState().can_play; }
    [[nodiscard]] bool canPause() const { return service_->currentState().can_pause; }
    [[nodiscard]] bool canSeek() const { return service_->currentState().can_seek; }
    [[nodiscard]] bool canControl() const { return true; }

  public slots:
    void Next() { emit service_->nextRequested(); }
    void Previous() { emit service_->previousRequested(); }
    void Pause() { emit service_->pauseRequested(); }
    void PlayPause() { emit service_->playPauseRequested(); }
    void Stop() { emit service_->stopRequested(); }
    void Play() { emit service_->playRequested(); }
    void Seek(qlonglong offset_us) {
        emit service_->positionRequested((position() + offset_us) / 1'000);
    }
    void SetPosition(const QDBusObjectPath& track, qlonglong position_us) {
        if (track == track_object_path(service_->currentState().track_key)) {
            emit service_->positionRequested(position_us / 1'000);
        }
    }
    void OpenUri(const QString&) {}

  signals:
    void Seeked(qlonglong position_us);

  private:
    MprisService* service_;
};

MprisService::MprisService(QObject* parent) : QObject(parent) {
    new MprisRootAdaptor(this);
    new MprisPlayerAdaptor(this);

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    if (!bus.registerObject(QLatin1String(mpris_object_path), this)) {
        return;
    }
    // The plain well-known name first; a second running instance (or the test
    // suite beside the app) falls back to the pid-qualified variant.
    service_name_ = QStringLiteral("org.mpris.MediaPlayer2.trackknife");
    if (!bus.registerService(service_name_)) {
        service_name_ = QStringLiteral("org.mpris.MediaPlayer2.trackknife.instance%1")
                            .arg(QCoreApplication::applicationPid());
        if (!bus.registerService(service_name_)) {
            bus.unregisterObject(QLatin1String(mpris_object_path));
            service_name_.clear();
            return;
        }
    }
    registered_ = true;
}

MprisService::~MprisService() {
    if (registered_) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterService(service_name_);
        bus.unregisterObject(QLatin1String(mpris_object_path));
    }
}

QVariantMap MprisService::metadataMap() const {
    QVariantMap metadata;
    metadata.insert(QStringLiteral("mpris:trackid"),
                    QVariant::fromValue(track_object_path(state_.track_key)));
    if (state_.length_us >= 0) {
        metadata.insert(QStringLiteral("mpris:length"), state_.length_us);
    }
    if (!state_.title.isEmpty()) {
        metadata.insert(QStringLiteral("xesam:title"), state_.title);
    }
    if (!state_.artist.isEmpty()) {
        metadata.insert(QStringLiteral("xesam:artist"), QStringList{state_.artist});
    }
    if (!state_.album.isEmpty()) {
        metadata.insert(QStringLiteral("xesam:album"), state_.album);
    }
    return metadata;
}

void MprisService::emitPropertiesChanged(const QString& interface, const QVariantMap& changed) {
    if (!registered_ || changed.isEmpty()) {
        return;
    }
    auto message = QDBusMessage::createSignal(QLatin1String(mpris_object_path),
                                              QStringLiteral("org.freedesktop.DBus.Properties"),
                                              QStringLiteral("PropertiesChanged"));
    message << interface << changed << QStringList{};
    QDBusConnection::sessionBus().send(message);
}

void MprisService::publish(const MprisPlaybackState& state) {
    const auto previous = state_;
    const auto had_clock = position_clock_.isValid();
    // Project the previous position to "now" before comparing, so ordinary
    // playback progress does not read as a seek.
    const auto projected_previous = previous.status == QStringLiteral("Playing") && had_clock
                                        ? previous.position_us + position_clock_.elapsed() * 1'000
                                        : previous.position_us;
    state_ = state;
    position_clock_.start();

    QVariantMap changed;
    if (previous.status != state.status) {
        changed.insert(QStringLiteral("PlaybackStatus"), state.status);
    }
    if (previous.track_key != state.track_key || previous.title != state.title ||
        previous.artist != state.artist || previous.album != state.album ||
        previous.length_us != state.length_us) {
        changed.insert(QStringLiteral("Metadata"), metadataMap());
    }
    if (previous.volume_percent != state.volume_percent) {
        changed.insert(QStringLiteral("Volume"),
                       state.volume_percent < 0 ? 1.0 : state.volume_percent / 100.0);
    }
    if (previous.can_next != state.can_next) {
        changed.insert(QStringLiteral("CanGoNext"), state.can_next);
    }
    if (previous.can_previous != state.can_previous) {
        changed.insert(QStringLiteral("CanGoPrevious"), state.can_previous);
    }
    if (previous.can_play != state.can_play) {
        changed.insert(QStringLiteral("CanPlay"), state.can_play);
    }
    if (previous.can_pause != state.can_pause) {
        changed.insert(QStringLiteral("CanPause"), state.can_pause);
    }
    if (previous.can_seek != state.can_seek) {
        changed.insert(QStringLiteral("CanSeek"), state.can_seek);
    }
    emitPropertiesChanged(QLatin1String(player_interface), changed);

    // A jump of more than 1.5 s against projected progress on the same track
    // is a seek, per the MPRIS contract.
    if (registered_ && had_clock && previous.track_key == state.track_key &&
        !state.track_key.isEmpty() &&
        std::llabs(state.position_us - projected_previous) > 1'500'000) {
        auto message =
            QDBusMessage::createSignal(QLatin1String(mpris_object_path),
                                       QLatin1String(player_interface), QStringLiteral("Seeked"));
        message << state.position_us;
        QDBusConnection::sessionBus().send(message);
    }
}

} // namespace trackknife::bench

#include "mpris_service.moc"
