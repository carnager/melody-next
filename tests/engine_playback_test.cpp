// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 2: playing a track in the workspace drives the engine's
// player rather than the window's own.
//
// This is the observable the whole phase exists for. Until now the engine
// served the library and said nothing when music played, which from the
// outside is indistinguishable from the engine not being used at all.
//
// What is asserted is the command reaching the engine and the order it
// arrives in -- not that sound comes out. Starting audio needs a device, and
// a test that skips itself on a headless machine proves nothing there; the
// queue the engine was handed and the entry it was asked to play are
// device-independent and are exactly the thing that was missing.

#include "bench/bench_main_window.hpp"
#include "bench/local_list_model.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/protocol/message.hpp"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;

namespace {

bool materialize_audio_fixture(const QString& encoded_name, const QString& output_path) {
    QFile source{QStringLiteral(TRACKKNIFE_AUDIO_FIXTURE_DIR) + QLatin1Char('/') + encoded_name};
    if (!source.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto decoded = QByteArray::fromBase64(source.readAll());
    QFile output{output_path};
    return !decoded.isEmpty() && output.open(QIODevice::WriteOnly) &&
           output.write(decoded) == decoded.size();
}

// Every playback method, recorded and then handled for real. Wrapping the
// dispatcher rather than replacing handlers keeps the engine's actual
// behaviour in the loop: the queue below is the one the real handler built.
class RecordingEngine final {
  public:
    explicit RecordingEngine(engine::Player& player) {
        engine::register_playback_methods(inner_, player);
        // Asked of the dispatcher rather than listed here. A hand-written list
        // silently drops a method added later, which reads as the feature not
        // working -- twice while this was being built.
        for (const auto& name : inner_.methods()) {
            outer_.on(name, [this, name](const protocol::Json& params) {
                record(name);
                const protocol::Request forwarded{.id = 1, .method = name, .params = params};
                auto response = inner_.dispatch(forwarded);
                if (response.error) {
                    core::Error failure;
                    failure.code = core::ErrorCode::invariant;
                    failure.message = response.error->message;
                    return core::Result<protocol::Json>{std::unexpected(std::move(failure))};
                }
                return core::Result<protocol::Json>{*response.result};
            });
        }
    }

    [[nodiscard]] protocol::Dispatcher& dispatcher() noexcept { return outer_; }

    // Commands as the engine saw them, in arrival order. State queries are
    // excluded: the client polls those, and they would bury the sequence.
    [[nodiscard]] std::vector<std::string> commands() const {
        const std::lock_guard guard{mutex_};
        return commands_;
    }

  private:
    void record(const std::string& method) {
        if (method == "playback.state") {
            return;
        }
        const std::lock_guard guard{mutex_};
        commands_.push_back(method);
    }

    protocol::Dispatcher inner_;
    protocol::Dispatcher outer_;
    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
};

} // namespace

class EnginePlaybackTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();
    void playingATrackDrivesTheEnginesPlayer();
    void transportControlsDriveTheEngine();
    void jumpToPlayingFindsTheEnginesTrack();
    void modesAndReplayGainReachTheEngine();
    void withoutAnEngineNothingChanges();

  private:
    QTemporaryDir settings_directory_;
};

