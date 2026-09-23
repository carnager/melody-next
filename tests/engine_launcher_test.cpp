// SPDX-License-Identifier: GPL-3.0-only

// ADR-0226: the engine a workspace starts for itself. Each case runs a real
// melodyd on a temporary database and socket, and stops it afterwards: the
// point of the launcher is that the engine outlives its client, so a test that
// forgot would leave one running.

#include "bench/engine_launcher.hpp"

#include <QTemporaryDir>
#include <QtTest>

#include <signal.h>

#include <thread>

namespace trackknife::bench {

class EngineLauncherTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void startsAnEngineAndFindsItAgain();
    void startsItAgainAfterItStops();
    void twoClientsStartingAtOnceShareOneEngine();
    void aMissingProgramSaysSo();
    void nothingStartsOneUnlessAllowed();
    void onlyTheEngineBesideItIsStarted();
};

namespace {

// The daemon's pid, from the lock it holds: fuser would work too, but this
// needs nothing installed.
[[nodiscard]] pid_t enginePid(const LocalEngine& engine) {
    QProcess fuser;
    fuser.start(QStringLiteral("fuser"),
                {QString::fromStdString((engine.state / "engine.lock").string())});
    fuser.waitForFinished();
    const auto text = QString::fromUtf8(fuser.readAllStandardOutput()).trimmed();
    return static_cast<pid_t>(text.section(QLatin1Char(' '), 0, 0).toInt());
}

void stop(const LocalEngine& engine) {
    const auto pid = enginePid(engine);
    if (pid <= 0) {
        return;
    }
    ::kill(pid, SIGTERM);
    for (int attempt = 0; attempt < 100 && ::kill(pid, 0) == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

struct Scratch {
    QTemporaryDir directory;
    LocalEngine engine;
    Scratch() {
        engine.state = directory.filePath(QStringLiteral("state")).toStdString();
        engine.socket = directory.filePath(QStringLiteral("engine.sock")).toStdString();
    }
    ~Scratch() { stop(engine); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

} // namespace

void EngineLauncherTest::initTestCase() { qputenv("TRACKKNIFE_ENGINE", TRACKKNIFE_ENGINE_BINARY); }

void EngineLauncherTest::startsAnEngineAndFindsItAgain() {
    Scratch scratch;
    QVERIFY(scratch.directory.isValid());
    auto first = connectLocalEngine(scratch.engine);
    QVERIFY2(first.has_value(), first ? "" : first.error().message.c_str());
    QVERIFY((*first)->call("catalogue.roots").has_value());
    const auto pid = enginePid(scratch.engine);
    QVERIFY(pid > 0);
    // Its own database, the one the workspace would have used.
    QVERIFY(std::filesystem::exists(scratch.engine.state / "lists.sqlite"));
    // The client going away leaves the engine running; the next finds it.
    (*first)->close();
    first->reset();
    auto second = connectLocalEngine(scratch.engine);
    QVERIFY(second.has_value());
    QCOMPARE(enginePid(scratch.engine), pid);
}

void EngineLauncherTest::startsItAgainAfterItStops() {
    Scratch scratch;
    QVERIFY(connectLocalEngine(scratch.engine).has_value());
    const auto pid = enginePid(scratch.engine);
    stop(scratch.engine);
    QVERIFY(::kill(pid, 0) != 0);
    auto again = connectLocalEngine(scratch.engine);
    QVERIFY(again.has_value());
    QVERIFY(enginePid(scratch.engine) > 0);
    QVERIFY(enginePid(scratch.engine) != pid);
}

void EngineLauncherTest::twoClientsStartingAtOnceShareOneEngine() {
    Scratch scratch;
    core::Result<std::unique_ptr<protocol::Client>> left = std::unexpected(core::Error{});
    core::Result<std::unique_ptr<protocol::Client>> right = std::unexpected(core::Error{});
    std::thread other{[&] { right = connectLocalEngine(scratch.engine); }};
    left = connectLocalEngine(scratch.engine);
    other.join();
    QVERIFY2(left.has_value(), left ? "" : left.error().message.c_str());
    QVERIFY2(right.has_value(), right ? "" : right.error().message.c_str());
    // One engine answers both: the loser of the race exited on the lock.
    QProcess fuser;
    fuser.start(QStringLiteral("fuser"),
                {QString::fromStdString((scratch.engine.state / "engine.lock").string())});
    fuser.waitForFinished();
    QCOMPARE(QString::fromUtf8(fuser.readAllStandardOutput())
                 .split(QLatin1Char(' '), Qt::SkipEmptyParts)
                 .size(),
             1);
}

void EngineLauncherTest::aMissingProgramSaysSo() {
    Scratch scratch;
    const auto saved = qgetenv("TRACKKNIFE_ENGINE");
    qputenv("TRACKKNIFE_ENGINE", "/nonexistent/melodyd");
    const auto result = connectLocalEngine(scratch.engine, std::chrono::seconds{2});
    qputenv("TRACKKNIFE_ENGINE", saved);
    QVERIFY(!result.has_value());
}

void EngineLauncherTest::nothingStartsOneUnlessAllowed() {
    // Off until main() says otherwise, test mode or not: this process has
    // not enabled Qt's test mode, which is how a test once started one.
    QVERIFY(!QStandardPaths::isTestModeEnabled());
    QVERIFY(!localEngine().has_value());
    allowLocalEngine(true);
    QVERIFY(localEngine().has_value());
    allowLocalEngine(false);
    QVERIFY(!localEngine().has_value());
}

void EngineLauncherTest::onlyTheEngineBesideItIsStarted() {
    // With no override, the program is found beside the executable or not at
    // all -- never on PATH, where another melodyd may live.
    const auto saved = qgetenv("TRACKKNIFE_ENGINE");
    qunsetenv("TRACKKNIFE_ENGINE");
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    QFile impostor{bin.filePath(QStringLiteral("melodyd"))};
    QVERIFY(impostor.open(QIODevice::WriteOnly));
    impostor.write("#!/bin/sh\nexit 0\n");
    impostor.close();
    impostor.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    const auto path = qgetenv("PATH");
    qputenv("PATH", bin.path().toLocal8Bit() + ":" + path);
    const auto program = engineProgram();
    qputenv("PATH", path);
    qputenv("TRACKKNIFE_ENGINE", saved);
    QVERIFY(!program.startsWith(bin.path()));
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::EngineLauncherTest)
#include "engine_launcher_test.moc"
