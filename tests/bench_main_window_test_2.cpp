// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"

#include <QGroupBox>
#include <QMessageBox>

#include <sqlite3.h>

namespace trackknife::bench {

// ADR-0234: pending removals are kept by engine key now; one an older release
// left as "l:<id>" is still carried out on connecting. ADR-0286: it named a
// working list, which the engine has set aside since -- so it is let go of,
// and touches no saved playlist.
void BenchMainWindowTest::aRemovalLeftPendingByAnOlderReleaseIsCarriedOut() {
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto kept = (*engine)->call(
        "list.save",
        protocol::Json{{"name", "Kept"}, {"kind", "saved"}, {"items", protocol::Json::array()}});
    QVERIFY(kept.has_value());
    const auto kept_id = kept->value("id", std::string{});
    const auto gone_id = core::StableId::random().to_string();
    QSettings{}.setValue(QStringLiteral("lists/pending-removals"),
                         QStringList{QStringLiteral("l:") + QString::fromStdString(gone_id)});
    const auto listed = [&engine](const std::string& id) {
        auto all = (*engine)->call("list.all");
        return all && std::ranges::any_of(all->value("lists", protocol::Json::array()),
                                          [&id](const protocol::Json& list) {
                                              return list.value("id", std::string{}) == id;
                                          });
    };
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(
        QSettings{}.value(QStringLiteral("lists/pending-removals")).toStringList().isEmpty());
    QVERIFY(listed(kept_id));
    QVERIFY((*engine)->call("list.delete", protocol::Json{{"id", kept_id}}).has_value());
    (*engine)->close();
}

// ADR-0234: an older release's remote lists said only "the remote". Reached,
// the remote says who it is: its lists take its id, which is remembered, and
// a restart finds them under it without asking again.
void BenchMainWindowTest::theRemotesListsTakeItsIdOnceItSaysIt() {
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    QFile id_file{remote_state.path() + QStringLiteral("/engine-id")};
    QVERIFY(id_file.open(QIODevice::ReadOnly));
    const auto id = QString::fromUtf8(id_file.readAll()).trimmed();
    QVERIFY(!id.isEmpty());
    QSettings{}.remove(QStringLiteral("library/engine-id"));
    std::string list_id;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
        auto* tab = window.remoteQueueTab();
        QVERIFY(tab != nullptr);
        list_id = tab->document.id.to_string();
        QTRY_COMPARE(QString::fromStdString(tab->document.engine), id);
        QCOMPARE(window.remoteEngine()->key.text(), id);
        QCOMPARE(engineOfView(tab->view).text(), id);
        QCOMPARE(window.remoteLibrary()->engine().text(), id);
        QCOMPARE(QSettings{}.value(QStringLiteral("library/engine-id")).toString(), id);
        window.persistNow(true);
        window.close();
    }
    // Kept under the id, not the placeholder (ADR-0259: as the window keeps
    // its tabs).
    {
        const auto kept = TabStore::loadState();
        const auto found = std::ranges::find(
            kept.tabs, list_id, [](const TabStore::Tab& tab) { return tab.id.to_string(); });
        QVERIFY(found != kept.tabs.end());
        QCOMPARE(QString::fromStdString(found->engine), id);
    }
    // Again: known by its id from the start, and its tab with it.
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY(window.remoteEngine() != nullptr);
    QCOMPARE(window.remoteEngine()->key.text(), id);
    auto* tab = window.tabForDocument(QString::fromStdString(list_id));
    QVERIFY(tab != nullptr);
    QCOMPARE(QString::fromStdString(tab->document.engine), id);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QCOMPARE(window.remoteQueueTab(), tab);
    QSettings{}.remove(QStringLiteral("library/engine-id"));
}

// The address now leads to another engine than the one whose id was kept --
// a NAS replaced, say. The link takes the new id; the lists of the engine it
// led to before stay that engine's rather than move to one without their files.
void BenchMainWindowTest::anotherEngineAtTheSameAddressLeavesTheOldOnesLists() {
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto before = QString::fromStdString(core::StableId::random().to_string());
    QSettings{}.setValue(QStringLiteral("library/engine-id"), before);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    // A list of the engine that was there before, open as the window starts.
    auto* old_tab =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Of the old NAS",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {},
                                                    .engine = before.toStdString()},
                          false);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QTRY_VERIFY(window.remoteEngine()->key.text() != before);
    QCOMPARE(QString::fromStdString(old_tab->document.engine), before);
    QCOMPARE(QSettings{}.value(QStringLiteral("library/engine-id")).toString(),
             window.remoteEngine()->key.text());
    // The remote's own tab is its own, not the old engine's list.
    QVERIFY(window.remoteQueueTab() != old_tab);
    QSettings{}.remove(QStringLiteral("library/engine-id"));
}

// ADR-0234: several engines elsewhere, kept as a list; the one remote an
// older release kept becomes its first, once, and the old keys keep naming
// the first for a release that reads only them.
void BenchMainWindowTest::remoteEnginesStartFromTheOneRemoteOfBefore() {
    QSettings settings;
    settings.remove(QStringLiteral("engines"));
    settings.setValue(QLatin1String(SettingsDialog::library_engine_socket_key),
                      QStringLiteral("192.0.2.7:6603"));
    settings.setValue(QLatin1String(SettingsDialog::library_engine_token_key), QString{});
    settings.setValue(QLatin1String(SettingsDialog::library_remote_folder_key),
                      QStringLiteral("/mnt/nas/Music"));
    settings.setValue(QLatin1String(SettingsDialog::library_remote_mount_key), QString{});
    settings.setValue(QLatin1String(SettingsDialog::library_engine_id_key), QStringLiteral("abc"));
    settings.sync();

    auto engines = loadRemoteEngines();
    QCOMPARE(engines.size(), std::size_t{1});
    QCOMPARE(engines.front().address, QStringLiteral("192.0.2.7:6603"));
    QCOMPARE(engines.front().music_folder, QStringLiteral("/mnt/nas/Music"));
    QCOMPARE(engines.front().id, QStringLiteral("abc"));

    engines.push_back({.address = QStringLiteral("198.51.100.3:6603"),
                       .password = QStringLiteral("other"),
                       .music_folder = QStringLiteral("/srv/music"),
                       .reachable_at = QStringLiteral("/media/galactica"),
                       .id = {}});
    saveRemoteEngines(engines);
    QCOMPARE(loadRemoteEngines(), engines);
    QCOMPARE(loadRemoteEngines().back().effectivePassword(), QStringLiteral("other"));

    rememberEngineId(QStringLiteral("198.51.100.3:6603"), QStringLiteral("def"));
    QCOMPARE(loadRemoteEngines().back().id, QStringLiteral("def"));

    // With none left, the old keys say so too.
    saveRemoteEngines({});
    QVERIFY(loadRemoteEngines().empty());
    QVERIFY(QSettings{}
                .value(QLatin1String(SettingsDialog::library_engine_socket_key))
                .toString()
                .isEmpty());
    QSettings{}.remove(QStringLiteral("engines"));
}

// ADR-0234: two engines elsewhere at once -- a desktop's library and a
// NAS's -- each by its own id, with its own library tab and its own lists,
// grouped after this computer's in the order Settings have them.
void BenchMainWindowTest::twoEnginesElsewhereSideBySide() {
    QTemporaryDir first_state;
    QTemporaryDir second_state;
    QVERIFY(first_state.isValid() && second_state.isValid());
    testing::TestEngine first;
    testing::TestEngine second;
    QVERIFY2(first.start(first_state.path().toStdString(), true), first.log().constData());
    QVERIFY2(second.start(second_state.path().toStdString(), true), second.log().constData());
    const auto id_of = [](const QTemporaryDir& state) {
        QFile file{state.path() + QStringLiteral("/engine-id")};
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed()
                                              : QString{};
    };
    const auto first_id = id_of(first_state);
    const auto second_id = id_of(second_state);
    QVERIFY(!first_id.isEmpty() && !second_id.isEmpty() && first_id != second_id);
    saveRemoteEngines({{.address = first.socket(),
                        .password = {},
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {}},
                       {.address = second.socket(),
                        .password = {},
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {}}});

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QCOMPARE(window.engines_.size(), std::size_t{3});
    QTRY_VERIFY(window.engines_[1]->playback->active() && window.engines_[2]->playback->active());
    QTRY_COMPARE(window.engines_[1]->key.text(), first_id);
    QTRY_COMPARE(window.engines_[2]->key.text(), second_id);
    // Each remembered, by its address.
    const auto kept = loadRemoteEngines();
    QCOMPARE(kept.size(), std::size_t{2});
    QCOMPARE(kept[0].id, first_id);
    QCOMPARE(kept[1].id, second_id);

    // A library tab each, found by the engine's key.
    QStringList keys;
    for (int index = 0; index < window.local_source_tabs_->count(); ++index) {
        keys << window.local_source_tabs_->tabData(index).toString();
    }
    QVERIFY2(keys.contains(first_id) && keys.contains(second_id), qPrintable(keys.join(u',')));
    QVERIFY(window.engines_[1]->library != window.engines_[2]->library);
    QCOMPARE(window.engines_[2]->library->engine().text(), second_id);

    // Each its own tab, of its own engine, after this computer's.
    auto* first_tab = window.engineTab(*window.engines_[1]);
    auto* second_tab = window.engineTab(*window.engines_[2]);
    QVERIFY(first_tab != nullptr && second_tab != nullptr && first_tab != second_tab);
    QCOMPARE(QString::fromStdString(first_tab->document.engine), first_id);
    QCOMPARE(QString::fromStdString(second_tab->document.engine), second_id);
    const auto at = [&window](const BenchMainWindow::ListTab* tab) {
        return window.tabs_->indexOf(tab->view);
    };
    QVERIFY(at(window.list_tabs_.front().get()) < at(first_tab));
    QVERIFY(at(first_tab) < at(second_tab));
    QCOMPARE(window.playbackOf(EngineKey::of(second_tab->document)), window.engines_[2]->playback);
    saveRemoteEngines({});
}

// ADR-0234: Settings list the engines elsewhere; the form below edits the
// one chosen, Add and Remove change the list, and what is saved is those
// with an address, in order.
void BenchMainWindowTest::settingsListTheEnginesElsewhere() {
    saveRemoteEngines({{.address = QStringLiteral("192.0.2.10:6603"),
                        .password = {},
                        .music_folder = QStringLiteral("/mnt/nas/Music"),
                        .reachable_at = {},
                        .id = QStringLiteral("nas-id")}});
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::engine);
    QVERIFY(dialog != nullptr);
    auto* list = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-engines"));
    auto* address = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-socket"));
    auto* folder = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-remote-folder"));
    auto* reachable = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-remote-mount"));
    auto* add = dialog->findChild<QPushButton*>(QStringLiteral("bench-settings-engine-add"));
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("bench-settings-engine-remove"));
    QVERIFY(list && address && folder && reachable && add && remove);
    // ADR-0286: this computer first, then the engines elsewhere, each with
    // its "holds the library" choice.
    const auto row = [list](const int index) {
        auto* holder = qobject_cast<QRadioButton*>(list->itemWidget(list->item(index)));
        return holder != nullptr ? holder->text() : QString{};
    };
    QCOMPARE(list->count(), 2);
    QVERIFY(row(0).startsWith(QStringLiteral("This computer")));
    QVERIFY(row(1).startsWith(QStringLiteral("192.0.2.10:6603")));
    QCOMPARE(address->text(), QStringLiteral("192.0.2.10:6603"));
    QCOMPARE(folder->text(), QStringLiteral("/mnt/nas/Music"));

    // One more, filled in.
    add->click();
    QCOMPARE(list->count(), 3);
    QVERIFY(address->text().isEmpty());
    address->setText(QStringLiteral("192.0.2.20:6603"));
    folder->setText(QStringLiteral("/srv/music"));
    reachable->setText(QStringLiteral("/media/desktop"));
    QVERIFY(row(2).startsWith(QStringLiteral("192.0.2.20:6603")));
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-engines.png")));
    }
    // Back to the first: its fields, untouched.
    list->setCurrentRow(1);
    QCOMPARE(address->text(), QStringLiteral("192.0.2.10:6603"));
    QVERIFY(reachable->text().isEmpty());
    // This computer chosen: nothing of an engine elsewhere to edit or remove.
    list->setCurrentRow(0);
    QVERIFY(!remove->isEnabled());
    QVERIFY(!address->isVisibleTo(dialog));
    list->setCurrentRow(2);
    QVERIFY(remove->isEnabled());
    // A third, left empty, and removed again.
    add->click();
    QCOMPARE(list->count(), 4);
    remove->click();
    QCOMPARE(list->count(), 3);
    // The second engine elsewhere holds the library; its folders are shown.
    auto* holder = qobject_cast<QRadioButton*>(list->itemWidget(list->item(2)));
    QVERIFY(holder != nullptr);
    holder->click();
    const auto folders_title = [dialog] {
        for (auto* box : dialog->findChildren<QGroupBox*>()) {
            if (box->title().startsWith(QStringLiteral("Music folders of "))) {
                return box->title();
            }
        }
        return QString{};
    };
    QCOMPARE(folders_title(), QStringLiteral("Music folders of 192.0.2.20:6603"));
    QVERIFY(row(2).endsWith(QStringLiteral("holds the library")));

    QPointer<SettingsDialog> lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    const auto saved = loadRemoteEngines();
    QCOMPARE(saved.size(), std::size_t{2});
    QCOMPARE(saved[0].address, QStringLiteral("192.0.2.10:6603"));
    QCOMPARE(saved[0].id, QStringLiteral("nas-id"));
    QCOMPARE(saved[1].address, QStringLiteral("192.0.2.20:6603"));
    QCOMPARE(saved[1].music_folder, QStringLiteral("/srv/music"));
    QCOMPARE(saved[1].reachable_at, QStringLiteral("/media/desktop"));
    QVERIFY(saved[1].id.isEmpty());
    QCOMPARE(QSettings{}.value(QLatin1String(SettingsDialog::library_engine_key)).toString(),
             QStringLiteral("192.0.2.20:6603"));
    QCOMPARE(libraryEngine(), QStringLiteral("192.0.2.20:6603"));
    QVERIFY(!localLibraryShown());
    QSettings{}.remove(QLatin1String(SettingsDialog::library_engine_key));
    saveRemoteEngines({});
}

// ADR-0286: one password for every engine. One of its own kept from before
// still works, is said under Advanced, and can go.
void BenchMainWindowTest::anEnginesOwnPasswordIsSaidAndCanGo() {
    QSettings{}.setValue(QLatin1String(SettingsDialog::engine_password_key),
                         QStringLiteral("shared"));
    saveRemoteEngines({{.address = QStringLiteral("192.0.2.10:6603"),
                        .password = QStringLiteral("its own"),
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {}}});
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::engine);
    QVERIFY(dialog != nullptr);
    auto* list = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-engines"));
    auto* own = dialog->findChild<QWidget*>(QStringLiteral("bench-settings-engine-own-password"));
    auto* drop =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-settings-engine-own-password-drop"));
    QVERIFY(list && own && drop);
    list->setCurrentRow(1);
    QVERIFY(own->isVisibleTo(own->parentWidget()));
    drop->click();
    QVERIFY(!own->isVisibleTo(own->parentWidget()));
    QPointer<SettingsDialog> lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    const auto saved = loadRemoteEngines();
    QCOMPARE(saved.size(), std::size_t{1});
    QVERIFY(saved[0].password.isEmpty());
    QCOMPARE(saved[0].effectivePassword(), QStringLiteral("shared"));
    QSettings{}.remove(QLatin1String(SettingsDialog::engine_password_key));
    saveRemoteEngines({});
}

// Engines added in Settings are connected, and those removed let go, at
// once -- no restart. A removed engine's lists stay, as an engine's that is
// not reached.
void BenchMainWindowTest::enginesAddedOrRemovedApplyAtOnce() {
    QTemporaryDir first_state;
    QTemporaryDir second_state;
    QVERIFY(first_state.isValid() && second_state.isValid());
    testing::TestEngine first;
    testing::TestEngine second;
    QVERIFY2(first.start(first_state.path().toStdString(), true), first.log().constData());
    QVERIFY2(second.start(second_state.path().toStdString(), true), second.log().constData());
    const RemoteEngineSetting first_setting{.address = first.socket(),
                                            .password = {},
                                            .music_folder = {},
                                            .reachable_at = {},
                                            .id = {}};
    const RemoteEngineSetting second_setting{.address = second.socket(),
                                             .password = {},
                                             .music_folder = {},
                                             .reachable_at = {},
                                             .id = {}};
    saveRemoteEngines({first_setting});
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QCOMPARE(window.engines_.size(), std::size_t{2});
    QTRY_VERIFY(window.engines_[1]->playback->active());
    const auto sidebar_keys = [&window] {
        QStringList keys;
        for (int index = 0; index < window.local_source_tabs_->count(); ++index) {
            keys << window.local_source_tabs_->tabData(index).toString();
        }
        return keys;
    };

    // Added: connected, with its library tab.
    saveRemoteEngines({first_setting, second_setting});
    window.syncRemoteEngines();
    QCOMPARE(window.engines_.size(), std::size_t{3});
    QTRY_VERIFY(window.engines_[2]->playback->active());
    QTRY_VERIFY(sidebar_keys().contains(window.engines_[2]->key.text()));
    const auto first_key = window.engines_[1]->key;
    auto* first_tab = window.engineTab(*window.engines_[1]);
    QVERIFY(first_tab != nullptr);
    const auto first_list = QString::fromStdString(first_tab->document.id.to_string());

    // Removed: let go -- its tab in the sidebar too -- its list kept.
    saveRemoteEngines({second_setting});
    window.syncRemoteEngines();
    QCOMPARE(window.engines_.size(), std::size_t{2});
    QVERIFY(window.link(first_key) == nullptr);
    QVERIFY(!sidebar_keys().contains(first_key.text()));
    QVERIFY(window.tabForDocument(first_list) != nullptr);
    QVERIFY(window.playbackOf(first_key) == nullptr);
    // The one left still works.
    QVERIFY(window.engines_[1]->playback->active());
    QCOMPARE(window.engines_[1]->setting.address, second.socket());
    saveRemoteEngines({});
}

