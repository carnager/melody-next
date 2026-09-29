// SPDX-License-Identifier: GPL-3.0-only

#include "quick/engine_client.hpp"

#include <QMetaObject>
#include <QPointer>

#include <utility>

namespace trackknife::quick {

namespace {

[[nodiscard]] core::Error notConnected() {
    return core::Error{.code = core::ErrorCode::io,
                       .message = "not connected to the engine",
                       .context = {}};
}

} // namespace

EngineClient::EngineClient(Connector connector, QObject* parent)
    : QObject(parent), connector_(std::move(connector)) {
    commands_.setMaxThreadCount(1);
    reads_.setMaxThreadCount(4);
    retry_.setSingleShot(true);
    retry_.setInterval(2000);
    connect(&retry_, &QTimer::timeout, this, &EngineClient::connectInBackground);
}

EngineClient::~EngineClient() {
    stopping_ = true;
    retry_.stop();
    if (auto current = client()) {
        // Fails whatever is outstanding, so the pools drain at once.
        current->close();
    }
    reads_.clear();
    commands_.clear();
    reads_.waitForDone();
    commands_.waitForDone();
}

void EngineClient::start() { connectInBackground(); }

bool EngineClient::connected() const {
    const auto current = client();
    return current && current->connected();
}

QString EngineClient::failure() const {
    const std::lock_guard guard{mutex_};
    return failure_;
}

QString EngineClient::announcedName() const {
    const std::lock_guard guard{mutex_};
    return name_;
}

std::shared_ptr<protocol::Client> EngineClient::client() const {
    const std::lock_guard guard{mutex_};
    return client_;
}

void EngineClient::connectInBackground() {
    if (connecting_ || stopping_ || connected()) {
        return;
    }
    connecting_ = true;
    const QPointer self{this};
    reads_.start([self, this] {
        std::shared_ptr<protocol::Client> made;
        QString why;
        QString name;
        if (auto connection = connector_()) {
            made = std::shared_ptr<protocol::Client>(std::move(*connection));
        } else {
            why = QString::fromStdString(connection.error().message);
        }
        if (made) {
            // Handlers first: the client wants them before its first call,
            // and they must never own the client -- one destroyed on its
            // own reader thread would join itself.
            made->on_event([self, this](const protocol::Event& event) {
                QMetaObject::invokeMethod(
                    self,
                    [self, this, name = event.name, data = event.data] {
                        if (!self) {
                            return;
                        }
                        const auto handlers = handlers_;
                        for (const auto& handler : handlers) {
                            handler(name, data);
                        }
                    },
                    Qt::QueuedConnection);
            });
            made->on_closed([self, this, raw = made.get()] {
                QMetaObject::invokeMethod(
                    self,
                    [self, this, raw] {
                        if (self) {
                            dropped(raw);
                        }
                    },
                    Qt::QueuedConnection);
            });
            if (auto info = made->call("engine.info", Json::object(), std::chrono::seconds{2})) {
                name = QString::fromStdString(protocol::displayable_text(info->value("name", std::string{})));
            }
        }
        QMetaObject::invokeMethod(
            self,
            [self, this, made, why, name] {
                if (self) {
                    adopt(made, why, name);
                }
            },
            Qt::QueuedConnection);
    });
}

void EngineClient::adopt(std::shared_ptr<protocol::Client> client, const QString& failure,
                         const QString& name) {
    connecting_ = false;
    {
        const std::lock_guard guard{mutex_};
        client_ = std::move(client);
        failure_ = failure;
        if (!name.isEmpty()) {
            name_ = name;
        }
    }
    if (!connected()) {
        retry_.start();
    }
    emit connectedChanged();
}

void EngineClient::dropped(const protocol::Client* which) {
    {
        const std::lock_guard guard{mutex_};
        // A connection that closed after a newer one replaced it.
        if (client_.get() != which) {
            return;
        }
        client_.reset();
        failure_ = QStringLiteral("The engine went away. Reconnecting…");
    }
    emit connectedChanged();
    if (!stopping_) {
        retry_.start();
    }
}

void EngineClient::deliver(const Answer& answer, core::Result<Json> result) {
    if (!answer) {
        return;
    }
    const QPointer self{this};
    QMetaObject::invokeMethod(
        this,
        [self, answer, result = std::move(result)] {
            if (self) {
                answer(result);
            }
        },
        Qt::QueuedConnection);
}

void EngineClient::call(const QString& method, Json params, Answer answer, const Lane lane) {
    const auto current = client();
    if (!current) {
        if (answer) {
            answer(std::unexpected(notConnected()));
        }
        return;
    }
    auto& pool = lane == Lane::command ? commands_ : reads_;
    pool.start([this, current, method = method.toStdString(), params = std::move(params),
                answer = std::move(answer)] {
        deliver(answer, current->call(method, params));
    });
}

void EngineClient::command(const QString& method, Json params, Answer answer) {
    call(method, std::move(params), std::move(answer), Lane::command);
}

void EngineClient::onEvent(EventHandler handler) { handlers_.push_back(std::move(handler)); }

core::Result<EngineClient::Json> EngineClient::callNow(const std::string& method,
                                                       const Json& params) const {
    const auto current = client();
    if (!current) {
        return std::unexpected(notConnected());
    }
    return current->call(method, params);
}

} // namespace trackknife::quick
