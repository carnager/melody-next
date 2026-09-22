// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 2: the workspace talking to a real engine over a real socket.
// Every layer below has been tested on its own; this is the first test where
// the Qt side and the engine side meet, which is where a threading or
// marshalling mistake would actually bite.

#include "trackknife/engine/server.hpp"
#include "uicommon/engine_connection.hpp"

#include <QSignalSpy>
#include <QtTest>

#include <filesystem>

namespace trackknife::ui {

namespace protocol = trackknife::protocol;
namespace engine = trackknife::engine;
namespace core = trackknife::core;

class EngineConnectionTest final : public QObject {
    Q_OBJECT

  private slots:
    void callsAndEventsCrossTheThreadBoundary();
    void failureIsReportedRatherThanSwallowed();
};

void EngineConnectionTest::callsAndEventsCrossTheThreadBoundary() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};

    protocol::Dispatcher dispatcher;
    dispatcher.on("echo", [](const protocol::Json& params) -> core::Result<protocol::Json> {
        return params;
    });
    dispatcher.on("fails", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found, .message = "nothing there", .context = {}});
    });

    auto server = engine::Server::listen(socket, dispatcher);
    QVERIFY(server.has_value());
    (*server)->start();

    EngineConnection connection;
    QSignalSpy connected{&connection, &EngineConnection::connected};
    QSignalSpy events{&connection, &EngineConnection::engineEvent};
    connection.connectTo(socket);
    QTRY_COMPARE(connected.count(), 1);
    QVERIFY(connection.isConnected());

    // The completion must arrive on the thread that asked, because a handler
    // is entitled to touch widgets.
    const auto caller = QThread::currentThread();
    QThread* completion_thread = nullptr;
    protocol::Json answer;
    connection.call(QStringLiteral("echo"), protocol::Json{{"value", 7}},
                    [&](core::Result<protocol::Json> outcome) {
                        completion_thread = QThread::currentThread();
                        QVERIFY(outcome.has_value());
                        answer = *outcome;
                    });
    QTRY_VERIFY(completion_thread != nullptr);
    QCOMPARE(completion_thread, caller);
    QCOMPARE(answer.at("value").get<int>(), 7);

    // An engine-side failure arrives as a failure, with its code intact, not
    // as an empty success the caller has to notice.
    std::optional<core::Error> failure;
    connection.call(QStringLiteral("fails"), protocol::Json::object(),
                    [&](core::Result<protocol::Json> outcome) {
                        if (!outcome) {
                            failure = outcome.error();
                        }
                    });
    QTRY_VERIFY(failure.has_value());
    QCOMPARE(failure->code, core::ErrorCode::not_found);

    // An unsolicited event reaches the Qt side as a signal on its own thread.
    const auto sink = (*server)->sink();
    sink(
        protocol::Event{.name = "playback.changed", .data = protocol::Json{{"status", "playing"}}});
    QTRY_COMPARE(events.count(), 1);
    QCOMPARE(events.front().at(0).toString(), QStringLiteral("playback.changed"));
    const auto payload =
        protocol::Json::parse(events.front().at(1).toByteArray().toStdString(), nullptr, false);
    QVERIFY(!payload.is_discarded());
    QCOMPARE(payload.at("status").get<std::string>(), std::string{"playing"});

    (*server)->stop();
}

void EngineConnectionTest::failureIsReportedRatherThanSwallowed() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path missing{
        (directory.path() + QStringLiteral("/absent.sock")).toStdString()};

    EngineConnection connection;
    QSignalSpy failed{&connection, &EngineConnection::failed};
    connection.connectTo(missing);
    // Connecting to nothing says so, with a reason: "it stopped working" is
    // not something a user can act on.
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(!failed.front().at(0).toString().isEmpty());
    QVERIFY(!connection.isConnected());

    // Calling while unconnected fails rather than hanging or pretending.
    std::optional<core::Error> failure;
    connection.call(QStringLiteral("echo"), protocol::Json::object(),
                    [&](core::Result<protocol::Json> outcome) {
                        if (!outcome) {
                            failure = outcome.error();
                        }
                    });
    QTRY_VERIFY(failure.has_value());
    QCOMPARE(failure->code, core::ErrorCode::io);
}

} // namespace trackknife::ui

QTEST_MAIN(trackknife::ui::EngineConnectionTest)
#include "engine_connection_test.moc"