// An engine on the network added without its password, and "the password
// above" filled in afterwards: pressing OK again connects with it, rather
// than keeping the connection made with none until Trackknife restarts.
void BenchMainWindowTest::aPasswordGivenLaterReachesTheEngine() {
    QTemporaryDir state;
    QTemporaryDir music;
    QVERIFY(state.isValid() && music.isValid());
    const auto port = [] {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        return probe.serverPort();
    }();
    const auto password = QStringLiteral("correct horse");
    QProcess engine;
    engine.setProgram(QStringLiteral(MELODY_ENGINE_BINARY));
    engine.setArguments({QStringLiteral("--socket"), state.filePath(QStringLiteral("melodyd.sock")),
                         QStringLiteral("--state"), state.path(), QStringLiteral("--listen"),
                         QStringLiteral("127.0.0.1:%1").arg(port), QStringLiteral("--password"),
                         password});
    engine.setProcessChannelMode(QProcess::MergedChannels);
    engine.start();
    QVERIFY(engine.waitForStarted());
    const auto address = QStringLiteral("127.0.0.1:%1").arg(port);
    QTRY_VERIFY_WITH_TIMEOUT(
        protocol::Client::connect(
            *protocol::Endpoint::parse(address.toStdString(), password.toStdString()))
            .has_value(),
        10'000);
    saveRemoteEngines({});
    QSettings{}.remove(QLatin1String(SettingsDialog::engine_password_key));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    const RemoteEngineSetting setting{
        .address = address, .password = {}, .music_folder = {}, .reachable_at = {}, .id = {}};
    saveRemoteEngines({setting});
    window.syncRemoteEngines();
    QCOMPARE(window.engines_.size(), std::size_t{2});
    // Refused: no password yet.
    QVERIFY(!window.engines_[1]->catalogue->open()->roots().has_value());
    QSettings{}.setValue(QLatin1String(SettingsDialog::engine_password_key), password);
    window.syncRemoteEngines();
    QCOMPARE(window.engines_.size(), std::size_t{2});
    auto& link = *window.engines_[1];
    const auto folder = QFile::encodeName(music.path()).toStdString();
    link.library->addRoot(folder);
    const auto roots = [&link] {
        std::vector<std::string> found;
        if (auto listed = link.catalogue->open()->roots()) {
            for (const auto& root : *listed) {
                found.push_back(root.raw_path);
            }
        }
        return found;
    };
    QTRY_VERIFY2_WITH_TIMEOUT(
        std::ranges::contains(roots(), folder),
        qPrintable(
            link.library->findChild<QLabel*>(QStringLiteral("local-library-status"))->text()),
        5'000);
    saveRemoteEngines({});
    QSettings{}.remove(QLatin1String(SettingsDialog::engine_password_key));
    engine.terminate();
    QVERIFY(engine.waitForFinished(5'000));
}

void BenchMainWindowTest::followPlaybackAndJumpRespectBrowsing() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& playing = *window.list_tabs_.front();
    LocalTrackRow row;
    row.raw_path = "/missing/cursor.flac";
    row.title = "Cursor track";
    row.probed = true;
    playing.model->replaceRows({row, row});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    tabs->setCurrentWidget(playing.view);
    window.playback_.anchors.document = playing.document.id;
    window.playback_.anchors.current = playing.model->rows().at(0).entry_id;
    window.playback_.row = 0;
    auto* follow = window.findChild<QAction*>(QStringLiteral("action-follow-playback"));
    auto* jump = window.findChild<QAction*>(QStringLiteral("action-jump-to-playing"));
    QVERIFY(follow && jump);
    follow->setChecked(true);
    QCOMPARE(playing.view->currentIndex().row(), 0);
    playing.view->selectRow(1);
    window.refreshPlaybackCursor();
    QCOMPARE(playing.view->currentIndex().row(), 1); // No reselection on every timer tick.
    window.playback_.anchors.current = playing.model->rows().at(1).entry_id;
    window.playback_.row = 1;
    playing.view->selectRow(0);
    window.refreshPlaybackCursor();
    QCOMPARE(playing.view->currentIndex().row(), 1);
    auto other = playing.document;
    other.id = core::StableId::random();
    other.name = "Browsing";
    auto* browsing = window.addListTab(std::move(other), true);
    QVERIFY(browsing);
    window.playback_.anchors.document = playing.document.id;
    window.playback_.anchors.current = playing.model->rows().at(0).entry_id;
    window.playback_.row = 0;
    window.refreshPlaybackCursor();
    QCOMPARE(tabs->currentWidget(), browsing->view);
    follow->setChecked(false);
    jump->trigger();
    QCOMPARE(tabs->currentWidget(), playing.view);
    QCOMPARE(playing.view->currentIndex().row(), 0);
    QCOMPARE(jump->shortcut(), QKeySequence(QStringLiteral("Ctrl+J")));

    QVERIFY(!QSettings{}.value(QStringLiteral("workspace/follow-playback")).toBool());
}

void BenchMainWindowTest::activeTabAccentSurvivesThemeTextColor() {
    class WhiteLabelStyle final : public QProxyStyle {
      public:
        WhiteLabelStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}
        void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                         const QWidget* widget = nullptr) const override {
            if (element == CE_TabBarTabShape) {
                painter->fillRect(option->rect, Qt::black);
            } else if (element == CE_TabBarTabLabel) {
                painter->setPen(Qt::white);
                painter->drawText(option->rect, Qt::AlignCenter, QStringLiteral("Theme label"));
            } else {
                QProxyStyle::drawControl(element, option, painter, widget);
            }
        }
    };
    PlaybackTabBar bar;
    auto* style = new WhiteLabelStyle;
    style->setParent(&bar);
    bar.setStyle(style);
    auto palette = bar.palette();
    const QColor accent(240, 30, 160);
    palette.setColor(QPalette::Highlight, accent);
    bar.setPalette(palette);
    bar.addTab(QStringLiteral("Album"));
    bar.addTab(QStringLiteral("Browsing"));
    bar.resize(360, 40);
    bar.setTabIcon(0, playbackSpeakerIcon(palette));
    bar.setTabData(0, true);
    const auto colored_pixels = [&] {
        const auto image = bar.grab().toImage();
        int count = 0;
        const auto rect = bar.tabRect(0).adjusted(0, 0, 0, -4);
        for (int y = rect.top(); y <= rect.bottom(); ++y)
            for (int x = rect.left(); x <= rect.right(); ++x)
                if (const auto pixel = image.pixelColor(x, y);
                    pixel.red() > 80 && pixel.blue() > 40 && pixel.green() < pixel.red() / 3)
                    ++count;
        return count;
    };
    QVERIFY(colored_pixels() > 10);
    bar.setCurrentIndex(1);
    QVERIFY(colored_pixels() > 10);
    // The tab being browsed is filled with the list's ground; the others
    // are not, whichever is playing.
    const auto image = bar.grab().toImage();
    const auto ground = bar.palette().color(QPalette::Base);
    QCOMPARE(image.pixelColor(bar.tabRect(1).center().x(), bar.tabRect(1).bottom() - 1), ground);
    QVERIFY(image.pixelColor(bar.tabRect(0).center().x(), bar.tabRect(0).bottom() - 1) != ground);
    bar.setTabData(0, false);
    bar.setTabIcon(0, QIcon{});
    QCOMPARE(colored_pixels(), 0);
}

// The list tabs are sized and their close buttons placed by the bar, not the
// style: the same under any, each tab with room for its whole text and the
// button on its right, after it.
void BenchMainWindowTest::listTabsMeasureTheSameUnderEveryStyle() {
    const auto previous = QApplication::style()->name();
    const auto measure = [](const QString& style) {
        QApplication::setStyle(QStyleFactory::create(style));
        PlaybackTabWidget tabs;
        tabs.setDocumentMode(true);
        tabs.addTab(new QWidget, QStringLiteral("Local *"));
        const auto playing = tabs.addTab(new QWidget, playbackSpeakerIcon(tabs.palette()),
                                         QStringLiteral("gemenon *"));
        tabs.tabBar()->setTabData(playing, true);
        tabs.setTabText(playing, tabs.tabText(playing));
        tabs.resize(700, 60);
        tabs.show();
        auto* bar = tabs.tabBar();
        std::vector<std::pair<QRect, QRect>> found;
        for (int index = 0; index < bar->count(); ++index) {
            auto* close = bar->tabButton(index, QTabBar::RightSide);
            const auto tab = bar->tabRect(index);
            if (close == nullptr || bar->tabButton(index, QTabBar::LeftSide) != nullptr) {
                return std::vector<std::pair<QRect, QRect>>{};
            }
            // Room for the text, and the button after it.
            const auto text = bar->fontMetrics().horizontalAdvance(bar->tabText(index));
            if (close->geometry().left() < tab.left() + text + 12 ||
                close->geometry().right() > tab.right()) {
                return std::vector<std::pair<QRect, QRect>>{};
            }
            found.emplace_back(tab, close->geometry());
        }
        return found;
    };
    const auto fusion = measure(QStringLiteral("Fusion"));
    const auto windows = measure(QStringLiteral("Windows"));
    QApplication::setStyle(QStyleFactory::create(previous));
    QCOMPARE(fusion.size(), std::size_t{2});
    QVERIFY(fusion == windows);
}

void BenchMainWindowTest::doubleClickOnEmptyTabBarMakesAList() {
    BenchMainWindow window;
    window.resize(1200, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    auto* bar = tabs->tabBar();
    const auto before = tabs->count();
    const auto prompt = [&window] {
        for (auto* dialog : window.findChildren<QInputDialog*>())
            if (dialog->isVisible())
                return dialog;
        return static_cast<QInputDialog*>(nullptr);
    };

    // On a tab it is no new list.
    QTest::mouseDClick(bar, Qt::LeftButton, {}, bar->tabRect(0).center());
    QTest::qWait(50);
    QVERIFY(prompt() == nullptr);
    QCOMPARE(tabs->count(), before);

    // Beside the tabs it is, named as File -> New list names one.
    QTimer answer;
    connect(&answer, &QTimer::timeout, &window, [&prompt] {
        if (auto* dialog = prompt()) {
            dialog->setTextValue(QStringLiteral("Fresh"));
            dialog->accept();
        }
    });
    answer.start(10);
    const QPoint beside(tabs->width() - 8, bar->geometry().center().y());
    QVERIFY(bar->tabAt(bar->mapFrom(tabs, beside)) < 0);
    QTest::mouseDClick(tabs, Qt::LeftButton, {}, beside);
    QTRY_COMPARE(tabs->count(), before + 1);
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("Fresh"));
}

void BenchMainWindowTest::activePlaybackTabRemainsMarkedWhileBrowsing() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& local = *window.list_tabs_.front();
    window.setActiveLocalList(QString::fromStdString(local.document.id.to_string()));
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    const auto index = tabs->indexOf(local.view);
    // Marked by the bar's dot; the icon is left to say which engine a tab
    // plays on, and this one plays here.
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    QVERIFY(tabs->tabIcon(index).isNull());
    QVERIFY(tabs->tabToolTip(index).contains(QStringLiteral("Active playback queue")));
    window.refreshUpNext();
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    window.stop_action_->trigger();
    window.playback_.anchors.document = core::StableId{};
    window.refreshTransport();
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    auto other = local.document;
    other.id = core::StableId::random();
    other.name = "Another list";
    auto* next = window.addListTab(std::move(other), true);
    QVERIFY(next != nullptr);
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    window.setActiveLocalList(QString::fromStdString(next->document.id.to_string()));
    QVERIFY(!tabs->tabBar()->tabData(index).toBool());
    QVERIFY(tabs->tabBar()->tabData(tabs->indexOf(next->view)).toBool());
}

// ADR-0226: the buffer is the engine's. The window shows what the engine
// reports, and a restarted window finds the engine's choice, not its own.
void BenchMainWindowTest::playbackBufferProfilesPersistAndExposeDiagnostics() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.playingOnEngine());

    auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-buffer-menu"));
    auto* responsive = window.findChild<QAction*>(QStringLiteral("action-buffer-responsive"));
    auto* balanced = window.findChild<QAction*>(QStringLiteral("action-buffer-balanced"));
    auto* resilient = window.findChild<QAction*>(QStringLiteral("action-buffer-resilient"));
    auto* custom = window.findChild<QAction*>(QStringLiteral("action-buffer-custom"));
    auto* device = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    QVERIFY(menu != nullptr);
    QVERIFY(responsive != nullptr);
    QVERIFY(balanced != nullptr);
    QVERIFY(resilient != nullptr);
    QVERIFY(custom != nullptr);
    QVERIFY(device != nullptr);
    // An engine that has never been told starts balanced.
    QTRY_VERIFY(balanced->isChecked());
    responsive->trigger();
    QTRY_VERIFY(responsive->isChecked());
    QVERIFY(!balanced->isChecked());
    QCOMPARE(responsive->toolTip(), QStringLiteral("250 ms capacity; playback starts at 50 ms"));
    QCOMPARE(custom->text(), QStringLiteral("Custom…"));
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 250);
    QVERIFY(!window.property("trackknife-player-buffer-pending").toBool());
    QCOMPARE(window.property("trackknife-player-underruns").toULongLong(), 0ULL);

    resilient->trigger();
    QVERIFY(resilient->isChecked());
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 2'000);
    QTRY_VERIFY(device->toolTip().contains(QStringLiteral("Resilient")));
    QVERIFY(device->toolTip().contains(QStringLiteral("Underruns: 0")));

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-profile")).toString(),
             QStringLiteral("resilient"));
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-capacity-ms")).toInt(), 2'000);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-start-threshold-ms")).toInt(), 250);

    custom->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    auto* profile = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-buffer-profile"));
    QCOMPARE(profile->currentData().toString(), QStringLiteral("custom"));
    auto* capacity = dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-capacity"));
    auto* threshold =
        dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-threshold"));
    QVERIFY(capacity && threshold);
    capacity->setValue(1234);
    threshold->setValue(234);
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QVERIFY(custom->isChecked());
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 1'234);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-profile")).toString(),
             QStringLiteral("custom"));
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-capacity-ms")).toInt(), 1'234);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-start-threshold-ms")).toInt(), 234);
    QVERIFY(window.close());

    BenchMainWindow restored;
    restored.show();
    auto* restored_custom = restored.findChild<QAction*>(QStringLiteral("action-buffer-custom"));
    QVERIFY(restored_custom != nullptr);
    QVERIFY(restored_custom->isChecked());
    QTRY_COMPARE(restored.property("trackknife-player-buffer-capacity-ms").toLongLong(), 1'234);
}

