// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/desktop_media_session.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <functional>

namespace trackknife::bench {

// ADR-0144: posts a quiet "now playing" notification on track changes
// with an optional background-only restriction. Off by default; delivery
// failures are reported without affecting playback. See ADR-0195.
class DesktopNotifier final : public QObject {
    Q_OBJECT

  public:
    explicit DesktopNotifier(QObject* parent = nullptr);

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    void setBackgroundOnly(bool value) noexcept { background_only_ = value; }
    void sendTest();
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

    // Consumes one authority-aware now-playing snapshot; returns true
    // when a notification was posted for it. Track keys are tracked
    // even while disabled or suppressed so state changes never
    // retro-notify an old transition.
    bool publish(const MprisPlaybackState& state, bool window_active);
    // Whether `state` is a track this notifier has not seen play yet: what
    // publish would announce, if enabled.
    [[nodiscard]] bool isNewTrack(const MprisPlaybackState& state) const {
        return state.status == QStringLiteral("Playing") && !state.track_key.isEmpty() &&
               state.track_key != last_track_key_;
    }

    [[nodiscard]] quint64 sentCount() const noexcept { return sent_count_; }
    [[nodiscard]] QString lastSummary() const { return last_summary_; }
    [[nodiscard]] QString lastBody() const { return last_body_; }
    // The cover file the last notification showed; empty, none.
    [[nodiscard]] QString lastImage() const { return last_image_; }

    // Test seam: replaces the D-Bus delivery; the decision stream is
    // unchanged.
    void setSendOverride(std::function<void(const QString& summary, const QString& body)> send) {
        send_override_ = std::move(send);
    }
    // How long a notification is taken to be on screen: the next track's
    // replaces it until then, so skipping through tracks leaves one popup.
    static constexpr qint64 replace_window_ms = 4'000;
    // Which notification the next one replaces: the previous, while it can
    // still be showing; after that none, as a new one. A server may keep a
    // notification it no longer shows -- DMS does, and says nothing of its
    // closing -- and then shows nothing for one that replaces it.
    [[nodiscard]] static quint32 replacing(quint32 previous, qint64 since_sent_ms) {
        return previous != 0U && since_sent_ms >= 0 && since_sent_ms < replace_window_ms ? previous
                                                                                         : 0U;
    }

  signals:
    void deliveryFinished(const QString& error);

  private slots:
    // The server says a notification closed; a closed one is not replaced.
    void notificationClosed(uint id, uint reason);

  private:
    void send(const QString& summary, const QString& body, const QString& image = {});

    bool enabled_{false};
    bool background_only_{false};
    QString last_track_key_;
    QString last_summary_;
    QString last_body_;
    QString last_image_;
    quint64 sent_count_{0U};
    quint32 replace_id_{0U};
    QElapsedTimer replace_sent_;
    std::function<void(const QString&, const QString&)> send_override_;
};

} // namespace trackknife::bench
