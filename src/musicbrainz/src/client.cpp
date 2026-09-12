// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/client.hpp"

#include <QDateTime>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <utility>

namespace trackknife::musicbrainz {
namespace {

[[nodiscard]] core::Error client_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

} // namespace

MusicBrainzClient::MusicBrainzClient(WebTransport transport, ResponseCacheHooks cache,
                                     const int minimum_interval_ms, QObject* parent)
    : QObject(parent), transport_(std::move(transport)), cache_(std::move(cache)),
      minimum_interval_ms_(std::max(minimum_interval_ms, 0)) {}

std::size_t MusicBrainzClient::pending_request_count() const noexcept {
    return pending_.size() + (in_flight_ ? 1U : 0U);
}

void MusicBrainzClient::fetch(const QString& url, Completion completion) {
    if (!completion) {
        return;
    }
    if (!transport_) {
        QTimer::singleShot(0, this, [completion = std::move(completion)] {
            completion(std::unexpected(
                client_error(core::ErrorCode::unsupported,
                             "the MusicBrainz client has no transport configured")));
        });
        return;
    }
    if (cache_.load) {
        if (auto cached = cache_.load(url)) {
            QTimer::singleShot(0, this,
                               [completion = std::move(completion), body = std::move(*cached)] {
                                   completion(body);
                               });
            return;
        }
    }
    pending_.push_back(Pending{.url = url, .completion = std::move(completion)});
    scheduleDispatch();
}

void MusicBrainzClient::scheduleDispatch() {
    if (dispatch_scheduled_ || in_flight_ || pending_.empty()) {
        return;
    }
    const auto elapsed = dispatched_once_ ? since_last_dispatch_.elapsed() : minimum_interval_ms_;
    const auto cooldown =
        since_throttle_.isValid() ? throttle_delay_ms_ - since_throttle_.elapsed() : 0;
    const auto wait = std::max<qint64>({0, minimum_interval_ms_ - elapsed, cooldown});
    dispatch_scheduled_ = true;
    QTimer::singleShot(static_cast<int>(wait), Qt::PreciseTimer, this,
                       &MusicBrainzClient::dispatchNext);
}

void MusicBrainzClient::dispatchNext() {
    dispatch_scheduled_ = false;
    if (in_flight_ || pending_.empty()) {
        return;
    }
    auto request = std::move(pending_.front());
    pending_.pop_front();
    in_flight_ = true;
    dispatched_once_ = true;
    since_last_dispatch_.restart();
    QPointer<MusicBrainzClient> self{this};
    const auto url = QUrl{request.url};
    transport_(url, [self, request = std::move(request)](const WebResponse& response) mutable {
        if (self.isNull()) {
            return;
        }
        self->finishRequest(std::move(request), response);
    });
}

void MusicBrainzClient::finishRequest(Pending request, const WebResponse& response) {
    in_flight_ = false;
    if (response.transport_error.isEmpty() &&
        (response.status_code == 429 || response.status_code == 503)) {
        throttle_delay_ms_ =
            std::max(response.retry_after_ms.value_or(0), 1'000 * (1 << request.retries));
        since_throttle_.restart();
        if (request.retries < 2 && throttle_delay_ms_ <= 60'000) {
            ++request.retries;
            pending_.push_front(std::move(request));
            scheduleDispatch();
            return;
        }
        scheduleDispatch();
        request.completion(std::unexpected(client_error(
            core::ErrorCode::backend,
            "MusicBrainz is busy or rate-limiting requests; retry after " +
                std::to_string((static_cast<qint64>(throttle_delay_ms_) + 999) / 1'000) +
                " seconds")));
        return;
    }
    scheduleDispatch();
    auto completion = std::move(request.completion);
    if (!response.transport_error.isEmpty()) {
        completion(std::unexpected(
            client_error(core::ErrorCode::io,
                         "MusicBrainz is unreachable: " + response.transport_error.toStdString())));
        return;
    }
    if (response.status_code == 404) {
        completion(std::unexpected(
            client_error(core::ErrorCode::not_found, "MusicBrainz has no such entity")));
        return;
    }
    if (response.status_code != 200 || response.body.isEmpty()) {
        completion(std::unexpected(client_error(
            core::ErrorCode::backend, "MusicBrainz returned an unexpected response (status " +
                                          std::to_string(response.status_code) + ")")));
        return;
    }
    if (cache_.store) {
        cache_.store(request.url, response.body);
    }
    completion(response.body);
}

WebTransport MusicBrainzClient::qtNetworkTransport(QNetworkAccessManager* manager,
                                                   const QString& user_agent) {
    QPointer<QNetworkAccessManager> guarded{manager};
    return [guarded, user_agent](const QUrl& url, std::function<void(WebResponse)> completion) {
        if (guarded.isNull()) {
            completion(WebResponse{.status_code = 0,
                                   .body = {},
                                   .transport_error = QStringLiteral("network manager is gone")});
            return;
        }
        QNetworkRequest request{url};
        request.setTransferTimeout(30'000);
        request.setHeader(QNetworkRequest::UserAgentHeader, user_agent);
        // Cover Art Archive image URLs redirect cross-origin to the
        // Internet Archive; refuse only scheme downgrades.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = guarded->get(request);
        QObject::connect(
            reply, &QNetworkReply::finished, reply, [reply, completion = std::move(completion)] {
                const auto status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                WebResponse response{
                    .status_code = status,
                    .body = reply->readAll(),
                    .transport_error =
                        reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString(),
                };
                const auto retry_after = reply->rawHeader("Retry-After").trimmed();
                if (!retry_after.isEmpty()) {
                    bool numeric = false;
                    const auto seconds = retry_after.toLongLong(&numeric);
                    auto date =
                        QDateTime::fromString(QString::fromLatin1(retry_after), Qt::RFC2822Date);
                    if (!date.isValid()) {
                        const auto parsed = QLocale::c().toDateTime(
                            QString::fromLatin1(retry_after),
                            QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
                        date =
                            QDateTime{parsed.date(), parsed.time(), QTimeZone{QByteArray{"UTC"}}};
                    }
                    const auto max_seconds = std::numeric_limits<int>::max() / 1'000;
                    if (numeric && seconds >= 0) {
                        response.retry_after_ms =
                            static_cast<int>(std::min<qint64>(seconds, max_seconds) * 1'000);
                    } else if (date.isValid()) {
                        response.retry_after_ms = static_cast<int>(
                            std::clamp<qint64>(QDateTime::currentDateTimeUtc().msecsTo(date), 0,
                                               std::numeric_limits<int>::max()));
                    }
                }
                // HTTP-level failures carry a status; keep them
                // out of the transport-error channel so the
                // client maps them precisely.
                if (status != 0 && reply->error() != QNetworkReply::NoError &&
                    reply->error() != QNetworkReply::OperationCanceledError) {
                    response.transport_error.clear();
                }
                reply->deleteLater();
                completion(response);
            });
    };
}

} // namespace trackknife::musicbrainz