void BenchMainWindowTest::statusBarSummarizesTrackSelection() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first_path = media.filePath(QStringLiteral("selection-first.wav"));
    const auto second_path = media.filePath(QStringLiteral("selection-second.wav"));
    write_wave(first_path, wave_sample_rate / 10U);
    write_wave(second_path, wave_sample_rate / 10U);
    const auto first_encoded = QFile::encodeName(first_path);
    const auto second_encoded = QFile::encodeName(second_path);
    const std::string first_raw{first_encoded.constData(),
                                static_cast<std::size_t>(first_encoded.size())};
    const std::string second_raw{second_encoded.constData(),
                                 static_cast<std::size_t>(second_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({first_raw, second_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("bench-selection-status"));
    QVERIFY(tabs != nullptr);
    QVERIFY(status != nullptr);
    QCOMPARE(status->accessibleName(), QStringLiteral("Selected track information"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);

    LocalTrackRow first_metadata{
        .raw_path = {},
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "First title",
        .artist = "First artist",
        .album = "First album",
        .album_artist = {},
        .date = "2026",
        .track_number = {},
        .duration_ms = 61'000,
        .metadata = {},
        .source_revision = std::nullopt,
    };
    LocalTrackRow second_metadata{
        .raw_path = {},
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "Second title",
        .artist = "Second artist",
        .album = "Second album",
        .album_artist = {},
        .date = "2025",
        .track_number = {},
        .duration_ms = 120'000,
        .metadata = {},
        .source_revision = std::nullopt,
    };
    QVERIFY(model->applyMetadata(first_raw, 0, std::move(first_metadata)));
    QVERIFY(model->applyMetadata(second_raw, 1, std::move(second_metadata)));

    view->selectionModel()->select(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QCOMPARE(status->text(),
             QStringLiteral("First artist — First title · First album (2026) · 1:01"));
    QCOMPARE(status->toolTip(), QString::fromStdString(core::display_raw_path(first_raw)));

    view->selectionModel()->select(model->index(1, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(status->text(), QStringLiteral("2 tracks selected · 3:01 total"));

    view->clearSelection();
    QCOMPARE(status->text(), QStringLiteral("No tracks selected"));
}

void BenchMainWindowTest::committedMetadataRefreshesDuplicatesAndPreservesCueOverlay() {
    const std::string source{"/music/shared.flac"};
    const auto field = [](std::string canonical_name, std::string value,
                          const metadata::FieldProvenance provenance) {
        return metadata::MetadataField{
            .canonical_name = canonical_name,
            .native_name = canonical_name,
            .values = {std::move(value)},
            .qualifier = {},
            .provenance = provenance,
        };
    };
    LocalTrackRow whole{
        .raw_path = source,
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "Old",
        .artist = {},
        .album = "Old album",
        .album_artist = {},
        .date = {},
        .track_number = {},
        .duration_ms = 1'000,
        .metadata =
            metadata::MetadataDocument{
                .fields = {field("title", "Old", metadata::FieldProvenance::embedded),
                           field("album", "Old album", metadata::FieldProvenance::embedded)},
                .unsupported_native_objects = {}},
        .source_revision = std::nullopt,
        .probed = true,
    };
    auto cue = whole;
    cue.logical_reference = std::string{"cue-v1\0sheet\0track", 18U};
    cue.segment = formats::SampleRange{.start_sample = 0, .end_sample = 44'100};
    cue.title = "CUE title";
    cue.metadata.fields.push_back(field("title", "CUE title", metadata::FieldProvenance::sidecar));
    LocalTrackRow unrelated = whole;
    unrelated.raw_path = "/music/other.flac";
    unrelated.title = "Other";
    unrelated.metadata.fields.front().values = {"Other"};

    LocalListModel model;
    model.replaceRows({whole, cue, unrelated});
    const QPersistentModelIndex playing{model.index(2, 0)};
    const QPersistentModelIndex duplicate{model.index(1, 0)};
    QSignalSpy resets{&model, &QAbstractItemModel::modelReset};
    QSignalSpy changes{&model, &QAbstractItemModel::dataChanged};
    const metadata::MetadataDocument committed{
        .fields = {field("title", "New", metadata::FieldProvenance::embedded),
                   field("album", "New album", metadata::FieldProvenance::embedded)},
        .unsupported_native_objects = {},
    };
    const core::LocalSourceRevision revision{.device = 1,
                                             .inode = 2,
                                             .size = 3,
                                             .modification_time_seconds = 4,
                                             .modification_time_nanoseconds = 5};
    const auto refreshed = model.applyCommittedMetadata(source, committed, revision);
    QVERIFY(refreshed.has_value());
    QCOMPARE(*refreshed, 2U);
    QCOMPARE(model.rows()[0].title, std::string{"New"});
    QCOMPARE(model.rows()[0].album, std::string{"New album"});
    QCOMPARE(model.rows()[1].title, std::string{"CUE title"});
    QCOMPARE(model.rows()[1].album, std::string{"New album"});
    QCOMPARE(model.rows()[1].metadata.fields.back().provenance, metadata::FieldProvenance::sidecar);
    QCOMPARE(model.rows()[0].source_revision, std::optional{revision});
    QCOMPARE(model.rows()[2].title, std::string{"Other"});
    QVERIFY(playing.isValid());
    QVERIFY(duplicate.isValid());
    QCOMPARE(playing.row(), 2);
    QCOMPARE(duplicate.row(), 1);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(changes.count(), 2);

    model.setCurrentSource(model.source(1), 1);
    const core::LocalSourceRevision relocated_revision{.device = 6,
                                                       .inode = 7,
                                                       .size = 8,
                                                       .modification_time_seconds = 9,
                                                       .modification_time_nanoseconds = 10};
    const std::string relocated{"/archive/shared.flac"};
    const auto relocated_rows =
        model.applyCommittedRelocation(source, relocated, revision, relocated_revision);
    QVERIFY(relocated_rows.has_value());
    QCOMPARE(*relocated_rows, 2U);
    QCOMPARE(model.rows()[0].raw_path, relocated);
    QCOMPARE(model.rows()[1].raw_path, relocated);
    QCOMPARE(model.rows()[0].source_revision, std::optional{relocated_revision});
    QCOMPARE(model.rows()[1].logical_reference, cue.logical_reference);
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/other.flac"});
    QVERIFY(model.data(model.index(1, 0), ui::track_current_role).toBool());
    QVERIFY(playing.isValid());
    QVERIFY(duplicate.isValid());
    QCOMPARE(resets.count(), 0);

    auto legacy = cue;
    legacy.metadata.fields = {
        field("title", "Flattened CUE", metadata::FieldProvenance::cached_snapshot)};
    model.replaceRows({legacy});
    const auto rejected = model.applyCommittedMetadata(source, committed, revision);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, core::ErrorCode::conflict);
    QCOMPARE(model.rows()[0], legacy);
}

// ADR-0183 addendum: while a tag editor tab is active, its file list is
// hosted as a temporary Files page in the local sources sidebar; leaving
// or closing the editor returns the widget and the sidebar state.
void BenchMainWindowTest::propertiesFileListLivesInTheTaggerWindow() {
    // ADR-0221: the tagger is a window and owns its file list. The list sits
    // beside the field table rather than above it, shows paths relative to the
    // selection's common folder, and keeps the checkbox selection that scopes
    // edits. None of that depends on the workspace sidebar any more.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto file = directory.filePath(QStringLiteral("hosted.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), file));
    const auto encoded = QFile::encodeName(file);

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(
        {std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    auto* source_tabs = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(tabs != nullptr && properties_action != nullptr && source_tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* list_view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(list_view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(list_view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE(list_model->rowCount(), 1);
    QTRY_VERIFY(list_model->rows().front().probed);
    list_view->selectionModel()->select(
        list_model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    const auto sidebar_tabs_before = source_tabs->count();
    properties_action->trigger();
    auto* properties =
        window.findChild<MetadataPropertiesDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);

    // The workspace is untouched: no extra sidebar page, no extra tab.
    QCOMPARE(source_tabs->count(), sidebar_tabs_before);
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget*>(list_view));

    // The grid, and with it the file list, builds asynchronously.
    QTRY_VERIFY(properties->fileListView() != nullptr);
    auto* files_view = properties->fileListView();
    QVERIFY(properties->isAncestorOf(files_view));
    QTRY_VERIFY(files_view->isVisible());
    QCOMPARE(window.findChildren<QTableView*>(QStringLiteral("bench-metadata-files")).size(), 1);

    // Beside the fields, not above them, so the field table gets full height.
    auto* splitter = properties->findChild<QSplitter*>(QStringLiteral("bench-metadata-splitter"));
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);

    // Checkbox selection scopes the edit: click the indicator or press Space.
    auto* selection = files_view->selectionModel();
    QCOMPARE(selection->selectedRows().size(), 1);
    const auto cell = files_view->visualRect(files_view->model()->index(0, 0));
    QTest::mouseClick(files_view->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(cell.left() + 14, cell.center().y()));
    QCOMPARE(selection->selectedRows().size(), 0);
    QTest::keyClick(files_view, Qt::Key_Space);
    QCOMPARE(selection->selectedRows().size(), 1);
    if (const auto screenshots = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !screenshots.isEmpty()) {
        QVERIFY(
            properties->grab().save(screenshots + QStringLiteral("/tag-window-checkboxes.png")));
    }

    // The breadcrumb carries the common folder and belongs to the tagger.
    auto* crumb = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-files-dir"));
    QVERIFY(crumb != nullptr);
    QVERIFY(properties->isAncestorOf(crumb));
    QTRY_VERIFY(crumb->text().endsWith(QLatin1Char('/')));
    QVERIFY(!crumb->text().contains(QStringLiteral("hosted.flac")));

    // A second tagger stands alongside the first, each owning its own view and
    // an independent selection -- which is what windows buy over a single tab.
    selection->clearSelection();
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    MetadataPropertiesDialog* second = nullptr;
    for (auto* candidate : window.findChildren<MetadataPropertiesDialog*>(
             QStringLiteral("bench-metadata-properties"))) {
        if (candidate != properties) {
            second = candidate;
        }
    }
    QVERIFY(second != nullptr);
    QVERIFY(second->isWindow());
    QTRY_VERIFY(second->fileListView());
    auto* second_view = second->fileListView();
    QVERIFY(second->isAncestorOf(second_view));
    QCOMPARE(second_view->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(window.findChildren<QTableView*>(QStringLiteral("bench-metadata-files")).size(), 2);
    QCOMPARE(selection->selectedRows().size(), 0);

    // Closing one tears down only its own view; the other is unaffected.
    QPointer<QTableView> second_lifetime = second_view;
    QVERIFY(second->close());
    QTRY_VERIFY(second_lifetime.isNull());
    QVERIFY(properties->isAncestorOf(files_view));
    QCOMPARE(files_view->selectionModel(), selection);

    QPointer<QTableView> lifetime = files_view;
    QVERIFY(properties->close());
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(source_tabs->count(), sidebar_tabs_before);
}

void BenchMainWindowTest::metadataReadyPlanAppliesAndRefreshesHistory() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("apply-ready.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto encoded = QFile::encodeName(source_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr);
    QVERIFY(properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY(!window.property("trackknife-metadata-operation-running").toBool());
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();

    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    QCOMPARE(properties->property("trackknife-file-work").toString(), QStringLiteral("engine"));
    // ADR-0221: a window, so the tab strip is untouched and the list the
    // selection came from stays current.
    QVERIFY(properties->isWindow());
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<MetadataPropertiesDialog*>(tabs->currentWidget()) == nullptr);
    QVERIFY(properties->windowTitle().startsWith(QStringLiteral("Edit tags · 1 track")));
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(preview != nullptr);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    const auto title_draft = aggregate_model->index(*title_row, 2);
    QVERIFY(aggregate_model->setData(title_draft, QStringLiteral("Applied from ready preview"),
                                     Qt::EditRole));
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // Direct apply: a ready plan runs immediately with inline progress; no
    // review or result dialog appears; the tagger stays open to undo it, and is closed.
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().title,
                              std::string{"Applied from ready preview"}, 5'000);
    const auto reread = metadata::read_local_metadata(raw_path);
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.effective_values("title"),
             (std::vector<std::string>{"Applied from ready preview"}));
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QVERIFY(window_test::closeTaggerAfterWrite(window));
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
}

// ADR-0289: a write from the tagger is undone while it is open; closed, it
// lets go of what it kept, and the engine keeps nothing.
void BenchMainWindowTest::aTaggerUndoesItsWriteUntilClosed() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("undo-write.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto encoded = QFile::encodeName(source_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto before = metadata::read_local_metadata(raw_path);
    QVERIFY(before.has_value());
    const auto original_title = before->document.effective_values("title");
    const auto database =
        std::filesystem::path{qEnvironmentVariable("MELODY_STATE_DIR").toStdString()} /
        "lists.sqlite";

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    QPointer<QDialog> properties =
        window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(!properties.isNull());
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo-write"));
    QVERIFY(aggregate_model != nullptr && apply != nullptr && undo != nullptr);
    QVERIFY(!undo->isVisible());
    const auto write_title = [&](const QString& title) {
        const auto row = aggregate_model->fieldRow(QStringLiteral("title"));
        QVERIFY(row.has_value());
        QVERIFY(aggregate_model->setData(aggregate_model->index(*row, 2), title, Qt::EditRole));
        QTRY_VERIFY(apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(undo->isVisible() && undo->isEnabled(), 10'000);
    };

    // Written, and kept to undo: the window stays.
    write_title(QStringLiteral("Written to be undone"));
    QVERIFY(!properties.isNull() && properties->isVisible());
    QCOMPARE(metadata::read_local_metadata(raw_path)->document.effective_values("title"),
             (std::vector<std::string>{"Written to be undone"}));
    QCOMPARE(engine::kept_undo_copies(database).size(), 1U);

    // Undone: the file as it was, the row too, nothing more to undo.
    QTest::mouseClick(undo, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!undo->isVisible(), 10'000);
    QCOMPARE(metadata::read_local_metadata(raw_path)->document.effective_values("title"),
             original_title);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().title,
                              original_title.empty() ? std::string{} : original_title.front(),
                              5'000);
    QVERIFY(engine::kept_undo_copies(database).empty());

    // Written again and closed: kept no longer.
    write_title(QStringLiteral("Written and kept"));
    QCOMPARE(engine::kept_undo_copies(database).size(), 1U);
    properties->close();
    QTRY_VERIFY(properties.isNull() || !properties->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(engine::kept_undo_copies(database).empty(), 10'000);
    QCOMPARE(metadata::read_local_metadata(raw_path)->document.effective_values("title"),
             (std::vector<std::string>{"Written and kept"}));
}

// Rename ticked, the file already named as the layout names it: nothing
// moves, so its folder cover, tags and all are written -- not refused as for
// files moving away from their cover (ADR-0184).
void BenchMainWindowTest::aFileAlreadyWhereRenamingPutsItGetsItsCover() {
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_embed_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_name_key),
                         QStringLiteral("cover.jpg"));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto staging = media.filePath(QStringLiteral("staging.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), staging));
    const auto read = metadata::read_local_metadata(QFile::encodeName(staging).toStdString());
    QVERIFY(read.has_value());
    const auto titles = read->document.effective_values("title");
    QVERIFY(!titles.empty());
    // Named as the layout names it.
    const auto source_path =
        media.filePath(QString::fromStdString(titles.front()) + QStringLiteral(".flac"));
    QVERIFY(QFile::rename(staging, source_path));
    const auto raw_path = QFile::encodeName(source_path).toStdString();
    const auto cover_file = media.filePath(QStringLiteral("picked.png"));
    QImage picked{40, 40, QImage::Format_RGB32};
    picked.fill(Qt::darkYellow);
    QVERIFY(picked.save(cover_file, "PNG"));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "By title",
                    .dialect = {},
                    .relative_directory_expression = {},
                    .basename_expression = "%title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();
    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* artwork = properties->findChild<ArtworkSession*>();
    QVERIFY(aggregate_model && rename_files && apply && status && artwork);
    QTRY_VERIFY(rename_files->isEnabled());
    const auto album_row = aggregate_model->fieldRow(QStringLiteral("album"));
    QVERIFY(album_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*album_row, 2),
                                     QStringLiteral("Kept in place"), Qt::EditRole));
    // A cover staged: as Fetch cover… or Add image… stage one.
    artwork->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->canAdd(), 10'000);
    artwork->add(cover_file, 0);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->hasPendingChanges(), 10'000);
    rename_files->setChecked(true);
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    // Not refused: the folder cover is offered for review, as always.
    QDialog* review = nullptr;
    QTRY_VERIFY2_WITH_TIMEOUT((review = properties->findChild<QDialog*>(
                                   QStringLiteral("bench-folder-cover-review"))) != nullptr,
                              qPrintable(status->text()), 15'000);
    review->findChild<QDialogButtonBox*>(QStringLiteral("bench-folder-cover-review-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    // The cover beside the file, the tag in it, the name kept.
    const auto cover = media.filePath(QStringLiteral("cover.jpg"));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(cover), qPrintable(status->text()), 15'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const auto after = metadata::read_local_metadata(raw_path);
            return after && after->document.effective_values("album") ==
                                std::vector<std::string>{"Kept in place"};
        }(),
        10'000);
    QVERIFY(QFile::exists(source_path));
    QVERIFY(!status->text().contains(QStringLiteral("Save folder covers")));
    QPointer guard{properties};
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
}

// ADR-0318: an album moved with a folder cover staged -- its old cover
// travels along and is replaced by the new one in the folder it moved to,
// the old folder gone; undone, the old cover is back where it was.
void BenchMainWindowTest::aMovedAlbumGetsItsFolderCoverWhereItMoved_data() {
    QTest::addColumn<bool>("undone");
    QTest::newRow("undone") << true;
    QTest::newRow("kept") << false;
}

void BenchMainWindowTest::aMovedAlbumGetsItsFolderCoverWhereItMoved() {
    QFETCH(bool, undone);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_embed_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_name_key),
                         QStringLiteral("cover.jpg"));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QVERIFY(QDir{media.path()}.mkpath(QStringLiteral("Old")));
    QVERIFY(QDir{media.path()}.mkpath(QStringLiteral("Sorted")));
    const auto staging = media.filePath(QStringLiteral("Old/staging.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), staging));
    const auto read = metadata::read_local_metadata(QFile::encodeName(staging).toStdString());
    QVERIFY(read.has_value());
    const auto title = QString::fromStdString(read->document.effective_values("title").front());
    const auto source_path =
        media.filePath(QStringLiteral("Old/") + title + QStringLiteral(".flac"));
    QVERIFY(QFile::rename(staging, source_path));
    const auto raw_path = QFile::encodeName(source_path).toStdString();
    QImage old_cover{30, 30, QImage::Format_RGB32};
    old_cover.fill(Qt::red);
    QVERIFY(old_cover.save(media.filePath(QStringLiteral("Old/cover.jpg")), "JPEG"));
    const auto picked_file = media.filePath(QStringLiteral("picked.png"));
    QImage picked{40, 40, QImage::Format_RGB32};
    picked.fill(Qt::yellow);
    QVERIFY(picked.save(picked_file, "PNG"));
    const auto colour_of = [](const QString& path) { return QImage{path}.pixelColor(10, 10); };

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "Moved",
                    .dialect = {},
                    .relative_directory_expression = "Album",
                    .basename_expression = "%title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();
    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    TaggerSession* session = nullptr;
    ArtworkSession* artwork = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((session = properties->findChild<TaggerSession*>()) != nullptr &&
                                 (artwork = properties->findChild<ArtworkSession*>()) != nullptr,
                             10'000);
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(apply && status);
    QTRY_VERIFY_WITH_TIMEOUT(session->renameAvailable(), 10'000);
    session->chooseMoveFolder(
        QFile::encodeName(media.filePath(QStringLiteral("Sorted"))).toStdString());
    QTRY_VERIFY_WITH_TIMEOUT(session->moveAvailable(), 10'000);
    session->chooseMove(true);
    artwork->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->canAdd(), 10'000);
    artwork->add(picked_file, 0);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->hasPendingChanges(), 10'000);
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    QDialog* review = nullptr;
    QTRY_VERIFY2_WITH_TIMEOUT((review = properties->findChild<QDialog*>(
                                   QStringLiteral("bench-folder-cover-review"))) != nullptr,
                              qPrintable(status->text()), 15'000);
    review->findChild<QDialogButtonBox*>(QStringLiteral("bench-folder-cover-review-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    const auto moved =
        media.filePath(QStringLiteral("Sorted/Album/") + title + QStringLiteral(".flac"));
    const auto cover = media.filePath(QStringLiteral("Sorted/Album/cover.jpg"));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(moved), qPrintable(status->text()), 15'000);
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(cover) && colour_of(cover).green() > 200 &&
                                  colour_of(cover).red() > 200,
                              qPrintable(status->text()), 10'000);
    // Nothing of the album left where it was: the file it moved from is kept
    // hidden, to undo the move while the tagger is open.
    QVERIFY(QDir{media.filePath(QStringLiteral("Old"))}
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty());
    if (!undone) {
        // Closed, the kept file let go of, and the folder with it.
        QPointer kept_guard{properties};
        QVERIFY(window_test::closeTaggerAfterWrite(kept_guard));
        QTRY_VERIFY_WITH_TIMEOUT(!QDir{media.filePath(QStringLiteral("Old"))}.exists(), 10'000);
        return;
    }
    // Undone: the album back, with its own cover.
    QTRY_VERIFY_WITH_TIMEOUT(session->canUndoWrite(), 10'000);
    session->undoWrite();
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(source_path), qPrintable(status->text()), 15'000);
    const auto old_back = media.filePath(QStringLiteral("Old/cover.jpg"));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(old_back) && colour_of(old_back).red() > 200 &&
                                  colour_of(old_back).green() < 60,
                              qPrintable(status->text()), 10'000);
    QPointer guard{properties};
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
}

// A file shown without a cover -- it had none -- that is given one as it is
// moved shows it in its tab: the miss cached before does not outlive the
// write.
void BenchMainWindowTest::aCoverWrittenWithAMoveShowsInItsTab() {
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_embed_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_key), false);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QVERIFY(QDir{media.path()}.mkpath(QStringLiteral("Old")));
    QVERIFY(QDir{media.path()}.mkpath(QStringLiteral("Sorted")));
    const auto staging = media.filePath(QStringLiteral("Old/staging.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), staging));
    const auto read = metadata::read_local_metadata(QFile::encodeName(staging).toStdString());
    QVERIFY(read.has_value());
    const auto title = QString::fromStdString(read->document.effective_values("title").front());
    const auto source_path =
        media.filePath(QStringLiteral("Old/") + title + QStringLiteral(".flac"));
    QVERIFY(QFile::rename(staging, source_path));
    const auto picked_file = media.filePath(QStringLiteral("picked.png"));
    QImage picked{40, 40, QImage::Format_RGB32};
    picked.fill(Qt::yellow);
    QVERIFY(picked.save(picked_file, "PNG"));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "Moved",
                    .dialect = {},
                    .relative_directory_expression = "Album",
                    .basename_expression = "%title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);
    window.openLocalPaths({QFile::encodeName(source_path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    // Looked for, and none: the miss is cached.
    const auto key = list_model->groupKey(0);
    QTRY_VERIFY_WITH_TIMEOUT(window.workspace_.artwork_cache_.contains(key), 10'000);
    QVERIFY(!list_model->hasArtwork(key));

    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();
    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    TaggerSession* session = nullptr;
    ArtworkSession* artwork = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((session = properties->findChild<TaggerSession*>()) != nullptr &&
                                 (artwork = properties->findChild<ArtworkSession*>()) != nullptr,
                             10'000);
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(apply && status);
    QTRY_VERIFY_WITH_TIMEOUT(session->renameAvailable(), 10'000);
    session->chooseMoveFolder(
        QFile::encodeName(media.filePath(QStringLiteral("Sorted"))).toStdString());
    QTRY_VERIFY_WITH_TIMEOUT(session->moveAvailable(), 10'000);
    session->chooseMove(true);
    artwork->setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->canAdd(), 10'000);
    artwork->add(picked_file, 0);
    QTRY_VERIFY_WITH_TIMEOUT(artwork->hasPendingChanges(), 10'000);
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    const auto moved =
        media.filePath(QStringLiteral("Sorted/Album/") + title + QStringLiteral(".flac"));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(moved), qPrintable(status->text()), 15'000);
    // The tab shows the cover written with the move.
    QTRY_VERIFY2_WITH_TIMEOUT(list_model->hasArtwork(list_model->groupKey(0)),
                              qPrintable(status->text()), 10'000);
    QPointer guard{properties};
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
}

void BenchMainWindowTest::metadataRenameMovesTheFileAndItsRow() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("rename-me.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto encoded = QFile::encodeName(source_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    bool layout_saved = false;
    window.persistence_->saveOutputLayoutProfile(
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "By title",
                    .dialect = {},
                    .relative_directory_expression = {},
                    .basename_expression = "%title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
        [&layout_saved](const QString& error) { layout_saved = error.isEmpty(); });
    QTRY_VERIFY(layout_saved);
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr);
    QVERIFY(properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY(!window.property("trackknife-metadata-operation-running").toBool());
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();

    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    QCOMPARE(properties->property("trackknife-file-work").toString(), QStringLiteral("engine"));
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(rename_files != nullptr);
    QVERIFY(preview != nullptr);
    QTRY_VERIFY(rename_files->isEnabled());
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Renamed by title"), Qt::EditRole));
    rename_files->setChecked(true);
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // The row follows the file to its new name, which the new title made.
    const auto target = media.filePath(QStringLiteral("Renamed by title.flac"));
    const auto target_raw = QFile::encodeName(target).toStdString();
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().raw_path, target_raw, 10'000);
    // ADR-0278: the row reads its new tags from the file a moment after.
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().title, std::string{"Renamed by title"},
                              5'000);
    QVERIFY(!QFile::exists(source_path));
    const auto reread = metadata::read_local_metadata(target_raw);
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.effective_values("title"),
             (std::vector<std::string>{"Renamed by title"}));
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QVERIFY(window_test::closeTaggerAfterWrite(window));
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
}

// ADR-0268: a file changed by another tagger after the editor read it is no
// dead end. Apply reads it again, the draft kept, for another look; the
// next Apply writes it.
void BenchMainWindowTest::applyReadsAgainAFileChangedElsewhere() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("changed-elsewhere.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    properties_action->trigger();

    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr && status != nullptr && apply != nullptr);
    // Read and ready: the status says so, or has moved on to the draft.
    QTRY_VERIFY2_WITH_TIMEOUT(status->text().contains(QStringLiteral("Tags read")) ||
                                  status->text().contains(QStringLiteral("No pending edits")),
                              qPrintable(status->text()), 10'000);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Edited here"), Qt::EditRole));

    // Another tagger writes the file meanwhile: it is no longer the one read.
    const auto before = metadata::read_local_metadata(raw_path);
    QVERIFY(before.has_value());
    {
        QFile file{path};
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-3'600),
                                 QFileDevice::FileModificationTime));
    }

    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("Changed since read")), 10'000);
    QVERIFY(status->text().contains(QStringLiteral("1 draft kept")));
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QTRY_COMPARE(aggregate_model->index(*title_row, 2).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Edited here"));
    const auto untouched = metadata::read_local_metadata(raw_path);
    QVERIFY(untouched.has_value());
    QCOMPARE(untouched->document.effective_values("title"),
             before->document.effective_values("title"));

    // Looked at again, applied again: written.
    QTRY_VERIFY(apply->isEnabled());
    QTest::mouseClick(apply, Qt::LeftButton);
    QVERIFY(window_test::closeTaggerAfterWrite(window));
    const auto written = metadata::read_local_metadata(raw_path);
    QVERIFY(written.has_value());
    QCOMPARE(written->document.effective_values("title"),
             (std::vector<std::string>{"Edited here"}));
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
}

// ADR-0268: the rows a change names -- at its paths, or under a folder of
// them -- are described again, a CUE track by its sheet and not; and a row
// described again after a rename, naming the file as written, still follows
// the file to its new name.
void BenchMainWindowTest::listRowsFollowChangedFiles() {
    LocalListModel model;
    const auto row = [](const char* path) {
        LocalTrackRow track;
        track.raw_path = path;
        track.probed = true;
        return track;
    };
    auto cue = row("/music/A/live.flac");
    cue.segment = formats::SampleRange{.start_sample = 0, .end_sample = 100};
    model.replaceRows({row("/music/A/01.flac"), row("/music/A/02.flac"), row("/music/B/01.flac"),
                       row("/music/AB/01.flac"), cue});
    const auto ids = [&model](const std::vector<core::StableId>& entries) {
        std::vector<std::string> paths;
        for (const auto& entry : entries) {
            for (const auto& track : model.rows()) {
                if (track.entry_id == entry) {
                    paths.push_back(track.raw_path);
                }
            }
        }
        return paths;
    };
    QCOMPARE(ids(model.entriesAt({"/music/A/02.flac"}, false)),
             (std::vector<std::string>{"/music/A/02.flac"}));
    QCOMPARE(ids(model.entriesAt({"/music/A"}, false)),
             (std::vector<std::string>{"/music/A/01.flac", "/music/A/02.flac"}));
    QCOMPARE(model.entriesAt({}, true).size(), std::size_t{4U});

    const core::LocalSourceRevision before{.device = 1, .inode = 10};
    const core::LocalSourceRevision written{.device = 1, .inode = 11};
    auto described = row("/music/old.flac");
    described.source_revision = written;
    model.replaceRows({described});
    const auto moved =
        model.applyCommittedRelocation("/music/old.flac", "/music/new.flac", before, written);
    QVERIFY(moved && *moved == 1U);
    QCOMPARE(model.rows().front().raw_path, std::string{"/music/new.flac"});
    auto other = row("/music/old.flac");
    other.source_revision = core::LocalSourceRevision{.device = 1, .inode = 99};
    model.replaceRows({other});
    QVERIFY(!model.applyCommittedRelocation("/music/old.flac", "/music/new.flac", before, written));
}

// ADR-0277: a long file job holds idle sleep off, one platform hold however
// many jobs run, let go with the last. Where logind answers, it lists the
// hold while it lasts.
void BenchMainWindowTest::longJobsKeepTheComputerAwake() {
    const auto ours = [] {
        auto bus = QDBusConnection::systemBus();
        if (!bus.isConnected()) {
            return -1;
        }
        auto call = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
            QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("ListInhibitors"));
        const auto reply = bus.call(call, QDBus::Block, 2'000);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            return -1;
        }
        const auto listed = reply.arguments().front().value<QDBusArgument>();
        int count = 0;
        listed.beginArray();
        while (!listed.atEnd()) {
            QString what;
            QString who;
            QString why;
            QString mode;
            uint uid = 0;
            uint pid = 0;
            listed.beginStructure();
            listed >> what >> who >> why >> mode >> uid >> pid;
            listed.endStructure();
            if (who == QStringLiteral("Melody") &&
                pid == static_cast<uint>(QCoreApplication::applicationPid())) {
                ++count;
            }
        }
        listed.endArray();
        return count;
    };
    QCOMPARE(KeepAwake::holds(), 0);
    auto first = KeepAwake::hold(QStringLiteral("Writing tags"));
    auto second = KeepAwake::hold(QStringLiteral("Measuring ReplayGain"));
    QCOMPARE(KeepAwake::holds(), 2);
    const bool logind = ours() >= 0;
    if (logind) {
        QTRY_COMPARE_WITH_TIMEOUT(ours(), 1, 3'000);
    }
    first.reset();
    QCOMPARE(KeepAwake::holds(), 1);
    if (logind) {
        QTest::qWait(200);
        QCOMPARE(ours(), 1);
    }
    second.reset();
    QCOMPARE(KeepAwake::holds(), 0);
    if (logind) {
        QTRY_COMPARE_WITH_TIMEOUT(ours(), 0, 3'000);
    }
}

// ADR-0237: files are written only by the engine holding them. Without one
// that does file work, the tools say so rather than open to an Apply that
// cannot happen.
void BenchMainWindowTest::fileWorkWithoutAnEngineSaysWhy() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("unwritable.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    window.localEngine().does_file_work = false;

    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(properties != nullptr);
    QTRY_VERIFY(properties->isEnabled());
    properties->trigger();
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties")) == nullptr);
    QCOMPARE(window.statusBar()->currentMessage(),
             QStringLiteral("Editing tags is done by the engine on this computer, which is not "
                            "available right now"));

    auto* replaygain = window.findChild<QAction*>(QStringLiteral("action-replaygain-dialog"));
    QVERIFY(replaygain != nullptr);
    replaygain->trigger();
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-replaygain-dialog")) == nullptr);
    QVERIFY(window.statusBar()->currentMessage().startsWith(
        QStringLiteral("Measuring ReplayGain is done by the engine on this computer")));
}

void BenchMainWindowTest::metadataApplyCancellationPreservesDraftForFreshPreview() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    for (int index = 0; index < 3; ++index) {
        const auto path = media.filePath(QStringLiteral("cancel-%1.flac").arg(index));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        const auto read = metadata::read_local_metadata(raw_path);
        QVERIFY(read.has_value());
        sources.push_back(MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = raw_path,
                    .source_revision = read->source_revision,
                    .baseline = read->document,
                },
            .track_label = QStringLiteral("Cancel %1").arg(index),
        });
    }

    std::atomic_size_t admitted{0U};
    std::optional<operations::MetadataApplyResult> observed;
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {},
        [&admitted] {
            return MetadataWritePlanApplier{
                [&admitted](const metadata::MetadataWritePlan& plan,
                            const operations::MetadataApplyProgressCallback& progress,
                            const core::CancellationToken& cancellation) {
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&admitted](const metadata::MetadataWritePlanSource& source,
                                    const core::CancellationToken& token)
                            -> core::Result<operations::MetadataCommitResult> {
                            admitted.fetch_add(1U, std::memory_order_relaxed);
                            while (!token.is_cancellation_requested()) {
                                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                            }
                            return std::unexpected(core::Error{
                                .code = core::ErrorCode::cancelled,
                                .message = "cancelled " + source.raw_path,
                                .context = {},
                            });
                        },
                        {}, {}, progress, cancellation,
                        operations::MetadataApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [&observed](const operations::MetadataApplyResult& result) { observed = result; });
    properties->show();

    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(preview != nullptr);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Cancelled batch title"), Qt::EditRole));
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // The ready plan applies directly; progress and Stop live in the footer.
    auto* stop = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-stop"));
    auto* progress_bar =
        properties->findChild<QProgressBar*>(QStringLiteral("bench-metadata-apply-progress"));
    QVERIFY(stop != nullptr);
    QVERIFY(progress_bar != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(stop->isVisible(), 5'000);
    QVERIFY(progress_bar->isVisible());
    QTRY_COMPARE(admitted.load(std::memory_order_relaxed), 2U);
    QTest::mouseClick(stop, Qt::LeftButton);

    // A stopped run reports the untouched files in one compact dialog.
    QDialog* feedback = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((feedback = properties->findChild<QDialog*>(
                                  QStringLiteral("bench-preparation-feedback"))) != nullptr,
                             5'000);
    auto* feedback_table =
        feedback->findChild<QTreeWidget*>(QStringLiteral("bench-preparation-feedback-table"));
    auto* feedback_summary =
        feedback->findChild<QLabel*>(QStringLiteral("bench-preparation-feedback-summary"));
    auto* close =
        feedback->findChild<QPushButton*>(QStringLiteral("bench-preparation-feedback-close"));
    QVERIFY(feedback_table != nullptr);
    QVERIFY(feedback_summary != nullptr);
    QVERIFY(close != nullptr);
    QCOMPARE(feedback->windowTitle(), QStringLiteral("Save stopped"));
    QCOMPARE(feedback_table->topLevelItemCount(), 3);
    QVERIFY(feedback_summary->text().contains(QStringLiteral("0 saved")));
    QVERIFY(feedback_summary->text().contains(QStringLiteral("3 stopped")));
    QVERIFY(observed.has_value());
    QCOMPARE(observed->cancelled_source_count(), 3U);
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
                nullptr);
    QVERIFY(!stop->isVisible());
    QVERIFY(!progress_bar->isVisible());
    QTRY_VERIFY(preview->isEnabled());
    QTRY_COMPARE(
        aggregate_model->data(aggregate_model->index(*title_row, 2), Qt::EditRole).toString(),
        QStringLiteral("Cancelled batch title"));
    delete properties;
}

// The size a window of any toolkit restores: stored on close, handed back
// to the next tagger as it was.
void BenchMainWindowTest::taggerWindowStateRoundTrips() {
    QHash<QString, QByteArray> saved_states;
    TaggerServices services;
    services.layout_store = MetadataDialogLayoutStore{
        .load =
            [this, &saved_states](QString key,
                                  MetadataDialogLayoutStore::LoadCompletion completion) {
                const auto state = saved_states.value(key);
                QTimer::singleShot(0, this, [completion = std::move(completion), state]() mutable {
                    completion(state, {});
                });
            },
        .save =
            [&saved_states](QString key, QByteArray value,
                            MetadataDialogLayoutStore::Completion completion) {
                saved_states.insert(std::move(key), std::move(value));
                if (completion) {
                    completion({});
                }
            },
    };
    const auto no_source = [](std::size_t) -> std::optional<MetadataPropertiesSource> {
        return std::nullopt;
    };
    {
        TaggerSession first(0U, no_source, {}, services);
        first.storeWindowState({{QStringLiteral("width"), 930},
                                {QStringLiteral("height"), 640},
                                {QStringLiteral("maximized"), true},
                                {QStringLiteral("listWidth"), 210}});
    }
    QVERIFY(saved_states.contains(QStringLiteral("workspace/metadata-properties-window-v1")));

    TaggerSession second(0U, no_source, {}, services);
    QVariantMap loaded;
    second.loadWindowState([&loaded](QVariantMap state) { loaded = std::move(state); });
    QTRY_VERIFY(!loaded.isEmpty());
    QCOMPARE(loaded.value(QStringLiteral("width")).toInt(), 930);
    QCOMPARE(loaded.value(QStringLiteral("height")).toInt(), 640);
    QCOMPARE(loaded.value(QStringLiteral("maximized")).toBool(), true);
    QCOMPARE(loaded.value(QStringLiteral("listWidth")).toInt(), 210);
}

void BenchMainWindowTest::metadataDialogLayoutsPersistAsynchronously() {
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = "/music/layout-fixture.flac",
                .source_revision = std::nullopt,
                .baseline = metadata::MetadataDocument{},
            },
        .track_label = QStringLiteral("Layout fixture"),
    };
    QHash<QString, QByteArray> saved_states;
    const MetadataDialogLayoutStore layout_store{
        .load =
            [this, &saved_states](QString key,
                                  MetadataDialogLayoutStore::LoadCompletion completion) {
                const auto state = saved_states.value(key);
                QTimer::singleShot(0, this, [completion = std::move(completion), state]() mutable {
                    completion(state, {});
                });
            },
        .save =
            [&saved_states](QString key, QByteArray value,
                            MetadataDialogLayoutStore::Completion completion) {
                saved_states.insert(std::move(key), std::move(value));
                if (completion) {
                    completion({});
                }
            },
    };
    const auto create_properties = [&] {
        return new MetadataPropertiesDialog(
            1U,
            [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
                return index == 0U ? std::optional{source} : std::nullopt;
            },
            {}, {}, {}, {}, {}, {}, {}, nullptr, layout_store);
    };

    auto* first = create_properties();
    first->show();
    QSplitter* first_metadata = nullptr;
    QTRY_VERIFY((first_metadata = first->findChild<QSplitter*>(
                     QStringLiteral("bench-metadata-splitter"))) != nullptr);
    first->resize(930, 640);
    first_metadata->setSizes({125, 405});

    auto* transform = first->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    QVERIFY(transform != nullptr);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);
    QDialog* first_editor = nullptr;
    QTRY_VERIFY((first_editor = first->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* first_editor_splitter = first_editor->findChild<QSplitter*>(
        QStringLiteral("bench-metadata-transformation-splitter"));
    QVERIFY(first_editor_splitter != nullptr);
    first_editor->resize(970, 720);
    first_editor_splitter->setSizes({365, 585});
    QVERIFY(first_editor->close());
    QTRY_VERIFY(first->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    QVERIFY(first->close());
    QTRY_COMPARE(saved_states.size(), 4);

    auto* second = create_properties();
    second->show();
    QSplitter* second_metadata = nullptr;
    QTRY_VERIFY((second_metadata = second->findChild<QSplitter*>(
                     QStringLiteral("bench-metadata-splitter"))) != nullptr);
    QTRY_COMPARE(second->height(), 640);
    QTRY_COMPARE(
        second_metadata->saveState(),
        saved_states.value(QStringLiteral("workspace/metadata-properties-metadata-splitter-v1")));

    transform = second->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    QVERIFY(transform != nullptr);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);
    QDialog* second_editor = nullptr;
    QTRY_VERIFY((second_editor = second->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* second_editor_splitter = second_editor->findChild<QSplitter*>(
        QStringLiteral("bench-metadata-transformation-splitter"));
    QVERIFY(second_editor_splitter != nullptr);
    QTRY_COMPARE(second_editor->height(), 720);
    QTRY_COMPARE(
        second_editor_splitter->saveState(),
        saved_states.value(QStringLiteral("workspace/metadata-transformation-splitter-v1")));
    QVERIFY(second_editor->close());
    QVERIFY(second->close());
}

void BenchMainWindowTest::metadataFieldLayoutsLoadFilterAndPersist() {
    const auto make_field = [](std::string name, std::string value) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = {std::move(value)},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = "/music/field-layout.flac",
                .source_revision = std::nullopt,
                .baseline =
                    metadata::MetadataDocument{
                        .fields = {make_field("TITLE", "One"), make_field("ARTIST", "Artist"),
                                   make_field("ALBUM", "Album")},
                        .unsupported_native_objects = {},
                    },
            },
        .track_label = QStringLiteral("Field layout fixture"),
    };
    QHash<QString, QByteArray> states;
    states.insert(
        QStringLiteral("workspace/metadata-field-layouts-v1"),
        QByteArrayLiteral(
            R"({"schema":1,"active":"compact","layouts":[{"id":"compact","name":"Compact","fields":["title","artist"]}]})"));
    const MetadataDialogLayoutStore store{
        .load = [&states](QString key, auto completion) { completion(states.value(key), {}); },
        .save =
            [&states](QString key, QByteArray value, auto completion) {
                states.insert(std::move(key), std::move(value));
                if (completion) {
                    completion({});
                }
            },
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, store);
    properties->show();
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* combo = properties->findChild<QComboBox*>(QStringLiteral("bench-metadata-field-layout"));
    QVERIFY(combo != nullptr);
    QVERIFY(combo->isHidden());
    auto* model = qobject_cast<MetadataAggregateModel*>(fields->model());
    QVERIFY(model != nullptr);
    const auto title = model->fieldRow(QStringLiteral("title"));
    const auto artist = model->fieldRow(QStringLiteral("artist"));
    const auto album = model->fieldRow(QStringLiteral("album"));
    QVERIFY(title && artist && album);
    QCOMPARE(*title, 0);
    QCOMPARE(*artist, 1);
    QVERIFY(!fields->isRowHidden(*title));
    QVERIFY(!fields->isRowHidden(*artist));
    QVERIFY(!fields->isRowHidden(*album));
    QVERIFY(!fields->isRowHidden(*album));
    QCOMPARE(
        states.value(QStringLiteral("workspace/metadata-field-layouts-v1")),
        QByteArrayLiteral(
            R"({"schema":1,"active":"compact","layouts":[{"id":"compact","name":"Compact","fields":["title","artist"]}]})"));
    delete properties;
}

void BenchMainWindowTest::preparationSidePanelEditsReusableOutputProfiles() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("profiles.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Profile fixture"),
    };

    std::vector<persistence::SavedOutputLayoutProfile> layouts{
        persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = 1U,
                    .name = "Albums",
                    .dialect = {},
                    .relative_directory_expression = "%album artist%/%album%",
                    .basename_expression = "%tracknumber% - %title%",
                    .sanitization_policy = {"linux", 1U},
                },
        },
    };
    std::vector<persistence::SavedDestinationProfile> destinations{
        persistence::SavedDestinationProfile{
            .id = core::StableId::random(),
            .profile =
                operations::DestinationProfile{
                    .schema_version = 1U,
                    .name = "Library",
                    .root_raw_path = QFile::encodeName(media.path()).toStdString(),
                    .containment_policy = {"lexical-beneath-root", 1U},
                },
        },
    };
    OutputProfileStore output_store{
        .load =
            [&layouts, &destinations](OutputProfileStore::LoadCompletion completion) {
                completion(layouts, destinations, {});
            },
        .save_layout =
            [&layouts](persistence::SavedOutputLayoutProfile saved,
                       OutputProfileStore::Completion completion) {
                const auto found = std::ranges::find(layouts, saved.id,
                                                     &persistence::SavedOutputLayoutProfile::id);
                if (found == layouts.end()) {
                    layouts.push_back(std::move(saved));
                } else {
                    *found = std::move(saved);
                }
                completion({});
            },
        .remove_layout =
            [&layouts](core::StableId id, OutputProfileStore::Completion completion) {
                std::erase_if(layouts, [id](const auto& saved) { return saved.id == id; });
                completion({});
            },
        .save_destination =
            [&destinations](persistence::SavedDestinationProfile saved,
                            OutputProfileStore::Completion completion) {
                const auto found = std::ranges::find(destinations, saved.id,
                                                     &persistence::SavedDestinationProfile::id);
                if (found == destinations.end()) {
                    destinations.push_back(std::move(saved));
                } else {
                    *found = std::move(saved);
                }
                completion({});
            },
        .remove_destination =
            [&destinations](core::StableId id, OutputProfileStore::Completion completion) {
                std::erase_if(destinations, [id](const auto& saved) { return saved.id == id; });
                completion({});
            },
        .destinations_on = {},
        .destinations_key = {},
        .places = {},
    };

    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, output_store);
    properties->show();

    QWidget* side_panel = nullptr;
    QTRY_VERIFY((side_panel = properties->findChild<QWidget*>(
                     QStringLiteral("bench-metadata-side-panel"))) != nullptr);
    // ADR-0183: the apply options live in the third sections tab and the
    // footer carries the plan summary label.
    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    QTRY_COMPARE(sections->count(), 2);
    QVERIFY(properties->findChild<QLabel*>(QStringLiteral("bench-metadata-apply-summary")) !=
            nullptr);

    // ADR-0238: the footer Actions button opens a popover with every
    // apply option in view, driving the hidden state controls.
    auto* actions_button =
        properties->findChild<QToolButton*>(QStringLiteral("bench-metadata-actions"));
    QVERIFY(actions_button != nullptr);
    const auto open_actions = [&]() -> QFrame* {
        QTest::mouseClick(actions_button, Qt::LeftButton);
        QFrame* popover = nullptr;
        [&] {
            QTRY_VERIFY((popover = properties->findChild<QFrame*>(
                             QStringLiteral("bench-metadata-actions-popover"))) != nullptr &&
                        popover->isVisible());
        }();
        return popover;
    };
    auto* popover = open_actions();
    QVERIFY(popover != nullptr);
    auto* save_tags_choice =
        popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-save-tags"));
    auto* rename_choice =
        popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-rename-files"));
    auto* manage_layouts_link =
        popover->findChild<QLabel*>(QStringLiteral("bench-actions-manage-layouts"));
    QVERIFY(save_tags_choice != nullptr && rename_choice != nullptr &&
            manage_layouts_link != nullptr);
    QVERIFY(save_tags_choice->isChecked());
    QVERIFY(!rename_choice->isEnabled());
    QSignalSpy settings_requests{properties, &MetadataPropertiesDialog::openSettingsRequested};
    emit manage_layouts_link->linkActivated(QStringLiteral("#"));
    QCOMPARE(settings_requests.count(), 1);
    QTRY_VERIFY(!popover->isVisible() || properties->findChild<QFrame*>(QStringLiteral(
                                             "bench-metadata-actions-popover")) != popover);
    popover = open_actions();
    save_tags_choice = popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-save-tags"));
    save_tags_choice->click();
    auto* save_tags =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    QVERIFY(save_tags != nullptr);
    QVERIFY(!save_tags->isChecked());
    save_tags_choice->click();
    QVERIFY(save_tags->isChecked());
    popover->close();
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* move_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-move-files"));
    auto* replaygain = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    QVERIFY(save_tags != nullptr);
    QVERIFY(rename_files != nullptr);
    QVERIFY(move_files != nullptr);
    QVERIFY(replaygain != nullptr);
    QVERIFY(save_tags->isChecked());
    QVERIFY(save_tags->isEnabled());
    QVERIFY(!rename_files->isEnabled());
    QVERIFY(!move_files->isEnabled());
    QTRY_VERIFY(replaygain->isEnabled());
    QTableView* fields = nullptr;
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    auto* preparation_status =
        properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    QVERIFY(preview != nullptr);
    QVERIFY(preparation_status != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    QVERIFY(aggregate_model != nullptr);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Naming context"), Qt::EditRole));
    QTRY_VERIFY(preview->isEnabled());
    save_tags->setChecked(false);
    QVERIFY(!preview->isEnabled());
    QVERIFY(preparation_status->text().contains(QStringLiteral("Save tags is off")));
    save_tags->setChecked(true);
    QTRY_VERIFY(preview->isEnabled());

    auto* layout_combo =
        properties->findChild<QComboBox*>(QStringLiteral("bench-output-layout-profile"));
    auto* destination_combo =
        properties->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QVERIFY(layout_combo != nullptr);
    QVERIFY(destination_combo != nullptr);
    QCOMPARE(layout_combo->count(), 1);
    QCOMPARE(layout_combo->currentText(), QStringLiteral("Albums"));
    // The saved one, then Choose folder… (ADR-0262).
    QCOMPARE(destination_combo->count(), 2);
    QCOMPARE(destination_combo->currentText(), QStringLiteral("Library"));

    // ADR-0185: the Edit buttons ask for the Settings screen instead of
    // opening local managers.
    auto* layout_manage =
        properties->findChild<QPushButton*>(QStringLiteral("bench-output-layout-manage"));
    QVERIFY(layout_manage != nullptr);
    QSignalSpy manage_requests{properties, &MetadataPropertiesDialog::openSettingsRequested};
    QTest::mouseClick(layout_manage, Qt::LeftButton);
    QCOMPARE(manage_requests.count(), 1);

    // The Settings Naming page owns profile CRUD through the same store.
    SettingsDialog settings{nullptr, output_store};
    settings.showPage(SettingsDialog::Page::naming);
    settings.show();
    auto* layout_list =
        settings.findChild<QTreeWidget*>(QStringLiteral("bench-output-layout-list"));
    auto* layout_remove =
        settings.findChild<QPushButton*>(QStringLiteral("bench-output-layout-remove"));
    QVERIFY(layout_remove != nullptr);
    auto* layout_name = settings.findChild<QLineEdit*>(QStringLiteral("bench-output-layout-name"));
    auto* layout_directory =
        settings.findChild<QLineEdit*>(QStringLiteral("bench-output-layout-directory-expression"));
    auto* layout_basename =
        settings.findChild<QLineEdit*>(QStringLiteral("bench-output-layout-basename-expression"));
    auto* layout_sanitization =
        settings.findChild<QComboBox*>(QStringLiteral("bench-output-layout-sanitization"));
    auto* layout_new = settings.findChild<QPushButton*>(QStringLiteral("bench-output-layout-new"));
    auto* layout_save =
        settings.findChild<QPushButton*>(QStringLiteral("bench-output-layout-save"));
    auto* destination_list =
        settings.findChild<QTreeWidget*>(QStringLiteral("bench-destination-list"));
    auto* destination_name =
        settings.findChild<QLineEdit*>(QStringLiteral("bench-destination-name"));
    auto* destination_root =
        settings.findChild<QLineEdit*>(QStringLiteral("bench-destination-root"));
    auto* destination_new =
        settings.findChild<QPushButton*>(QStringLiteral("bench-destination-new"));
    auto* destination_save =
        settings.findChild<QPushButton*>(QStringLiteral("bench-destination-save"));
    QVERIFY(layout_list != nullptr && layout_name != nullptr && layout_directory != nullptr &&
            layout_basename != nullptr && layout_sanitization != nullptr && layout_new != nullptr &&
            layout_save != nullptr && destination_list != nullptr && destination_name != nullptr &&
            destination_root != nullptr && destination_new != nullptr &&
            destination_save != nullptr);
    // ADR-0264: the layouts as a list, name and whole pattern, the selected
    // one edited below.
    QTRY_COMPARE(layout_list->topLevelItemCount(), 1);
    QCOMPARE(layout_list->topLevelItem(0)->text(0), QStringLiteral("Albums"));
    QVERIFY(
        layout_list->topLevelItem(0)->text(1).endsWith(QStringLiteral("%tracknumber% - %title%")));
    QCOMPARE(layout_list->currentItem(), layout_list->topLevelItem(0));
    QVERIFY(layout_remove->isEnabled());
    QCOMPARE(layout_name->text(), QStringLiteral("Albums"));
    QCOMPARE(layout_basename->text(), QStringLiteral("%tracknumber% - %title%"));
    QCOMPARE(layout_sanitization->currentData().toString(), QStringLiteral("linux"));
    // Only this computer's: listed without a heading.
    QTRY_COMPARE(destination_list->topLevelItemCount(), 1);
    QCOMPARE(destination_list->topLevelItem(0)->text(0), QStringLiteral("Library"));

    // ADR-0264: Naming layouts, Move destinations, ReplayGain, Undo -- one
    // row of tabs.
    auto* profile_sections =
        settings.findChild<QTabWidget*>(QStringLiteral("bench-settings-file-operations"));
    QVERIFY(profile_sections);
    QCOMPARE(profile_sections->count(), 4);
    QVERIFY(layout_directory->width() > 450);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(settings.grab().save(directory + QStringLiteral("/settings-naming.png")));
    }
    QSignalSpy profile_changes{&settings, &SettingsDialog::outputProfilesChanged};
    QTest::mouseClick(layout_new, Qt::LeftButton);
    // A new one: nothing selected, nothing to remove.
    QVERIFY(layout_list->currentItem() == nullptr);
    QVERIFY(!layout_remove->isEnabled());
    layout_name->setText(QStringLiteral("Artist folders"));
    layout_directory->setText(QStringLiteral("%artist%"));
    layout_basename->setText(QStringLiteral("%title%"));
    layout_sanitization->setCurrentIndex(layout_sanitization->findData(QStringLiteral("portable")));
    QTRY_VERIFY(layout_save->isEnabled());
    QTest::mouseClick(layout_save, Qt::LeftButton);
    QTRY_COMPARE(layouts.size(), 2U);
    QCOMPARE(profile_changes.count(), 1);
    QCOMPARE(layouts.back().profile.sanitization_policy,
             (operations::PolicyVersion{"portable", 1U}));

    QTRY_COMPARE(layout_list->topLevelItemCount(), 2);
    // Saved: listed, and still the one selected.
    QCOMPARE(layout_list->currentItem()->text(0), QStringLiteral("Artist folders"));
    QCOMPARE(layout_list->currentItem()->text(1), QStringLiteral("%artist%/%title%"));
    QVERIFY(layout_remove->isEnabled());
    layout_list->setCurrentItem(layout_list->topLevelItem(0));
    QCOMPARE(layout_name->text(), layout_list->currentItem()->text(0));
    QCOMPARE(layout_basename->text(), QStringLiteral("%tracknumber% - %title%"));
    layout_list->setCurrentItem(layout_list->topLevelItem(1));
    QCOMPARE(layout_name->text(), QStringLiteral("Artist folders"));
    QCOMPARE(layout_basename->text(), QStringLiteral("%title%"));
    // Remove removes the one selected.
    QTest::mouseClick(layout_remove, Qt::LeftButton);
    QTRY_COMPARE(layouts.size(), 1U);
    QCOMPARE(layout_list->topLevelItemCount(), 1);
    QCOMPARE(layout_list->topLevelItem(0)->text(0), QStringLiteral("Albums"));
    // The first left is selected in its place.
    QCOMPARE(layout_list->currentItem(), layout_list->topLevelItem(0));
    QCOMPARE(layout_name->text(), QStringLiteral("Albums"));
    profile_sections->setCurrentIndex(1);
    QCOMPARE(destination_name->text(), QStringLiteral("Library"));
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(settings.grab().save(directory + QStringLiteral("/settings-destinations.png")));
    }
    QTest::mouseClick(destination_new, Qt::LeftButton);
    destination_name->setText(QStringLiteral("Archive"));
    // The root comes from the browse dialog in real use; the raw path is
    // what enables Save, so drive the widget's browse-backed state through
    // typing is no longer supported — use the store round trip instead.
    QVERIFY(!destination_save->isEnabled());

    // The editor's selectors refresh from the shared store on request.
    properties->reloadOutputProfiles();
    QTRY_COMPARE(layout_combo->count(), 1);

    delete properties;
}

void BenchMainWindowTest::pathOnlyPreparationUsesActualTagsAndAppliesReviewedPlan() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("before.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto actual_title = read->document.first_effective_value("title");
    QVERIFY(actual_title.has_value());
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Path fixture"),
    };
    const std::vector layouts{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Draft title",
                .dialect = {},
                .relative_directory_expression = {},
                .basename_expression = "%title%",
                .sanitization_policy = {"linux", 1U},
            },
    }};
    OutputProfileStore output_store{
        .load = [layouts](
                    OutputProfileStore::LoadCompletion completion) { completion(layouts, {}, {}); },
        .save_layout = {},
        .remove_layout = {},
        .save_destination = {},
        .remove_destination = {},
        .destinations_on = {},
        .destinations_key = {},
        .places = {},
    };
    const std::vector automatic_chains{persistence::SavedMetadataTransformationChain{
        .id = core::StableId::random(),
        .chain =
            metadata::MetadataTransformationChain{
                .schema_version = 1U,
                .name = "Synthetic path title",
                .actions = {metadata::MetadataFormatValueAction{
                    .target_field = "title", .dialect = {}, .source = "Synthetic path title"}},
            },
        .automatic = true,
    }};
    MetadataTransformationStore transformation_store{
        .load =
            [automatic_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(automatic_chains, {});
            },
        .save = {},
        .remove = {},
    };
    std::optional<operations::FilePublicationApplyResult> observed;
    std::string reviewed_target;
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store, output_store,
        [&reviewed_target] {
            return FilePublicationPlanApplier{
                [&reviewed_target](const operations::PreparationPlan& plan,
                                   const operations::FilePublicationApplyProgressCallback& progress,
                                   const core::CancellationToken&)
                    -> core::Result<operations::FilePublicationApplyResult> {
                    if (!plan.path_preflight || plan.path_preflight->sources.size() != 1U) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::invariant,
                            .message = "Expected one reviewed path",
                            .context = {},
                        });
                    }
                    const auto& preflight = *plan.path_preflight;
                    const auto& checked_source = preflight.sources.front();
                    reviewed_target = checked_source.planned.target_raw_path;
                    if (progress) {
                        progress(operations::FilePublicationApplyProgress{
                            .source_index = 0U,
                            .source_raw_path = checked_source.planned.source_raw_path,
                            .target_raw_path = checked_source.planned.target_raw_path,
                            .publication = checked_source.publication,
                            .state = operations::FilePublicationApplySourceState::committed,
                            .completed_sources = 1U,
                            .total_sources = 1U,
                            .issue = std::nullopt,
                        });
                    }
                    auto commit = operations::FilePublicationCommitResult{
                        .journal_id = core::StableId::random(),
                        .source_raw_path = checked_source.planned.source_raw_path,
                        .target_raw_path = checked_source.planned.target_raw_path,
                        .source_revision = checked_source.planned.source_revision,
                        .target_revision = checked_source.observed_revision,
                        .occurrence_indexes = checked_source.planned.item_indexes,
                        // As on an NFS share: done, but ownership not kept.
                        .notes = {"File ownership was not preserved: the destination "
                                  "filesystem refused the change"},
                    };
                    return operations::FilePublicationApplyResult{
                        .sources = {operations::FilePublicationApplySourceResult{
                            .source_index = 0U,
                            .source_raw_path = checked_source.planned.source_raw_path,
                            .target_raw_path = checked_source.planned.target_raw_path,
                            .publication = checked_source.publication,
                            .state = operations::FilePublicationApplySourceState::committed,
                            .commit = std::move(commit),
                            .metadata_commit = std::nullopt,
                            .folder_image_commit = std::nullopt,
                            .published_metadata = std::nullopt,
                            .issue = std::nullopt,
                        }},
                        .cancellation_requested = false,
                    };
                }};
        },
        [&observed](const operations::FilePublicationApplyResult& result) { observed = result; });
    QSignalSpy status{properties, &MetadataPropertiesDialog::statusMessage};
    properties->show();
    // Success auto-closes the WA_DeleteOnClose dialog; only a pointer
    // guarded from the start may observe that.
    const QPointer<MetadataPropertiesDialog> closed_guard{properties};

    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* save_tags =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    auto* rename_files =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-rename-files"));
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(save_tags != nullptr);
    QVERIFY(rename_files != nullptr);
    QVERIFY(preview != nullptr);
    QTRY_VERIFY(rename_files->isEnabled());
    QListWidget* automatic_list = nullptr;
    QTRY_VERIFY((automatic_list = properties->findChild<QListWidget*>(
                     QStringLiteral("bench-metadata-transformation-list"))) != nullptr);
    QTRY_COMPARE(automatic_list->count(), 1);
    QCOMPARE(automatic_list->item(0)->checkState(), Qt::Checked);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Draft path title"), Qt::EditRole));
    save_tags->setChecked(false);
    rename_files->setChecked(true);
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // Direct apply: the checked plan runs immediately. The applier receives the
    // preflighted path derived strictly from the file's actual tags — never the
    // unsaved draft or the automatic script output.
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 5'000);
    QCOMPARE(observed->committed_source_count(), 1U);
    const auto expected_target =
        media.filePath(QString::fromStdString(*actual_title) + QStringLiteral(".flac"));
    QVERIFY(!expected_target.endsWith(QStringLiteral("/Draft path title.flac")));
    QVERIFY(!expected_target.endsWith(QStringLiteral("/Synthetic path title.flac")));
    QCOMPARE(QString::fromStdString(reviewed_target), expected_target);
    // A note is a line in the status bar, not a window to close first.
    QVERIFY(window_test::closeTaggerAfterWrite(closed_guard));
    QCOMPARE(status.count(), 1);
    QVERIFY(
        status.front().front().toString().contains(QStringLiteral("ownership was not preserved")));
}

// ADR-0286: the working lists an engine set aside when tabs became the
// window's are offered once; opened, they are tabs of this window -- never
// sent back -- and gone from the engine.
void BenchMainWindowTest::listsLeftOnTheEngineAreOfferedOnce() {
    engine_.stop();
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto folder = std::filesystem::path{state.path().toStdString()};
    testing::TestEngine own;
    QVERIFY2(own.start(folder), own.log().constData());
    own.stop();
    // As schema 56 sets a working list aside, written where it keeps them.
    const auto list = core::StableId::random().to_string();
    const auto first = core::StableId::random().to_string();
    const auto second = core::StableId::random().to_string();
    {
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open((folder / "lists.sqlite").c_str(), &database), SQLITE_OK);
        const auto sql =
            "INSERT INTO retired_engine_lists(id, name, kind, revision, created_ms, modified_ms, "
            "draft_of, draft_base) VALUES('" +
            list +
            "', CAST('Left over' AS BLOB), 0, 3, 1, 1, "
            "NULL, 0);"
            "INSERT INTO retired_engine_list_items(list_id, position, entry_id, raw_path, "
            "duration_ms, title, artist, album) VALUES"
            "('" +
            list + "', 0, '" + first +
            "', CAST('/music/left/one.flac' AS BLOB), 1000, "
            "'One', 'Someone', 'Album'),"
            "('" +
            list + "', 1, '" + second +
            "', CAST('/music/left/two.flac' AS BLOB), 1000, "
            "'Two', 'Someone', 'Album');";
        char* error = nullptr;
        const auto done = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
        QVERIFY2(done == SQLITE_OK, error);
        sqlite3_close(database);
    }
    QVERIFY2(own.start(folder), own.log().constData());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = own.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    QCOMPARE((*engine)->call("list.retired")->at("lists").size(), std::size_t{1});

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QMessageBox* offer = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (offer = window.findChild<QMessageBox*>(QStringLiteral("bench-retired-lists"))) != nullptr,
        10'000);
    QVERIFY(offer->informativeText().contains(QStringLiteral("Left over")));
    QAbstractButton* open = nullptr;
    for (auto* button : offer->buttons()) {
        if (button->text() == QStringLiteral("Open as tabs")) {
            open = button;
        }
    }
    QVERIFY(open != nullptr);
    open->click();
    BenchMainWindow::ListTab* tab = nullptr;
    QTRY_VERIFY((tab = window.tabForDocument(QString::fromStdString(list))) != nullptr);
    QCOMPARE(rowPaths(*tab->model),
             (std::vector<std::string>{"/music/left/one.flac", "/music/left/two.flac"}));
    QCOMPARE(tab->model->rows().front().entry_id.to_string(), first);
    QCOMPARE(displayText(tab->document.name), QStringLiteral("Left over"));
    QVERIFY(tab->document.kind != persistence::ListKind::saved);
    QTRY_VERIFY((*engine)->call("list.retired")->at("lists").empty());
    // A tab of this window's: not sent back to the engine.
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(std::ranges::none_of((*engine)->call("list.all")->at("lists"),
                                 [&list](const protocol::Json& summary) {
                                     return summary.value("id", std::string{}) == list;
                                 }));
    (*engine)->close();
    own.stop();
}

// ADR-0286: what changes in the playing tab reaches the engine's queue as the
// edits it is -- so an entry the engine let go of meanwhile (consumed, say)
// is not brought back by sending the tab whole.
void BenchMainWindowTest::aPlayingTabsEditsReachTheQueueAsEdits() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto queued = [&other] {
        std::vector<std::string> entries;
        auto queue = (*other)->call("playback.queue");
        for (const auto& entry :
             queue ? queue->value("entries", protocol::Json::array()) : protocol::Json::array()) {
            entries.push_back(entry.value("entry", std::string{}));
        }
        return entries;
    };
    auto* tab = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                            .kind = persistence::ListKind::scratch,
                                                            .name = "Playing",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {}},
                                  true);
    std::vector<LocalTrackRow> made;
    for (int index = 0; index < 5; ++index) {
        LocalTrackRow row;
        row.raw_path = "/music/playing/" + std::to_string(index) + ".flac";
        row.title = "Track " + std::to_string(index);
        row.duration_ms = 60'000;
        made.push_back(std::move(row));
    }
    tab->model->appendRows(std::move(made));
    std::vector<std::string> ids;
    for (const auto& row : tab->model->rows()) {
        ids.push_back(row.entry_id.to_string());
    }
    window.playRow(*tab, 1);
    QTRY_VERIFY_WITH_TIMEOUT(queued() == ids, 10'000);
    // The window knows the queue it handed over.
    QTRY_VERIFY(window.engine_queue_.has_value() && window.engine_queue_->size() == 5U);

    // Let go of on the engine -- as consume does -- before the window hears.
    QVERIFY((*other)->call(
        "playback.edit_queue",
        protocol::Json{{"list", tab->document.id.to_string()},
                       {"edits", protocol::Json::array({protocol::Json{{"remove", {ids[4]}}}})}}));
    // And now a row taken out here.
    tab->model->removeRowIndexes({0});
    window.markTabDirty(*tab);
    QTRY_VERIFY(queued() == (std::vector<std::string>{ids[1], ids[2], ids[3]}));
    QTest::qWait(500);
    QCOMPARE(queued(), (std::vector<std::string>{ids[1], ids[2], ids[3]}));
    (*other)->close();
}

// ADR-0286: the library's tabs are the usual ones; this computer's are
// marked -- a computer icon, a tooltip saying where its files are -- and kept
// after them. The sidebar calls this computer's folders by their name.
void BenchMainWindowTest::thisComputersTabsAreMarkedWhenTheLibraryIsElsewhere() {
    // The library on an engine elsewhere, reached or not.
    saveRemoteEngines({{.address = QStringLiteral("192.0.2.10:6603"),
                        .password = {},
                        .music_folder = {},
                        .reachable_at = {},
                        .id = {},
                        .stream_kbps = -1,
                        .name = QStringLiteral("gemenon")}});
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_key),
                         QStringLiteral("192.0.2.10:6603"));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY(!window.list_tabs_.empty());
    auto& local = *window.list_tabs_.front();
    QVERIFY(EngineKey::of(local.document).isLocal());
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    const auto index = tabs->indexOf(local.view);
    QVERIFY(!tabs->tabIcon(index).isNull());
    QVERIFY(tabs->tabToolTip(index).contains(QStringLiteral("files on ")));
    QVERIFY(tabs->tabToolTip(index).contains(QStringLiteral("played here")));
    auto* sources = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(sources != nullptr);
    // This computer's library is no library: its tab is not shown.
    QCOMPARE(sources->tabText(0), QStringLiteral("Library"));
    QVERIFY(!sources->isTabVisible(0));

    // The library here again: nothing to mark.
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_engine_key),
                         QLatin1String(SettingsDialog::library_engine_local));
    window.refreshTabChrome(local);
    QVERIFY(tabs->tabIcon(tabs->indexOf(local.view)).isNull());
    QVERIFY(!tabs->tabToolTip(tabs->indexOf(local.view)).contains(QStringLiteral("files on ")));
    saveRemoteEngines({});
}

// ADR-0287: Help › About Melody shows the mascot, the release and the
// licence.
void BenchMainWindowTest::aboutShowsTheMascotAndTheRelease() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QCOMPARE(window.windowTitle(), QStringLiteral("Melody"));
    auto* about = window.findChild<QAction*>(QStringLiteral("action-about"));
    QVERIFY(about != nullptr);
    QCOMPARE(about->text(), QStringLiteral("About Melody"));
    about->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-about"))) != nullptr);
    const auto* mascot = dialog->findChild<QLabel*>(QStringLiteral("bench-about-mascot"));
    QVERIFY(mascot != nullptr && !mascot->pixmap().isNull());
    // Drawn, not an empty square: the SVG reached the label.
    const auto image = mascot->pixmap().toImage();
    int drawn = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            drawn += qAlpha(image.pixel(x, y)) > 0 ? 1 : 0;
        }
    }
    QVERIFY2(drawn > 100, qPrintable(QString::number(drawn)));
    const auto* version = dialog->findChild<QLabel*>(QStringLiteral("bench-about-version"));
    QVERIFY(version != nullptr && version->text().startsWith(QStringLiteral("Version ")));
    dialog->close();
}

// ADR-0288: an enabled plugin's action in a tab's and in the library's
// context menu, handed what is chosen; its answer notified and copied.
void BenchMainWindowTest::aPluginActionSharesWhatIsChosen() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto album_folder = media.filePath(QStringLiteral("music/Band/Record"));
    QVERIFY(QDir{}.mkpath(album_folder));
    const auto file = album_folder + QStringLiteral("/01.flac");
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), file));
    const auto raw = [](const QString& path) {
        const auto encoded = QFile::encodeName(path);
        return std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    };
    // A plugin answering with the album's folder within the music folder,
    // else with its name.
    const auto folder = pluginFolders().value(0) + QStringLiteral("/echo");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Echo", "actions": [{"id": "share",
                          "label": "Share album", "run": ["./share"],
                          "result": "notify-and-copy"},
                          {"id": "artist", "label": "Artist", "for": "tracks",
                           "run": ["./share", "artist"], "result": "copy"},
                          {"id": "list", "label": "List", "for": "tracks",
                           "run": ["./share", "list"], "result": "list"}],
                          "settings": [{"key": "server", "label": "Server",
                                        "default": "http://localhost"},
                                       {"key": "remote", "label": "Remote support",
                                        "kind": "toggle", "link": "https://example.org/remote"},
                                       {"key": "token", "label": "Token", "kind": "secret",
                                        "requires": "remote"}]})");
        QFile script{folder + QStringLiteral("/share")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"sh(#!/bin/sh
input=$(cat)
artist=$(printf '%s' "$input" | grep -o '"artist":"[^"]*"' | head -n 1 | cut -d '"' -f 4)
[ "$1" = artist ] && { echo "artist:$artist"; exit 0; }
[ "$1" = list ] && { printf '{"title": "%s: 2 missing", "items": [{"text": "First", "detail": "1987", "url": "https://example.org/1"}, {"text": "Second"}]}' "$artist"; exit 0; }
relative=$(printf '%s' "$input" | sed -n 's/.*"relative_folder":"\([^"]*\)".*/\1/p')
album=$(printf '%s' "$input" | sed -n 's/.*"album":"\([^"]*\)".*/\1/p')
echo "shared:${relative:-$album}"
)sh");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("echo")), true);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY(window.notifier_ != nullptr);
    QStringList notified;
    window.notifier_->setSendOverride([&notified](const QString& summary, const QString& body) {
        notified << summary + ": " + body;
    });
    window.openLocalPaths({raw(file)});
    Workspace::ListTab* tab = nullptr;
    QTRY_VERIFY((tab = [&window]() -> Workspace::ListTab* {
                    for (auto& candidate : window.list_tabs_) {
                        if (candidate->model->rowCount() == 1) {
                            return candidate.get();
                        }
                    }
                    return nullptr;
                }()) != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    QTRY_VERIFY_WITH_TIMEOUT(tab->model->rows().front().probed, 5'000);
    const auto album = QString::fromStdString(tab->model->rows().front().album);
    QVERIFY(!album.isEmpty());
    tab->view->selectionModel()->select(
        tab->model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(
        tab->view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, tab->view->visualRect(tab->model->index(0, local_title_column)).center())));
    auto* share = window.findChild<QAction*>(QStringLiteral("action-plugin-echo-share"));
    QVERIFY(share != nullptr);
    QCOMPARE(share->text(), QStringLiteral("Share album"));
    window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"))->close();
    QGuiApplication::clipboard()->clear();
    share->trigger();
    // Not in a music folder of this computer's: its album's name.
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("shared:") + album);
    QTRY_VERIFY(notified.contains(QStringLiteral("Echo: shared:") + album));

    // The library's: the album's folder within the music folder, the
    // folder added as Settings adds it.
    auto* panel = window.localLibrary();
    QVERIFY(panel != nullptr);
    panel->browser().addRoot(raw(media.filePath(QStringLiteral("music"))));
    QTRY_VERIFY(!panel->browser().rootPaths().empty());
    {
        persistence::LibraryScanProgress progress;
        QVERIFY(window.localCatalogue()->open()->scan({}, progress).has_value());
    }
    panel->browser().reload();
    auto* tree = panel->findChild<QTreeView*>();
    QTRY_VERIFY(tree->model()->rowCount() > 0);
    const auto first = tree->model()->index(0, 0);
    tree->scrollTo(first);
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualRect(first).center()));
    auto* menu = panel->findChild<QMenu*>(QStringLiteral("local-library-context-menu"));
    QVERIFY(menu != nullptr);
    auto* from_library = menu->findChild<QAction*>(QStringLiteral("action-plugin-echo-share"));
    QVERIFY(from_library != nullptr);
    menu->close();
    QGuiApplication::clipboard()->clear();
    from_library->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(),
                              QStringLiteral("shared:Band/Record"), 10'000);

    // From the artist's row too: the artist is said, as from an album.
    const auto artist = QString::fromStdString(tab->model->rows().front().album_artist.empty()
                                                   ? tab->model->rows().front().artist
                                                   : tab->model->rows().front().album_artist);
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualRect(first).center()));
    menu = panel->findChild<QMenu*>(QStringLiteral("local-library-context-menu"));
    QVERIFY(menu != nullptr);
    auto* of_artist = menu->findChild<QAction*>(QStringLiteral("action-plugin-echo-artist"));
    QVERIFY(of_artist != nullptr);
    menu->close();
    QGuiApplication::clipboard()->clear();
    of_artist->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(),
                              QStringLiteral("artist:") + artist, 10'000);

    // A list answered: a window of its own, its rows drawn by Melody. The
    // menu again: one closed goes, and its entries with it.
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualRect(first).center()));
    menu = panel->findChild<QMenu*>(QStringLiteral("local-library-context-menu"));
    QVERIFY(menu != nullptr);
    auto* as_list = menu->findChild<QAction*>(QStringLiteral("action-plugin-echo-list"));
    QVERIFY(as_list != nullptr);
    menu->close();
    as_list->trigger();
    QDialog* list_window = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (list_window = window.findChild<QDialog*>(QStringLiteral("bench-plugin-list"))) != nullptr,
        10'000);
    QCOMPARE(list_window->windowTitle(), QStringLiteral("Echo"));
    QCOMPARE(list_window->findChild<QLabel*>(QStringLiteral("bench-plugin-list-title"))->text(),
             artist + QStringLiteral(": 2 missing"));
    auto* rows = list_window->findChild<QListWidget*>(QStringLiteral("bench-plugin-list-rows"));
    QVERIFY(rows != nullptr);
    QCOMPARE(rows->count(), 2);
    QCOMPARE(rows->item(0)->text(), QStringLiteral("First"));
    QCOMPARE(rows->item(0)->data(Qt::UserRole + 1).toString(), QStringLiteral("1987"));
    QCOMPARE(rows->item(0)->toolTip(), QStringLiteral("https://example.org/1"));
    list_window->findChild<QPushButton*>(QStringLiteral("bench-plugin-list-copy"))->click();
    QCOMPARE(QGuiApplication::clipboard()->text(),
             artist + QStringLiteral(": 2 missing\nFirst -- 1987\nSecond"));
    list_window->close();

    // Settings has a page of plugins, each enabled there; turned off and
    // saved, its action is gone.
    {
        auto* dialog = window.showSettingsDialog(SettingsDialog::Page::plugins);
        QVERIFY(dialog != nullptr);
        auto* pages = dialog->findChild<QListWidget*>();
        QVERIFY(pages != nullptr);
        QCOMPARE(pages->currentItem()->text(), QStringLiteral("Plugins"));
        auto* enabled = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-plugin-echo"));
        QVERIFY(enabled != nullptr && enabled->isChecked());
        QCOMPARE(enabled->text(), QStringLiteral("Echo"));
        // ADR-0300: what it asks to be set, behind its Settings button in a
        // dialog of its own; a secret not shown.
        QVERIFY(dialog->findChild<QLineEdit*>(
                    QStringLiteral("bench-settings-plugin-echo-server")) == nullptr);
        const auto edit = [dialog](const bool keep) {
            dialog->findChild<QPushButton*>(QStringLiteral("bench-settings-plugin-echo-settings"))
                ->click();
            QDialog* settings = nullptr;
            QTRY_VERIFY((settings = dialog->findChild<QDialog*>(
                             QStringLiteral("bench-plugin-settings-echo"))) != nullptr &&
                        settings->isVisible());
            auto* server = settings->findChild<QLineEdit*>(
                QStringLiteral("bench-settings-plugin-echo-server"));
            auto* token =
                settings->findChild<QLineEdit*>(QStringLiteral("bench-settings-plugin-echo-token"));
            QVERIFY(server != nullptr && token != nullptr);
            QCOMPARE(server->text(), QStringLiteral("http://localhost"));
            QCOMPARE(token->echoMode(), QLineEdit::Password);
            // The token needs Remote support: greyed until it is ticked.
            auto* remote = settings->findChild<QCheckBox*>(
                QStringLiteral("bench-settings-plugin-echo-remote"));
            QVERIFY(remote != nullptr);
            QCOMPARE(remote->text(), QStringLiteral("Remote support"));
            QVERIFY(!remote->isChecked() && !token->isEnabled());
            QVERIFY(settings
                        ->findChild<QLabel*>(
                            QStringLiteral("bench-settings-plugin-echo-remote-caption"))
                        ->text()
                        .contains(QStringLiteral("href=\"https://example.org/remote\"")));
            remote->setChecked(true);
            QVERIFY(token->isEnabled());
            // For a look at them: the dialog, and the page it is opened from.
            if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
                keep && !directory.isEmpty()) {
                QVERIFY(settings->grab().save(directory + QStringLiteral("/plugin-settings.png")));
                QVERIFY(dialog->grab().save(directory + QStringLiteral("/plugins-page.png")));
            }
            server->selectAll();
            QTest::keyClicks(server, QStringLiteral("http://nas:6600"));
            QTest::keyClicks(token, QStringLiteral("s3cret"));
            QPointer<QDialog> open = settings;
            settings->findChild<QDialogButtonBox*>(QStringLiteral("bench-plugin-settings-buttons"))
                ->button(keep ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)
                ->click();
            QTRY_VERIFY(open.isNull());
        };
        // Cancelled, nothing it set is kept: the dialog opens as it was.
        edit(false);
        if (QTest::currentTestFailed()) {
            return;
        }
        edit(true);
        if (QTest::currentTestFailed()) {
            return;
        }
        enabled->setChecked(false);
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    }
    QVERIFY(!pluginEnabled(QStringLiteral("echo")));
    QVERIFY(enabledPlugins().empty());
    // Saved: the address with the settings, the secret apart from them.
    const auto echo = discoverPlugins();
    const auto found = std::ranges::find(echo, QStringLiteral("echo"), &FoundPlugin::name);
    QVERIFY(found != echo.end() && found->plugin);
    const auto told = pluginSettings(*found->plugin);
    QCOMPARE(told.value(QStringLiteral("server")).toString(), QStringLiteral("http://nas:6600"));
    QCOMPARE(told.value(QStringLiteral("token")).toString(), QStringLiteral("s3cret"));
    QCOMPARE(told.value(QStringLiteral("remote")).toBool(), true);
    QSettings{}.sync();
    {
        QFile settings_file{QSettings{}.fileName()};
        QVERIFY(settings_file.open(QIODevice::ReadOnly));
        QVERIFY(!settings_file.readAll().contains("s3cret"));
    }
    QSettings{}.remove(QStringLiteral("plugins"));
    QFile::remove(pluginSecretsFile());
    QDir{folder}.removeRecursively();
}

