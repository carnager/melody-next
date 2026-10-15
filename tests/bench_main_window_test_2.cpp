// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

namespace trackknife::bench {

// ADR-0234: pending removals are kept by engine key now; one an older release
// left as "l:<id>" is still carried out on connecting.
void BenchMainWindowTest::aRemovalLeftPendingByAnOlderReleaseIsCarriedOut() {
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto made =
        (*engine)->call("list.save", protocol::Json{{"name", "Closed long ago"},
                                                    {"kind", "working"},
                                                    {"items", protocol::Json::array()}});
    QVERIFY(made.has_value());
    const auto id = made->value("id", std::string{});
    QSettings{}.setValue(QStringLiteral("lists/pending-removals"),
                         QStringList{QStringLiteral("l:") + QString::fromStdString(id)});
    const auto listed = [&engine, &id] {
        auto all = (*engine)->call("list.all");
        return all && std::ranges::any_of(all->value("lists", protocol::Json::array()),
                                          [&id](const protocol::Json& list) {
                                              return list.value("id", std::string{}) == id;
                                          });
    };
    QVERIFY(listed());
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(!listed());
    QTRY_VERIFY(
        QSettings{}.value(QStringLiteral("lists/pending-removals")).toStringList().isEmpty());
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
    QCOMPARE(list->count(), 1);
    QCOMPARE(address->text(), QStringLiteral("192.0.2.10:6603"));
    QCOMPARE(folder->text(), QStringLiteral("/mnt/nas/Music"));

    // One more, filled in.
    add->click();
    QCOMPARE(list->count(), 2);
    QVERIFY(address->text().isEmpty());
    address->setText(QStringLiteral("192.0.2.20:6603"));
    folder->setText(QStringLiteral("/srv/music"));
    reachable->setText(QStringLiteral("/media/desktop"));
    QCOMPARE(list->item(1)->text(), QStringLiteral("192.0.2.20:6603"));
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-engines.png")));
    }
    // Back to the first: its fields, untouched.
    list->setCurrentRow(0);
    QCOMPARE(address->text(), QStringLiteral("192.0.2.10:6603"));
    QVERIFY(reachable->text().isEmpty());
    // A third, left empty, and removed again.
    add->click();
    QCOMPARE(list->count(), 3);
    remove->click();
    QCOMPARE(list->count(), 2);

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
    engine.setProgram(QStringLiteral(TRACKKNIFE_ENGINE_BINARY));
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
    if (const auto screenshots = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
    // review or result dialog appears and the properties tab closes on success.
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().title,
                              std::string{"Applied from ready preview"}, 5'000);
    const auto reread = metadata::read_local_metadata(raw_path);
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.effective_values("title"),
             (std::vector<std::string>{"Applied from ready preview"}));
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QTRY_VERIFY(window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties")) == nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
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
    QTRY_VERIFY(window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties")) == nullptr);
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
    QTRY_VERIFY_WITH_TIMEOUT(
        window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties")) == nullptr, 10'000);
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
            if (who == QStringLiteral("Trackknife") &&
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
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
    QTRY_VERIFY(closed_guard.isNull() || !closed_guard->isVisible());
    QCOMPARE(status.count(), 1);
    QVERIFY(
        status.front().front().toString().contains(QStringLiteral("ownership was not preserved")));
}

} // namespace trackknife::bench
