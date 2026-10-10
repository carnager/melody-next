// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/desktop_media_session.hpp"

#include <QElapsedTimer>
#include <QString>

namespace trackknife::bench {

class MprisRootAdaptor;
class MprisPlayerAdaptor;

// Exports org.mpris.MediaPlayer2.trackknife on the session bus and forwards
// desktop commands as signals; the owning window binds them to the same
// transport actions the visible controls use, so MPRIS follows the active
// authority exactly (ADR-0135). Without a session bus the service is inert.
class MprisService final : public DesktopMediaSession {
    Q_OBJECT

  public:
    explicit MprisService(QObject* parent = nullptr);
    ~MprisService() override;

    [[nodiscard]] bool registered() const noexcept { return registered_; }
    [[nodiscard]] QString serviceName() const { return service_name_; }
    // Diffs against the previous snapshot, emits PropertiesChanged for real
    // changes and Seeked for discontinuous jumps on an unchanged track.
    void publish(const MprisPlaybackState& state) override;

  private:
    friend class MprisRootAdaptor;
    friend class MprisPlayerAdaptor;

    void emitPropertiesChanged(const QString& interface, const QVariantMap& changed);
    [[nodiscard]] QVariantMap metadataMap() const;

    QElapsedTimer position_clock_;
    bool registered_{false};
    QString service_name_;
};

} // namespace trackknife::bench
