// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/desktop_media_session.hpp"

#include <QElapsedTimer>

#include <memory>

namespace trackknife::bench {

// ADR-0275: the macOS desktop session. Publishes the snapshot to
// MPNowPlayingInfoCenter -- Control Center, the lock screen, the menu bar --
// and takes MPRemoteCommandCenter's commands: the keyboard's media keys,
// AirPods and other Bluetooth headsets, and Control Center's buttons.
class NowPlayingSession final : public DesktopMediaSession {
    Q_OBJECT

  public:
    explicit NowPlayingSession(QObject* parent = nullptr);
    ~NowPlayingSession() override;

    void publish(const MprisPlaybackState& state) override;

  private:
    struct Commands;
    std::unique_ptr<Commands> commands_;
    bool published_{false};
    // The position last handed to the system, and since when.
    qlonglong written_position_us_{0};
    QElapsedTimer written_clock_;
};

} // namespace trackknife::bench
