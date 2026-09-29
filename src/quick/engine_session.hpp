// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/engine_client.hpp"
#include "quick/library_model.hpp"
#include "quick/lists_model.hpp"
#include "quick/player_state.hpp"
#include "quick/up_next_model.hpp"

#include <QObject>
#include <QtQmlIntegration>

namespace trackknife::quick {

// One engine this window is connected to -- this computer's, or one
// elsewhere (ADR-0234) -- with everything the window shows of it. Engines
// differ only in where they are: each has the same player, lists, library
// and up-next.
class EngineSession final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Reached through Engine.sessions")

    Q_PROPERTY(int index READ index CONSTANT)
    Q_PROPERTY(QString key READ key CONSTANT)
    Q_PROPERTY(QString name READ name NOTIFY connectedChanged)
    Q_PROPERTY(bool local READ local CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString failure READ failure NOTIFY connectedChanged)
    Q_PROPERTY(trackknife::quick::PlayerState* player READ player CONSTANT)
    Q_PROPERTY(trackknife::quick::ListsModel* lists READ lists CONSTANT)
    Q_PROPERTY(trackknife::quick::LibraryModel* library READ library CONSTANT)
    Q_PROPERTY(trackknife::quick::UpNextModel* upNext READ upNext CONSTANT)

  public:
    // `key` names it in settings: "local", or its address. `fallback` is
    // what to call it before it has said its name.
    EngineSession(int index, QString key, bool local, QString fallback, EngineClient::Connector connector,
                  QObject* parent = nullptr);

    [[nodiscard]] int index() const { return index_; }
    [[nodiscard]] QString key() const { return key_; }
    [[nodiscard]] QString name() const;
    [[nodiscard]] bool local() const { return local_; }
    [[nodiscard]] bool connected() const { return client_.connected(); }
    [[nodiscard]] QString failure() const { return client_.failure(); }
    [[nodiscard]] EngineClient& client() { return client_; }
    [[nodiscard]] const EngineClient& client() const { return client_; }
    [[nodiscard]] PlayerState* player() { return &player_; }
    [[nodiscard]] ListsModel* lists() { return &lists_; }
    [[nodiscard]] LibraryModel* library() { return &library_; }
    [[nodiscard]] UpNextModel* upNext() { return &up_next_; }

    // A cover on this engine, by the track's encoded path.
    Q_INVOKABLE [[nodiscard]] QString coverForPath(const QString& encoded_path) const;

    // Onto up-next, in order. Tracks the engine does not hold yet are added
    // to its queue first: it can only be asked for what it has.
    void enqueue(const std::vector<QueuedTrack>& tracks);

    void start() { client_.start(); }

  signals:
    void connectedChanged();

  private:
    int index_;
    QString key_;
    bool local_;
    QString fallback_;
    EngineClient client_;
    PlayerState player_;
    ListsModel lists_;
    LibraryModel library_;
    UpNextModel up_next_;
};

} // namespace trackknife::quick
