// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/desktop_notifier.hpp"
#include "bench/mpris_service.hpp"

#include <QObject>
#include <QTimer>
#include <QtQmlIntegration>

namespace trackknife::quick {

class Engine;

// The desktop's view of what plays (ADR-0135, ADR-0144): media keys and
// MPRIS widgets, and an optional notification when the track changes. Both
// follow the current engine -- the one whose list is on show -- as the
// transport does, and commands from the desktop go to it.
class DesktopBridge final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through Engine.desktop")

    Q_PROPERTY(bool notifications READ notifications WRITE setNotifications NOTIFY settingsChanged)
    Q_PROPERTY(bool backgroundOnly READ backgroundOnly WRITE setBackgroundOnly NOTIFY settingsChanged)

  public:
    explicit DesktopBridge(Engine& engine, QObject* parent = nullptr);

    [[nodiscard]] bool notifications() const { return notifier_.isEnabled(); }
    void setNotifications(bool on);
    [[nodiscard]] bool backgroundOnly() const { return background_only_; }
    void setBackgroundOnly(bool on);

  signals:
    void settingsChanged();
    // The desktop asked to see the window.
    void raiseRequested();

  private:
    void follow();
    void publish();

    Engine& engine_;
    bench::MprisService mpris_;
    bench::DesktopNotifier notifier_;
    bool background_only_{false};
    QMetaObject::Connection changed_;
    QMetaObject::Connection metadata_;
    // Several changes arrive together -- a state, then the formatted title;
    // they are published once.
    QTimer settle_;
};

} // namespace trackknife::quick
