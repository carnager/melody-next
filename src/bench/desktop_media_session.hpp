// SPDX-License-Identifier: GPL-3.0-only

#pragma once

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

// The desktop's view of playback and its way to steer it: MPRIS on the
// session bus on Linux (ADR-0135), Now Playing and the remote commands on
// macOS (ADR-0275). Each publishes the same snapshot and turns the desktop's
// commands into the same signals, which the window binds to the transport
// actions the visible controls use.
class DesktopMediaSession : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;

    [[nodiscard]] const MprisPlaybackState& currentState() const noexcept { return state_; }
    virtual void publish(const MprisPlaybackState& state) = 0;

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

  protected:
    MprisPlaybackState state_;
};

// The session this platform offers: Now Playing on macOS, MPRIS elsewhere.
[[nodiscard]] DesktopMediaSession* createDesktopMediaSession(QObject* parent);

} // namespace trackknife::bench
