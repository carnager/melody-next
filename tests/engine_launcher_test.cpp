// SPDX-License-Identifier: GPL-3.0-only

// ADR-0226: the engine a workspace starts for itself. Each case runs a real
// melodyd on a temporary database and socket, and stops it afterwards: the
// point of the launcher is that the engine outlives its client, so a test that
// forgot would leave one running.

#include "bench/engine_launcher.hpp"
#include "bench/remote_engines.hpp"
#include "bench/settings_dialog.hpp"

#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include <signal.h>

#include <filesystem>
#include <thread>

namespace trackknife::bench {

class EngineLauncherTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void startsAnEngineAndFindsItAgain();
    void startsItAgainAfterItStops();
    void aProcessIsRunningUntilItsLastThreadEnds();
    void twoClientsStartingAtOnceShareOneEngine();
    void aMissingProgramSaysSo();
    void nothingStartsOneUnlessAllowed();
    void onlyTheEngineBesideItIsStarted();
    void sharingSettingsReachTheEngine();
    void theEnginePlaysForTheRemoteWithoutAnAgent();
    void anOutdatedEngineIsSeenAndStopped();
    void thisUsersEngineIsNotStartedHere();
};

namespace {

// The daemon's pid, from the lock it holds.
[[nodiscard]] pid_t enginePid(const LocalEngine& engine) {
    const auto holders = lockHolders(engine.state / "engine.lock");
    return holders.empty() ? 0 : holders.front();
}

// What the engine said, and what there is to see of engines, for a failure
// that needs it: its log, its socket, and every melodyd with the lock files it
// holds -- a CI container once found none holding this one's.
[[nodiscard]] QByteArray engineLog(const LocalEngine& engine) {
    QByteArray said;
    QFile log{QString::fromStdString((engine.state / "melodyd.log").string())};
    said += log.open(QIODevice::ReadOnly)
                ? "log (" + QByteArray::number(log.size()) + " bytes): " + log.readAll()
                : QByteArray{"no log"};
    std::error_code error;
    said += "\nsocket " + QByteArray::fromStdString(engine.socket.string()) +
            (std::filesystem::exists(engine.socket, error) ? " exists" : " is missing");
    said += "\nlock looked for: " +
            QByteArray::fromStdString(
                std::filesystem::weakly_canonical(engine.state / "engine.lock", error).string());
    for (const auto& process : std::filesystem::directory_iterator{"/proc", error}) {
        std::error_code unreadable;
        const auto name = process.path().filename().string();
        QFile comm{QString::fromStdString((process.path() / "comm").string())};
        if (!comm.open(QIODevice::ReadOnly) || !comm.readAll().startsWith("melodyd")) {
            continue;
        }
        QFile stat{QString::fromStdString((process.path() / "stat").string())};
        said += "\nmelodyd " + QByteArray::fromStdString(name) + " stat " +
                (stat.open(QIODevice::ReadOnly) ? stat.readLine().left(40).trimmed() : "?");
        for (const auto& descriptor :
             std::filesystem::directory_iterator{process.path() / "fd", unreadable}) {
            std::error_code gone;
            const auto target = std::filesystem::read_symlink(descriptor.path(), gone).string();
            if (target.find(".lock") != std::string::npos ||
                target.find(".sock") != std::string::npos) {
                said += "\n  holds " + QByteArray::fromStdString(target);
            }
        }
        if (unreadable) {
            said += "\n  fds unreadable: " + QByteArray::fromStdString(unreadable.message());
        }
    }
    return said;
}

void stop(const LocalEngine& engine) {
    const auto pid = enginePid(engine);
    if (pid <= 0) {
        return;
    }
    ::kill(pid, SIGTERM);
    for (int attempt = 0; attempt < 100 && processRunning(pid); ++attempt) {
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
    qputenv("MELODY_ENGINE", MELODY_ENGINE_BINARY);
}

void EngineLauncherTest::startsAnEngineAndFindsItAgain() {
    Scratch scratch;
    QVERIFY(scratch.directory.isValid());
    auto first = connectLocalEngine(scratch.engine);
    QVERIFY2(first.has_value(), first ? "" : first.error().message.c_str());
    QVERIFY((*first)->call("catalogue.roots").has_value());
    const auto pid = enginePid(scratch.engine);
    QVERIFY2(pid > 0, engineLog(scratch.engine).constData());
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
    QVERIFY(!processRunning(pid));
    auto again = connectLocalEngine(scratch.engine);
    QVERIFY(again.has_value());
    QVERIFY2(enginePid(scratch.engine) > 0, engineLog(scratch.engine).constData());
    QVERIFY(enginePid(scratch.engine) != pid);
}

// The engine, stopped, can show as a zombie while its other threads are still
// going and still hold its socket and its lock: in a CI container the test
// took it for gone, found its socket still answering, and no engine holding
// the lock. Running, then, and holding, until the last thread ends.
void EngineLauncherTest::aProcessIsRunningUntilItsLastThreadEnds() {
#ifdef __APPLE__
    QSKIP("Linux's /proc semantics");
#else
    // A program of its own, not a fork of this one: a fork of a threaded Qt
    // process can deadlock in its own exit.
    QTemporaryDir directory;
    const auto lock =
        std::filesystem::path{directory.filePath(QStringLiteral("held.lock")).toStdString()};
    QProcess helper;
    helper.start(QStringLiteral(MELODY_ZOMBIE_LEADER_HELPER),
                 {QString::fromStdString(lock.string())});
    QVERIFY(helper.waitForStarted());
    const auto pid = static_cast<pid_t>(helper.processId());
    const auto zombie = [pid] {
        QFile stat{QStringLiteral("/proc/%1/stat").arg(pid)};
        const auto line = stat.open(QIODevice::ReadOnly) ? stat.readAll() : QByteArray{};
        const auto end = line.lastIndexOf(')');
        return end >= 0 && end + 2 < line.size() && line[end + 2] == 'Z';
    };
    QTRY_VERIFY(zombie());
    QVERIFY(processRunning(pid));
    QCOMPARE(lockHolders(lock), std::vector<pid_t>{pid});

    helper.write("x");
    QVERIFY(helper.waitForFinished());
    QVERIFY(!processRunning(pid));
    QVERIFY(lockHolders(lock).empty());
#endif
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
    QCOMPARE(lockHolders(scratch.engine.state / "engine.lock").size(), std::size_t{1U});
}

void EngineLauncherTest::aMissingProgramSaysSo() {
    Scratch scratch;
    const auto saved = qgetenv("MELODY_ENGINE");
    qputenv("MELODY_ENGINE", "/nonexistent/melodyd");
    const auto result = connectLocalEngine(scratch.engine, std::chrono::seconds{2});
    qputenv("MELODY_ENGINE", saved);
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
    const auto saved = qgetenv("MELODY_ENGINE");
    qunsetenv("MELODY_ENGINE");
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
    qputenv("MELODY_ENGINE", saved);
    QVERIFY(!program.startsWith(bin.path()));
}

namespace {

// What the engine's melodyd.conf says, beside its database.
[[nodiscard]] QString configOf(const LocalEngine& engine) {
    QFile file{QString::fromStdString((engine.state / "melodyd.conf").string())};
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}

[[nodiscard]] const protocol::Json* change(const protocol::Json& changes, const std::string& key,
                                           const std::string& engine = {}) {
    for (const auto& item : changes) {
        if (item.value("key", std::string{}) == key &&
            item.value("engine", std::string{}) == engine) {
            return &item;
        }
    }
    return nullptr;
}

} // namespace

// ADR-0286: sharing this computer's engine is a setting, handed to the
// engine as its melodyd.conf and taken up by a restart.
void EngineLauncherTest::sharingSettingsReachTheEngine() {
    Scratch scratch;
    QVERIFY(scratch.directory.isValid());
    // Both held while they are chosen: asked one after the other, the second
    // could be the first again, and the engine then cannot start.
    QTcpServer engine_probe;
    QTcpServer stream_probe;
    QVERIFY(engine_probe.listen(QHostAddress::LocalHost, 0));
    QVERIFY(stream_probe.listen(QHostAddress::LocalHost, 0));
    const auto port = engine_probe.serverPort();
    const auto stream_port = stream_probe.serverPort();
    engine_probe.close();
    stream_probe.close();
    const auto tcp = [port](const std::string& password) {
        return protocol::Client::connect(
            protocol::Endpoint{.socket = {}, .host = "127.0.0.1", .port = port, .token = password});
    };

    // Started here -- not this user's engine -- with its own file, empty yet.
    auto started = connectLocalEngine(scratch.engine);
    QVERIFY2(
        started.has_value(),
        started
            ? ""
            : (started.error().message + "\n" + engineLog(scratch.engine).toStdString()).c_str());
    (*started)->close();
    QVERIFY(!localEngineIsService(scratch.engine));

    QSettings settings;
    const auto unshared = localEngineSharing();
    settings.setValue(QLatin1String(SettingsDialog::engine_share_key), true);
    settings.setValue(QLatin1String(SettingsDialog::engine_listen_key),
                      QStringLiteral("127.0.0.1:%1").arg(port));
    settings.setValue(QLatin1String(SettingsDialog::engine_stream_port_key), stream_port);
    settings.setValue(QLatin1String(SettingsDialog::engine_password_key),
                      QStringLiteral("correct horse"));
    settings.sync();
    const auto shared = localEngineSharing();
    auto configured = configureLocalEngine(scratch.engine, sharingChanges(shared, unshared));
    QVERIFY2(configured.has_value(), configured ? "" : configured.error().message.c_str());
    // In its file, which only its owner can read; nothing in argv.
    const auto config = configOf(scratch.engine);
    QVERIFY(config.contains(QStringLiteral("listen = 127.0.0.1:%1\n").arg(port)));
    QVERIFY(config.contains(QStringLiteral("http = 127.0.0.1:%1\n").arg(stream_port)));
    QVERIFY(config.contains(QStringLiteral("password = correct horse\n")));
    QCOMPARE(std::filesystem::status(scratch.engine.state / "melodyd.conf").permissions() &
                 std::filesystem::perms::all,
             std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    QVERIFY(!tcp({}).has_value());
    QVERIFY(!tcp("wrong").has_value());
    auto admitted = tcp("correct horse");
    QVERIFY(admitted.has_value() && (*admitted)->call("catalogue.roots").has_value());
    (*admitted)->close();
    const auto first = enginePid(scratch.engine);

    // ADR-0286: without a password nothing listens on the network -- there
    // is no switch of its own -- and the password is gone from the file.
    settings.remove(QLatin1String(SettingsDialog::engine_password_key));
    settings.sync();
    configured =
        configureLocalEngine(scratch.engine, sharingChanges(localEngineSharing(), shared));
    QVERIFY2(configured.has_value(), configured ? "" : configured.error().message.c_str());
    QVERIFY(enginePid(scratch.engine) != first);
    QVERIFY(!tcp("correct horse").has_value());
    QVERIFY(configOf(scratch.engine).contains(QStringLiteral("local-only = yes\n")));
    // Taken out: no password line, only the skeleton's commented example.
    QVERIFY(!configOf(scratch.engine)
                 .contains(QRegularExpression{QStringLiteral("^password = "),
                                              QRegularExpression::MultilineOption}));

    // The old switch decides nothing now: off, with a password, it is
    // shared; on, without one, it is not (ADR-0223: no open TCP engine).
    settings.setValue(QLatin1String(SettingsDialog::engine_share_key), false);
    settings.setValue(QLatin1String(SettingsDialog::engine_password_key),
                      QStringLiteral("correct horse"));
    settings.sync();
    auto changes = sharingChanges(localEngineSharing(), shared);
    QVERIFY(change(changes, "local-only")->at("value").is_null());
    settings.setValue(QLatin1String(SettingsDialog::engine_share_key), true);
    settings.remove(QLatin1String(SettingsDialog::engine_password_key));
    settings.sync();
    changes = sharingChanges(localEngineSharing(), shared);
    QCOMPARE(change(changes, "local-only")->at("value"), protocol::Json("yes"));
    QVERIFY(change(changes, "password")->at("value").is_null());
    settings.clear();
}

// ADR-0228, ADR-0286: with a remote engine configured, this computer's
// engine offers itself as one of the remote's outputs -- no melody-agent
// needed here -- through an [engine] section of its melodyd.conf.
void EngineLauncherTest::theEnginePlaysForTheRemoteWithoutAnAgent() {
    Scratch scratch;
    QVERIFY(scratch.directory.isValid());
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    const auto port = probe.serverPort();
    probe.close();
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    QProcess remote;
    remote.setProgram(QStringLiteral(MELODY_ENGINE_BINARY));
    remote.setArguments({QStringLiteral("--socket"),
                         remote_state.filePath(QStringLiteral("r.sock")), QStringLiteral("--state"),
                         remote_state.path(), QStringLiteral("--name"), QStringLiteral("remote"),
                         QStringLiteral("--listen"), QStringLiteral("127.0.0.1:%1").arg(port),
                         QStringLiteral("--password"), QStringLiteral("correct horse"),
                         QStringLiteral("--no-agent")});
    remote.setProcessChannelMode(QProcess::MergedChannels);
    remote.start();
    QVERIFY(remote.waitForStarted());
    const protocol::Endpoint remote_endpoint{
        .socket = {}, .host = "127.0.0.1", .port = port, .token = "correct horse"};
    QTRY_VERIFY_WITH_TIMEOUT(protocol::Client::connect(remote_endpoint).has_value(), 10'000);

    QSettings settings;
    settings.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                      QStringLiteral("127.0.0.1:%1").arg(port));
    // One password everywhere, as melodyd has: this computer's reaches the
    // remote too, with nothing entered for it -- and one entered for the
    // remote alone wins when it differs.
    settings.setValue(QLatin1String(SettingsDialog::engine_password_key),
                      QStringLiteral("correct horse"));
    settings.setValue(QLatin1String(SettingsDialog::library_engine_token_key),
                      QStringLiteral("its own"));
    settings.sync();
    QCOMPARE(SettingsDialog::remoteEnginePassword(), QStringLiteral("its own"));
    settings.remove(QLatin1String(SettingsDialog::library_engine_token_key));
    settings.sync();
    QCOMPARE(SettingsDialog::remoteEnginePassword(), QStringLiteral("correct horse"));
    // Shared, with that password, the remote's is this engine's own: the
    // section needs none of its own.
    QVERIFY(change(sharingChanges(localEngineSharing(), {}), "password", "127.0.0.1")
                ->at("value")
                .is_null());
    // This computer's left without one -- not on the network -- and the
    // remote's given as its own.
    settings.remove(QLatin1String(SettingsDialog::engine_password_key));
    settings.setValue(QLatin1String(SettingsDialog::library_engine_token_key),
                      QStringLiteral("correct horse"));
    settings.sync();

    // The remote as an [engine] section named after its host, its address
    // used while the network does not find it; this engine, not shared, has
    // no password of its own, so the section carries the remote's.
    auto changes = sharingChanges(localEngineSharing(), {});
    QCOMPARE(change(changes, "address", "127.0.0.1")->at("value"),
             protocol::Json("127.0.0.1:" + std::to_string(port)));
    QCOMPARE(change(changes, "password", "127.0.0.1")->at("value"),
             protocol::Json("correct horse"));
    QVERIFY(change(changes, "agent")->at("value").is_null());

    auto started = connectLocalEngine(scratch.engine);
    QVERIFY2(started.has_value(), started ? "" : started.error().message.c_str());
    (*started)->close();
    auto configured = configureLocalEngine(scratch.engine, changes);
    QVERIFY2(configured.has_value(), configured ? "" : configured.error().message.c_str());
    QVERIFY(configOf(scratch.engine).contains(QStringLiteral("[engine 127.0.0.1]\n")));

    // The remote lists this computer's engine among its outputs.
    const auto listed = [&remote_endpoint] {
        auto client = protocol::Client::connect(remote_endpoint);
        if (!client) {
            return false;
        }
        auto outputs = (*client)->call("outputs.list");
        (*client)->close();
        if (!outputs) {
            return false;
        }
        for (const auto& output : outputs->value("outputs", protocol::Json::array())) {
            if (output.value("id", std::string{}).starts_with("agent:") &&
                output.value("online", false)) {
                return true;
            }
        }
        return false;
    };
    const auto audio_here = [&scratch] {
        QFile log{QString::fromStdString((scratch.engine.state / "melodyd.log").string())};
        return !(log.open(QIODevice::ReadOnly) && log.readAll().contains("no audio here"));
    };
    // After the network's grace (five seconds), by its address.
    QTRY_VERIFY_WITH_TIMEOUT(listed() || !audio_here(), 15'000);
    if (!audio_here()) {
        QSKIP("no audio output here for the built-in agent");
    }

    // ADR-0239: one fixed for an engine, whatever the route; the engines
    // found as Settings have it; another engine elsewhere its own section.
    {
        auto engines = loadRemoteEngines();
        QCOMPARE(engines.size(), std::size_t{1});
        const auto before = localEngineSharing();
        engines.push_back({.address = QStringLiteral("10.8.0.9:6603"),
                           .password = QStringLiteral("another"),
                           .music_folder = {},
                           .reachable_at = QStringLiteral("/media/other"),
                           .id = {},
                           .stream_kbps = 64});
        saveRemoteEngines(engines);
        settings.setValue(QLatin1String(SettingsDialog::engine_stream_away_key), 96);
        settings.sync();
        changes = sharingChanges(localEngineSharing(), before);
        QCOMPARE(change(changes, "address", "10.8.0.9")->at("value"),
                 protocol::Json("10.8.0.9:6603"));
        QCOMPARE(change(changes, "music-root", "10.8.0.9")->at("value"),
                 protocol::Json("/media/other"));
        QCOMPARE(change(changes, "password", "10.8.0.9")->at("value"), protocol::Json("another"));
        QCOMPARE(change(changes, "stream-home", "10.8.0.9")->at("value"),
                 protocol::Json("opus 64"));
        QCOMPARE(change(changes, "stream-away", "10.8.0.9")->at("value"),
                 protocol::Json("opus 64"));
        QCOMPARE(change(changes, "agent-stream-away")->at("value"), protocol::Json("opus 96"));
        QVERIFY(change(changes, "agent-stream-home")->at("value").is_null());

        // Removed again: its section's keys taken out.
        const auto with = localEngineSharing();
        engines.pop_back();
        saveRemoteEngines(engines);
        changes = sharingChanges(localEngineSharing(), with);
        QVERIFY(change(changes, "address", "10.8.0.9")->at("value").is_null());
        settings.remove(QLatin1String(SettingsDialog::engine_stream_away_key));
    }

    // Turned off: plays for no one, and the sections go.
    const auto on = localEngineSharing();
    settings.setValue(QLatin1String(SettingsDialog::engine_play_for_remote_key), false);
    settings.sync();
    changes = sharingChanges(localEngineSharing(), on);
    QCOMPARE(change(changes, "agent")->at("value"), protocol::Json("no"));
    QVERIFY(change(changes, "address", "127.0.0.1")->at("value").is_null());
    settings.clear();
    stop(scratch.engine);
    remote.terminate();
    remote.waitForFinished(5'000);
}

// ADR-0286: this user's engine on Linux is systemd's: Trackknife does not
// start it, and says how to.
void EngineLauncherTest::thisUsersEngineIsNotStartedHere() {
#ifdef __APPLE__
    QSKIP("on macOS Trackknife starts it");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const LocalEngine engine{.state = directory.filePath(QStringLiteral("state")).toStdString(),
                             .socket = directory.filePath(QStringLiteral("s.sock")).toStdString(),
                             .standard = true};
    QVERIFY(qEnvironmentVariableIsSet("MELODY_NO_SYSTEMD"));
    const auto result = connectLocalEngine(engine, std::chrono::seconds{2});
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, core::ErrorCode::not_found);
    QVERIFY(result.error().message.find("systemctl --user enable --now melodyd.socket") !=
            std::string::npos);
    QVERIFY(lockHolders(engine.state / "engine.lock").empty());
    QVERIFY(!localEngineWantsService(engine));
#endif
}

// ADR-0226: this computer's engine outlives the window, so after a rebuild it
// runs the old program until restarted -- which must be seen, or the window
// talks to an engine that knows none of the new requests. And quitting stops
// it, where closing the window does not.
void EngineLauncherTest::anOutdatedEngineIsSeenAndStopped() {
    Scratch scratch;
    QVERIFY(scratch.directory.isValid());
    // Its own copy of the program, so the test can replace it as a rebuild
    // does without touching the build tree's.
    const auto program = scratch.directory.filePath(QStringLiteral("melodyd"));
    QVERIFY(QFile::copy(QStringLiteral(MELODY_ENGINE_BINARY), program));
    const auto saved = qgetenv("MELODY_ENGINE");
    qputenv("MELODY_ENGINE", program.toLocal8Bit());
    const auto restore = qScopeGuard([saved] { qputenv("MELODY_ENGINE", saved); });

    QVERIFY(connectLocalEngine(scratch.engine).has_value());
    QVERIFY(!localEngineOutdated(scratch.engine));

    // Rebuilt: a new file where the old one was, the old one still running.
    QVERIFY(QFile::remove(program));
    QVERIFY(QFile::copy(QStringLiteral(MELODY_ENGINE_BINARY), program));
    QVERIFY(localEngineOutdated(scratch.engine));
    QVERIFY(restartLocalEngine(scratch.engine).has_value());
    QVERIFY(!localEngineOutdated(scratch.engine));

    // Quit stops it.
    const auto pid = enginePid(scratch.engine);
    QVERIFY(pid > 0);
    QVERIFY(stopLocalEngine(scratch.engine).has_value());
    QVERIFY(!processRunning(pid));
    QVERIFY(lockHolders(scratch.engine.state / "engine.lock").empty());
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::EngineLauncherTest)
#include "engine_launcher_test.moc"