// ADR-0302, ADR-0303: the official Listen together panel, drawn -- out of
// a room, a room's name and password to start one; started, the room's
// queue with its votes -- against a stand-in for melody-cli.
void BenchMainWindowTest::theListeningRoomPanelIsTheRoom() {
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    const auto in_room = bin.filePath(QStringLiteral("in-room"));
    {
        QFile cli{bin.filePath(QStringLiteral("melody-cli"))};
        QVERIFY(cli.open(QIODevice::WriteOnly));
        cli.write(QStringLiteral(R"sh(#!/bin/sh
case "$3" in
  host|join) touch '%1'; echo '{}' ;;
  leave) rm -f '%1' ;;
  status)
    if [ -e '%1' ]; then
      echo '{"role":"host","room":"friday-doom","invite":"https://t.example/rooms/friday-doom#letmein","listeners":["Ana","Bo","Cy"],"track":"Royal Thunder — Time Machine","message":"","party":true,"suggestions":[{"id":7,"name":"Ana","title":"Glow","artist":"Royal Thunder","votes":3,"voted":true,"ready":true},{"id":8,"name":"Cy","title":"Wake Up","artist":"Royal Thunder","votes":1,"voted":false,"ready":false}],"by":"Ana","next":"Royal Thunder — Forget You","history":["Royal Thunder — One Day","Royal Thunder — Floor"],"upcoming":[]}'
    else
      echo '{"role":"","message":""}'
    fi ;;
esac
)sh")
                      .arg(in_room)
                      .toUtf8());
        cli.close();
        cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    const auto path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(bin.path()) + ":" + path);
    // The official plugin, as installed.
    const auto folder = pluginFolders().value(0) + QStringLiteral("/together");
    QVERIFY(QDir{}.mkpath(folder));
    for (const auto* name : {"plugin.json", "together"}) {
        QFile::remove(folder + QLatin1Char('/') + QLatin1String(name));
        QVERIFY(QFile::copy(QStringLiteral(MELODY_AUDIO_FIXTURE_DIR "/../../../plugins/together/") +
                                QLatin1String(name),
                            folder + QLatin1Char('/') + QLatin1String(name)));
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("together")), true);
    QSettings{}.setValue(pluginSettingKey(QStringLiteral("together"), QStringLiteral("server")),
                         QStringLiteral("https://t.example"));

    BenchMainWindow window;
    window.resize(1400, 900);
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-together-room"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());
    QTest::qWait(500);
    window.transport_timer_->stop();
    window.refreshPluginPanels(EnginePlayback::State{});
    QLineEdit* room = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((room = dock->findChild<QLineEdit*>(
                                  QStringLiteral("bench-plugin-panel-input-room"))) != nullptr,
                             10'000);
    const auto shot = [&dock](const QString& name) {
        if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
            !directory.isEmpty()) {
            QVERIFY(dock->grab().save(directory + QLatin1Char('/') + name));
        }
    };
    shot(QStringLiteral("room-panel-out.png"));
    QTest::keyClicks(room, QStringLiteral("friday-doom"));
    for (auto* button :
         dock->findChildren<QPushButton*>(QStringLiteral("bench-plugin-panel-button"))) {
        if (button->property("action").toString() == QStringLiteral("start")) {
            button->click();
        }
    }
    QTRY_VERIFY_WITH_TIMEOUT(dock->findChild<QAbstractButton*>(
                                 QStringLiteral("bench-plugin-panel-toggle-party")) != nullptr,
                             10'000);
    QVERIFY(dock->findChild<QAbstractButton*>(QStringLiteral("bench-plugin-panel-toggle-party"))
                ->isChecked());
    // Two votes, two removes; the room's web page and Stop.
    QCOMPARE(
        dock->findChildren<QToolButton*>(QStringLiteral("bench-plugin-panel-item-button")).size(),
        6);
    // ADR-0304: drawn as Melody's -- what plays, the next, two suggestions,
    // two played; the status; the people.
    const auto tracks = dock->findChildren<QWidget*>(QStringLiteral("bench-plugin-panel-track"));
    QCOMPARE(tracks.size(), 6);
    QVERIFY(tracks.front()->property("playing").toBool());
    QCOMPARE(tracks.front()
                 ->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-track-title"))
                 ->text(),
             QStringLiteral("Time Machine"));
    QVERIFY(dock->findChild<QWidget*>(QStringLiteral("bench-plugin-panel-status")) != nullptr);
    QCOMPARE(dock->findChildren<QWidget*>(QStringLiteral("bench-plugin-panel-person")).size(), 3);
    // The host's remove, quiet: shown only under the pointer -- on a row;
    // the status line's buttons always.
    for (auto* track : tracks) {
        for (auto* button :
             track->findChildren<QToolButton*>(QStringLiteral("bench-plugin-panel-item-button"))) {
            QCOMPARE(button->isVisibleTo(dock), button->property("role").toString().isEmpty());
        }
    }
    auto* status = dock->findChild<QWidget*>(QStringLiteral("bench-plugin-panel-status"));
    for (auto* button :
         status->findChildren<QToolButton*>(QStringLiteral("bench-plugin-panel-item-button"))) {
        QVERIFY(button->isVisibleTo(dock));
    }
    // Asked with nothing playing too, it does not name a track under the
    // room's name.
    QVERIFY(dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-asked")) == nullptr);
    QTest::qWait(300);
    shot(QStringLiteral("room-panel-in.png"));
    qputenv("PATH", path);
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0303: a panel of controls, asked with nothing playing; a button
// pressed runs it again, told what was pressed and what was typed -- which
// stays typed when it answers.
void BenchMainWindowTest::aPanelsControlsAskItAgain() {
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const auto folder = pluginFolders().value(0) + QStringLiteral("/controls");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Controls", "panels": [{"id": "p",
                          "label": "Controls", "run": ["./p"], "always": true}]})");
        QFile script{folder + QStringLiteral("/p")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral(R"sh(#!/bin/sh
input=$(cat)
printf '%s\n' "$input" >> '%1'
did=$(printf '%s' "$input" | grep -o '"action":{"id":"[^"]*","value":"[^"]*"}' | sed 's/.*"id":"\([^"]*\)","value":"\([^"]*\)".*/\1:\2/')
printf '{"title": "did %s", "parts": [{"input": "room", "label": "Room"},
  {"button": "Start", "action": "start", "primary": true},
  {"text": "Glow", "buttons": [{"text": "vote", "action": "vote", "value": "7"}]}]}' "$did"
)sh")
                         .arg(out.filePath(QStringLiteral("told")))
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("controls")), true);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-controls-p"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());
    QTest::qWait(500);
    window.transport_timer_->stop();
    // Nothing playing: asked all the same.
    window.refreshPluginPanels(EnginePlayback::State{});
    QLabel* title = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (title = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))) != nullptr,
        10'000);
    auto* room = dock->findChild<QLineEdit*>(QStringLiteral("bench-plugin-panel-input-room"));
    QVERIFY(room != nullptr);
    QTest::keyClicks(room, QStringLiteral("friday-doom"));
    QPushButton* start = nullptr;
    for (auto* button :
         dock->findChildren<QPushButton*>(QStringLiteral("bench-plugin-panel-button"))) {
        if (button->text() == QStringLiteral("Start")) {
            start = button;
        }
    }
    QVERIFY(start != nullptr && start->isDefault());
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        (title = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))) != nullptr &&
            title->text() == QStringLiteral("did start:"),
        10'000);
    QFile told{out.filePath(QStringLiteral("told"))};
    QVERIFY(told.open(QIODevice::ReadOnly));
    const auto lines = told.readAll().trimmed().split('\n');
    const auto last = QJsonDocument::fromJson(lines.last()).object();
    QCOMPARE(
        last.value(QStringLiteral("inputs")).toObject().value(QStringLiteral("room")).toString(),
        QStringLiteral("friday-doom"));
    QVERIFY(last.value(QStringLiteral("playing")).isNull());
    // Typed, it stays typed over the answer.
    QCOMPARE(dock->findChild<QLineEdit*>(QStringLiteral("bench-plugin-panel-input-room"))->text(),
             QStringLiteral("friday-doom"));
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dock->grab().save(directory + QStringLiteral("/panel-controls.png")));
    }
    // An item's button: its action and value.
    auto* vote = dock->findChild<QToolButton*>(QStringLiteral("bench-plugin-panel-item-button"));
    QVERIFY(vote != nullptr);
    vote->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))->text() ==
            QStringLiteral("did vote:7"),
        10'000);
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0312: a panel whose answer says so is asked again about the same,
// after the seconds it names -- and no more once it stops saying so.
void BenchMainWindowTest::aPanelIsAskedAgainWhenItSays() {
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const auto folder = pluginFolders().value(0) + QStringLiteral("/coming");
    QVERIFY(QDir{}.mkpath(folder));
    const auto asked = out.filePath(QStringLiteral("asked"));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Coming", "panels": [{"id": "p",
                          "label": "Coming", "run": ["./p"], "always": true}]})");
        QFile script{folder + QStringLiteral("/p")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        // Asked three times: twice asking again, the third not.
        script.write(QStringLiteral(R"sh(#!/bin/sh
cat > /dev/null
echo x >> '%1'
n=$(wc -l < '%1')
if [ "$n" -lt 3 ]; then
  printf '{"title": "run %s", "parts": [], "again_after": 1}' "$n"
else
  printf '{"title": "run %s", "parts": []}' "$n"
fi
)sh")
                         .arg(asked)
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("coming")), true);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-coming-p"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());
    QTest::qWait(500);
    window.transport_timer_->stop();
    window.refreshPluginPanels(EnginePlayback::State{});
    const auto title = [dock] {
        auto* label = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"));
        return label == nullptr ? QString{} : label->text();
    };
    QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("run 3"), 10'000);
    // Not asked since: the third answer did not ask.
    QTest::qWait(2'500);
    QFile told{asked};
    QVERIFY(told.open(QIODevice::ReadOnly));
    QCOMPARE(told.readAll().count('\n'), 3);
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0302: a panel showing the listening room is asked again as the room
// changes -- a vote, a friend arriving -- coalesced; one that did not ask
// for it is not.
void BenchMainWindowTest::aRoomPanelIsAskedAgainAsTheRoomChanges() {
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const auto write_plugin = [&out](const QString& name, const QString& refresh) {
        const auto folder = pluginFolders().value(0) + QLatin1Char('/') + name;
        QDir{}.mkpath(folder);
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(QStringLiteral(R"({"schema": 1, "name": "%1", "panels": [{"id": "p",
                          "label": "%1", "run": ["./p"]%2}]})")
                           .arg(name, refresh)
                           .toUtf8());
        QFile script{folder + QStringLiteral("/p")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral("#!/bin/sh\ncat > /dev/null\necho x >> '%1'\n"
                                    "printf '{\"title\": \"%2\", \"parts\": []}'\n")
                         .arg(out.filePath(name), name)
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QSettings{}.setValue(pluginEnabledKey(name), true);
    };
    write_plugin(QStringLiteral("room"), QStringLiteral(R"(, "refresh": ["listening-room"])"));
    write_plugin(QStringLiteral("plain"), QString{});
    const auto asked = [&out](const QString& name) {
        QFile file{out.filePath(name)};
        return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
    };

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    for (const auto* name : {"room", "plain"}) {
        auto* dock = window.findChild<QDockWidget*>(
            QStringLiteral("bench-plugin-panel-%1-p").arg(QLatin1String(name)));
        QVERIFY(dock != nullptr);
        if (dock->isHidden()) {
            window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
        }
        QTRY_VERIFY(dock->isVisible());
    }
    QTest::qWait(500);
    window.transport_timer_->stop();
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = QStringLiteral("/srv/music/a.flac");
    window.refreshPluginPanels(state);
    QTRY_VERIFY_WITH_TIMEOUT(
        asked(QStringLiteral("room")) >= 1 && asked(QStringLiteral("plain")) >= 1, 10'000);
    QTest::qWait(300);
    const auto room_before = asked(QStringLiteral("room"));
    const auto plain_before = asked(QStringLiteral("plain"));
    window.listeningRoomChanged();
    window.listeningRoomChanged();
    QTRY_VERIFY_WITH_TIMEOUT(asked(QStringLiteral("room")) == room_before + 1, 5'000);
    QTest::qWait(800);
    QCOMPARE(asked(QStringLiteral("room")), room_before + 1);
    QCOMPARE(asked(QStringLiteral("plain")), plain_before);
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{pluginFolders().value(0) + QStringLiteral("/room")}.removeRecursively();
    QDir{pluginFolders().value(0) + QStringLiteral("/plain")}.removeRecursively();
}

// ADR-0302: an invite opened in a browser hands Melody a melody:// link, and
// Melody asks its engine to join -- told here of a server not there.
void BenchMainWindowTest::aTogetherLinkAsksTheEngineToJoin() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().playback != nullptr &&
                                 window.localEngine().playback->active(),
                             10'000);
    QVERIFY(!window.openLink(QStringLiteral("https://example.org/rooms/r#k")));
    QVERIFY(window.openLink(QStringLiteral("melody://together/join")));
    QTRY_VERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("no invite")));
    // Every message the window shows from here: the status bar is shared,
    // and the refusal may be followed at once by another, from a window
    // still starting -- shown is what counts, not still showing.
    QStringList said;
    connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&said](const QString& text) {
        if (!text.isEmpty()) {
            said << text;
        }
    });
    // Port 9 on this machine: nothing answers.
    QVERIFY(window.openLink(QStringLiteral("melody://together/join?invite=") +
                            QString::fromLatin1(QUrl::toPercentEncoding(
                                QStringLiteral("http://127.0.0.1:9/rooms/r#k")))));
    QTRY_VERIFY2_WITH_TIMEOUT(std::ranges::any_of(said,
                                                  [](const QString& text) {
                                                      return text.startsWith(QStringLiteral(
                                                          "Could not join the listening room"));
                                                  }),
                              qPrintable(said.join(QStringLiteral(" | "))), 15'000);
}

