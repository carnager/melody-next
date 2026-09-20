// SPDX-License-Identifier: GPL-3.0-only
#include "bench/lastfm_service.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
using namespace trackknife::bench;

class LastFmServiceTest final : public QObject {
    Q_OBJECT
  private slots:
    void accounting();
    void accountAndPersistence();
};
void LastFmServiceTest::accounting() {
    LastFmListen listen;
    QVERIFY(!listen.observe("one", 40, 0, true, 0));
    for (int n = 1; n <= 10; ++n)
        QVERIFY(!listen.observe("one", 40, n, true, n * 1000));
    QVERIFY(!listen.observe("one", 40, 10, false, 11000));
    QVERIFY(!listen.observe("one", 40, 10, false, 21000));
    QVERIFY(!listen.observe("one", 40, 10, true, 22000));
    QVERIFY(!listen.observe("one", 40, 35, true, 23000));
    QVERIFY(!listen.observe("one", 40, 0, true, 24000));
    QCOMPARE(listen.listened(), 10.0);
    for (int n = 1; n < 10; ++n)
        QVERIFY(!listen.observe("one", 40, n, true, (24 + n) * 1000));
    QVERIFY(listen.observe("one", 40, 10, true, 34000));
    QVERIFY(!listen.observe("one", 40, 11, true, 35000));
    QVERIFY(!listen.observe("two", 40, 0, true, 36000));
    for (int n = 1; n < 20; ++n)
        QVERIFY(!listen.observe("two", 40, n, true, (36 + n) * 1000));
    QVERIFY(listen.observe("two", 40, 20, true, 56000));
    listen.reset();
    for (int n = 0; n <= 30; ++n)
        QVERIFY(!listen.observe("short", 30, n, true, n * 1000));
    listen.reset();
    for (int n = 0; n < 240; ++n)
        QVERIFY(!listen.observe("long", 1000, n, true, n * 1000));
    QVERIFY(listen.observe("long", 1000, 240, true, 240000));
    listen.reset();
    for (int n = 0; n < 20; ++n)
        QVERIFY(!listen.observe("coarse", 40, (n / 5) * 5, true, n * 1000));
    QVERIFY(listen.observe("coarse", 40, 20, true, 20000));
}
void LastFmServiceTest::accountAndPersistence() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("lastfm.json");
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
    QList<QUrlQuery> requests;
    bool failing = true;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (server.hasPendingConnections()) {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                auto bytes = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", bytes);
                const auto split = bytes.indexOf("\r\n\r\n");
                if (split < 0)
                    return;
                int length = 0;
                for (const auto& line : bytes.left(split).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toInt();
                if (bytes.size() < split + 4 + length || socket->property("done").toBool())
                    return;
                socket->setProperty("done", true);
                const auto request_line = bytes.left(bytes.indexOf("\r\n")).split(' ');
                const auto get = request_line[0] == "GET";
                QUrlQuery query = get ? QUrlQuery(QUrl::fromEncoded(request_line[1]))
                                      : QUrlQuery(QString::fromUtf8(bytes.mid(split + 4, length)));
                requests.append(query);
                QMap<QString, QString> params;
                for (const auto& pair : query.queryItems(QUrl::FullyDecoded))
                    params.insert(pair.first, pair.second);
                const auto method = params.value("method");
                if (method == "track.getInfo") {
                    QVERIFY(get);
                    QVERIFY(!params.contains("sk"));
                    QVERIFY(!params.contains("api_sig"));
                    QCOMPARE(params.value("username"), QStringLiteral("listener"));
                    QCOMPARE(params.value("artist"), QStringLiteral("Björk & A"));
                    QCOMPARE(params.value("track"), QStringLiteral("A + B"));
                } else {
                    QVERIFY(!get);
                    QCOMPARE(params.value("api_sig").toLatin1(),
                             lastFmSignature(params, QString(32, 'b')));
                }
                QByteArray response = "{}";
                if (method == "auth.getToken")
                    response = R"({"token":"TOKEN"})";
                if (method == "auth.getSession")
                    response = R"({"session":{"name":"listener","key":"SESSION"}})";
                if (method == "track.getInfo")
                    response = R"({"track":{"userloved":"1"}})";
                if (method == "track.scrobble")
                    response =
                        failing ? QByteArray(R"({"error":11})")
                                : QByteArray(
                                      R"({"scrobbles":{"@attr":{"accepted":"1","ignored":"0"}}})");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: "
                              "close\r\nContent-Length: " +
                              QByteArray::number(response.size()) + "\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
        }
    });
    {
        LastFmService service(path, nullptr, endpoint);
        QSignalSpy spy(&service, &LastFmService::completed);
        service.execute("begin", {QString(32, 'a'), QString(32, 'b')});
        QTRY_COMPARE(spy.size(), 1);
        QCOMPARE(spy.last()[2].toString(), QString{});
        QVERIFY(spy.last()[1].toJsonObject().value("url").toString().contains("TOKEN"));
        service.execute("finish");
        QTRY_COMPARE(spy.size(), 2);
        QCOMPARE(spy.last()[2].toString(), QString{});
        service.execute("enable", {"1"});
        QTRY_COMPARE(spy.size(), 3);
        service.execute("love", {QStringLiteral("Björk & A"), "A + B"});
        QTRY_COMPARE(spy.size(), 4);
        QCOMPARE(spy.last()[2].toString(), QString{});
        QCOMPARE(requests.last().queryItemValue("track", QUrl::FullyDecoded),
                 QStringLiteral("A + B"));
        service.execute("info", {QStringLiteral("Björk & A"), "A + B"});
        QTRY_COMPARE(spy.size(), 5);
        QVERIFY(spy.last()[1].toJsonObject().value("loved").toBool());
        const auto status = QJsonDocument(spy.last()[1].toJsonObject()).toJson();
        QVERIFY(!status.contains("SESSION"));
        QVERIFY(!status.contains(QByteArray(32, 'b')));
        for (int n = 0; n <= 20; ++n)
            service.observe({{"identity", "one"},
                             {"artist", "A"},
                             {"title", "Song"},
                             {"duration", 40},
                             {"position", n},
                             {"playing", true},
                             {"monotonic", n * 1000}});
        service.execute("status");
        QTRY_COMPARE(spy.size(), 6);
        QCOMPARE(spy.last()[1].toJsonObject().value("pending").toInt(), 1);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("pending").toArray().size(),
                 1);
        QVERIFY(!(file.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(requests.begin(), requests.end(),
                                             [](const auto& q) {
                                                 return q.queryItemValue("method") ==
                                                        "track.scrobble";
                                             }),
                                 4000);
    }
    failing = false;
    {
        LastFmService service(path, nullptr, endpoint);
        QSignalSpy spy(&service, &LastFmService::completed);
        service.execute("status");
        QTRY_COMPARE(spy.size(), 1);
        QCOMPARE(spy.last()[1].toJsonObject().value("pending").toInt(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly))
                    return false;
                return QJsonDocument::fromJson(file.readAll())
                    .object()
                    .value("pending")
                    .toArray()
                    .isEmpty();
            }(),
            4000);
        service.execute("disconnect");
        QTRY_COMPARE(spy.size(), 2);
        QVERIFY(!spy.last()[1].toJsonObject().value("connected").toBool());
    }
}
QTEST_GUILESS_MAIN(LastFmServiceTest)
#include "lastfm_service_test.moc"
