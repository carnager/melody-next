// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/mpris_service.hpp"

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

    [[nodiscard]] quint64 sentCount() const noexcept { return sent_count_; }
    [[nodiscard]] QString lastSummary() const { return last_summary_; }
    [[nodiscard]] QString lastBody() const { return last_body_; }

    // Test seam: replaces the D-Bus delivery; the decision stream is
    // unchanged.
    void setSendOverride(std::function<void(const QString& summary, const QString& body)> send) {
        send_override_ = std::move(send);
    }

  signals:
    void deliveryFinished(const QString& error);

  private:
    void send(const QString& summary, const QString& body);

    bool enabled_{false};
    bool background_only_{false};
    QString last_track_key_;
    QString last_summary_;
    QString last_body_;
    quint64 sent_count_{0U};
    quint32 replace_id_{0U};
    std::function<void(const QString&, const QString&)> send_override_;
};

} // namespace trackknife::bench
