// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/protocol/message.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <filesystem>
#include <functional>
#include <memory>

class QThread;

namespace trackknife::ui {

// ADR-0220 Phase 2: the workspace's end of the socket.
//
// protocol::Client is deliberately Qt-free and blocks; this owns one on a
// worker thread and hands results back on the thread that asked, so a widget
// can talk to an engine without either side knowing about the other's
// threading. Nothing here interprets a method or a payload: that belongs to
// whoever is asking.
class EngineConnection final : public QObject {
    Q_OBJECT

  public:
    explicit EngineConnection(QObject* parent = nullptr);
    ~EngineConnection() override;

    // Completions run on this object's thread, so a handler may touch widgets.
    using Completion = std::function<void(core::Result<protocol::Json>)>;

    // Connects in the background. Answers through connected() or failed().
    void connectTo(std::filesystem::path socket_path);
    void disconnectFromEngine();
    [[nodiscard]] bool isConnected() const;

    void call(QString method, protocol::Json params, Completion completion);
    // Fire and forget; there is no response to wait for and none will come.
    void notify(QString method, protocol::Json params);

  signals:
    void connected();
    // Carries why, because "it stopped working" is not actionable.
    void failed(QString reason);
    void disconnected();
    // The payload travels as encoded JSON rather than a registered metatype:
    // it crosses a queued connection, and keeping it bytes means no type
    // registration and a signal that is readable in a debugger.
    void engineEvent(QString name, QByteArray data);

  private:
    struct State;

    QThread* thread_{nullptr};
    QObject* worker_{nullptr};
    std::shared_ptr<State> state_;
};

} // namespace trackknife::ui
