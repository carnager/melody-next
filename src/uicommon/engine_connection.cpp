// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/engine_connection.hpp"

#include "trackknife/protocol/client.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <mutex>
#include <utility>

namespace trackknife::ui {
namespace {

// The same queued-invocation shim ListPersistenceService uses, so both
// services marshal the same way rather than inventing two idioms.
template <typename Callable> struct QueuedInvocation {
    Callable callable;
    void operator()() { std::invoke(callable); }
};

template <typename Callable> void invokeQueued(QObject* receiver, Callable&& callable) {
    static_cast<void>(QMetaObject::invokeMethod(
        receiver, QueuedInvocation<std::decay_t<Callable>>{std::forward<Callable>(callable)},
        Qt::QueuedConnection));
}

} // namespace

struct EngineConnection::State {
    std::mutex mutex;
    std::unique_ptr<protocol::Client> client;
    std::atomic_bool connected{false};
};

EngineConnection::EngineConnection(QObject* parent)
    : QObject(parent), thread_(new QThread(this)), worker_(new QObject),
      state_(std::make_shared<State>()) {
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->setObjectName(QStringLiteral("trackknife-engine"));
    thread_->start();
}

EngineConnection::~EngineConnection() {
    if (thread_ == nullptr) {
        return;
    }
    {
        // Closing from here rather than on the worker: the worker may be
        // blocked inside a call, and close() is what unblocks it.
        const std::lock_guard guard{state_->mutex};
        if (state_->client) {
            state_->client->close();
        }
    }
    thread_->quit();
    thread_->wait();
}

bool EngineConnection::isConnected() const { return state_->connected.load(); }

void EngineConnection::connectTo(std::filesystem::path socket_path) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, socket_path = std::move(socket_path)]() mutable {
        auto client = protocol::Client::connect(socket_path);
        if (!client) {
            const auto reason = QString::fromUtf8(client.error().message);
            if (self) {
                invokeQueued(self, [self, reason] {
                    if (self) {
                        emit self->failed(reason);
                    }
                });
            }
            return;
        }
        // Events arrive on the client's reader thread and are marshalled
        // before anyone sees them, so a handler may touch widgets.
        (*client)->on_event([self, state](const protocol::Event& event) {
            const auto name = QString::fromUtf8(event.name);
            const auto data = QByteArray::fromStdString(event.data.dump());
            if (self) {
                invokeQueued(self, [self, name, data] {
                    if (self) {
                        emit self->engineEvent(name, data);
                    }
                });
            }
        });
        {
            const std::lock_guard guard{state->mutex};
            state->client = std::move(*client);
        }
        state->connected.store(true);
        if (self) {
            invokeQueued(self, [self] {
                if (self) {
                    emit self->connected();
                }
            });
        }
    });
}

void EngineConnection::disconnectFromEngine() {
    const QPointer self{this};
    {
        const std::lock_guard guard{state_->mutex};
        if (state_->client) {
            state_->client->close();
        }
    }
    state_->connected.store(false);
    invokeQueued(worker_, [self, state = state_] {
        {
            const std::lock_guard guard{state->mutex};
            state->client.reset();
        }
        if (self) {
            invokeQueued(self, [self] {
                if (self) {
                    emit self->disconnected();
                }
            });
        }
    });
}

void EngineConnection::call(QString method, protocol::Json params, Completion completion) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, method = std::move(method),
                           params = std::move(params),
                           completion = std::move(completion)]() mutable {
        protocol::Client* client = nullptr;
        {
            const std::lock_guard guard{state->mutex};
            client = state->client.get();
        }
        auto outcome = client == nullptr ? core::Result<protocol::Json>{std::unexpected(
                                               core::Error{.code = core::ErrorCode::io,
                                                           .message = "not connected to an engine",
                                                           .context = {}})}
                                         : client->call(method.toStdString(), params);
        if (!outcome && outcome.error().code == core::ErrorCode::io) {
            state->connected.store(false);
        }
        if (!completion) {
            return;
        }
        if (self) {
            invokeQueued(
                self, [completion = std::move(completion), outcome = std::move(outcome)]() mutable {
                    completion(std::move(outcome));
                });
        }
    });
}

void EngineConnection::notify(QString method, protocol::Json params) {
    invokeQueued(worker_, [state = state_, method = std::move(method),
                           params = std::move(params)]() mutable {
        const std::lock_guard guard{state->mutex};
        if (state->client) {
            static_cast<void>(state->client->notify(method.toStdString(), params));
        }
    });
}

} // namespace trackknife::ui