// ADR-0301: a panel is told every tag the library has of what plays, and,
// asking for them, the artist's albums there -- by the album artist's
// MusicBrainz id and name, one for each release group.
void BenchMainWindowTest::aPluginPanelIsToldTheLibrary() {
    const auto folder = pluginFolders().value(0) + QStringLiteral("/told");
    QVERIFY(QDir{}.mkpath(folder));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto input = media.filePath(QStringLiteral("input.json"));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Told", "panels": [{"id": "told",
                          "label": "Told", "run": ["./told"], "library": ["artist-albums"]}]})");
        QFile script{folder + QStringLiteral("/told")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        // What it was given, kept for the test to read.
        script.write(QStringLiteral("#!/bin/sh\ncat > '%1'\n"
                                    "printf '{\"title\": \"Told\", \"parts\": []}'\n")
                         .arg(input)
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("told")), true);
    // Two tracks of one release group, and another artist's.
    const auto music = media.filePath(QStringLiteral("music"));
    for (const auto* name : {"Credit/Rich/one.flac", "Credit/Rich/two.flac"}) {
        const auto path = music + QLatin1Char('/') + QLatin1String(name);
        QVERIFY(QDir{}.mkpath(QFileInfo{path}.absolutePath()));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    }
    const auto other = music + QStringLiteral("/Project/Tone/tone.flac");
    QVERIFY(QDir{}.mkpath(QFileInfo{other}.absolutePath()));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), other));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto* panel = window.localLibrary();
    QVERIFY(panel != nullptr);
    panel->browser().addRoot(QFile::encodeName(music).toStdString());
    QTRY_VERIFY(!panel->browser().rootPaths().empty());
    {
        persistence::LibraryScanProgress progress;
        QVERIFY(window.localCatalogue()->open()->scan({}, progress).has_value());
    }
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-told-told"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());

    // Told here that it plays; the engine, playing nothing, says nothing
    // of it, and the library says the rest.
    QTest::qWait(500);
    window.transport_timer_->stop();
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = music + QStringLiteral("/Credit/Rich/one.flac");
    window.refreshPluginPanels(state);
    QTRY_VERIFY_WITH_TIMEOUT(
        dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title")) != nullptr, 10'000);
    QFile told_file{input};
    QVERIFY(told_file.open(QIODevice::ReadOnly));
    const auto told = QJsonDocument::fromJson(told_file.readAll()).object();
    const auto tags =
        told.value(QStringLiteral("playing")).toObject().value(QStringLiteral("tags")).toObject();
    QCOMPARE(tags.value(QStringLiteral("musicbrainzalbumartistid")).toArray().first().toString(),
             QStringLiteral("77777777-7777-7777-7777-777777777777"));
    QCOMPARE(tags.value(QStringLiteral("artist")).toArray().size(), 2);
    // A tag the library does not know as it is spelled, case aside.
    QCOMPARE(tags.value(QStringLiteral("custom_field")).toArray().last().toString(),
             QStringLiteral("second custom value"));
    const auto albums = told.value(QStringLiteral("artist_albums")).toArray();
    QCOMPARE(albums.size(), 1);
    const auto album = albums.first().toObject();
    QCOMPARE(album.value(QStringLiteral("album")).toString(), QStringLiteral("Rich Metadata"));
    QCOMPARE(album.value(QStringLiteral("album_artist")).toString(),
             QStringLiteral("Album Credit"));
    QCOMPARE(album.value(QStringLiteral("tracks")).toInt(), 2);
    QCOMPARE(album.value(QStringLiteral("relative_folder")).toString(),
             QStringLiteral("Credit/Rich"));
    QCOMPARE(album.value(QStringLiteral("tags"))
                 .toObject()
                 .value(QStringLiteral("musicbrainzreleasegroupid"))
                 .toArray()
                 .first()
                 .toString(),
             QStringLiteral("44444444-4444-4444-4444-444444444444"));
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0291: a plugin's panel, toggled in the Playback menu beside Lyrics,
// asked about what plays while open and drawn as the parts it answers.
void BenchMainWindowTest::aPluginPanelShowsWhatItSaysAboutThePlayingTrack() {
    const auto folder = pluginFolders().value(0) + QStringLiteral("/about");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "About", "panels": [{"id": "artist",
                          "label": "About the artist", "run": ["./about"]}],
                          "settings": [{"key": "greeting", "label": "Greeting",
                                        "default": "Hello"}]})");
        QFile script{folder + QStringLiteral("/about")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"sh(#!/bin/sh
input=$(cat)
artist=$(printf '%s' "$input" | grep -o '"artist":"[^"]*"' | head -n 1 | cut -d '"' -f 4)
greeting=$(printf '%s' "$input" | grep -o '"greeting":"[^"]*"' | cut -d '"' -f 4)
path=$(printf '%s' "$input" | grep -o '"path":"[^"]*"' | head -n 1 | cut -d '"' -f 4)
printf '{"title": "About %s", "parts": [{"heading": "About"}, {"paragraph": "%s from %s."}, {"text": "Site", "detail": "official", "url": "https://example.org"}]}' "$artist" "$greeting" "$path"
)sh");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("about")), true);

    BenchMainWindow window;
    window.show();
    auto* toggle =
        window.findChild<QAction*>(QStringLiteral("action-show-plugin-panel-about-artist"));
    QVERIFY(toggle != nullptr);
    QCOMPARE(toggle->text(), QStringLiteral("About the artist"));
    // ADR-0299: no switch of its own in a menu -- enabled, it is there,
    // shown with the side panels by Panels.
    auto* lyrics = window.findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    for (auto* menu : window.menuBar()->findChildren<QMenu*>()) {
        QVERIFY(!menu->actions().contains(lyrics) && !menu->actions().contains(toggle));
    }
    QVERIFY(window.createPopupMenu() == nullptr);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-about-artist"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());
    QVERIFY(dock->findChild<QToolButton*>(QStringLiteral("bench-plugin-panel-close")) == nullptr);
    auto* message = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-message"));
    QVERIFY(message != nullptr);
    QCOMPARE(message->text(), QStringLiteral("Nothing is playing"));

    // A track plays -- told here, not by the engine, which plays nothing in
    // this test: asked about it, the answer drawn as its parts.
    // Once the engine has said what it has, it says nothing more here.
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QTest::qWait(500);
    window.transport_timer_->stop();
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = QStringLiteral("/srv/music/Testament/01.flac");
    window.refreshPluginPanels(state);
    QLabel* title = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (title = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))) != nullptr,
        10'000);
    QStringList parts;
    for (const auto* part :
         dock->findChildren<QLabel*>(QStringLiteral("bench-plugin-panel-part"))) {
        parts << part->property("part").toString();
    }
    QVERIFY2(parts.contains(QStringLiteral("heading")) &&
                 parts.contains(QStringLiteral("paragraph")) &&
                 parts.contains(QStringLiteral("item")),
             qPrintable(parts.join(QLatin1Char(','))));
    const auto paragraphs = dock->findChildren<QLabel*>(QStringLiteral("bench-plugin-panel-part"));
    QVERIFY(std::ranges::any_of(paragraphs, [](const QLabel* label) {
        return label->property("part").toString() == QStringLiteral("paragraph") &&
               label->text() == QStringLiteral("Hello from /srv/music/Testament/01.flac.");
    }));
    QCOMPARE(dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-detail"))->text(),
             QStringLiteral("official"));
    // For a look at it: an About as the official plugin answers one.
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QImage photo{480, 320, QImage::Format_RGB32};
        photo.fill(QColor{90, 110, 140});
        const auto photo_path = directory + QStringLiteral("/photo.png");
        QVERIFY(photo.save(photo_path));
        PluginPanelContent about;
        about.title = QStringLiteral("Testament");
        using Part = PluginPanelContent::Part;
        about.parts = {
            Part{.kind = Part::Kind::image,
                 .text = {},
                 .detail = {},
                 .url = {},
                 .image = photo_path,
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::item,
                 .text = QStringLiteral("Group from Berkeley, United States"),
                 .detail = QStringLiteral("since 1983"),
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::paragraph,
                 .text = QStringLiteral(
                     "Testament is an American thrash metal band from Berkeley, California. "
                     "Formed in 1983 under the name Legacy, the band's current lineup comprises "
                     "vocalist Chuck Billy, guitarists Eric Peterson and Alex Skolnick, bassist "
                     "Steve Di Giorgio and drummer Chris Dovas. Testament has released fourteen "
                     "studio albums, and is one of the most successful thrash metal bands of "
                     "the Bay Area scene, credited with influencing many that followed."),
                 .detail = {},
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Wikipedia"),
                 .detail = QStringLiteral("Testament"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Official site"),
                 .detail = QStringLiteral("Testament"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("MusicBrainz"),
                 .detail = QStringLiteral("Testament"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::heading,
                 .text = QStringLiteral("The Legacy"),
                 .detail = {},
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::item,
                 .text = QStringLiteral("Album · 1987"),
                 .detail = QStringLiteral("Atlantic, Megaforce Worldwide"),
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::paragraph,
                 .text = QStringLiteral("The Legacy is the debut studio album by Testament."),
                 .detail = {},
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Wikipedia"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("MusicBrainz"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Discogs"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Bandcamp"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Rate Your Music"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("AllMusic"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
        };
        PluginPanelView shown{QStringLiteral("About"), nullptr};
        shown.resize(300, 900);
        shown.setSubject(QStringLiteral("Testament — Over the Wall"));
        shown.showContent(about);
        shown.show();
        QTest::qWait(200);
        QVERIFY(shown.grab().save(directory + QStringLiteral("/about.png")));
    }
    // Escaped and shown as rich text: a quote or an ampersand reads as one.
    for (const auto* part : dock->findChildren<QLabel*>()) {
        if (QStringList{QStringLiteral("bench-plugin-panel-part"),
                        QStringLiteral("bench-plugin-panel-title"),
                        QStringLiteral("bench-plugin-panel-detail")}
                .contains(part->objectName())) {
            QCOMPARE(part->textFormat(), Qt::RichText);
        }
    }

    // Turned off and saved, its panel goes.
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("about")), false);
    window.buildPluginPanels();
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-about-artist")) ==
            nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-show-plugin-panel-about-artist")) ==
            nullptr);
    QSettings{}.remove(QStringLiteral("plugins"));
    QSettings{}.remove(QStringLiteral("plugin-panels"));
    QDir{folder}.removeRecursively();
}

