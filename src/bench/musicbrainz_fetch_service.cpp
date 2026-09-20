// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_fetch_service.hpp"

#include "trackknife/musicbrainz/acoustid.hpp"
#include "trackknife/musicbrainz/web_service.hpp"
#include "trackknife/persistence/musicbrainz_cache.hpp"

#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <cstddef>
#include <ctime>
#include <string>
#include <utility>

namespace trackknife::bench {
namespace {

// MusicBrainz requires an identifying User-Agent naming the application.
const auto user_agent =
    QStringLiteral("Trackknife/0.0.1 ( https://github.com/carnager/trackknife )");

[[nodiscard]] std::optional<QByteArray> load_cached(const std::filesystem::path& database_path,
                                                    const std::string& url) {
    auto cache = persistence::SqliteMusicBrainzResponseCache::open(database_path);
    if (!cache) {
        return std::nullopt;
    }
    const auto loaded = cache->load(url, static_cast<std::int64_t>(std::time(nullptr)),
                                    musicbrainz::response_cache_ttl_seconds);
    if (!loaded || !*loaded) {
        return std::nullopt;
    }
    return QByteArray{reinterpret_cast<const char*>((*loaded)->data()),
                      static_cast<qsizetype>((*loaded)->size())};
}

void store_cached(const std::filesystem::path& database_path, const std::string& url,
                  const QByteArray& body) {
    auto cache = persistence::SqliteMusicBrainzResponseCache::open(database_path);
    if (!cache) {
        return;
    }
    static_cast<void>(cache->store(
        url, std::string_view{body.constData(), static_cast<std::size_t>(body.size())},
        static_cast<std::int64_t>(std::time(nullptr)), musicbrainz::response_cache_ttl_seconds,
        musicbrainz::response_cache_maximum_entries));
}

} // namespace

MusicBrainzFetchService::MusicBrainzFetchService(std::filesystem::path database_path,
                                                 QObject* parent)
    : QObject(parent), database_path_(std::move(database_path)),
      network_(new QNetworkAccessManager(this)),
      client_(new musicbrainz::MusicBrainzClient(
          musicbrainz::MusicBrainzClient::qtNetworkTransport(network_, user_agent), {},
          musicbrainz::minimum_request_interval_ms, this)) {}

MusicBrainzFetchService::~MusicBrainzFetchService() = default;

MusicBrainzLookupService MusicBrainzFetchService::lookupService() {
    QPointer<MusicBrainzFetchService> self{this};
    const auto gone = [] {
        return core::Error{
            .code = core::ErrorCode::cancelled,
            .message = "MusicBrainz lookups are shutting down",
            .context = {},
        };
    };
    return MusicBrainzLookupService{
        .fetch =
            [self, gone](const QString& url,
                         std::function<void(core::Result<QByteArray>)> completion) {
                if (self.isNull()) {
                    completion(std::unexpected(gone()));
                    return;
                }
                self->fetch(url, std::move(completion));
            },
        .fingerprint =
            [self, gone](const QString& file_path,
                         std::function<void(core::Result<AcoustIdFingerprint>)> completion) {
                if (self.isNull()) {
                    completion(std::unexpected(gone()));
                    return;
                }
                self->fingerprintFile(file_path, std::move(completion));
            },
        .acoustid_lookup =
            [self, gone](const AcoustIdFingerprint& fingerprint,
                         std::function<void(core::Result<QByteArray>)> completion) {
                if (self.isNull()) {
                    completion(std::unexpected(gone()));
                    return;
                }
                self->acoustidLookup(fingerprint, std::move(completion));
            },
    };
}

// One fpcalc invocation per file, fully asynchronous. fpcalc is the same
// external fingerprinter Picard ships; a missing binary fails typed.
void MusicBrainzFetchService::fingerprintFile(
    const QString& file_path, std::function<void(core::Result<AcoustIdFingerprint>)> completion) {
    auto* process = new QProcess(this);
    process->setProgram(QStringLiteral("fpcalc"));
    process->setArguments({QStringLiteral("-json"), file_path});
    auto shared_completion =
        std::make_shared<std::function<void(core::Result<AcoustIdFingerprint>)>>(
            std::move(completion));
    connect(process, &QProcess::errorOccurred, this,
            [process, shared_completion](const QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) {
                    return;
                }
                process->deleteLater();
                (*shared_completion)(std::unexpected(core::Error{
                    .code = core::ErrorCode::unsupported,
                    .message = "fpcalc (chromaprint) is not installed",
                    .context = {},
                }));
            });
    connect(
        process, &QProcess::finished, this,
        [process, shared_completion](const int exit_code, const QProcess::ExitStatus exit_status) {
            process->deleteLater();
            if (exit_status != QProcess::NormalExit || exit_code != 0) {
                (*shared_completion)(std::unexpected(core::Error{
                    .code = core::ErrorCode::backend,
                    .message = "fpcalc could not fingerprint the file",
                    .context = {},
                }));
                return;
            }
            const auto document = QJsonDocument::fromJson(process->readAllStandardOutput());
            const auto root = document.object();
            AcoustIdFingerprint fingerprint{
                .duration_seconds =
                    static_cast<std::size_t>(root.value(QStringLiteral("duration")).toDouble(0.0)),
                .fingerprint = root.value(QStringLiteral("fingerprint")).toString(),
            };
            if (fingerprint.duration_seconds == 0U || fingerprint.fingerprint.isEmpty()) {
                (*shared_completion)(std::unexpected(core::Error{
                    .code = core::ErrorCode::backend,
                    .message = "fpcalc produced no usable fingerprint",
                    .context = {},
                }));
                return;
            }
            (*shared_completion)(std::move(fingerprint));
        });
    process->start();
}

