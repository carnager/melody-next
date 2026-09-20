// SPDX-License-Identifier: GPL-3.0-only
#include "lastfm_service.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <deque>
#include <functional>

namespace trackknife::bench {
QByteArray lastFmSignature(const QMap<QString, QString>& parameters, const QString& secret) {
    QByteArray bytes;
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it)
        if (it.key() != "format" && it.key() != "callback" && it.key() != "api_sig")
            bytes += it.key().toUtf8() + it.value().toUtf8();
    return QCryptographicHash::hash(bytes + secret.toUtf8(), QCryptographicHash::Md5).toHex();
}
namespace {
class Worker final : public QObject {
  public:
    Worker(QString path, QUrl endpoint, std::function<void(QString, QJsonObject, QString)> done)
        : path_(std::move(path)), done_(std::move(done)),
          endpoint_(endpoint.isEmpty() ? QUrl(QStringLiteral("https://ws.audioscrobbler.com/2.0/"))
                                       : std::move(endpoint)) {}
    void init() {
        network_ = new QNetworkAccessManager(this);
        clock_.start();
        QFile file(path_);
        if (file.exists()) {
            if (file.open(QIODevice::ReadOnly)) {
                state_ = QJsonDocument::fromJson(file.readAll()).object();
                if (state_.value("version").toInt() != 1) {
                    blocked_ = true;
                    message_ = "Unsupported Last.fm state file";
                }
            } else {
                blocked_ = true;
                message_ = "Cannot read Last.fm state file";
            }
        }
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { flush(); });
        timer->start(1000);
    }
    QJsonObject status() const {
        return {{"connected", !state_.value("session").toString().isEmpty()},
                {"user", state_.value("user")},
                {"enabled", state_.value("enabled").toBool()},
                {"pending", state_.value("pending").toArray().size()},
                {"message", message_}};
    }
    bool save() {
        if (blocked_)
            return false;
        state_["version"] = 1;
        if (!QDir{}.mkpath(QFileInfo(path_).absolutePath()))
            return false;
        QSaveFile file(path_);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const auto data = QJsonDocument(state_).toJson(QJsonDocument::Compact);
        return file.write(data) == data.size() && file.commit();
    }
    void finish(const QString& op, QJsonObject extra = {}, const QString& error = {}) {
        auto result = status();
        for (auto it = extra.begin(); it != extra.end(); ++it)
            result[it.key()] = it.value();
        done_(op, result, error);
    }
    using Callback = std::function<void(QJsonObject, int, bool)>;
    void call(const QString& method, QMap<QString, QString> params, Callback callback) {
        busy_ = true;
        params["method"] = method;
        params["api_key"] = state_.value("key").toString();
        const auto read_only = method == "track.getInfo";
        if (!read_only) {
            if (!method.startsWith("auth."))
                params["sk"] = state_.value("session").toString();
            params["api_sig"] =
                QString::fromLatin1(lastFmSignature(params, state_.value("secret").toString()));
        }
        params["format"] = "json";
        QByteArray body;
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (!body.isEmpty())
                body += '&';
            body += QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(it.value());
        }
        QNetworkRequest request(endpoint_);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        request.setTransferTimeout(8000);
        if (read_only) {
            auto url = endpoint_;
            url.setQuery(QString::fromLatin1(body));
            request.setUrl(url);
        }
        auto* reply = read_only ? network_->get(request) : network_->post(request, body);
        connect(reply, &QNetworkReply::readyRead, this, [reply] {
            if (reply->bytesAvailable() > 1024 * 1024)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, callback = std::move(callback)] {
                    QJsonParseError parse_error;
                    const auto document = QJsonDocument::fromJson(reply->readAll(), &parse_error);
                    const auto object = document.object();
                    int code = object.value("error").toInt();
                    const bool transport = reply->error() != QNetworkReply::NoError;
                    const int http_status =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (code == 0 && http_status >= 400)
                        code = http_status;
                    if (code == 0 && (transport || parse_error.error != QJsonParseError::NoError ||
                                      !document.isObject()))
                        code = -1;
                    reply->deleteLater();
                    busy_ = false;
                    callback(object, code,
                             code == -1 || code == 11 || code == 16 || code == 29 || code == 429 ||
                                 code >= 500);
                    if (!commands_.empty()) {
                        auto command = std::move(commands_.front());
                        commands_.pop_front();
                        execute(command.first, command.second);
                    }
                });
    }
    void execute(const QString& op, const QStringList& args) {
        if (op == "status") {
            finish(op);
            return;
        }
        if (blocked_) {
            finish(op, {}, message_);
            return;
        }
        if (busy_) {
            if (commands_.size() < 32)
                commands_.emplace_back(op, args);
            else
                finish(op, {}, "Last.fm is busy");
            return;
        }
        auto fail = [this, op](const QString& error) { finish(op, {}, error); };
        if (op == "disconnect") {
            state_ = {};
            token_.clear();
            listen_.reset();
            now_playing_ = {};
            message_ = "Disconnected; pending scrobbles cleared";
            finish(op, {}, save() ? QString{} : QStringLiteral("Could not save Last.fm state"));
            return;
        }
        if (op == "enable") {
            if (args.size() != 1 || (args[0] != "0" && args[0] != "1")) {
                fail("Invalid enable value");
                return;
            }
            if (args[0] == "1" && state_.value("session").toString().isEmpty()) {
                fail("Connect Last.fm first");
                return;
            }
            state_["enabled"] = args[0] == "1";
            listen_.reset();
            now_playing_ = {};
            finish(op, {}, save() ? QString{} : QStringLiteral("Could not save Last.fm state"));
            return;
        }
        QString method;
        QMap<QString, QString> params;
        if (op == "begin") {
            if (args.size() != 2 || args[0].size() != 32 || args[1].size() != 32) {
                fail("Provide the 32-character API key and shared secret");
                return;
            }
            if (!state_.value("session").toString().isEmpty() &&
                (state_.value("key") != args[0] || state_.value("secret") != args[1])) {
                fail("Disconnect before changing API credentials");
                return;
            }
            state_["key"] = args[0];
            state_["secret"] = args[1];
            method = "auth.getToken";
        } else if (op == "finish") {
            if (token_.isEmpty()) {
                fail("Start authorization first");
                return;
            }
            method = "auth.getSession";
            params["token"] = token_;
        } else if (op == "love" || op == "unlove" || op == "info") {
            if (state_.value("session").toString().isEmpty()) {
                fail("Connect Last.fm first");
                return;
            }
            if (args.size() != 2 || args[0].isEmpty() || args[1].isEmpty()) {
                fail("Artist and title are required");
                return;
            }
            method = "track." + op;
            params["artist"] = args[0];
            params["track"] = args[1];
            if (op == "info") {
                method = "track.getInfo";
                params["username"] = state_.value("user").toString();
                params["autocorrect"] = "0";
            }
        } else {
            fail("Unknown Last.fm operation");
            return;
        }
        call(method, params, [this, op, args](QJsonObject response, int code, bool) {
            if (code != 0) {
                finish(op, {}, QStringLiteral("Last.fm request failed (code %1)").arg(code));
                return;
            }
            QJsonObject extra;
            if (op == "begin") {
                token_ = response.value("token").toString();
                if (token_.isEmpty()) {
                    finish(op, {}, "Last.fm returned no token");
                    return;
                }
                QUrl url("https://www.last.fm/api/auth/");
                QUrlQuery query;
                query.addQueryItem("api_key", state_.value("key").toString());
                query.addQueryItem("token", token_);
                url.setQuery(query);
                extra["url"] = url.toString();
            } else if (op == "finish") {
                auto session = response.value("session").toObject();
                if (session.value("key").toString().isEmpty()) {
                    finish(op, {}, "Last.fm returned no session");
                    return;
                }
                if (state_.value("user") != session.value("name"))
                    state_["pending"] = QJsonArray{};
                if (!state_.value("user").toString().isEmpty() &&
                    state_.value("user") != session.value("name"))
                    state_["enabled"] = false;
                listen_.reset();
                started_ = 0;
                now_playing_ = {};
                state_["session"] = session.value("key");
                state_["user"] = session.value("name");
                token_.clear();
                retry_ = 0;
                message_ = "Connected";
                auth_failed_ = false;
            } else {
                extra["artist"] = args[0];
                extra["title"] = args[1];
                if (op == "info") {
                    auto track = response.value("track").toObject();
                    if (!track.contains("userloved")) {
                        finish(op, {}, "Last.fm returned no loved state");
                        return;
                    }
                    extra["loved"] = track.value("userloved").toString() == "1";
                } else {
                    extra["loved"] = op == "love";
                    message_ = "Last.fm updated";
                }
            }
            finish(op, extra,
                   op == "info" || save() ? QString{}
                                          : QStringLiteral("Could not save Last.fm state"));
        });
    }
    void observe(const QJsonObject& sample) {
        if (blocked_ || !state_.value("enabled").toBool() ||
            state_.value("session").toString().isEmpty()) {
            listen_.reset();
            return;
        }
        const bool valid = !sample.value("artist").toString().isEmpty() &&
                           !sample.value("title").toString().isEmpty();
        const bool eligible = listen_.observe(
            valid ? sample.value("identity").toString().toStdString() : std::string{},
            sample.value("duration").toDouble(), sample.value("position").toDouble(),
            sample.value("playing").toBool(), sample.value("monotonic").toInteger());
        if (listen_.changed()) {
            started_ = 0;
            now_playing_ = {};
        }
        if (started_ == 0 && valid && sample.value("playing").toBool()) {
            started_ = QDateTime::currentSecsSinceEpoch();
            now_playing_ = sample;
        }
        if (eligible) {
            auto pending = state_.value("pending").toArray();
            if (pending.size() >= 1000) {
                message_ = "Scrobble outbox is full";
                return;
            }
            auto item = sample;
            item["timestamp"] = started_;
            pending.append(item);
            state_["pending"] = pending;
            durable_ = save();
            if (!durable_)
                message_ = "Could not save pending scrobble";
        }
    }
    void flush() {
        if (busy_ || blocked_ || auth_failed_ || !state_.value("enabled").toBool() ||
            state_.value("session").toString().isEmpty() || clock_.elapsed() < retry_)
            return;
        if (!durable_) {
            durable_ = save();
            if (!durable_)
                return;
        }
        auto pending = state_.value("pending").toArray();
        const bool now = !now_playing_.isEmpty();
        if (!now && pending.isEmpty())
            return;
        const auto item =
            now ? std::exchange(now_playing_, QJsonObject{}) : pending.first().toObject();
        QMap<QString, QString> params{
            {"artist", item.value("artist").toString()},
            {"track", item.value("title").toString()},
            {"album", item.value("album").toString()},
            {"duration", QString::number(static_cast<int>(item.value("duration").toDouble()))}};
        if (!now)
            params["timestamp"] = QString::number(item.value("timestamp").toInteger());
        call(now ? "track.updateNowPlaying" : "track.scrobble", params,
             [this, now](QJsonObject response, int code, bool retry) {
                 if (now)
                     return;
                 if (code == 9) {
                     auth_failed_ = true;
                     message_ = "Last.fm session expired; reconnect";
                     return;
                 }
                 if (retry) {
                     failures_ = std::min(failures_ + 1, 8);
                     retry_ = clock_.elapsed() + (1 << failures_) * 1000;
                     message_ = "Last.fm unavailable; scrobble queued";
                     return;
                 }
                 const auto attributes =
                     response.value("scrobbles").toObject().value("@attr").toObject();
                 const auto count = [&](const QString& key) {
                     const auto value = attributes.value(key);
                     return value.isString() ? value.toString().toInt() : value.toInt();
                 };
                 if (code == 0 && count("accepted") + count("ignored") != 1) {
                     retry_ = clock_.elapsed() + 60000;
                     message_ = "Invalid scrobble acknowledgement";
                     return;
                 }
                 auto queue = state_.value("pending").toArray();
                 if (!queue.isEmpty())
                     queue.removeFirst();
                 state_["pending"] = queue;
                 failures_ = 0;
                 message_ = code == 0 ? QStringLiteral("Scrobble submitted")
                                      : QStringLiteral("Scrobble rejected (code %1)").arg(code);
                 if (count("ignored") > 0)
                     message_ = "Last.fm ignored the scrobble (metadata or timestamp)";
                 if (!save())
                     message_ = "Could not save scrobble acknowledgement";
             });
    }

  private:
    QString path_, token_, message_;
    QJsonObject state_, now_playing_;
    std::function<void(QString, QJsonObject, QString)> done_;
    QUrl endpoint_;
    QNetworkAccessManager* network_{};
    QElapsedTimer clock_;
    LastFmListen listen_;
    qint64 started_{}, retry_{};
    int failures_{};
    bool busy_{}, blocked_{}, auth_failed_{};
    bool durable_{true};
    std::deque<std::pair<QString, QStringList>> commands_;
};
} // namespace
LastFmService::LastFmService(QString path, QObject* parent, QUrl endpoint) : QObject(parent) {
    auto* worker = new Worker(
        std::move(path), std::move(endpoint), [this](QString op, QJsonObject state, QString error) {
            QMetaObject::invokeMethod(
                this,
                [this, op = std::move(op), state = std::move(state), error = std::move(error)] {
                    emit completed(op, state, error);
                },
                Qt::QueuedConnection);
        });
    worker_ = worker;
    worker->moveToThread(&thread_);
    connect(&thread_, &QThread::started, worker, [worker] { worker->init(); });
    connect(&thread_, &QThread::finished, worker, &QObject::deleteLater);
    thread_.start();
}
LastFmService::~LastFmService() {
    thread_.quit();
    thread_.wait();
}
void LastFmService::execute(const QString& op, const QStringList& args) {
    auto* worker = static_cast<Worker*>(worker_);
    QMetaObject::invokeMethod(
        worker, [worker, op, args] { worker->execute(op, args); }, Qt::QueuedConnection);
}
void LastFmService::observe(QJsonObject sample) {
    auto* worker = static_cast<Worker*>(worker_);
    QMetaObject::invokeMethod(
        worker, [worker, sample = std::move(sample)] { worker->observe(sample); },
        Qt::QueuedConnection);
}
} // namespace trackknife::bench
