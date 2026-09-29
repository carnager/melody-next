// SPDX-License-Identifier: GPL-3.0-only

#include "quick/engine.hpp"

#include "bench/engine_launcher.hpp"
#include "bench/remote_engines.hpp"
#include "quick/format.hpp"

#include <QJSEngine>

namespace trackknife::quick {

namespace {

Engine* the_instance = nullptr;

[[nodiscard]] core::Error unreachable(std::string message) {
    return core::Error{.code = core::ErrorCode::io, .message = std::move(message), .context = {}};
}

// What to call an engine elsewhere before it says: its host.
[[nodiscard]] QString hostOf(const QString& address) {
    const auto colon = address.lastIndexOf(QLatin1Char(':'));
    return colon > 0 && !address.contains(QLatin1Char('/')) ? address.left(colon) : address;
}

} // namespace

Engine::Engine(QObject* parent) : QObject(parent) {
    the_instance = this;

    // This computer's engine, started if nothing answers (ADR-0226).
    const auto local_engine = bench::localEngine();
    sessions_.push_back(std::make_unique<EngineSession>(
        0, QStringLiteral("local"), true, tr("This computer"),
        protocol::Endpoint{.socket = local_engine ? local_engine->socket : std::filesystem::path{},
                           .host = {},
                           .port = 0,
                           .token = {}},
        []() -> core::Result<std::unique_ptr<protocol::Client>> {
            const auto engine = bench::localEngine();
            if (!engine) {
                return std::unexpected(unreachable("there is no engine on this computer"));
            }
            return bench::connectLocalEngine(*engine);
        }));

    // Each configured elsewhere, as the widgets window keeps them.
    for (const auto& setting : bench::loadRemoteEngines()) {
        const auto address = setting.address.trimmed();
        if (address.isEmpty()) {
            continue;
        }
        const auto endpoint =
            protocol::Endpoint::parse(address.toStdString(), setting.effectivePassword().toStdString());
        if (!endpoint) {
            continue;
        }
        const auto index = static_cast<int>(sessions_.size());
        sessions_.push_back(std::make_unique<EngineSession>(
            index, address, false, hostOf(address), *endpoint,
            [endpoint = *endpoint]() { return protocol::Client::connect(endpoint); }));
    }

    for (auto& session : sessions_) {
        // The list on show was changed elsewhere -- by the widgets window, say.
        connect(session->lists(), &ListsModel::listChanged, this, [this, raw = session.get()](const QString& id) {
            if (tracks_.session() == raw && tracks_.listId() == id) {
                tracks_.reload();
            }
        });
        session->start();
    }
    current_ = sessions_.front().get();
    desktop_ = std::make_unique<DesktopBridge>(*this);
    // Written tags show at once: the engine refreshed its library in the
    // same commit, and the list on show asks for its facts again.
    connect(&tag_editor_, &TagEditor::written, this, [this](EngineSession* session) {
        if (tracks_.session() == session) {
            tracks_.reload();
        }
        session->library()->refresh();
    });
}

Engine::~Engine() {
    desktop_.reset();
    if (the_instance == this) {
        the_instance = nullptr;
    }
}

Engine* Engine::create(QQmlEngine* /*qml*/, QJSEngine* /*js*/) {
    QJSEngine::setObjectOwnership(the_instance, QJSEngine::CppOwnership);
    return the_instance;
}

QList<EngineSession*> Engine::sessions() const {
    QList<EngineSession*> all;
    for (const auto& session : sessions_) {
        all.push_back(session.get());
    }
    return all;
}

const EngineClient* Engine::clientAt(const int index) const {
    return index >= 0 && index < static_cast<int>(sessions_.size())
               ? &sessions_[static_cast<std::size_t>(index)]->client()
               : nullptr;
}

void Engine::show(EngineSession* session, const QString& listId) {
    tracks_.show(session, listId);
    if (session != nullptr && session != current_) {
        current_ = session;
        emit currentChanged();
    }
}

EngineSession* Engine::sessionByKey(const QString& key) const {
    for (const auto& session : sessions_) {
        if (session->key() == key) {
            return session.get();
        }
    }
    return nullptr;
}

QString Engine::formatDuration(const qreal seconds) const { return quick::formatDuration(seconds); }

} // namespace trackknife::quick