// ADR-0293: no lyrics in the file -- the lyrics providers asked in turn,
// the first to find them shown in the Lyrics panel, saying where from.
void BenchMainWindowTest::lyricsProvidersAreAskedWhenTheFileHasNone() {
    const auto provider = [](const QString& name, const QByteArray& answer) {
        const auto folder = pluginFolders().value(0) + QLatin1Char('/') + name;
        QVERIFY(QDir{}.mkpath(folder));
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(QStringLiteral(R"({"schema": 1, "name": "%1", "providers": [{"id": "l",
                                         "provides": "lyrics", "run": ["./words"]}]})")
                           .arg(name)
                           .toUtf8());
        QFile script{folder + QStringLiteral("/words")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\ncat > /dev/null\nprintf '%s' '" + answer + "'\n");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QSettings{}.setValue(pluginEnabledKey(name), true);
    };
    provider(QStringLiteral("a-nothing"), R"({"lyrics": null})");
    provider(
        QStringLiteral("words"),
        R"({"lyrics": "[00:01.00]First line\n[00:02.00]Second line", "source": "Words test"})");
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto file = media.filePath(QStringLiteral("no-lyrics.wav"));
    write_wave(file, 44'100);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QTest::qWait(500);
    window.transport_timer_->stop();
    auto* show_lyrics = window.findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    QVERIFY(show_lyrics != nullptr);
    if (!show_lyrics->isChecked()) {
        show_lyrics->trigger();
    }
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-lyrics"));
    QTRY_VERIFY(dock != nullptr && dock->isVisible());
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = file;
    window.refreshLyrics(state);
    auto* lines = dock->findChild<QListWidget*>(QStringLiteral("lyrics-lines"));
    QVERIFY(lines != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(lines->count(), 2, 15'000);
    QCOMPARE(lines->item(0)->text(), QStringLiteral("First line"));
    QCOMPARE(dock->findChild<QLabel*>(QStringLiteral("lyrics-source"))->text(),
             QStringLiteral("Synced · from Words test"));
    show_lyrics->trigger();
    QSettings{}.remove(QStringLiteral("plugins"));
    QSettings{}.remove(QStringLiteral("lyrics"));
    for (const auto* name : {"a-nothing", "words"}) {
        QDir{pluginFolders().value(0) + QLatin1Char('/') + QLatin1String(name)}.removeRecursively();
    }
}

// ADR-0288: more than a few plugin entries go in a submenu for each plugin,
// under its name; a few stand in the menu itself (above).
void BenchMainWindowTest::manyPluginActionsGoInASubmenu() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto file = media.filePath(QStringLiteral("01.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), file));
    const auto folder = pluginFolders().value(0) + QStringLiteral("/many");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Many", "actions": [
            {"id": "a", "label": "First", "run": ["true"]},
            {"id": "b", "label": "Second", "run": ["true"]},
            {"id": "c", "label": "Third", "run": ["true"]},
            {"id": "d", "label": "Fourth", "run": ["true"]}]})");
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("many")), true);
    // A plugin with only a panel, and one with only a provider: nothing of
    // theirs in the menu.
    const auto panel_only = pluginFolders().value(0) + QStringLiteral("/side");
    const auto provider_only = pluginFolders().value(0) + QStringLiteral("/finder");
    for (const auto& [where, json] :
         {std::pair{panel_only, QByteArray{R"({"schema": 1, "name": "Side", "panels": [
             {"id": "p", "label": "Side panel", "run": ["true"]}]})"}},
          std::pair{provider_only, QByteArray{R"({"schema": 1, "name": "Finder", "providers": [
             {"id": "l", "provides": "lyrics", "run": ["true"]}]})"}}}) {
        QVERIFY(QDir{}.mkpath(where));
        QFile manifest{where + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(json);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("side")), true);
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("finder")), true);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    const auto encoded = QFile::encodeName(file);
    window.openLocalPaths(
        {std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
    Workspace::ListTab* tab = nullptr;
    QTRY_VERIFY((tab = [&window]() -> Workspace::ListTab* {
                    for (auto& candidate : window.list_tabs_) {
                        if (candidate->model->rowCount() == 1) {
                            return candidate.get();
                        }
                    }
                    return nullptr;
                }()) != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    tab->view->selectionModel()->select(
        tab->model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(
        tab->view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, tab->view->visualRect(tab->model->index(0, local_title_column)).center())));
    auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    QVERIFY(menu != nullptr);
    auto* submenu = menu->findChild<QMenu*>(QStringLiteral("menu-plugin-many"));
    QVERIFY(submenu != nullptr);
    QCOMPARE(submenu->title(), QStringLiteral("Many"));
    QCOMPARE(submenu->actions().size(), 4);
    QCOMPARE(submenu->actions().front()->text(), QStringLiteral("First"));
    // None of them in the menu itself.
    QVERIFY(std::ranges::none_of(menu->actions(), [](const QAction* action) {
        return action->text() == QStringLiteral("First");
    }));
    QVERIFY(menu->findChild<QMenu*>(QStringLiteral("menu-plugin-side")) == nullptr);
    QVERIFY(menu->findChild<QMenu*>(QStringLiteral("menu-plugin-finder")) == nullptr);
    menu->close();
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
    QDir{panel_only}.removeRecursively();
    QDir{provider_only}.removeRecursively();
}

// ADR-0295: a plugin's triggers -- at start, by hand from Workspace › Run,
// when the library has changed and been still, for new albums -- one run of
// each at a time, asked again while it runs, once more after.
void BenchMainWindowTest::pluginTriggersRunOnTheLibrarysNews() {
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const auto log = out.filePath(QStringLiteral("log"));
    const auto folder = pluginFolders().value(0) + QStringLiteral("/hooks");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Hooks", "triggers": [
            {"id": "page", "label": "Write the page", "run": ["./hook"],
             "on": ["start", "hand", "library-changed"]},
            {"id": "new", "on": "albums-added", "run": ["./hook"]}]})");
        QFile script{folder + QStringLiteral("/hook")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral(R"sh(#!/bin/sh
input=$(cat)
on=$(printf '%s' "$input" | grep -o '"on":"[^"]*"' | cut -d '"' -f 4)
folders=$(printf '%s' "$input" | grep -o '"folder":"[^"]*"' | cut -d '"' -f 4 | tr '\n' ' ')
echo "$on $folders" >> '%1'
[ "$on" = library-changed ] && sleep 1
exit 0
)sh")
                         .arg(log)
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("hooks")), true);
    qputenv("MELODY_LIBRARY_SETTLE_SECONDS", "0");
    const auto lines = [&log] {
        QFile file{log};
        if (!file.open(QIODevice::ReadOnly)) {
            return QStringList{};
        }
        QStringList read;
        for (const auto& line :
             QString::fromUtf8(file.readAll()).trimmed().split(QLatin1Char('\n'))) {
            read << line.trimmed();
        }
        return read;
    };

    BenchMainWindow window;
    window.show();
    // At start, once.
    QTRY_COMPARE(lines(), QStringList{QStringLiteral("start")});

    // By hand, from Workspace › Run.
    auto* menu = window.findChild<QMenu*>(QStringLiteral("menu-run-plugins"));
    QVERIFY(menu != nullptr && menu->menuAction()->isVisible());
    auto* page = window.findChild<QAction*>(QStringLiteral("action-run-plugin-hooks-page"));
    QVERIFY(page != nullptr && menu->actions().contains(page));
    QCOMPARE(page->text(), QStringLiteral("Write the page"));
    page->trigger();
    QTRY_COMPARE(lines().size(), 2);
    QCOMPARE(lines().back(), QStringLiteral("hand"));

    // The library changed, three times while it ran: once, and once after.
    window.libraryChanged();
    QTRY_COMPARE(lines().size(), 3);
    window.libraryChanged();
    window.libraryChanged();
    QTRY_COMPARE(lines().size(), 4);
    QTest::qWait(1'500);
    QCOMPARE(lines().size(), 4);
    QCOMPARE(lines().mid(2),
             (QStringList{QStringLiteral("library-changed"), QStringLiteral("library-changed")}));

    // New albums, told with their folders.
    window.albumsAdded({AlbumArrival{.artist = "A",
                                     .album = "B",
                                     .date = "1999",
                                     .folder = "/srv/music/A/B",
                                     .paths = {"/srv/music/A/B/01.flac"}}});
    QTRY_COMPARE(lines().size(), 5);
    QCOMPARE(lines().back(), QStringLiteral("albums-added /srv/music/A/B"));

    // Turned off: its entry goes, and the menu with it.
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("hooks")), false);
    window.buildPluginTriggers();
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-run-plugin-hooks-page")) == nullptr);
    QVERIFY(!menu->menuAction()->isVisible());
    window.libraryChanged();
    QTest::qWait(300);
    QCOMPARE(lines().size(), 5);

    qunsetenv("MELODY_LIBRARY_SETTLE_SECONDS");
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0296: Settings › Engines says where the engine's playlists are --
// Playlists in its music folder -- and a style chosen there is the engine's.
void BenchMainWindowTest::settingsSayAndChooseThePlaylistFolder() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QVERIFY(window.localCatalogue()->open()->add_root(QFile::encodeName(music).toStdString()));
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::engine);
    QPointer<QDialog> guard{dialog};
    QRadioButton* standard = nullptr;
    QTRY_VERIFY((standard = dialog->findChild<QRadioButton*>(
                     QStringLiteral("bench-playlist-folder-default"))) != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(standard->isChecked() && standard->isEnabled(), 10'000);
    QVERIFY2(standard->text().endsWith(music + QStringLiteral("/Playlists")),
             qPrintable(standard->text()));
    auto* style = dialog->findChild<QComboBox*>(QStringLiteral("bench-playlist-folder-style"));
    QVERIFY(style != nullptr);
    QCOMPARE(style->currentData().toString(), QStringLiteral("root"));
    const auto absolute = style->findData(QStringLiteral("absolute"));
    style->setCurrentIndex(absolute);
    emit style->activated(absolute);
    QTRY_VERIFY_WITH_TIMEOUT(!style->isEnabled(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(style->isEnabled(), 10'000);
    // The engine's now: asked again, it says so.
    auto* again = new PlaylistFolderSession(window.localEngine().file_work, {}, &window);
    again->load();
    QTRY_VERIFY_WITH_TIMEOUT(again->loaded() && again->style() == "absolute", 10'000);
    // A folder typed -- on the engine's machine -- is taken as one chosen.
    QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    auto* path = dialog->findChild<QLineEdit*>(QStringLiteral("bench-playlist-folder-path"));
    QVERIFY(path != nullptr && !path->isReadOnly());
    path->setText(elsewhere.path());
    emit path->textEdited(path->text());
    emit path->returnPressed();
    auto* chosen = dialog->findChild<QRadioButton*>(QStringLiteral("bench-playlist-folder-chosen"));
    QTRY_VERIFY_WITH_TIMEOUT(chosen->isChecked() && chosen->isEnabled(), 10'000);
    // One that is not a folder there is refused, and stays to be mended.
    path->setText(elsewhere.filePath(QStringLiteral("not-there")));
    emit path->textEdited(path->text());
    emit path->returnPressed();
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-playlist-folder-status"));
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Not changed")), 10'000);
    QCOMPARE(path->text(), elsewhere.filePath(QStringLiteral("not-there")));
    if (guard) {
        dialog->reject();
    }
}

// ADR-0296, ADR-0298: the library's Playlists tab, its engine's playlists --
// a file someone dropped in the playlist folder too -- one opened as a tab by
// choosing it.
void BenchMainWindowTest::thePlaylistsTabOpensAPlaylistFile() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/Playlists")));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    // As MPD writes it: the track named under the music folder.
    {
        QFile mix{music + QStringLiteral("/Playlists/mix.m3u")};
        QVERIFY(mix.open(QIODevice::WriteOnly));
        mix.write("art.flac\n");
    }
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QVERIFY(window.localCatalogue()->open()->add_root(QFile::encodeName(music).toStdString()));

    // The library's Playlists tab: its engine's playlists.
    auto* sources = window.local_source_tabs_;
    auto* playlists = window.findChild<ListsPanel*>(QStringLiteral("bench-library-playlists"));
    QVERIFY(playlists != nullptr);
    QTRY_VERIFY(window.localLibrary() != nullptr);
    sources->setCurrentIndex(0);
    QTRY_VERIFY(!playlists->isVisibleTo(&window));
    sources->setCurrentIndex(window.playlists_tab_);
    QTRY_VERIFY(playlists->isVisibleTo(&window));
    // The engine reads its folder every few seconds; the list then shows.
    QTreeWidgetItem* found = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((found = [playlists]() -> QTreeWidgetItem* {
                                 const auto items =
                                     playlists->findItems(QStringLiteral("mix"),
                                                          Qt::MatchStartsWith | Qt::MatchRecursive);
                                 return items.isEmpty() ? nullptr : items.front();
                             }()) != nullptr,
                             15'000);
    const auto before = window.list_tabs_.size();
    playlists->setCurrentItem(found);
    QTRY_COMPARE(window.list_tabs_.size(), before + 1);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QCOMPARE(QString::fromStdString(tab->model->rows().front().raw_path),
             music + QStringLiteral("/art.flac"));
    // The engine's own only, flat.
    QCOMPARE(playlists->topLevelItemCount(), 1);
    QCOMPARE(playlists->topLevelItem(0)->text(0), QStringLiteral("mix"));
}

// ADR-0297: the heart by what plays -- into the Favorites playlist of the
// engine playing it, and out again; shown only while something plays.
void BenchMainWindowTest::theHeartMakesWhatPlaysAFavourite() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    const auto file = music + QStringLiteral("/art.flac");
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), file));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QVERIFY(window.localCatalogue()->open()->add_root(QFile::encodeName(music).toStdString()));
    // Told here what plays; the engine, in this test, plays nothing.
    QTest::qWait(500);
    window.transport_timer_->stop();
    auto* heart = window.findChild<QToolButton*>(QStringLiteral("bench-favorite"));
    auto* action = window.findChild<QAction*>(QStringLiteral("action-favorite"));
    QVERIFY(heart != nullptr && action != nullptr);
    QVERIFY(heart->isHidden());
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = file;
    window.refreshFavorite(state);
    QVERIFY(!heart->isHidden());
    QCOMPARE(heart->text(), QStringLiteral("♡"));
    action->trigger();
    QTRY_COMPARE(heart->text(), QStringLiteral("♥"));
    QVERIFY(action->isChecked());
    // The engine's: a Favorites playlist with the track.
    bool asked = false;
    window.localPlayback()->request(
        QStringLiteral("favorites.state"),
        protocol::Json{{"paths", protocol::Json::array({protocol::encode_raw_path(
                                     QFile::encodeName(file).toStdString())})}},
        [&asked](core::Result<protocol::Json> answer) {
            asked = answer && answer->at("favorites").front().get<bool>();
        });
    QTRY_VERIFY(asked);
    // Asked again for the same track, after another: still a favourite.
    state.entry = QStringLiteral("e2");
    state.path = music + QStringLiteral("/other.flac");
    window.refreshFavorite(state);
    QTRY_COMPARE(heart->text(), QStringLiteral("♡"));
    state.entry = QStringLiteral("e1");
    state.path = file;
    window.refreshFavorite(state);
    QTRY_COMPARE(heart->text(), QStringLiteral("♥"));
    // And out again.
    action->trigger();
    QTRY_COMPARE(heart->text(), QStringLiteral("♡"));
    // Nothing playing: no heart.
    state.status = QStringLiteral("stopped");
    window.refreshFavorite(state);
    QVERIFY(heart->isHidden());
}

// ADR-0282: Workspace › Track list layout lists every list view -- the
// shipped ones and the user's own -- the one shown checked, and choosing one
// shows it.
void BenchMainWindowTest::theTrackLayoutMenuListsEveryListView() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* menu = window.findChild<QMenu*>(QStringLiteral("menu-track-list-layout"));
    QVERIFY(menu != nullptr);
    const auto listed = [&window] {
        QStringList ids;
        for (auto* action : window.track_presentation_group_->actions()) {
            ids << action->data().toString();
        }
        return ids;
    };
    QStringList shipped;
    for (const auto& view : listViews()) {
        shipped << QString::fromStdString(view.id);
    }
    QCOMPARE(listed(), shipped);
    // One of the user's own: listed as soon as it is made.
    auto base = findListView(default_list_view_id);
    QVERIFY(base.has_value());
    auto made = listViewCopy(*base, base->columns, QStringLiteral("Mine"));
    QVERIFY2(made.has_value(), made ? "" : made.error().message.c_str());
    emit ListViewCatalog::instance().changed();
    QTRY_VERIFY(listed().contains(*made));
    auto* mine = window.findChild<QAction*>(QStringLiteral("action-list-view-menu-%1").arg(*made));
    QVERIFY(mine != nullptr && menu->actions().contains(mine));
    QCOMPARE(mine->text(), QStringLiteral("Mine"));
    mine->trigger();
    QTRY_COMPARE(window.currentListTab()->view_layout.view, *made);
    QVERIFY(mine->isChecked());
    QVERIFY(saveCustomListViews({}).has_value());
    emit ListViewCatalog::instance().changed();
    QTRY_VERIFY(!listed().contains(*made));
}

} // namespace trackknife::bench
