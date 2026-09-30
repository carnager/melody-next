// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/lastfm_settings_session.hpp"

#include "bench/lastfm_service.hpp"
#include "workspace/workspace.hpp"

#include <QDesktopServices>
#include <QFutureWatcher>
#include <QPointer>
#include <QSettings>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] bool validCredential(const QString& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](QChar c) {
               return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') ||
                      (c >= u'A' && c <= u'F');
           });
}

} // namespace

LastFmSettingsSession::LastFmSettingsSession(Workspace& work, QObject* parent)
    : QObject(parent), work_(work),
      key_(QSettings{}.value(QStringLiteral("lastfm/api-key")).toString()) {
    poll_.setObjectName(QStringLiteral("lastfm-auth-poll"));
    poll_.setSingleShot(true);
    poll_.setInterval(3000);
    deadline_.setObjectName(QStringLiteral("lastfm-auth-deadline"));
    deadline_.setSingleShot(true);
    deadline_.setInterval(5 * 60 * 1000);
    poll_.setParent(this);
    deadline_.setParent(this);
    refresh_.setParent(this);
    connect(&poll_, &QTimer::timeout, this, [this] { send(QStringLiteral("finish")); });
    connect(&deadline_, &QTimer::timeout, this, [this] {
        setWaiting(false);
        status_ = QStringLiteral("Authorization timed out. Connect again to retry.");
        emit changed();
    });
    if (work_.lastfm_ != nullptr) {
        connect(work_.lastfm_, &LastFmService::completed, this, &LastFmSettingsSession::received);
    }
    connect(&refresh_, &QTimer::timeout, this, [this] {
        if (!waiting_) {
            send(QStringLiteral("status"));
        }
    });
    refresh_.start(10000);

    // ADR-0220: the engines scrobble what they play. One session serves
    // them all; it is handed over, never read back.
    if (work_.localCatalogue() && work_.localCatalogue()->endpoint()) {
        engines_.push_back({.name = QStringLiteral("This computer"),
                            .id = QStringLiteral("lastfm-engine-local"),
                            .user = {},
                            .endpoint = *work_.localCatalogue()->endpoint()});
    }
    // Each engine elsewhere, in the order Settings have them.
    for (int number = 0; const auto& engine : work_.engines_) {
        if (engine->key.isLocal() || engine->catalogue == nullptr ||
            !engine->catalogue->endpoint()) {
            continue;
        }
        engines_.push_back({.name = engine->catalogue->name(),
                            .id = number == 0
                                      ? QStringLiteral("lastfm-engine-remote")
                                      : QStringLiteral("lastfm-engine-remote-%1").arg(number),
                            .user = {},
                            .endpoint = *engine->catalogue->endpoint()});
        ++number;
    }
    for (int row = 0; row < static_cast<int>(engines_.size()); ++row) {
        ask(row);
    }
    send(QStringLiteral("status"));
}

void LastFmSettingsSession::setKey(const QString& key) {
    key_ = key;
    emit changed();
}

void LastFmSettingsSession::setSecret(const QString& secret) {
    secret_ = secret;
    emit changed();
}

void LastFmSettingsSession::setReuseKey(const bool on) {
    reuse_key_ = on;
    emit changed();
}

QString LastFmSettingsSession::connectText() const {
    return credentials_saved_ ? QStringLiteral("Reconnect in browser…")
                              : QStringLiteral("Connect to Last.fm…");
}

bool LastFmSettingsSession::inUse(const int row) const {
    if (row < 0 || row >= static_cast<int>(engines_.size())) {
        return false;
    }
    const auto& user = engines_[static_cast<std::size_t>(row)].user;
    return !user.isEmpty() && user == work_.lastfm_user_;
}

void LastFmSettingsSession::send(const QString& op, const QStringList& args) {
    if (work_.lastfm_ == nullptr) {
        return;
    }
    if (op == QStringLiteral("begin") || op == QStringLiteral("finish")) {
        request_pending_ = true;
    }
    if (op != QStringLiteral("status") && op != QStringLiteral("finish")) {
        status_ = QStringLiteral("Working…");
        emit changed();
    }
    work_.lastfm_->execute(op, args);
}

