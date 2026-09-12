// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QUrl>

#include <deque>
#include <functional>
#include <optional>

class QNetworkAccessManager;

namespace trackknife::musicbrainz {

struct WebResponse {
    int status_code{0};
    QByteArray body;
    QString transport_error;
    std::optional<int> retry_after_ms{};
};

using WebTransport = std::function<void(const QUrl&, std::function<void(WebResponse)>)>;

// Optional response cache; both hooks run on the client's thread. A load hit
// answers without touching the network or the pacing budget.
struct ResponseCacheHooks {
    std::function<std::optional<QByteArray>(const QString& url)> load;
    std::function<void(const QString& url, const QByteArray& body)> store;
};

// Serialized, paced MusicBrainz fetcher: one request in flight, at least
// minimum_request_interval_ms between dispatches, cache consulted first, and
// every outcome delivered asynchronously as a typed result. Network use is
// always explicit — nothing fetches unless asked.
class MusicBrainzClient final : public QObject {
    Q_OBJECT

  public:
    using Completion = std::function<void(core::Result<QByteArray>)>;

    explicit MusicBrainzClient(WebTransport transport, ResponseCacheHooks cache = {},
                               int minimum_interval_ms = minimum_request_interval_ms,
                               QObject* parent = nullptr);

    void fetch(const QString& url, Completion completion);
    [[nodiscard]] std::size_t pending_request_count() const noexcept;

    // The production transport: sets the identifying User-Agent MusicBrainz
    // requires and follows redirects without allowing an HTTPS downgrade.
    [[nodiscard]] static WebTransport qtNetworkTransport(QNetworkAccessManager* manager,
                                                         const QString& user_agent);

  private:
    struct Pending {
        QString url;
        Completion completion;
        int retries{0};
    };

    void scheduleDispatch();
    void dispatchNext();
    void finishRequest(Pending request, const WebResponse& response);

    WebTransport transport_;
    ResponseCacheHooks cache_;
    int minimum_interval_ms_{minimum_request_interval_ms};
    std::deque<Pending> pending_;
    QElapsedTimer since_last_dispatch_;
    QElapsedTimer since_throttle_;
    int throttle_delay_ms_{0};
    bool dispatched_once_{false};
    bool in_flight_{false};
    bool dispatch_scheduled_{false};
};

} // namespace trackknife::musicbrainz
