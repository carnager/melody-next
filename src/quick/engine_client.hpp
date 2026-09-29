// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/protocol/client.hpp"

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace trackknife::quick {

// A connection to one engine, shared by that engine's models.
//
// The protocol client blocks, so calls run on pool threads and are answered
// on the window's thread, as events are. Commands go through a pool of one
// thread so they reach the engine in the order they were made -- a play must
// not overtake the enqueue it depends on. Reads (library pages, covers) run
// several at a time: the client matches responses by id, so a slow cover
// never holds up a list.
class EngineClient final : public QObject {
    Q_OBJECT

  public:
    using Json = protocol::Json;
    using Answer = std::function<void(const core::Result<Json>&)>;
    using EventHandler = std::function<void(const std::string& name, const Json& data)>;
    // Makes a connection; runs on a worker, and may block for seconds.
    using Connector = std::function<core::Result<std::unique_ptr<protocol::Client>>()>;
    enum class Lane : std::uint8_t { command, read };

    explicit EngineClient(Connector connector, QObject* parent = nullptr);
    EngineClient(const EngineClient&) = delete;
    EngineClient(EngineClient&&) = delete;
    EngineClient& operator=(const EngineClient&) = delete;
    EngineClient& operator=(EngineClient&&) = delete;
    ~EngineClient() override;

    // Connects in the background, and again whenever the connection drops.
    void start();

    [[nodiscard]] bool connected() const;
    // Why there is no connection, for the window to say.
    [[nodiscard]] QString failure() const;
    // What the engine calls itself (engine.info); empty until it has said.
    [[nodiscard]] QString announcedName() const;

    // Answered on this object's thread; with no connection, at once, with
    // the error. A null answer is fire and forget.
    void call(const QString& method, Json params, Answer answer = {}, Lane lane = Lane::read);
    // A command whose answer is not interesting beyond it failing.
    void command(const QString& method, Json params = Json::object(), Answer answer = {});

    // Every event the engine sends, on this object's thread.
    void onEvent(EventHandler handler);

    // Blocking, for a caller already on a worker thread: the cover provider.
    [[nodiscard]] core::Result<Json> callNow(const std::string& method, const Json& params) const;

  signals:
    void connectedChanged();

  private:
    void connectInBackground();
    void adopt(std::shared_ptr<protocol::Client> client, const QString& failure, const QString& name);
    void dropped(const protocol::Client* which);
    void deliver(const Answer& answer, core::Result<Json> result);
    [[nodiscard]] std::shared_ptr<protocol::Client> client() const;

    Connector connector_;
    mutable std::mutex mutex_;
    std::shared_ptr<protocol::Client> client_;
    QString failure_;
    QString name_;
    bool connecting_{false};
    bool stopping_{false};
    QThreadPool commands_;
    QThreadPool reads_;
    QTimer retry_;
    std::vector<EventHandler> handlers_;
};

} // namespace trackknife::quick