// The window persists its lists and reads them back at startup, so without
// its own settings and data directory this would open the developer's actual
// workspace -- and inherit whatever tabs were last left open.
void EnginePlaybackTest::initTestCase() {
    QVERIFY(settings_directory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("trackknife-engine-playback-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory_.path());
    QStandardPaths::setTestModeEnabled(true);
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
}

void EnginePlaybackTest::cleanup() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QSettings settings;
    settings.clear();
    settings.sync();
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
}

void EnginePlaybackTest::playingATrackDrivesTheEnginesPlayer() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto media = directory.filePath(QStringLiteral("played.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), media));
    const auto encoded = QFile::encodeName(media);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};
    auto player = engine::Player::create();
    QVERIFY(player.has_value());
    RecordingEngine recorder{**player};
    auto server = engine::Server::listen(socket, recorder.dispatcher());
    QVERIFY(server.has_value());
    (*server)->start();

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});

    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);

    // The engine's player is empty until the user plays something: connecting
    // must not have handed it anything.
    QCOMPARE((*player)->queue().size(), std::size_t{0});

    const auto expected = model->rows().front().entry_id;
    emit view->doubleClicked(model->index(0, 0));

    QTRY_VERIFY_WITH_TIMEOUT(!(*player)->queue().empty(), 5'000);
    QCOMPARE((*player)->queue().size(), std::size_t{1});
    QCOMPARE((*player)->queue().front().source.raw_path, raw_path);
    // ADR-0221: the row's identity travelled, so the engine's queue and the
    // model name the same entry rather than each minting its own.
    QVERIFY2((*player)->queue().front().entry_id == expected,
             "the engine invented its own identity for the entry");

    // And in this order. A play naming an entry the engine has not been given
    // yet is answered not_found, which loses the track silently. Positions
    // rather than indices, because the window also hands over its settings
    // when it connects.
    const auto position = [&recorder](const char* method) {
        const auto seen = recorder.commands();
        const auto found = std::find(seen.begin(), seen.end(), std::string{method});
        return found == seen.end() ? -1 : static_cast<int>(std::distance(seen.begin(), found));
    };
    QTRY_VERIFY_WITH_TIMEOUT(position("playback.play") >= 0, 5'000);
    QVERIFY(position("playback.replace_queue") >= 0);
    QVERIFY2(position("playback.replace_queue") < position("playback.play"),
             "the engine was asked to play an entry before it was given the queue");

    (*server)->stop();
}

// The buttons and the volume slider. Each was wired to the window's own
// player, and each needed its own branch -- routing playRow alone leaves a
// transport that starts the engine and then talks to a silent local player,
// which is worse than not routing it at all.
void EnginePlaybackTest::transportControlsDriveTheEngine() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    // Two tracks, because skipping is only offered when there is somewhere to
    // skip to.
    std::vector<std::string> raw_paths;
    for (const auto* name : {"first.flac", "second.flac"}) {
        const auto media = directory.filePath(QString::fromLatin1(name));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), media));
        const auto encoded = QFile::encodeName(media);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }

    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};
    auto player = engine::Player::create();
    QVERIFY(player.has_value());
    RecordingEngine recorder{**player};
    auto server = engine::Server::listen(socket, recorder.dispatcher());
    QVERIFY(server.has_value());
    (*server)->start();

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(raw_paths);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
    emit view->doubleClicked(model->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!(*player)->queue().empty(), 5'000);

    const auto asked = [&recorder](const QString& method) {
        const auto seen = recorder.commands();
        return std::any_of(seen.begin(), seen.end(), [&method](const std::string& command) {
            return QString::fromStdString(command) == method;
        });
    };

    for (const auto& [name, method] : std::vector<std::pair<QString, QString>>{
             {QStringLiteral("action-next-track"), QStringLiteral("playback.next")},
             {QStringLiteral("action-previous-track"), QStringLiteral("playback.previous")},
             // Nothing is playing (no device in a headless run), so the
             // toggle asks the engine to start rather than to pause.
             {QStringLiteral("action-play-pause"), QStringLiteral("playback.resume")},
             {QStringLiteral("action-stop"), QStringLiteral("playback.stop")}}) {
        auto* action = window.findChild<QAction*>(name);
        QVERIFY2(action != nullptr, qPrintable(name));
        // A disabled action ignores trigger(), which would make this pass for
        // the wrong reason if the enable rules were wrong.
        QTRY_VERIFY2_WITH_TIMEOUT(action->isEnabled(), qPrintable(name), 5'000);
        action->trigger();
        QTRY_VERIFY2_WITH_TIMEOUT(asked(method), qPrintable(method), 5'000);
    }

    // Volume is the engine's, because the engine owns the output.
    auto* volume = window.findChild<QSlider*>(QStringLiteral("bench-volume"));
    QVERIFY(volume != nullptr);
    volume->setValue(42);
    QTRY_VERIFY_WITH_TIMEOUT(asked(QStringLiteral("playback.set_volume")), 5'000);

    (*server)->stop();
}

// The mode buttons and the ReplayGain menu decide what plays next and how
// loud it is, which is the engine's business once it owns playback. They were
// still being sent to this process's idle player, so changing them did
// nothing audible.
void EnginePlaybackTest::modesAndReplayGainReachTheEngine() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};
    auto player = engine::Player::create();
    QVERIFY(player.has_value());
    RecordingEngine recorder{**player};
    auto server = engine::Server::listen(socket, recorder.dispatcher());
    QVERIFY(server.has_value());
    (*server)->start();
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         QString::fromStdString(socket.string()));
    // As if the user had chosen album gain in an earlier session.
    QSettings{}.setValue(QStringLiteral("playback/local-replaygain"), QStringLiteral("album"));

    // A list tab, because the mode buttons belong to local playback and are
    // hidden while an MPD tab is on screen.
    const auto media = directory.filePath(QStringLiteral("played.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), media));
    const auto encoded = QFile::encodeName(media);

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(
        {std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);

    // The saved settings reach the engine without the user touching anything.
    // A window that only sends them when a menu is used leaves the first
    // track playing with no gain applied, which is what "toggling it off and
    // on fixes it" means.
    QTRY_COMPARE_WITH_TIMEOUT((*player)->state().replay_gain_mode, audio::ReplayGainMode::album,
                              5'000);

    auto* repeat = window.findChild<QAction*>(QStringLiteral("action-local-repeat"));
    QVERIFY(repeat != nullptr);
    QVERIFY2(repeat->isEnabled(), "an engine can repeat even with no local device");
    repeat->trigger();
    QTRY_VERIFY_WITH_TIMEOUT((*player)->modes().repeat, 5'000);

    auto* consume = window.findChild<QAction*>(QStringLiteral("action-local-consume"));
    QVERIFY(consume != nullptr);
    consume->trigger();
    // The tri-states travel too: sending only repeat and random is how a mode
    // the window shows as on stays off in the engine.
    QTRY_VERIFY_WITH_TIMEOUT((*player)->modes().consume != audio::ModeState::off, 5'000);

    auto* album_gain = window.findChild<QAction*>(QStringLiteral("action-local-replaygain-album"));
    QVERIFY(album_gain != nullptr);
    album_gain->trigger();
    QTRY_COMPARE_WITH_TIMEOUT((*player)->state().replay_gain_mode, audio::ReplayGainMode::album,
                              5'000);

    auto* off = window.findChild<QAction*>(QStringLiteral("action-local-replaygain-off"));
    QVERIFY(off != nullptr);
    off->trigger();
    QTRY_COMPARE_WITH_TIMEOUT((*player)->state().replay_gain_mode, audio::ReplayGainMode::off,
                              5'000);

    (*server)->stop();
}

// Ctrl+J. The window knows where playback is only because it recorded the
// anchors when it asked the engine to play; nothing else tells it.
void EnginePlaybackTest::jumpToPlayingFindsTheEnginesTrack() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto media = directory.filePath(QStringLiteral("played.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), media));
    const auto encoded = QFile::encodeName(media);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    const std::filesystem::path socket{
        (directory.path() + QStringLiteral("/engine.sock")).toStdString()};
    auto player = engine::Player::create();
    QVERIFY(player.has_value());
    RecordingEngine recorder{**player};
    auto server = engine::Server::listen(socket, recorder.dispatcher());
    QVERIFY(server.has_value());
    (*server)->start();
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* playing = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(playing != nullptr);
    auto* model = qobject_cast<LocalListModel*>(playing->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
    emit playing->doubleClicked(model->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!(*player)->queue().empty(), 5'000);

    // The user looks somewhere else.
    tabs->setCurrentIndex(0);
    QVERIFY(tabs->currentWidget() != playing);

    auto* jump = window.findChild<QAction*>(QStringLiteral("action-jump-to-playing"));
    QVERIFY(jump != nullptr);
    QCOMPARE(jump->shortcut(), QKeySequence(QStringLiteral("Ctrl+J")));
    jump->trigger();
    QCOMPARE(tabs->currentWidget(), playing);
    QCOMPARE(playing->currentIndex().row(), 0);

    (*server)->stop();
}

// The unchanged path: no engine configured, so the window plays it itself and
// nothing about the local workspace is disturbed.
void EnginePlaybackTest::withoutAnEngineNothingChanges() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto media = directory.filePath(QStringLiteral("played.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), media));
    const auto encoded = QFile::encodeName(media);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);

    emit view->doubleClicked(model->index(0, 0));
    QTest::qWait(200);
    // No engine, so the engine branch must not have claimed the transport.
    QVERIFY(!window.property("trackknife-engine-playback").isValid());
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::EnginePlaybackTest)
#include "engine_playback_test.moc"