void LastFmSettingsSession::setWaiting(const bool active) {
    waiting_ = active;
    if (active) {
        deadline_.start();
    } else {
        poll_.stop();
        deadline_.stop();
    }
    emit changed();
}

void LastFmSettingsSession::connectAccount() {
    if (credentials_saved_) {
        setWaiting(true);
        send(QStringLiteral("begin"));
        return;
    }
    const auto api_key = key_.trimmed();
    const auto shared_secret = secret_.trimmed();
    if (!validCredential(api_key) || !validCredential(shared_secret)) {
        status_ = QStringLiteral("Paste the 32-character API key and shared secret "
                                 "from your Last.fm API account.");
        emit changed();
        return;
    }
    if (reuse_key_) {
        QSettings{}.setValue(QStringLiteral("lastfm/api-key"), api_key);
        // Settings shows it too, so its Save cannot overwrite the reused key.
        emit keyReused(api_key);
    }
    setWaiting(true);
    send(QStringLiteral("begin"), {api_key, shared_secret});
}

void LastFmSettingsSession::stopWaiting() {
    setWaiting(false);
    status_ = QStringLiteral("Stopped waiting for approval. Connect again to retry.");
    emit changed();
}

void LastFmSettingsSession::disconnectAccount() {
    setWaiting(false);
    send(QStringLiteral("disconnect"));
}

void LastFmSettingsSession::setScrobbling(const bool on) {
    scrobbling_ = on;
    send(QStringLiteral("enable"), {on ? QStringLiteral("1") : QStringLiteral("0")});
}

void LastFmSettingsSession::received(const QString& op, const QJsonObject& state,
                                     const QString& error) {
    static const QStringList answered{QStringLiteral("status"), QStringLiteral("begin"),
                                      QStringLiteral("finish"), QStringLiteral("disconnect"),
                                      QStringLiteral("enable")};
    if (!answered.contains(op)) {
        return;
    }
    const bool auth_reply = op == QStringLiteral("begin") || op == QStringLiteral("finish");
    if (auth_reply) {
        request_pending_ = false;
        if (!waiting_) {
            setWaiting(false);
            return;
        }
    }
    if (!error.isEmpty()) {
        // The provider's "not yet authorized" answer means keep waiting.
        if (op == QStringLiteral("finish") && error.contains(QStringLiteral("(code 14)"))) {
            poll_.start();
            return;
        }
        if (auth_reply) {
            setWaiting(false);
        }
        status_ = error;
        emit changed();
        return;
    }
    scrobbling_ = state.value("enabled").toBool();
    connected_ = state.value("connected").toBool();
    if (op == QStringLiteral("finish") && state.value("authorization_pending").toBool()) {
        poll_.start();
        emit changed();
        return;
    }
    if (op == QStringLiteral("finish")) {
        setWaiting(false);
    }
    credentials_saved_ = state.value("credentials_saved").toBool();
    if (op == QStringLiteral("begin")) {
        secret_.clear();
    }
    status_ = QStringLiteral("%1 · %2 pending\n%3")
                  .arg(connected_ ? QStringLiteral("Connected as %1")
                                        .arg(state.value("user").toString())
                                  : QStringLiteral("Not connected"))
                  .arg(state.value("pending").toInt())
                  .arg(state.value("message").toString());
    if (op == QStringLiteral("begin")) {
        status_ = QStringLiteral("Waiting for browser approval…");
        poll_.start();
        const QUrl url(state.value("url").toString());
        if (url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("www.last.fm")) {
            QDesktopServices::openUrl(url);
        }
    }
    emit changed();
}

void LastFmSettingsSession::setEngineState(const QString& id, const QString& state,
                                           const QString& user, const bool answered) {
    const auto found = std::ranges::find(engines_, id, &Engine::id);
    if (found == engines_.end()) {
        return;
    }
    found->state = state;
    if (answered) {
        found->user = user;
    }
    emit enginesChanged();
}

