// SPDX-License-Identifier: GPL-3.0-only

// ADR-0226: the engine a workspace starts for itself. Each case runs a real
// melodyd on a temporary database and socket, and stops it afterwards: the
// point of the launcher is that the engine outlives its client, so a test that
// forgot would leave one running.

#include "bench/engine_launcher.hpp"
#include "bench/settings_dialog.hpp"

#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
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
    void sharingSettingsReachTheEngine();
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

void EngineLauncherTest::initTestCase() {
    // The launcher reads Settings; never the real ones. Not through Qt's
    // test mode, which one case here needs off.
    static QTemporaryDir settings_home;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settings_home.path());
    QCoreApplication::setOrganizationName(QStringLiteral("trackknife-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("engine-launcher-test"));
    QSettings{}.clear();
    qputenv("TRACKKNIFE_ENGINE", TRACKKNIFE_ENGINE_BINARY);
}

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

// ADR-0226/0228: sharing this computer's engine is a setting, which reaches
// the engine the next time it starts -- and restarting it is how a change is
// taken up.
void EngineLauncherTest::sharingSettingsReachTheEngine() {
    Scratch scratch;
    QVERIFY(scratch.directory.isValid());
    const auto free_port = [] {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        return probe.serverPort();
    };
    const auto port = free_port();
    QSettings settings;
    settings.setValue(QLatin1String(SettingsDialog::engine_share_key), true);
    settings.setValue(QLatin1String(SettingsDialog::engine_listen_key),
                      QStringLiteral("127.0.0.1:%1").arg(port));
    settings.setValue(QLatin1String(SettingsDialog::engine_stream_port_key), free_port());
    settings.setValue(QLatin1String(SettingsDialog::engine_password_key),
                      QStringLiteral("correct horse"));
    settings.sync();
    const auto tcp = [port](const std::string& password) {
        return protocol::Client::connect(protocol::Endpoint{
            .socket = {}, .host = "127.0.0.1", .port = port, .token = password});
    };

    auto started = connectLocalEngine(scratch.engine);
    QVERIFY2(started.has_value(), started ? "" : started.error().message.c_str());
    // The password travels in a file only its owner can read, not in argv.
    const auto password_file = scratch.engine.state / "engine.password";
    QVERIFY(std::filesystem::exists(password_file));
    QCOMPARE(std::filesystem::status(password_file).permissions() & std::filesystem::perms::all,
             std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    QVERIFY(!tcp({}).has_value() || !(*tcp({}))->call("catalogue.roots").has_value());
    auto admitted = tcp("correct horse");
    QVERIFY(admitted.has_value() && (*admitted)->call("catalogue.roots").has_value());
    (*admitted)->close();
    const auto first = enginePid(scratch.engine);

    // No password: open, and the file is gone.
    settings.remove(QLatin1String(SettingsDialog::engine_password_key));
    settings.sync();
    QVERIFY(restartLocalEngine(scratch.engine).has_value());
    QVERIFY(enginePid(scratch.engine) != first);
    QVERIFY(!std::filesystem::exists(password_file));
    auto open = tcp({});
    QVERIFY(open.has_value() && (*open)->call("catalogue.roots").has_value());
    (*open)->close();

    // Not shared: nothing listens on the network.
    settings.setValue(QLatin1String(SettingsDialog::engine_share_key), false);
    settings.sync();
    QVERIFY(restartLocalEngine(scratch.engine).has_value());
    QVERIFY(!tcp({}).has_value());
    settings.clear();
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::EngineLauncherTest)
#include "engine_launcher_test.moc"