void MusicBrainzFetchService::acoustidLookup(
    const AcoustIdFingerprint& fingerprint,
    std::function<void(core::Result<QByteArray>)> completion) {
    const QSettings settings;
    const auto client_key =
        settings.value(QStringLiteral("musicbrainz/acoustid-client-key")).toString();
    if (client_key.isEmpty()) {
        completion(std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "Set an AcoustID client key in Settings → Metadata services before "
                       "identifying by audio fingerprint",
            .context = {},
        }));
        return;
    }
    auto body = musicbrainz::build_acoustid_lookup_body(client_key.toStdString(),
                                                        fingerprint.duration_seconds,
                                                        fingerprint.fingerprint.toStdString());
    if (!body) {
        completion(std::unexpected(std::move(body.error())));
        return;
    }

    // Honest pacing below AcoustID's three requests per second.
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto earliest =
        last_acoustid_dispatch_ms_ + musicbrainz::acoustid_minimum_request_interval_ms;
    const auto delay_ms = std::max<qint64>(0, earliest - now);
    last_acoustid_dispatch_ms_ = now + delay_ms;
    QPointer<MusicBrainzFetchService> self{this};
    QTimer::singleShot(
        static_cast<int>(delay_ms), this,
        [self, body = std::move(*body), completion = std::move(completion)]() mutable {
            if (self.isNull()) {
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "MusicBrainz lookups are shutting down",
                    .context = {},
                }));
                return;
            }
            QNetworkRequest request{QUrl{QString::fromUtf8(
                musicbrainz::acoustid_lookup_url.data(),
                static_cast<qsizetype>(musicbrainz::acoustid_lookup_url.size()))}};
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/x-www-form-urlencoded"));
            request.setHeader(
                QNetworkRequest::UserAgentHeader,
                QStringLiteral("Trackknife/0.0.1 ( https://github.com/carnager/trackknife )"));
            auto* reply = self->network_->post(
                request, QByteArray{body.data(), static_cast<qsizetype>(body.size())});
            connect(reply, &QNetworkReply::finished, self,
                    [reply, completion = std::move(completion)] {
                        const auto payload = reply->readAll();
                        const auto failed = reply->error() != QNetworkReply::NoError;
                        reply->deleteLater();
                        if (failed && payload.isEmpty()) {
                            completion(std::unexpected(core::Error{
                                .code = core::ErrorCode::io,
                                .message = "the AcoustID service is unreachable",
                                .context = {},
                            }));
                            return;
                        }
                        completion(payload);
                    });
        });
}

void MusicBrainzFetchService::fetch(const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
    auto* watcher = new QFutureWatcher<std::optional<QByteArray>>(this);
    QPointer<MusicBrainzFetchService> self{this};
    connect(watcher, &QFutureWatcherBase::finished, this,
            [self, watcher, url, completion = std::move(completion)]() mutable {
                watcher->deleteLater();
                if (self.isNull()) {
                    completion(std::unexpected(core::Error{
                        .code = core::ErrorCode::cancelled,
                        .message = "MusicBrainz lookups are shutting down",
                        .context = {},
                    }));
                    return;
                }
                if (auto cached = watcher->result()) {
                    completion(std::move(*cached));
                    return;
                }
                const auto database_path = self->database_path_;
                self->client_->fetch(url, [database_path, url, completion = std::move(completion)](
                                              core::Result<QByteArray> body) {
                    if (body) {
                        // Fire-and-forget durable store off the UI thread.
                        auto stored = QtConcurrent::run(
                            [database_path, url = url.toStdString(), payload = *body] {
                                store_cached(database_path, url, payload);
                            });
                        static_cast<void>(stored);
                    }
                    completion(std::move(body));
                });
            });
    watcher->setFuture(
        QtConcurrent::run([database_path = database_path_, encoded = url.toStdString()] {
            return load_cached(database_path, encoded);
        }));
}

} // namespace trackknife::bench