void LastFmSettingsSession::ask(const int row) {
    const auto& engine = engines_[static_cast<std::size_t>(row)];
    // The line to show, and the account the engine uses (empty for none).
    using Answer = std::pair<QString, QString>;
    auto* watcher = new QFutureWatcher<Answer>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, id = engine.id] {
        watcher->deleteLater();
        const auto [text, user] = watcher->result();
        setEngineState(id, text, user, true);
    });
    watcher->setFuture(QtConcurrent::run([endpoint = engine.endpoint]() -> Answer {
        auto client = protocol::Client::connect(endpoint);
        if (!client) {
            return {QStringLiteral("Not reachable"), {}};
        }
        auto answer = (*client)->call("lastfm.status", protocol::Json::object(),
                                      std::chrono::seconds{3});
        (*client)->close();
        if (!answer) {
            return {QStringLiteral("Cannot scrobble (engine too old)"), {}};
        }
        const auto user = answer->value("user", protocol::Json{});
        if (!user.is_string() || !answer->value("enabled", false)) {
            return {QStringLiteral("Not scrobbling"), {}};
        }
        const auto name = QString::fromStdString(user.get<std::string>());
        return {QStringLiteral("Scrobbling as %1 · %2 waiting")
                    .arg(name)
                    .arg(answer->value("pending", 0)),
                name};
    }));
}

void LastFmSettingsSession::useAccount(const int row) {
    if (row < 0 || row >= static_cast<int>(engines_.size()) || work_.lastfm_ == nullptr) {
        return;
    }
    const auto id = engines_[static_cast<std::size_t>(row)].id;
    const auto endpoint = engines_[static_cast<std::size_t>(row)].endpoint;
    setEngineState(id, QStringLiteral("Handing over…"), {}, false);
    // The session, from this window's own sign-in, then to the engine.
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(
        work_.lastfm_, &LastFmService::completed, this,
        [this, endpoint, id, connection](const QString& op, const QJsonObject& session,
                                         const QString& error) {
            if (op != QStringLiteral("session")) {
                return;
            }
            disconnect(*connection);
            if (!error.isEmpty()) {
                setEngineState(id, error, {}, false);
                return;
            }
            const protocol::Json params{
                {"api_key", session.value("api_key").toString().toStdString()},
                {"secret", session.value("secret").toString().toStdString()},
                {"session_key", session.value("session_key").toString().toStdString()},
                {"user", session.value("user").toString().toStdString()}};
            auto* watcher = new QFutureWatcher<QString>(this);
            connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, id] {
                watcher->deleteLater();
                const auto said = watcher->result();
                const bool handed = said.startsWith(QStringLiteral("Scrobbling as "));
                setEngineState(id, said, work_.lastfm_user_, handed);
                // The engines this window plays on may scrobble now: it
                // stops crediting them itself.
                for (const auto& engine : work_.engines_) {
                    if (engine->playback != nullptr) {
                        engine->playback->refreshScrobbling();
                    }
                }
            });
            watcher->setFuture(QtConcurrent::run([endpoint, params] {
                auto client = protocol::Client::connect(endpoint);
                if (!client) {
                    return QStringLiteral("Not reachable: %1")
                        .arg(QString::fromStdString(client.error().message));
                }
                auto answer = (*client)->call("lastfm.set_session", params);
                (*client)->close();
                if (!answer) {
                    return QStringLiteral("Not handed over: %1")
                        .arg(QString::fromStdString(answer.error().message));
                }
                return QStringLiteral("Scrobbling as %1")
                    .arg(QString::fromStdString(answer->value("user", std::string{})));
            }));
        });
    work_.lastfm_->execute(QStringLiteral("session"));
}

QString LastFmSettingsSession::addEngine(const QString& address, const QString& password) {
    const auto endpoint =
        protocol::Endpoint::parse(address.trimmed().toStdString(), password.toStdString());
    if (!endpoint) {
        return QStringLiteral("Not an engine address: %1").arg(address.trimmed());
    }
    engines_.push_back({.name = address.trimmed(),
                        .id = other_engines_ == 0
                                  ? QStringLiteral("lastfm-engine-other")
                                  : QStringLiteral("lastfm-engine-other-%1").arg(other_engines_),
                        .user = {},
                        .endpoint = *endpoint});
    ++other_engines_;
    emit enginesChanged();
    ask(static_cast<int>(engines_.size()) - 1);
    return {};
}

} // namespace trackknife::bench
