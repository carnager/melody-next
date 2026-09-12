// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/client.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrl>

#include <functional>
#include <optional>
#include <vector>

namespace trackknife::musicbrainz {

class MusicBrainzClientTest final : public QObject {
    Q_OBJECT

  private slots:
    void cacheHitAnswersWithoutTransport();
    void requestsAreSerializedAndPaced();
    void failureStatesAreTyped();
    void successStoresIntoTheCache();
    void throttlingRetriesRespectCooldownAndQueueOrder();
    void throttlingRetriesAreBounded();
    void longRetryAfterReturnsActionableFailure();
    void transportParsesRetryAfter_data();
    void transportParsesRetryAfter();
};

void MusicBrainzClientTest::cacheHitAnswersWithoutTransport() {
    int transport_calls = 0;
    MusicBrainzClient client{
        [&transport_calls](const QUrl&, std::function<void(WebResponse)> completion) {
            ++transport_calls;
            completion(WebResponse{.status_code = 200, .body = "live", .transport_error = {}});
        },
        ResponseCacheHooks{
            .load = [](const QString&) { return std::optional<QByteArray>{"cached"}; },
            .store = {},
        },
        0};
    std::optional<core::Result<QByteArray>> delivered;
    client.fetch(QStringLiteral("https://musicbrainz.org/ws/2/release/?query=x"),
                 [&delivered](core::Result<QByteArray> result) { delivered = std::move(result); });
    QTRY_VERIFY(delivered.has_value());
    QVERIFY(delivered->has_value());
    QCOMPARE(**delivered, QByteArray{"cached"});
    QCOMPARE(transport_calls, 0);
}

void MusicBrainzClientTest::requestsAreSerializedAndPaced() {
    struct Dispatch {
        qint64 at_ms{0};
        std::function<void(WebResponse)> completion;
    };
    QElapsedTimer clock;
    clock.start();
    std::vector<Dispatch> dispatches;
    MusicBrainzClient client{
        [&clock, &dispatches](const QUrl&, std::function<void(WebResponse)> completion) {
            dispatches.push_back(
                Dispatch{.at_ms = clock.elapsed(), .completion = std::move(completion)});
        },
        {},
        120};
    int completed = 0;
    const auto completion = [&completed](core::Result<QByteArray> result) {
        QVERIFY(result.has_value());
        ++completed;
    };
    client.fetch(QStringLiteral("https://musicbrainz.org/one"), completion);
    client.fetch(QStringLiteral("https://musicbrainz.org/two"), completion);
    QCOMPARE(client.pending_request_count(), std::size_t{2U});

    // Only one request is in flight; the second waits for the first response
    // AND the pacing interval.
    QTRY_COMPARE(dispatches.size(), std::size_t{1U});
    QTest::qWait(200);
    QCOMPARE(dispatches.size(), std::size_t{1U});
    dispatches.front().completion(
        WebResponse{.status_code = 200, .body = "one", .transport_error = {}});
    QTRY_COMPARE(dispatches.size(), std::size_t{2U});
    QVERIFY(dispatches[1].at_ms - dispatches[0].at_ms >= 120);
    dispatches.back().completion(
        WebResponse{.status_code = 200, .body = "two", .transport_error = {}});
    QTRY_COMPARE(completed, 2);
    QCOMPARE(client.pending_request_count(), std::size_t{0U});
}

void MusicBrainzClientTest::failureStatesAreTyped() {
    WebResponse scripted;
    MusicBrainzClient client{[&scripted](const QUrl&, std::function<void(WebResponse)> completion) {
                                 completion(scripted);
                             },
                             {},
                             0};
    const auto fetch_error = [&client](const QString& url) {
        std::optional<core::Result<QByteArray>> delivered;
        client.fetch(
            url, [&delivered](core::Result<QByteArray> result) { delivered = std::move(result); });
        [&] { QTRY_VERIFY(delivered.has_value()); }();
        return delivered->has_value() ? std::optional<core::Error>{}
                                      : std::optional{delivered->error()};
    };

    scripted = WebResponse{
        .status_code = 0, .body = {}, .transport_error = QStringLiteral("no route to host")};
    auto offline = fetch_error(QStringLiteral("https://musicbrainz.org/a"));
    QVERIFY(offline && offline->code == core::ErrorCode::io);

    scripted = WebResponse{.status_code = 503, .body = "busy", .transport_error = {}};
    auto throttled = fetch_error(QStringLiteral("https://musicbrainz.org/b"));
    QVERIFY(throttled && throttled->code == core::ErrorCode::backend);

    scripted = WebResponse{.status_code = 404, .body = {}, .transport_error = {}};
    auto missing = fetch_error(QStringLiteral("https://musicbrainz.org/c"));
    QVERIFY(missing && missing->code == core::ErrorCode::not_found);

    scripted = WebResponse{.status_code = 500, .body = "oops", .transport_error = {}};
    auto server = fetch_error(QStringLiteral("https://musicbrainz.org/d"));
    QVERIFY(server && server->code == core::ErrorCode::backend);
}

void MusicBrainzClientTest::throttlingRetriesRespectCooldownAndQueueOrder() {
    QElapsedTimer clock;
    clock.start();
    std::vector<qint64> times;
    QStringList urls;
    int completions = 0;
    int cached = 0;
    MusicBrainzClient client{
        [&](const QUrl& url, std::function<void(WebResponse)> complete) {
            times.push_back(clock.elapsed());
            urls.push_back(url.path());
            complete(times.size() == 1U
                         ? WebResponse{.status_code = 429,
                                       .body = "busy",
                                       .transport_error = {},
                                       .retry_after_ms = 1'500}
                         : WebResponse{.status_code = 200, .body = "ok", .transport_error = {}});
        },
        ResponseCacheHooks{.load = {},
                           .store = [&](const QString&, const QByteArray&) { ++cached; }},
        0};
    const auto done = [&](core::Result<QByteArray> result) {
        QVERIFY(result);
        ++completions;
    };
    client.fetch(QStringLiteral("https://musicbrainz.org/first"), done);
    client.fetch(QStringLiteral("https://musicbrainz.org/second"), done);
    QTRY_COMPARE(completions, 2);
    QCOMPARE(urls, (QStringList{QStringLiteral("/first"), QStringLiteral("/first"),
                                QStringLiteral("/second")}));
    QVERIFY(times[1] - times[0] >= 1'500);
    QCOMPARE(cached, 2);
    QCOMPARE(client.pending_request_count(), 0U);
}

void MusicBrainzClientTest::throttlingRetriesAreBounded() {
    int calls = 0;
    int completed = 0;
    MusicBrainzClient client{
        [&](const QUrl&, std::function<void(WebResponse)> complete) {
            ++calls;
            complete(WebResponse{.status_code = 503, .body = "busy", .transport_error = {}});
        },
        {},
        0};
    client.fetch(QStringLiteral("https://musicbrainz.org/busy"),
                 [&](core::Result<QByteArray> result) {
                     QVERIFY(!result);
                     QVERIFY(result.error().message.find("rate-limiting") != std::string::npos);
                     ++completed;
                 });
    QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 6'000);
    QCOMPARE(calls, 3);
    QCOMPARE(client.pending_request_count(), 0U);
}

void MusicBrainzClientTest::longRetryAfterReturnsActionableFailure() {
    int calls = 0;
    std::optional<core::Result<QByteArray>> result;
    MusicBrainzClient client{
        [&](const QUrl&, std::function<void(WebResponse)> complete) {
            ++calls;
            complete(WebResponse{
                .status_code = 429, .body = {}, .transport_error = {}, .retry_after_ms = 70'000});
        },
        {},
        0};
    client.fetch(QStringLiteral("https://musicbrainz.org/busy"),
                 [&](core::Result<QByteArray> value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QVERIFY(!result->has_value());
    QVERIFY(result->error().message.find("70 seconds") != std::string::npos);
    QCOMPARE(calls, 1);
}

void MusicBrainzClientTest::transportParsesRetryAfter_data() {
    QTest::addColumn<bool>("http_date");
    QTest::newRow("seconds") << false;
    QTest::newRow("http-date") << true;
}

void MusicBrainzClientTest::transportParsesRetryAfter() {
    QFETCH(bool, http_date);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QByteArray request;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto* socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
            request += socket->readAll();
            if (!request.contains("\r\n\r\n")) {
                return;
            }
            const auto delay =
                http_date ? QLocale::c()
                                .toString(QDateTime::currentDateTimeUtc().addSecs(30),
                                          QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"))
                                .toLatin1()
                          : QByteArray{"30"};
            socket->write("HTTP/1.1 429 Too Many Requests\r\nRetry-After: " + delay +
                          "\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbusy");
            socket->disconnectFromHost();
        });
    });
    QNetworkAccessManager manager;
    const auto transport =
        MusicBrainzClient::qtNetworkTransport(&manager, QStringLiteral("Trackknife-test/1.0"));
    std::optional<WebResponse> response;
    transport(QUrl{QStringLiteral("http://127.0.0.1:%1/test").arg(server.serverPort())},
              [&](WebResponse result) { response = std::move(result); });
    QTRY_VERIFY(response.has_value());
    QCOMPARE(response->status_code, 429);
    QVERIFY(response->transport_error.isEmpty());
    QCOMPARE(response->body, QByteArray{"busy"});
    QVERIFY(response->retry_after_ms.has_value());
    QVERIFY(*response->retry_after_ms <= 30'000 && *response->retry_after_ms >= 28'000);
    QVERIFY(request.contains("User-Agent: Trackknife-test/1.0"));
}

void MusicBrainzClientTest::successStoresIntoTheCache() {
    QString stored_url;
    QByteArray stored_body;
    MusicBrainzClient client{
        [](const QUrl&, std::function<void(WebResponse)> completion) {
            completion(WebResponse{.status_code = 200, .body = "payload", .transport_error = {}});
        },
        ResponseCacheHooks{
            .load = [](const QString&) { return std::optional<QByteArray>{}; },
            .store =
                [&stored_url, &stored_body](const QString& url, const QByteArray& body) {
                    stored_url = url;
                    stored_body = body;
                },
        },
        0};
    std::optional<core::Result<QByteArray>> delivered;
    client.fetch(QStringLiteral("https://musicbrainz.org/ws/2/release/x"),
                 [&delivered](core::Result<QByteArray> result) { delivered = std::move(result); });
    QTRY_VERIFY(delivered.has_value());
    QVERIFY(delivered->has_value());
    QCOMPARE(stored_url, QStringLiteral("https://musicbrainz.org/ws/2/release/x"));
    QCOMPARE(stored_body, QByteArray{"payload"});
}

} // namespace trackknife::musicbrainz

QTEST_GUILESS_MAIN(trackknife::musicbrainz::MusicBrainzClientTest)
#include "musicbrainz_client_test.moc"
