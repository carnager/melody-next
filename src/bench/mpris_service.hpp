// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

namespace trackknife::bench {

// Compact playback snapshot published to the desktop session (ADR-0135).
// Position changes continuously and is deliberately excluded from equality:
// it never triggers PropertiesChanged, only on-demand reads and Seeked.
struct MprisPlaybackState {
    QString status{QStringLiteral("Stopped")}; // Playing | Paused | Stopped
    QString title;
    QString artist;
    QString album;
    // Stable per playing track (raw path or server URI); drives the track id
    // and the Seeked-vs-track-change distinction.
    QString track_key;
    qlonglong length_us{-1};
    qlonglong position_us{0};
    // -1 hides the volume (no authority volume available right now).
    int volume_percent{-1};
    bool can_next{false};
    bool can_previous{false};
    bool can_play{false};
    bool can_pause{false};
    bool can_seek{false};
    // The cover, as a file the desktop can read (empty: none).
    QString art_path;
    // None | Track | Playlist, and whether the order is shuffled.
    QString loop_status{QStringLiteral("None")};
    bool shuffle{false};

    [[nodiscard]] bool sameExceptPosition(const MprisPlaybackState& other) const {
        return status == other.status && title == other.title && artist == other.artist &&
               album == other.album && track_key == other.track_key &&
               length_us == other.length_us && volume_percent == other.volume_percent &&
               can_next == other.can_next && can_previous == other.can_previous &&
               can_play == other.can_play && can_pause == other.can_pause &&
               can_seek == other.can_seek && art_path == other.art_path &&
               loop_status == other.loop_status && shuffle == other.shuffle;
    }
};

class MprisRootAdaptor;
class MprisPlayerAdaptor;

// Exports org.mpris.MediaPlayer2.trackknife on the session bus and forwards
// desktop commands as signals; the owning window binds them to the same
// transport actions the visible controls use, so MPRIS follows the active
// authority exactly (ADR-0135). Without a session bus the service is inert.
class MprisService final : public QObject {
    Q_OBJECT

  public:
    explicit MprisService(QObject* parent = nullptr);
    ~MprisService() override;

    [[nodiscard]] bool registered() const noexcept { return registered_; }
    [[nodiscard]] QString serviceName() const { return service_name_; }
    [[nodiscard]] const MprisPlaybackState& currentState() const noexcept { return state_; }
    // Diffs against the previous snapshot, emits PropertiesChanged for real
    // changes and Seeked for discontinuous jumps on an unchanged track.
    void publish(const MprisPlaybackState& state);

  signals:
    void playPauseRequested();
    void playRequested();
    void pauseRequested();
    void stopRequested();
    void nextRequested();
    void previousRequested();
    void positionRequested(qlonglong position_ms);
    void volumeRequested(int volume_percent);
    // The desktop setting the order: None, Track or Playlist; shuffled.
    void loopStatusRequested(const QString& status);
    void shuffleRequested(bool shuffle);
    void raiseRequested();

  private:
    friend class MprisRootAdaptor;
    friend class MprisPlayerAdaptor;

    void emitPropertiesChanged(const QString& interface, const QVariantMap& changed);
    [[nodiscard]] QVariantMap metadataMap() const;

    MprisPlaybackState state_;
    QElapsedTimer position_clock_;
    bool registered_{false};
    QString service_name_;
};

} // namespace trackknife::bench
