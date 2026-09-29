// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/desktop_bridge.hpp"
#include "quick/engine_session.hpp"
#include "quick/track_list_model.hpp"

#include <QList>
#include <QObject>
#include <QtQmlIntegration>

#include <memory>
#include <vector>

class QQmlEngine;
class QJSEngine;

namespace trackknife::quick {

// The engines this window is connected to, as one singleton for QML.
//
// ADR-0234: engines are connections, several at once -- this computer's
// and each configured elsewhere -- and every one offers the same things. The
// window shows one list at a time, from whichever engine it belongs to;
// the transport and up-next follow that engine.
class Engine final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QList<trackknife::quick::EngineSession*> sessions READ sessions CONSTANT)
    Q_PROPERTY(trackknife::quick::EngineSession* current READ current NOTIFY currentChanged)
    Q_PROPERTY(trackknife::quick::TrackListModel* tracks READ tracks CONSTANT)
    Q_PROPERTY(trackknife::quick::DesktopBridge* desktop READ desktop CONSTANT)

  public:
    explicit Engine(QObject* parent = nullptr);
    Engine(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine() override;

    // The one instance, made by main() before QML asks for it.
    static Engine* create(QQmlEngine* qml, QJSEngine* js);

    [[nodiscard]] QList<EngineSession*> sessions() const;
    [[nodiscard]] EngineSession* current() const { return current_; }
    [[nodiscard]] TrackListModel* tracks() { return &tracks_; }
    [[nodiscard]] DesktopBridge* desktop() { return desktop_.get(); }
    // For the cover provider, on its workers: sessions never change once
    // made, so reading them there needs no lock.
    [[nodiscard]] const EngineClient* clientAt(int index) const;

    // Shows a list of an engine, and makes that engine the current one.
    Q_INVOKABLE void show(trackknife::quick::EngineSession* session, const QString& listId);
    Q_INVOKABLE [[nodiscard]] trackknife::quick::EngineSession* sessionByKey(const QString& key) const;
    Q_INVOKABLE [[nodiscard]] QString formatDuration(qreal seconds) const;

  signals:
    void currentChanged();

  private:
    std::vector<std::unique_ptr<EngineSession>> sessions_;
    EngineSession* current_{nullptr};
    TrackListModel tracks_{this};
    // Made once the sessions are: it follows the current one.
    std::unique_ptr<DesktopBridge> desktop_;
};

} // namespace trackknife::quick
