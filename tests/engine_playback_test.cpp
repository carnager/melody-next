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
#include "bench/mpris_service.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/protocol/message.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;

namespace {

// Whether the engine actually started playing, given time to. A skip decided
// too early is a skip on a loaded machine that has audio, and a skipped test
// cannot catch the regression it exists for -- so "no output here" is only
// concluded after the play command has had every chance to land.
bool engine_started(const std::unique_ptr<engine::Player>& player) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (!player->state().entry.is_nil()) {
            return true;
        }
        QTest::qWait(10);
    }
    return false;
}

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
    void upNextDecidesWhatTheEnginePlaysNext();
    void editingThePlayingListReachesTheEngine();
    void aQueueChangedElsewhereReachesTheList();
    void consumeDropsTheRowFromTheList();
    void listeningIsCreditedWhileTheEnginePlays();
    void theDesktopSeesWhatTheEnginePlays();
    void aNewWindowAttachesToWhatTheEngineIsPlaying();
    void anEngineQueueNoListHoldsBecomesATab();
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

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});

    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
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

    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(raw_paths);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
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
    QTRY_COMPARE(tabs->count(), 1);

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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* playing = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(playing != nullptr);
    auto* model = qobject_cast<LocalListModel*>(playing->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
    emit playing->doubleClicked(model->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!(*player)->queue().empty(), 5'000);

    // The user looks somewhere else.
    auto* new_list = window.findChild<QAction*>(QStringLiteral("action-new-list"));
    QVERIFY(new_list != nullptr);
    // Naming the list is a modal prompt; answer it rather than let it
    // block the test waiting for a person.
    QTimer::singleShot(0, &window, [&window] {
        auto* prompt = window.findChild<QInputDialog*>();
        if (prompt != nullptr) {
            prompt->setTextValue(QStringLiteral("Elsewhere"));
            prompt->accept();
        }
    });
    new_list->trigger();
    QVERIFY(tabs->currentWidget() != playing);

    auto* jump = window.findChild<QAction*>(QStringLiteral("action-jump-to-playing"));
    QVERIFY(jump != nullptr);
    QCOMPARE(jump->shortcut(), QKeySequence(QStringLiteral("Ctrl+J")));
    jump->trigger();
    QCOMPARE(tabs->currentWidget(), playing);
    QCOMPARE(playing->currentIndex().row(), 0);

    (*server)->stop();
}

// The engine keeps playing what it was handed. An edit to the list that is
// playing has to reach it, or a track removed here still plays.
void EnginePlaybackTest::editingThePlayingListReachesTheEngine() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::vector<std::string> raw_paths;
    for (const auto* name : {"one.flac", "two.flac", "three.flac"}) {
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(raw_paths);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 3, 5'000);

    emit view->doubleClicked(model->index(0, 0));
    QTRY_COMPARE_WITH_TIMEOUT((*player)->queue().size(), std::size_t{3}, 5'000);

    // Removed the way a user removes it, so the path under test is the one
    // the application takes rather than the model's own.
    const auto dropped = model->rows().at(2).entry_id;
    view->selectionModel()->select(model->index(2, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto* remove = window.findChild<QAction*>(QStringLiteral("action-remove-selected-tracks"));
    QVERIFY(remove != nullptr);
    QTRY_VERIFY(remove->isEnabled());
    remove->trigger();
    QTRY_COMPARE(model->rowCount(), 2);

    QTRY_COMPARE_WITH_TIMEOUT((*player)->queue().size(), std::size_t{2}, 5'000);
    const auto held = (*player)->queue();
    QVERIFY2(std::none_of(held.begin(), held.end(),
                          [&dropped](const auto& entry) { return entry.entry_id == dropped; }),
             "the engine still holds a track that was removed from the list");

    (*server)->stop();
}

// And the other direction. The queue is the engine's, so a change made to it
// elsewhere -- another client, or the engine itself -- is what the window has
// to show, rather than whatever it last pushed.
void EnginePlaybackTest::aQueueChangedElsewhereReachesTheList() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::vector<std::string> raw_paths;
    for (const auto* name : {"one.flac", "two.flac", "stranger.flac"}) {
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_paths[0], raw_paths[1]});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
    emit view->doubleClicked(model->index(0, 0));
    QTRY_COMPARE_WITH_TIMEOUT((*player)->queue().size(), std::size_t{2}, 5'000);

    // Somebody else edits the engine's list -- a second client appending to
    // it. (enqueue is not that: it holds asks for up-next, which are not list
    // entries and must not appear as rows.)
    engine::QueueEntry added;
    added.source.raw_path = raw_paths[2];
    auto edited = (*player)->queue();
    edited.push_back(added);
    (*player)->replace_queue(std::move(edited));

    QTRY_VERIFY2_WITH_TIMEOUT(model->rowOfEntry(added.entry_id, -1) >= 0,
                              "the window is still showing a queue the engine has moved past",
                              10'000);
    QCOMPARE(model->rowCount(), 3);
    // The rows it already had keep what this window read from the files;
    // adopting must not turn them back into filenames.
    QVERIFY(!model->rows().front().title.empty());

    (*server)->stop();
}

// Consume is the engine's decision, and the list it came from has to follow
// or the window keeps showing a track the engine no longer holds.
void EnginePlaybackTest::consumeDropsTheRowFromTheList() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::vector<std::string> raw_paths;
    for (const auto* name : {"one.flac", "two.flac"}) {
        const auto media = directory.filePath(QString::fromLatin1(name));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-long-flac.b64"), media));
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(raw_paths);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
    const auto first = model->rows().front().entry_id;

    auto* consume = window.findChild<QAction*>(QStringLiteral("action-local-consume"));
    QVERIFY(consume != nullptr);
    consume->trigger();
    QTRY_VERIFY_WITH_TIMEOUT((*player)->modes().consume_active(), 5'000);

    emit view->doubleClicked(model->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT((*player)->queue().size() == 2U, 5'000);
    if (!engine_started(*player)) {
        (*server)->stop();
        QSKIP("no audio output here, so nothing is played and nothing is consumed");
    }

    auto* next = window.findChild<QAction*>(QStringLiteral("action-next-track"));
    QVERIFY(next != nullptr);
    QTRY_VERIFY(next->isEnabled());
    next->trigger();

    QTRY_VERIFY2_WITH_TIMEOUT(model->rowOfEntry(first, -1) < 0,
                              "the list still holds a track the engine has dropped", 5'000);
    QCOMPARE(model->rowCount(), 1);

    (*server)->stop();
}

// Last.fm is fed from the local player's snapshot, which on the engine path is
// a player that is not running -- so nothing was ever credited. The engine
// knows the path and the position; the tags a scrobble needs are in this
// window's rows, and the two have to be put together.
void EnginePlaybackTest::listeningIsCreditedWhileTheEnginePlays() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto media = directory.filePath(QStringLiteral("scrobbled.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-long-flac.b64"), media));
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(model->rows().front().probed, 5'000);
    const auto title = QString::fromStdString(model->rows().front().title);
    QVERIFY(!title.isEmpty());

    emit view->doubleClicked(model->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!(*player)->queue().empty(), 5'000);

    // The tags travel with the sample, not just the path: a scrobble without
    // an artist and a title is not a scrobble.
    const auto credited = [&window, &title] {
        const auto sample = window.property("trackknife-lastfm-sample").toString();
        return sample.contains(title) && !sample.startsWith(QLatin1Char('|'));
    };
    QTRY_VERIFY2_WITH_TIMEOUT(credited(), "nothing was credited while the engine played", 5'000);

    (*server)->stop();
}

// MPRIS and notifications read the local player, which on the engine path
// is idle -- so media keys worked (they land on the transport actions) while
// the desktop was told nothing was playing.
void EnginePlaybackTest::theDesktopSeesWhatTheEnginePlays() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto media = directory.filePath(QStringLiteral("announced.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-long-flac.b64"), media));
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    auto* mpris = window.findChild<MprisService*>();
    QVERIFY(mpris != nullptr);

    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(model->rows().front().probed, 5'000);
    const auto title = QString::fromStdString(model->rows().front().title);
    const auto entry = QString::fromStdString(model->rows().front().entry_id.to_string());

    emit view->doubleClicked(model->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!(*player)->queue().empty(), 5'000);

    // Keyed by the entry, and titled from the row's tags rather than the
    // filename: the desktop is told which track, not which file.
    QTRY_COMPARE_WITH_TIMEOUT(mpris->currentState().track_key, entry, 5'000);
    QCOMPARE(mpris->currentState().title, title);
    QVERIFY(mpris->currentState().can_pause);

    (*server)->stop();
}

// Up Next has to reach the engine, because the engine decides what follows.
// A panel that only fills a list in this process changes nothing about what
// plays, which is what "hitting next does not play those" means.
void EnginePlaybackTest::upNextDecidesWhatTheEnginePlaysNext() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::vector<std::string> raw_paths;
    for (const auto* name : {"first.flac", "second.flac", "third.flac"}) {
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    BenchMainWindow window;
    window.show();
    // Two in the playing list, one that never enters it.
    window.openLocalPaths({raw_paths[0], raw_paths[1]});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);

    const auto wanted = model->rows().at(1).raw_path;
    emit view->doubleClicked(model->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT((*player)->queue().size() == 2U, 5'000);

    // Queue the second row ahead of the order.
    view->selectionModel()->select(model->index(1, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto* queue_next = window.findChild<QAction*>(QStringLiteral("action-queue-next"));
    QVERIFY(queue_next != nullptr);
    queue_next->trigger();

    // The ask is an occurrence of its own (ADR-0221/0226): a new identity for
    // the same track, held by the engine apart from the list.
    QTRY_VERIFY2_WITH_TIMEOUT((*player)->requests().size() == 1U,
                              "the engine was never told about the request", 5'000);
    QCOMPARE((*player)->queue().size(), 2U);

    // And Next honours it rather than walking the list.
    auto* next = window.findChild<QAction*>(QStringLiteral("action-next-track"));
    QVERIFY(next != nullptr);
    QTRY_VERIFY(next->isEnabled());
    next->trigger();
    QTRY_COMPARE_WITH_TIMEOUT((*player)->state().source.raw_path, wanted, 5'000);
    QCOMPARE((*player)->queue().size(), 2U);
    QCOMPARE(model->rowCount(), 2);

    (*server)->stop();
}

// ADR-0220 Phase 2's whole point: the engine outlives the window. A window
// that only learns about playback by having started it shows nothing after a
// restart while the music is still going.
void EnginePlaybackTest::aNewWindowAttachesToWhatTheEngineIsPlaying() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto media = directory.filePath(QStringLiteral("played.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-long-flac.b64"), media));
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    core::StableId playing;
    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_path});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
        playing = model->rows().front().entry_id;
        emit view->doubleClicked(model->index(0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!(*player)->queue().empty(), 5'000);
        // The play follows the queue on the same worker, so the queue landing
        // does not mean the engine has started yet.
        if (!engine_started(*player)) {
            (*server)->stop();
            QSKIP("no audio output here, so the engine is not playing anything to attach to");
        }
        // The list has to be on disk for the next window to restore it, which
        // is debounced.
        QTest::qWait(1'500);
    }

    // The window is gone; the engine is still playing.
    QVERIFY(!(*player)->state().entry.is_nil());

    BenchMainWindow reopened;
    reopened.show();
    auto* tabs = reopened.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    // The restored list is anchored to the engine's entry, without inventing a
    // second tab for something already open.
    // The tab holding the engine's entry becomes the current one, because
    // attaching jumps to what is playing. Asserted on the anchor rather than
    // on the status: the fixture is under a second long, so by now the engine
    // has finished it, and "what was playing" is still what the window has to
    // point at.
    const auto anchored = [&reopened, &tabs, &playing] {
        for (auto* view : tabs->findChildren<QTableView*>()) {
            auto* model = qobject_cast<LocalListModel*>(view->model());
            if (model == nullptr || model->rowOfEntry(playing, -1) < 0) {
                continue;
            }
            // The row is marked as the playing occurrence, which only
            // attaching to the engine can do: nothing in a restore knows
            // which entry another process is playing.
            const auto row = model->rowOfEntry(playing, -1);
            return model->index(row, 0).data(ui::track_current_role).toBool() &&
                   tabs->currentWidget() == view &&
                   !reopened.property("trackknife-engine-playback").toString().isEmpty();
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(anchored(), 10'000);

    (*server)->stop();
}

// And when no open list holds it -- the list was deleted, or another client
// queued it -- the queue is the only record of what is playing, so it becomes
// one.
void EnginePlaybackTest::anEngineQueueNoListHoldsBecomesATab() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto media = directory.filePath(QStringLiteral("stranger.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-long-flac.b64"), media));
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
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_local_engine_socket_key),
                         QString::fromStdString(socket.string()));

    engine::QueueEntry queued;
    queued.source.raw_path = raw_path;
    (*player)->replace_queue({queued});
    if (!(*player)->play_entry(queued.entry_id)) {
        (*server)->stop();
        QSKIP("no audio output here, so the engine is not playing anything to attach to");
    }

    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    const auto listed = [&tabs, &queued] {
        for (auto* view : tabs->findChildren<QTableView*>()) {
            auto* model = qobject_cast<LocalListModel*>(view->model());
            if (model != nullptr && model->rowOfEntry(queued.entry_id, -1) >= 0) {
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(listed(), 10'000);

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
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);

    emit view->doubleClicked(model->index(0, 0));
    QTest::qWait(200);
    // ADR-0226: no engine, nothing plays -- the window has no player of its
    // own to fall back on, and says so rather than pretending.
    QCOMPARE(window.property("trackknife-engine-playback").toString(),
             QStringLiteral("unavailable"));
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("action-play-pause"))->isEnabled());
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::EnginePlaybackTest)
#include "engine_playback_test.moc"
