// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

namespace trackknife::bench {

void BenchMainWindowTest::aRemoteEnginePlaysItsOwnTabs() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto here = media.filePath(QStringLiteral("here.wav"));
    // Long enough that only the switch, not the end of the track, stops it.
    write_wave(here, wave_sample_rate * 60U);
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    // A minute long: "playing" has to outlast what the test does while it
    // plays, or whether the test sees it is a race.
    const auto there = music + QStringLiteral("/there.wav");
    write_wave(there, wave_sample_rate * 60U);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    auto* remote_tab = window.remoteQueueTab();
    QVERIFY(remote_tab != nullptr && !EngineKey::of(remote_tab->document).isLocal());
    auto* sources = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(sources != nullptr);
    QCOMPARE(sources->tabData(sources->count() - 1).toString(), window.remoteEngine()->key.text());
    // Someone whose music is all on the remote can hide this computer's
    // library; its tab goes, and the remote's stays in front.
    {
        const auto library_tab = 1;
        QVERIFY(sources->isTabVisible(library_tab));
        sources->setCurrentIndex(library_tab);
        QSettings{}.setValue(QLatin1String(SettingsDialog::library_show_local_key), false);
        window.applyLocalLibraryVisibility();
        QVERIFY(!sources->isTabVisible(library_tab));
        QCOMPARE(sources->tabData(sources->currentIndex()).toString(),
                 window.remoteEngine()->key.text());
        // Gone from the bar too: the remote's tab is where a click finds it.
        const auto remote_rect = sources->tabRect(sources->currentIndex());
        QVERIFY(remote_rect.isValid());
        QCOMPARE(sources->tabAt(remote_rect.center()), sources->currentIndex());
        QSettings{}.remove(QLatin1String(SettingsDialog::library_show_local_key));
        window.applyLocalLibraryVisibility();
        QVERIFY(sources->isTabVisible(library_tab));
    }
    // Named by what the engine calls itself -- here its machine's name, as
    // it was started without one -- rather than by its socket or address.
    QTRY_COMPARE(sources->tabText(sources->count() - 1), QSysInfo::machineHostName());
    // And the library says so too, not the address.
    QTRY_COMPARE(window.remoteCatalogue()->describe(),
                 QStringLiteral("Library: %1").arg(QSysInfo::machineHostName()));

    // A folder is added to the remote's library by its path there: this
    // computer's file dialog would offer this computer's folders.
    {
        QWidget host;
        auto* folders = window.remoteLibrary()->createFoldersWidget(&host);
        auto* add = folders->findChild<QPushButton*>(QStringLiteral("local-library-folder-add"));
        QVERIFY(add != nullptr);
        QTimer::singleShot(0, [music] {
            auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (prompt != nullptr) {
                prompt->setTextValue(music);
                prompt->accept();
            }
        });
        add->click();
        QTRY_VERIFY([&] {
            const auto roots = window.remoteCatalogue()->open()->roots();
            return roots && roots->size() == 1U &&
                   roots->front().raw_path == QFile::encodeName(music).toStdString();
        }());
    }
    // The remote library fills the remote tab, from the remote's index.
    {
        persistence::LibraryScanProgress progress;
        QVERIFY(window.remoteCatalogue()->open()->scan({}, progress).has_value());
    }
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && !page->entries.empty());
    window.openLocalPaths({QFile::encodeName(here).toStdString()});
    QTRY_VERIFY(window.currentListTab() != nullptr &&
                EngineKey::of(window.currentListTab()->document).isLocal() &&
                window.currentListTab()->model->rowCount() == 1);
    auto* local_tab = window.currentListTab();
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::append);
    QTRY_COMPARE(remote_tab->model->rowCount(), 1);
    QCOMPARE(local_tab->model->rowCount(), 1);
    QVERIFY(!remote_tab->model->rows().front().title.empty());

    // Local plays here; the remote tab plays there and stops this one.
    window.playRow(*local_tab, 0);
    QTRY_COMPARE(window.localPlayback()->state().status, QStringLiteral("playing"));
    window.playRow(*remote_tab, 0);
    QTRY_COMPARE(window.remotePlayback()->state().status, QStringLiteral("playing"));
    window.refreshUpNext();
    QVERIFY2(window.up_next_status_->text().startsWith(QSysInfo::machineHostName() +
                                                       QStringLiteral(" · ")),
             qPrintable(window.up_next_status_->text()));
    // Adding to the remote tab while it plays: the rows stay as they were
    // added -- tagged, with their own identities -- rather than being traded
    // for the engine's bare paths.
    {
        emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                               LocalLibraryAction::append);
        QTRY_COMPARE(remote_tab->model->rowCount(), 2);
        const auto added = remote_tab->model->rows();
        for (int wait = 0; wait < 40; ++wait) {
            QTest::qWait(50);
            const auto& now = remote_tab->model->rows();
            QCOMPARE(static_cast<int>(now.size()), 2);
            for (std::size_t index = 0; index < now.size(); ++index) {
                QCOMPARE(now[index].entry_id, added[index].entry_id);
                QVERIFY2(now[index].probed && now[index].title == added[index].title,
                         now[index].title.c_str());
            }
        }
        remote_tab->model->removeRowIndexes({1});
        window.markTabDirty(*remote_tab);
        QTRY_VERIFY(!window.remotePlayback()->settling());
        QTRY_COMPARE(window.remotePlayback()->state().queue_size, std::size_t{1});
    }
    // And while the engine is slow to answer -- a remote busy scanning, as
    // gemenon was. An edit is on its way when an older state arrives, one
    // whose queue lacks the row just added. That is no reason to take the
    // engine's queue over: doing so blocked the window on the busy engine,
    // dropped the new row, and brought it back as a bare path once the
    // engine caught up.
    {
        auto extra = remote_tab->model->rows().front();
        extra.entry_id = core::StableId::random();
        const auto pid = static_cast<pid_t>(remote.processId());
        QVERIFY(pid > 0);
        QVERIFY(::kill(pid, SIGSTOP) == 0);
        const auto resume = qScopeGuard([pid] { ::kill(pid, SIGCONT); });
        remote_tab->model->appendRows({extra});
        window.markTabDirty(*remote_tab);
        QVERIFY(window.remotePlayback()->settling());
        // A state from before the edit, as far as the window can tell.
        window.engine_queue_revision_ = 0;
        QElapsedTimer waited;
        waited.start();
        emit window.remotePlayback()->changed();
        QVERIFY2(waited.elapsed() < 1'000, "the window does not wait on a busy engine");
        QCOMPARE(remote_tab->model->rowCount(), 2);
        QCOMPARE(remote_tab->model->rows().back().entry_id, extra.entry_id);
        ::kill(pid, SIGCONT);
        QTRY_VERIFY(!window.remotePlayback()->settling());
        QTest::qWait(300);
        QCOMPARE(remote_tab->model->rowCount(), 2);
        QCOMPARE(remote_tab->model->rows().back().entry_id, extra.entry_id);
        QVERIFY(remote_tab->model->rows().back().probed);
        remote_tab->model->removeRowIndexes({1});
        window.markTabDirty(*remote_tab);
    }
    QTRY_COMPARE(window.localPlayback()->state().status, QStringLiteral("stopped"));
    QVERIFY(window.transport_ == window.remotePlayback());
    // And back.
    window.playRow(*local_tab, 0);
    QTRY_COMPARE(window.localPlayback()->state().status, QStringLiteral("playing"));
    QTRY_COMPARE(window.remotePlayback()->state().status, QStringLiteral("stopped"));

    // Up Next is named by the engine it belongs to.
    {
        auto* status = window.up_next_status_;
        QVERIFY(status != nullptr);
        window.refreshUpNext();
        QVERIFY2(status->text().startsWith(QStringLiteral("This computer · ")),
                 qPrintable(status->text()));
    }
    // Up Next holds one engine's asks.
    window.enqueueLocalRequests({remote_tab->model->rows().front()}, -1,
                                window.remoteEngine()->key);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    window.enqueueLocalRequests({local_tab->model->rows().front()}, -1, EngineKey::local());
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    window.playback_.requests.clear();

    // The binding survives a restart.
    window.persistNow(false);
    QVERIFY(window.close());
    BenchMainWindow restored;
    QTRY_VERIFY(restored.lists_restored_);
    const auto remote_tabs = std::ranges::count_if(restored.list_tabs_, [](const auto& tab) {
        return !EngineKey::of(tab->document).isLocal();
    });
    QCOMPARE(remote_tabs, 1);
}

// ADR-0228: an output agent registered with the engine is offered in the
// speaker menu, and choosing it plays there. A real melody-agent process, on
// the engine's socket.
void BenchMainWindowTest::theDeviceMenuChoosesAnOutputAgent() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-device-menu"));
    QVERIFY(menu != nullptr);
    const auto output_action = [menu](const QString& id) {
        return menu->findChild<QAction*>(QStringLiteral("action-output-%1").arg(id));
    };
    // Nothing to choose between yet, so no choice is offered.
    QVERIFY(output_action(QStringLiteral("agent:bedside")) == nullptr);

    QProcess agent;
    agent.setProgram(QStringLiteral(TRACKKNIFE_AGENT_BINARY));
    agent.setArguments({QStringLiteral("--server"), engine_.socket(), QStringLiteral("--name"),
                        QStringLiteral("bedside")});
    agent.setProcessChannelMode(QProcess::MergedChannels);
    agent.start();
    QVERIFY(agent.waitForStarted());
    const auto stop_agent = qScopeGuard([&agent] {
        agent.terminate();
        if (!agent.waitForFinished(5'000)) {
            agent.kill();
            agent.waitForFinished();
        }
    });
    QTRY_VERIFY_WITH_TIMEOUT(output_action(QStringLiteral("agent:bedside")) != nullptr ||
                                 agent.state() != QProcess::Running,
                             10'000);
    if (output_action(QStringLiteral("agent:bedside")) == nullptr) {
        QSKIP("melody-agent has no audio output here");
    }
    auto* bedside = output_action(QStringLiteral("agent:bedside"));
    QVERIFY(bedside->isEnabled() && !bedside->isChecked());
    QVERIFY(!bedside->text().contains(QStringLiteral("offline")));

    bedside->trigger();
    QTRY_COMPARE(window.property("trackknife-player-output").toString(),
                 QStringLiteral("agent:bedside"));
    QTRY_VERIFY(output_action(QStringLiteral("agent:bedside")) != nullptr &&
                output_action(QStringLiteral("agent:bedside"))->isChecked());
    auto* button = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    QVERIFY(button != nullptr);
    // Music in another room is named where it can be seen.
    QCOMPARE(button->text(), QStringLiteral("bedside"));
    // Named, the pill ends in a chevron: it opens a choice.
    auto* chevron = button->findChild<QLabel*>(QStringLiteral("bench-device-chevron"));
    QVERIFY(chevron != nullptr && chevron->isVisible());
    // And the menu says which list is which: speakers, then the chosen
    // speaker's devices -- in any style, not only those that draw sections.
    {
        QStringList headings;
        for (const auto* heading :
             menu->findChildren<QLabel*>(QStringLiteral("bench-device-menu-heading"))) {
            headings << heading->text();
        }
        QVERIFY2(headings.contains(QStringLiteral("Speakers")), qPrintable(headings.join(u'|')));
        QVERIFY2(headings.contains(QStringLiteral("Sound device on bedside")),
                 qPrintable(headings.join(u'|')));
    }

    // Gone, it stays chosen and says so.
    agent.terminate();
    QVERIFY(agent.waitForFinished(5'000));
    QTRY_VERIFY_WITH_TIMEOUT(output_action(QStringLiteral("agent:bedside")) != nullptr &&
                                 output_action(QStringLiteral("agent:bedside"))
                                     ->text()
                                     .contains(QStringLiteral("offline")),
                             10'000);
    QVERIFY(output_action(QStringLiteral("agent:bedside"))->isChecked());

    // And this computer again, if the engine has audio of its own here.
    if (auto* local = output_action(QStringLiteral("local")); local != nullptr) {
        // Read before choosing it: choosing rebuilds the menu.
        const auto own_name = local->text();
        local->trigger();
        QTRY_COMPARE(window.property("trackknife-player-output").toString(),
                     QStringLiteral("local"));
        // With an agent to choose, where it plays stays named -- by the
        // engine's name for its machine, as other engines list it too.
        QVERIFY(!own_name.isEmpty() && own_name != QStringLiteral("This computer"));
        QCOMPARE(button->text(), own_name);
    }
}

// ADR-0227: a remote tab's files are on the remote's machine and need not be
// reachable from this one, so its tags and covers come from that engine --
// however its rows got there.
void BenchMainWindowTest::narrowWindowKeepsListAndUpNextCompact() {
    BenchMainWindow window;
    window.resize(1'600, 1'000);
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    std::vector<LocalTrackRow> rows;
    for (int index = 0; index < 30; ++index) {
        LocalTrackRow row;
        row.raw_path = "/tmp/narrow-" + std::to_string(index) + ".flac";
        row.title = "Track " + std::to_string(index);
        row.artist = "Artist";
        row.album = "Album";
        rows.push_back(std::move(row));
    }
    tab->model->appendRows(rows);
    window.enqueueLocalRequests({rows[3], rows[7]});
    // Narrowed while Up Next slides open: the columns are too wide for a
    // moment, then fit again.
    window.findChild<QAction*>(QStringLiteral("action-show-up-next"))->trigger();
    window.resize(1'100, 720);
    QTRY_VERIFY(window.up_next_dock_->isVisible());
    QTest::qWait(600);

    // Up Next: a stack of two-line rows, no table headers, and narrow enough
    // to leave the list its room.
    QVERIFY(dynamic_cast<UpNextDelegate*>(window.up_next_view_->itemDelegate()) != nullptr);
    QVERIFY(window.up_next_view_->horizontalHeader()->isHidden());
    QVERIFY(window.up_next_view_->verticalHeader()->isHidden());
    QCOMPARE(window.up_next_view_->rowHeight(0), UpNextDelegate::row_height);
    QTRY_VERIFY(window.up_next_dock_->width() <= 320);

    // The list's columns fit, so no scroll bar is left standing under it.
    QTRY_COMPARE(tab->view->horizontalHeader()->length(), tab->view->viewport()->width());
    QTRY_VERIFY(!tab->view->horizontalScrollBar()->isVisible());
}

// ADR-0140: Enter opens the kept search's tab at once, saying the tracks are
// being collected, and fills that tab when they come; nothing found, it says
// so there.
void BenchMainWindowTest::aKeptSearchOpensAtOnce() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* browser = window.findChild<LibraryBrowser*>();
    QVERIFY(browser != nullptr);
    const auto tabs_before = window.tabs_->count();

    emit browser->searchStarted(QStringLiteral("ALL"));
    QCOMPARE(window.tabs_->count(), tabs_before + 1);
    auto* view = qobject_cast<QTableView*>(window.tabs_->currentWidget());
    QVERIFY(view != nullptr);
    auto* tab = window.tabForDocument(view->property("bench-document-id").toString());
    QVERIFY(tab != nullptr && tab->model->rowCount() == 0);
    QCOMPARE(window.tabs_->tabText(window.tabs_->currentIndex()), QStringLiteral("Search: ALL"));
    QVERIFY(static_cast<ui::QueueTableView*>(view)->emptyTitle().contains(QStringLiteral("ALL")));

    LocalTrackRow row;
    row.raw_path = "/music/found.flac";
    row.title = "Found";
    emit browser->searchCommitted(QStringLiteral("ALL"), {row});
    QCOMPARE(window.tabs_->count(), tabs_before + 1);
    QCOMPARE(tab->model->rowCount(), 1);
    QCOMPARE(static_cast<ui::QueueTableView*>(view)->emptyTitle(),
             window.emptyListTitle(EngineKey::of(tab->document)));

    emit browser->searchStarted(QStringLiteral("nothing like it"));
    auto* empty = window.tabForDocument(qobject_cast<QTableView*>(window.tabs_->currentWidget())
                                            ->property("bench-document-id")
                                            .toString());
    QVERIFY(empty != nullptr && empty != tab);
    emit browser->searchFailed(QStringLiteral("nothing like it"),
                               QStringLiteral("Nothing matches"));
    QCOMPARE(static_cast<ui::QueueTableView*>(empty->view)->emptyTitle(),
             QStringLiteral("Nothing matches"));
    QCOMPARE(empty->model->rowCount(), 0);
}

void BenchMainWindowTest::emptyListsSayHowToFillThem() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    auto* view = static_cast<ui::QueueTableView*>(tab->view);
    QCOMPARE(view->emptyTitle(), QStringLiteral("This list is empty"));
    QCOMPARE(window.up_next_view_->emptyTitle(), QStringLiteral("Nothing waiting"));

    // Said in the middle of the list while it is empty, and gone once filled.
    const auto painted = [view] {
        const auto image = view->viewport()->grab().toImage();
        const auto ground = view->palette().color(QPalette::Base).lightness();
        const auto ink = view->palette().color(QPalette::Text).lightness();
        // Text shows as pixels well on the text's side of the background.
        const auto threshold = (ground + ink) / 2;
        for (int y = image.height() / 4; y < image.height() * 3 / 4; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const auto lightness = image.pixelColor(x, y).lightness();
                if (ink > ground ? lightness > threshold : lightness < threshold) {
                    return true;
                }
            }
        }
        return false;
    };
    QVERIFY(painted());
    LocalTrackRow row;
    row.raw_path = "/tmp/filled.flac";
    row.title = "Filled";
    tab->model->appendRows({row});
    QCoreApplication::processEvents();
    QVERIFY(!painted());
}

void BenchMainWindowTest::sourcePanelOpensOnALibrary() {
    QSettings{}.remove(QStringLiteral("local-library/view"));
    {
        // Nothing chosen yet: the library, not the folders.
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QCOMPARE(window.local_source_tabs_->currentIndex(), 1);
        // Clicking Folders is a choice, and is kept.
        emit window.local_source_tabs_->tabBarClicked(0);
        window.local_source_tabs_->setCurrentIndex(0);
    }
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QCOMPARE(window.local_source_tabs_->currentIndex(), 0);
        emit window.local_source_tabs_->tabBarClicked(1);
    }

    // This computer's library hidden: the remote's opens in its place.
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_show_local_key), false);
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_COMPARE(
        window.local_source_tabs_->tabData(window.local_source_tabs_->currentIndex()).toString(),
        window.remoteEngine()->key.text());
    QSettings{}.remove(QLatin1String(SettingsDialog::library_show_local_key));
}

void BenchMainWindowTest::aRemoteTabRatesOnItsEngine() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::append);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QTRY_VERIFY(!tab->model->rows().front().album_rating_hash.empty());
    const auto track_hash = tab->model->rows().front().rating_hash;
    const auto album_hash = tab->model->rows().front().album_rating_hash;

    window.tabs_->setCurrentWidget(tab->view);
    tab->view->selectionModel()->select(
        tab->model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(
        tab->view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, tab->view->visualRect(tab->model->index(0, local_title_column)).center())));
    auto* track_stars = window.findChild<QAction*>(QStringLiteral("action-local-rate-6"));
    auto* album_stars = window.findChild<QAction*>(QStringLiteral("action-local-album-rate-8"));
    QVERIFY(track_stars != nullptr && album_stars != nullptr);
    track_stars->trigger();
    album_stars->trigger();
    window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"))->close();

    // Stored where the track lives, so the remote's own queries see it...
    const auto remote_ratings = [&window, &track_hash, &album_hash] {
        auto found = window.remoteCatalogue()->open()->ratings({track_hash, album_hash});
        return found ? *found : std::vector<unsigned>{};
    };
    QTRY_COMPARE(remote_ratings(), (std::vector<unsigned>{6U, 8U}));
    // ...and read back from there onto the tab's cover.
    QTRY_COMPARE(tab->model->rows().front().album_rating, 8U);
    // Not on this computer's engine, which does not have the track.
    const auto here = window.localCatalogue()->open()->ratings({track_hash, album_hash});
    QVERIFY(here.has_value());
    QCOMPARE(*here, (std::vector<unsigned>{0U, 0U}));
}

void BenchMainWindowTest::aRatingSetElsewhereShowsInTheTabs() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::append);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QTRY_VERIFY(!tab->model->rows().front().rating_hash.empty());
    const auto hash = tab->model->rows().front().rating_hash;
    QCOMPARE(tab->model->rows().front().rating, 0U);

    // Another client -- the phone, a script -- rates it on the engine.
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    QVERIFY((*other)
                ->call("catalogue.set_rating", protocol::Json{{"hash", hash}, {"rating", 8}})
                .has_value());
    (*other)->close();

    // Shown without anyone asking again.
    QTRY_COMPARE(tab->model->rows().front().rating, 8U);
}

void BenchMainWindowTest::dynamicPlaylistsReadTheLibraryChosen() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* remote_tab = window.remoteQueueTab();
    QVERIFY(remote_tab != nullptr);

    // Opened from a remote tab, it starts on the remote's library.
    window.tabs_->setCurrentWidget(remote_tab->view);
    window.showDynamicPlaylists();
    auto* dialog = window.findChild<DynamicPlaylistDialog*>();
    QVERIFY(dialog != nullptr);
    auto* library = dialog->findChild<QComboBox*>(QStringLiteral("dynamic-library"));
    QVERIFY(library != nullptr);
    QCOMPARE(library->count(), 2);
    QVERIFY(!dialog->engine().isLocal());
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-query"))->setText(QStringLiteral("ALL"));
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-refresh"))->click();
    const auto remote_path = QFile::encodeName(music + QStringLiteral("/art.flac")).toStdString();
    QTRY_COMPARE(dialog->tracks().size(), 1U);
    QCOMPARE(dialog->tracks().front().raw_path, remote_path);

    // What it finds opens on the remote's engine.
    const auto before = window.list_tabs_.size();
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-open"))->click();
    QTRY_COMPARE(window.list_tabs_.size(), before + 1);
    QVERIFY(!EngineKey::of(window.list_tabs_.back()->document).isLocal());
    QCOMPARE(window.list_tabs_.back()->model->rows().front().raw_path, remote_path);

    // This computer's library, which has no such track.
    library->setCurrentIndex(library->findData(false));
    QVERIFY(dialog->engine().isLocal());
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-refresh"))->click();
    QTRY_VERIFY(dialog->findChild<QPushButton*>(QStringLiteral("dynamic-refresh"))->isEnabled());
    QVERIFY(std::ranges::none_of(
        dialog->tracks(), [&remote_path](const auto& row) { return row.raw_path == remote_path; }));
    dialog->close();
}

void BenchMainWindowTest::remoteUpNextKeepsItsIdentityAcrossARestart() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    write_wave(music + QStringLiteral("/first.wav"), wave_sample_rate * 60U);
    write_wave(music + QStringLiteral("/asked.wav"), wave_sample_rate * 60U);
    const auto engine_requests = [&remote] {
        auto client = protocol::Client::connect(protocol::Endpoint{
            .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
        std::vector<std::string> ids;
        if (!client) {
            return ids;
        }
        if (auto answer = (*client)->call("playback.requests")) {
            for (const auto& id : answer->value("entries", protocol::Json::array())) {
                ids.push_back(id.get<std::string>());
            }
        }
        (*client)->close();
        return ids;
    };

    std::string asked_id;
    std::string asked_title;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
        QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
        {
            auto catalogue = window.remoteCatalogue()->open();
            QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
            persistence::LibraryScanProgress progress;
            QVERIFY(catalogue->scan({}, progress).has_value());
        }
        auto* tab = window.remoteQueueTab();
        QVERIFY(tab != nullptr);
        persistence::LibraryQuery tracks;
        tracks.kind = persistence::LibraryEntryKind::track;
        const auto page = window.remoteCatalogue()->open()->query(tracks);
        QVERIFY(page && page->entries.size() == 2U);
        emit window.remoteLibrary()->browser().actionRequested({page->entries[0]},
                                                               LocalLibraryAction::replace);
        QTRY_COMPARE(window.remotePlayback()->state().status, QStringLiteral("playing"));
        window.remotePlayback()->setVolume(0);
        std::vector<LocalTrackRow> asked;
        window.remoteLibrary()->resolveEntryRows(
            {page->entries[1]},
            [&asked](std::vector<LocalTrackRow> rows) { asked = std::move(rows); });
        QTRY_COMPARE(asked.size(), std::size_t{1});
        asked_title = asked.front().title;
        window.enqueueLocalRequests(asked, -1, window.remoteEngine()->key);
        // An ask is an occurrence of its own, with an identity of its own.
        QCOMPARE(window.playback_.requests.pending().size(), std::size_t{1});
        asked_id = window.playback_.requests.pending().front().source.entry_id.to_string();
        // The engine holds it by the window's identity.
        QTRY_VERIFY(engine_requests() == std::vector<std::string>{asked_id});
        window.persistUpNext();
        QTest::qWait(300);
    }

    // Trackknife restarted: the same Up Next, on the same engine, by the same
    // identity -- so the engine and the window still agree what it is.
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    QCOMPARE(window.playback_.requests.pending().size(), std::size_t{1});
    QCOMPARE(window.playback_.requests.pending().front().source.entry_id.to_string(), asked_id);
    QVERIFY(window.up_next_engine_ == window.remoteEngine()->key);
    QVERIFY(engine_requests() == std::vector<std::string>{asked_id});
    // Played, it is named in the header by its title, and leaves the waiting list.
    QTRY_VERIFY(window.transport_ == window.remotePlayback());
    window.remotePlayback()->next();
    QTRY_VERIFY(window.playback_.requests.pending().empty());
    QTRY_COMPARE(window.now_playing_->text(), QString::fromStdString(asked_title));
    // The title bar -- what a taskbar shows -- names it too, and lets go of
    // it when nothing plays.
    QVERIFY2(window.windowTitle().contains(QString::fromStdString(asked_title)) &&
                 window.windowTitle().endsWith(QStringLiteral("Trackknife")),
             qPrintable(window.windowTitle()));
    window.remotePlayback()->stop();
    QTRY_COMPARE(window.windowTitle(), QStringLiteral("Trackknife"));
}

void BenchMainWindowTest::locateFindsARemoteTracksAlbumInTheRemoteLibrary() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    // All music on the remote: this computer's library hidden, as it is
    // for someone who keeps none here.
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_show_local_key), false);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    const auto album_name = QString::fromStdString(page->entries.front().album);
    const auto artist_name = QString::fromStdString(page->entries.front().artist);
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::replace);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    window.remoteLibrary()->refreshLibrary();

    auto* tree = window.remoteLibrary()->findChild<QTreeView*>();
    for (const bool album : {true, false}) {
        const auto position =
            tab->view->visualRect(tab->model->index(0, local_title_column)).center();
        QVERIFY(QMetaObject::invokeMethod(tab->view, "customContextMenuRequested",
                                          Qt::DirectConnection, Q_ARG(QPoint, position)));
        auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
        auto* locate =
            menu->findChild<QAction*>(album ? QStringLiteral("action-local-locate-album")
                                            : QStringLiteral("action-local-locate-artist"));
        QVERIFY(locate != nullptr);
        QVERIFY2(locate->isEnabled(), "a remote tab's track is located in the remote's library");
        locate->trigger();
        menu->close();
        // The remote's library comes forward, on the album or its artist.
        QTRY_COMPARE(window.local_source_tabs_->tabData(window.local_source_tabs_->currentIndex())
                         .toString(),
                     window.remoteEngine()->key.text());
        QTRY_VERIFY(
            tree->currentIndex().data().toString().contains(album ? album_name : artist_name));
    }
    window.remotePlayback()->stop();
    QSettings{}.remove(QLatin1String(SettingsDialog::library_show_local_key));
}

void BenchMainWindowTest::replacingARemoteTabFromItsLibraryPlays() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    // Long enough to still be playing when looked at.
    write_wave(music + QStringLiteral("/long.wav"), wave_sample_rate * 60U);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    // "Replace list and play" -- from the menu, or Shift+Enter in the quick
    // album popup -- plays, on the remote as it does here.
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::replace);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(window.remotePlayback()->state().status, QStringLiteral("playing"),
                              10'000);
    window.remotePlayback()->stop();
}

void BenchMainWindowTest::aRemoteTabGetsTagsAndCoversFromItsEngine() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    const auto art = music + QStringLiteral("/art.flac");
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), art));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    {
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.remoteQueueTab();
    QVERIFY(tab != nullptr);
    const auto covered = [tab](const int row) {
        return row < tab->model->rowCount() && tab->model->hasArtwork(tab->model->groupKey(row));
    };

    // With the buttons: tagged from the start, and a cover from the engine.
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remoteCatalogue()->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                           LocalLibraryAction::append);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QCOMPARE(tab->model->rows().front().title, std::string{"Fixture Tone"});
    QTRY_VERIFY(covered(0));

    // Dropped: a bare path, filled in from the engine's index -- not looked
    // for on this computer, where without a mount it would not be.
    window.insertRemotePaths(*tab, {QFile::encodeName(art).toStdString()}, -1);
    QCOMPARE(tab->model->rowCount(), 2);
    QTRY_COMPARE(tab->model->rows()[1].title, std::string{"Fixture Tone"});
    QCOMPARE(tab->model->rows()[1].artist, std::string{"Trackknife Project"});
    QTRY_VERIFY(covered(1));

    // A path the engine does not know keeps its file name, and is asked about
    // once rather than on every look at the tab.
    window.insertRemotePaths(*tab, {"/nowhere/on/either/machine.flac"}, -1);
    QTRY_VERIFY(tab->model->rows()[2].probed);
    QCOMPARE(tab->model->rows()[2].title, std::string{"machine.flac"});

    // Rows moving between the remote's tab and this computer's are translated
    // to where this computer sees the file -- here the same path, as with the
    // NAS mounted at the same place on both -- and what is not reachable here
    // is left out, with a word why.
    auto* local =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Here",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {},
                                                    .engine = {}},
                          true);
    const auto local_id = QString::fromStdString(local->document.id.to_string());
    QVERIFY(window.transferRows(tab->view, {0, 2}, local_id, false, -1));
    QCOMPARE(local->model->rowCount(), 1);
    QCOMPARE(local->model->rows().front().raw_path, QFile::encodeName(art).toStdString());
    QCOMPARE(local->model->rows().front().title, std::string{"Fixture Tone"});
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("1 of 2")));

    // Mounted elsewhere here: the path is translated, not kept.
    const auto mounted = media.filePath(QStringLiteral("mounted-here"));
    QVERIFY(QFile::link(music, mounted));
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_remote_folder_key), music);
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_remote_mount_key), mounted);
    QVERIFY(window.transferRows(tab->view, {0}, local_id, false, -1));
    QCOMPARE(local->model->rows().back().raw_path,
             QFile::encodeName(mounted + QStringLiteral("/art.flac")).toStdString());

    // And back: a file of this computer's goes to the remote only when the
    // remote's library has it.
    const auto remote_id = QString::fromStdString(tab->document.id.to_string());
    const auto remote_rows = tab->model->rowCount();
    QVERIFY(window.transferRows(local->view, {1}, remote_id, false, -1));
    QCOMPARE(tab->model->rowCount(), remote_rows + 1);
    QCOMPARE(tab->model->rows().back().raw_path, QFile::encodeName(art).toStdString());
    const auto download = media.filePath(QStringLiteral("download.wav"));
    write_wave(download, wave_sample_rate);
    auto stray = local->model->rows().front();
    stray.raw_path = QFile::encodeName(download).toStdString();
    stray.entry_id = core::StableId::random();
    local->model->appendRows({stray});
    QVERIFY(
        !window.transferRows(local->view, {local->model->rowCount() - 1}, remote_id, false, -1));
    QCOMPARE(tab->model->rowCount(), remote_rows + 1);
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("not in")));

    QSettings{}.remove(QLatin1String(SettingsDialog::library_remote_folder_key));
    QSettings{}.remove(QLatin1String(SettingsDialog::library_remote_mount_key));

    // The search dialog searches the remote's library too, as you type, with
    // artists, albums and tracks apart -- and what it finds goes to a remote
    // tab.
    // Opened from a remote tab, it searches the remote's library, ready to
    // type into.
    window.tabs_->setCurrentWidget(tab->view);
    window.openSearchDialog();
    auto* dialog = window.search_dialog_.data();
    QVERIFY(dialog != nullptr);
    auto* scope = dialog->findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* query_mode = dialog->findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* results = dialog->findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-search-status"));
    QVERIFY(scope && query_mode && input && results && status);
    QCOMPARE(scope->currentData().toString(), window.remoteEngine()->key.text());
    QTRY_COMPARE(dialog->focusWidget(), static_cast<QWidget*>(input));
    // From a local tab, this computer's; and back again, the remote's.
    window.tabs_->setCurrentWidget(local->view);
    window.openSearchDialog();
    QCOMPARE(scope->currentData().toString(), QStringLiteral("local"));
    window.tabs_->setCurrentWidget(tab->view);
    window.openSearchDialog();
    QCOMPARE(scope->currentData().toString(), window.remoteEngine()->key.text());
    query_mode->setChecked(false);
    input->setText(QStringLiteral("fixture"));
    QTRY_VERIFY(status->text().contains(QStringLiteral("1 album")));
    QVERIFY(status->text().contains(QStringLiteral("1 track")));
    QListWidgetItem* album = nullptr;
    QStringList headings;
    for (int row = 0; row < results->count(); ++row) {
        auto* item = results->item(row);
        if (item->flags() == Qt::NoItemFlags) {
            headings << item->text();
        } else if (album == nullptr && item->text().contains(QStringLiteral("Trackbench"))) {
            album = item;
        }
    }
    // The artist too: an artist matches by what it has, as in the library.
    QCOMPARE(headings, (QStringList{QStringLiteral("Artists (1)"), QStringLiteral("Albums (1)"),
                                    QStringLiteral("Tracks (1)")}));
    QVERIFY(album != nullptr);
    const auto before = tab->model->rowCount();
    results->clearSelection();
    album->setSelected(true);
    emit results->itemActivated(album);
    QTRY_COMPARE(tab->model->rowCount(), before + 1);
    QCOMPARE(tab->model->rows().back().title, std::string{"Fixture Tone"});
    dialog->close();

    // Files opened from this computer go into a local list, never the remote
    // tab that happens to be on screen.
    {
        window.tabs_->setCurrentWidget(tab->view);
        const auto rows_before = tab->model->rowCount();
        const auto opened = media.filePath(QStringLiteral("opened-here.wav"));
        write_wave(opened, wave_sample_rate);
        window.openLocalPaths({QFile::encodeName(opened).toStdString()});
        QTRY_VERIFY(
            window.currentListTab() != nullptr &&
            EngineKey::of(window.currentListTab()->document).isLocal() &&
            std::ranges::any_of(window.currentListTab()->model->rows(), [&opened](const auto& row) {
                return row.raw_path == QFile::encodeName(opened).toStdString();
            }));
        QCOMPARE(tab->model->rowCount(), rows_before);
    }
}

// Lists are restored before the remote engine is connected, so a restored
// remote tab once asked for its covers when there was no one to ask, and
// never again: they stayed missing until another album was added.
void BenchMainWindowTest::aRestoredRemoteTabGetsItsCovers() {
    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto music = media.filePath(QStringLiteral("remote-music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    const auto covered = [](BenchMainWindow::ListTab* tab) {
        return tab != nullptr && tab->model->rowCount() > 0 &&
               tab->model->hasArtwork(tab->model->groupKey(0));
    };
    const auto remote_tab = [](BenchMainWindow& window) -> BenchMainWindow::ListTab* {
        for (const auto& tab : window.list_tabs_) {
            if (!EngineKey::of(tab->document).isLocal() && tab->model->rowCount() > 0) {
                return tab.get();
            }
        }
        return nullptr;
    };
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
        auto catalogue = window.remoteCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
        persistence::LibraryQuery albums;
        albums.kind = persistence::LibraryEntryKind::album;
        const auto page = catalogue->query(albums);
        QVERIFY(page && page->entries.size() == 1U);
        emit window.remoteLibrary()->browser().actionRequested(page->entries,
                                                               LocalLibraryAction::append);
        QTRY_VERIFY(covered(remote_tab(window)));
        window.persistNow(false);
        QVERIFY(window.close());
    }
    BenchMainWindow restored;
    restored.show();
    QTRY_VERIFY(restored.lists_restored_);
    QTRY_VERIFY(remote_tab(restored) != nullptr);
    QTRY_VERIFY(covered(remote_tab(restored)));
}

// ADR-0220: the account signed in here is handed to an engine, which then
// scrobbles what it plays itself -- and this window stops crediting it, or
// every listen would count twice.
void BenchMainWindowTest::lastFmIsHandedToTheEngine() {
    {
        const auto path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                          QStringLiteral("/lastfm-v1.json");
        QDir{}.mkpath(QFileInfo{path}.absolutePath());
        QFile file{path};
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument{QJsonObject{{"version", 1},
                                             {"key", QString(32, QLatin1Char('k'))},
                                             {"secret", QString(32, QLatin1Char('s'))},
                                             {"session", "session-key"},
                                             {"user", "listener"},
                                             {"enabled", true}}}
                       .toJson());
    }
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    QVERIFY(!window.localPlayback()->scrobblesItself());

    QWidget host;
    auto* page = window.buildLastFmSettings(&host);
    auto* use = page->findChild<QPushButton*>(QStringLiteral("lastfm-engine-local-use"));
    auto* state = page->findChild<QLabel*>(QStringLiteral("lastfm-engine-local-state"));
    QVERIFY(use != nullptr && state != nullptr);
    QTRY_COMPARE(state->text(), QStringLiteral("Not scrobbling"));
    use->click();
    QTRY_COMPARE(state->text(), QStringLiteral("Scrobbling as listener"));
    QTRY_VERIFY(window.localPlayback()->scrobblesItself());
    // The engine has this account now: handing it over again would do nothing.
    QTRY_COMPARE(use->text(), QStringLiteral("In use"));
    QVERIFY(!use->isEnabled());
    // And a fresh look at the page says the same.
    {
        QWidget again_host;
        auto* again = window.buildLastFmSettings(&again_host);
        auto* again_use = again->findChild<QPushButton*>(QStringLiteral("lastfm-engine-local-use"));
        QTRY_COMPARE(again_use->text(), QStringLiteral("In use"));
    }

    // The window no longer credits what that engine plays.
    EnginePlayback::State playing;
    playing.status = QStringLiteral("playing");
    playing.position_ms = 1'000;
    playing.duration_ms = 200'000;
    window.lastfm_sample_time_ = -1'000'000;
    window.sampleLastFmFromEngine(playing);
    QCOMPARE(window.findChild<trackknife::bench::Workspace*>()
                 ->property("trackknife-lastfm-sample")
                 .toString(),
             QStringLiteral("engine"));
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                  QStringLiteral("/lastfm-v1.json"));
}

void BenchMainWindowTest::upNextPreservesNormalPlayback_data() {
    QTest::addColumn<bool>("consume");
    QTest::newRow("normal") << false;
    QTest::newRow("consume") << true;
}
void BenchMainWindowTest::upNextPreservesNormalPlayback() {
    QFETCH(bool, consume);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto a = media.filePath(QStringLiteral("a.wav"));
    const auto b = media.filePath(QStringLiteral("b.wav"));
    const auto x = media.filePath(QStringLiteral("x.wav"));
    write_wave(a, wave_sample_rate * 2U);
    write_wave(b, wave_sample_rate * 2U);
    write_wave(x, wave_sample_rate);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.up_next_restored_);
    window.openLocalPaths({QFile::encodeName(a).toStdString(), QFile::encodeName(b).toStdString()});
    QTRY_VERIFY(window.currentListTab() != nullptr &&
                window.currentListTab()->model->rowCount() == 2);
    QTRY_VERIFY(window.playingOnEngine());
    window.playback_.modes.consume = consume ? audio::ModeState::on : audio::ModeState::off;
    window.applyLocalPlaybackModes();
    auto* tab = window.currentListTab();
    window.playRow(*tab, 0);
    QTRY_COMPARE(window.property("trackknife-engine-playback").toString(),
                 QStringLiteral("playing"));
    LocalTrackRow request;
    request.raw_path = QFile::encodeName(b).toStdString();
    request.title = "X";
    window.enqueueLocalRequests({request, request});
    const auto first = window.playback_.requests.pending()[0].id;
    const auto second = window.playback_.requests.pending()[1].id;
    QTRY_VERIFY_WITH_TIMEOUT(window.playback_.requests.active() &&
                                 window.playback_.requests.active()->id == first,
                             5000);
    // The header names what Up Next is playing by its tags, though the entry
    // is in no open list -- not by its file name.
    auto* now_playing = window.findChild<QLabel*>(QStringLiteral("bench-now-playing"));
    QVERIFY(now_playing != nullptr);
    QTRY_COMPARE(now_playing->text(), QStringLiteral("X"));
    // And it is credited as what it is.
    QTRY_VERIFY(window.findChild<trackknife::bench::Workspace*>()
                    ->property("trackknife-lastfm-sample")
                    .toString()
                    .contains(QStringLiteral("|X|")));
    QCOMPARE(tab->model->rowCount(), consume ? 1 : 2);
    QTRY_VERIFY_WITH_TIMEOUT(window.playback_.requests.active() &&
                                 window.playback_.requests.active()->id == second,
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.playback_.requests.active() &&
                                 row_of(window.tabForDocument(window.playback_.anchors.document),
                                        window.playback_.anchors.current) == (consume ? 0 : 1),
                             4000);
    QCOMPARE(tab->model->rowCount(), consume ? 1 : 2);
    QVERIFY(window.playback_.requests.pending().empty());
}

void BenchMainWindowTest::dynamicPlaylistsShareRulesAndRecommendationMatching() {
    using Service = DynamicPlaylistService;
    const auto parsed = parseLastFmTracks(R"({"similartracks":{"track":[
        {"name":"Song \"quoted\"","artist":{"name":"A & B"}},
        {"name":"Missing","artist":{"name":"A & B"}},
        {"name":"Song \"quoted\"","artist":{"name":"A & B"}}
    ]}})",
                                          100);
    QVERIFY(parsed);
    QCOMPARE(parsed->size(), 3);
    QVERIFY(!parseLastFmTracks(R"({"error":10,"message":"Invalid API key"})", 100));
    QVERIFY(!parseLastFmTracks("not JSON", 100));
    {
        std::vector<Service::Completion> pending;
        std::vector<query::CompiledTkq> queries;
        Service service([&](engine::DynamicSelection selection, core::CancellationToken,
                            Service::Completion completion) {
            auto compiled = query::compile_tkq(selection.query);
            QVERIFY(compiled);
            queries.push_back(std::move(*compiled));
            pending.push_back(std::move(completion));
        });
        int completions = 0;
        Service::Tracks output;
        QString failure;
        int unmatched = 0;
        connect(&service, &Service::finished, &service,
                [&](const Service::Tracks& rows, int missing, const QString& error) {
                    ++completions;
                    output = rows;
                    unmatched = missing;
                    failure = error;
                });
        const auto rows = []() -> Service::Tracks {
            LocalTrackRow row;
            row.raw_path = std::string("/raw-\xff.flac");
            row.artist = "A & B";
            row.title = "Song \"quoted\"";
            return std::vector<LocalTrackRow>{row};
        };
        DynamicPlaylistDefinition definition{.id = QStringLiteral("rules"),
                                             .name = QStringLiteral("Favorites"),
                                             .profile = QStringLiteral("local")};
        service.refresh(definition, {});
        QCOMPARE(pending.size(), 1U);
        QCOMPARE(queries.front().source, std::string("rating GREATER 6"));
        auto complete = std::move(pending.back());
        complete(rows());
        QCOMPARE(completions, 1);
        QVERIFY(failure.isEmpty());
        QCOMPARE(output.front().raw_path, std::string("/raw-\xff.flac"));
        service.refresh(definition, {});
        auto cancelled = std::move(pending.back());
        service.cancel();
        cancelled(rows());
        QCOMPARE(completions, 1);
        definition.source = QStringLiteral("similar");
        service.matchRecommendations(definition, *parsed);
        QCOMPARE(queries.back().predicates.front().text, std::string("A & B"));
        QCOMPARE(queries.back().predicates.back().text, std::string("Song \"quoted\""));
        auto first = std::move(pending.back());
        first(rows());
        const auto before = pending.size();
        QTRY_COMPARE(pending.size(), before + 1);
        auto missing = std::move(pending.back());
        missing(Service::Tracks{});
        QTRY_COMPARE(pending.size(), before + 2);
        auto duplicate = std::move(pending.back());
        duplicate(rows());
        QTRY_COMPARE(completions, 2);
        QCOMPARE(unmatched, 1);
        QCOMPARE(output.size(), 1U);
        QVERIFY(failure.isEmpty());
        service.refresh(definition, {});
        QCOMPARE(completions, 3);
        QVERIFY(failure.contains(QStringLiteral("Last.fm API key")));
    }
}

void BenchMainWindowTest::lastFmRefreshSelectsFreshTracksFromLargerPool() {
    using Service = DynamicPlaylistService;
    {
        DynamicPlaylistDefinition definition{.id = QStringLiteral("variety"),
                                             .name = QStringLiteral("Variety"),
                                             .profile = QStringLiteral("local"),
                                             .source = QStringLiteral("similar"),
                                             .artist = QStringLiteral("Seed"),
                                             .track = QStringLiteral("Seed track"),
                                             .limit = 2,
                                             .shuffle = false};
        QVector<RecommendationTrack> pool;
        for (int i = 0; i < 6; ++i)
            pool.push_back({QStringLiteral("Artist"), QStringLiteral("Track %1").arg(i), {}});
        QStringList selection;
        std::size_t matched = 0;
        const auto run = [&](QVector<RecommendationTrack> candidates, bool cancel = false) {
            Service service([](engine::DynamicSelection asked, core::CancellationToken,
                               Service::Completion completion) {
                const auto title = query::compile_tkq(asked.query)->predicates.back().text;
                LocalTrackRow row;
                row.raw_path = std::string("/raw-\xff/") + title;
                row.artist = "Artist";
                row.title = title;
                completion(Service::Tracks{row});
            });
            int finished = 0;
            connect(&service, &Service::finished, &service,
                    [&](const Service::Tracks& rows, int, const QString& error) {
                        QVERIFY2(error.isEmpty(), qPrintable(error));
                        ++finished;
                        selection.clear();
                        for (const auto& row : rows) {
                            selection.push_back(QString::fromStdString(row.title));
                        }
                        matched = service.matchedPoolSize();
                    });
            service.matchRecommendations(definition, std::move(candidates));
            if (cancel) {
                service.cancel();
                QCoreApplication::processEvents();
                QCOMPARE(finished, 0);
            } else {
                QTRY_COMPARE(finished, 1);
            }
        };
        run(pool);
        QCOMPARE(selection.size(), 2);
        QCOMPARE(matched, 6U);
        QVERIFY(std::is_sorted(selection.begin(), selection.end()));
        const auto first = selection;
        // A fresh service instance (reopened editor) remembers the last result.
        run(pool);
        QCOMPARE(selection.size(), 2);
        for (const auto& title : selection)
            QVERIFY(!first.contains(title));
        const auto second = selection;
        run(pool, true); // Aborted matching must not advance history.
        run({});         // An empty response must not erase it either.
        QVERIFY(selection.isEmpty());
        run(pool);
        for (const auto& title : selection)
            QVERIFY(!second.contains(title));
        // With only one unseen track available, exactly one repeat is needed.
        pool.resize(3);
        run(pool);
        const auto small_first = selection;
        run(pool);
        QCOMPARE(selection.size(), 2);
        int overlap = 0;
        for (const auto& title : selection)
            if (small_first.contains(title))
                ++overlap;
        QCOMPARE(overlap, 1);
        // Undersized pools remain useful, with no duplicates or invented matches.
        definition.shuffle = true;
        pool.resize(1);
        run(pool);
        QCOMPARE(selection.size(), 1);
        QCOMPARE(matched, 1U);
    }
}

void BenchMainWindowTest::dynamicPlaylistCatalogAndEditor() {
    DynamicPlaylistDefinition local{.id = QStringLiteral("favorites"),
                                    .name = QStringLiteral("Favorites"),
                                    .profile = QStringLiteral("local"),
                                    .group_by = QStringLiteral("%albumartist%"),
                                    .groups = 4,
                                    .per_group = 2};
    QVERIFY(saveDynamicPlaylists(local.profile, {local}));
    auto loaded = loadDynamicPlaylists(local.profile);
    QVERIFY(loaded);
    QCOMPARE(loaded->size(), 1);
    QCOMPARE(loaded->front().query, local.query);
    QCOMPARE(loaded->front().group_by, local.group_by);
    QCOMPARE(loaded->front().groups, 4);
    QCOMPARE(loaded->front().per_group, 2);
    // Groups need an expression to group by.
    auto ungroupable = local;
    ungroupable.group_by.clear();
    QVERIFY(!saveDynamicPlaylists(local.profile, {ungroupable}));
    QVERIFY(loadDynamicPlaylists(QStringLiteral("mpd/other"))->isEmpty());
    QVERIFY(!saveDynamicPlaylists(QStringLiteral("mpd/other"), {local}));
    // ADR-0258: the shipped rules are always there, first, valid, and
    // never written.
    const auto shipped = shippedDynamicPlaylists(local.profile);
    QCOMPARE(shipped.size(), 7);
    for (const auto& rule : shipped) {
        QVERIFY(rule.shipped());
        QVERIFY2(query::compile_tkq(rule.query.toStdString()), qPrintable(rule.query));
        QVERIFY(saveDynamicPlaylists(local.profile, {rule}).has_value() == false);
    }
    const auto random_album = std::ranges::find(shipped, QStringLiteral("shipped:random-album"),
                                                &DynamicPlaylistDefinition::id);
    QVERIFY(random_album != shipped.end());
    QCOMPARE(random_album->selection().groups, 1U);
    QCOMPARE(random_album->selection().per_group, 0U);
    auto catalog_definitions = dynamicPlaylistCatalog(local.profile);
    QVERIFY(catalog_definitions);
    QCOMPARE(catalog_definitions->size(), 8);
    QCOMPARE(catalog_definitions->front().id, shipped.front().id);
    QCOMPARE(catalog_definitions->back().id, local.id);
    // Definitions the MPD backend saved under its own profile move over once.
    DynamicPlaylistDefinition old{.id = QStringLiteral("grunge"),
                                  .name = QStringLiteral("Last.fm: Grunge"),
                                  .profile = QStringLiteral("mpd/3cd2aea9"),
                                  .source = QStringLiteral("tag"),
                                  .tag = QStringLiteral("grunge")};
    QSettings{}.remove(QStringLiteral("dynamic-playlists/adopted-v1"));
    QVERIFY(saveDynamicPlaylists(old.profile, {old}));
    adoptDynamicPlaylists(local.profile);
    loaded = loadDynamicPlaylists(local.profile);
    QVERIFY(loaded);
    QCOMPARE(loaded->size(), 2);
    QCOMPARE(loaded->back().id, old.id);
    QCOMPARE(loaded->back().profile, local.profile);
    QVERIFY(loadDynamicPlaylists(old.profile)->isEmpty());
    QVERIFY(saveDynamicPlaylists(old.profile, {old}));
    adoptDynamicPlaylists(local.profile);
    QCOMPARE(loadDynamicPlaylists(old.profile)->size(), 1);
    QSettings{}.remove(QStringLiteral("dynamic-playlists/v1/mpd/3cd2aea9"));
    QVERIFY(saveDynamicPlaylists(local.profile, {local}));
    QSettings{}.setValue(QStringLiteral("dynamic-playlists/v1/mpd/future"),
                         QByteArray{"{\"version\":99,\"definitions\":[]}"});
    QVERIFY(!loadDynamicPlaylists(QStringLiteral("mpd/future")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.findChild<LocalLibraryPanel*>());
    auto* action = window.findChild<QAction*>(QStringLiteral("action-dynamic-playlists"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<DynamicPlaylistDialog*>();
    QVERIFY(dialog);
    auto* catalog = dialog->findChild<QComboBox*>(QStringLiteral("dynamic-catalog"));
    QVERIFY(catalog);
    // "New", the seven that ship, and the one saved.
    QCOMPARE(catalog->count(), 9);
    auto* save = dialog->findChild<QPushButton*>(QStringLiteral("dynamic-save"));
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("dynamic-remove"));
    // A shipped rule is used and copied, not changed or removed.
    catalog->setCurrentIndex(2);
    emit catalog->activated(2);
    QCOMPARE(catalog->itemText(2), QStringLiteral("Random album"));
    QCOMPARE(dialog->findChild<QSpinBox*>(QStringLiteral("dynamic-groups"))->value(), 1);
    QVERIFY(!remove->isEnabled());
    QCOMPARE(save->text(), QStringLiteral("Save as copy"));
    dialog->findChild<QSpinBox*>(QStringLiteral("dynamic-per-group"))->setValue(2);
    save->click();
    QCOMPARE(catalog->count(), 10);
    QCOMPARE(catalog->itemText(catalog->currentIndex()), QStringLiteral("Random album (copy)"));
    QVERIFY(remove->isEnabled());
    QCOMPARE(shippedDynamicPlaylists(QStringLiteral("local"))[1].per_group, 0);
    loaded = loadDynamicPlaylists(QStringLiteral("local"));
    QCOMPARE(loaded->size(), 2);
    QCOMPARE(loaded->back().per_group, 2);
    remove->click();
    QCOMPARE(catalog->count(), 9);
    catalog->setCurrentIndex(0);
    emit catalog->activated(0);
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-name"))->setText(QStringLiteral("Rock"));
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-query"))
        ->setText(QStringLiteral("genre HAS rock"));
    save->click();
    QCOMPARE(catalog->count(), 10);
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-refresh"))->click();
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("dynamic-status"));
    QTRY_VERIFY(status->text().startsWith(QStringLiteral("0 tracks")));
    QVERIFY(!dialog->findChild<QPushButton*>(QStringLiteral("dynamic-open"))->isEnabled());
    if (const auto screenshot_dir = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
        !screenshot_dir.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshot_dir + QStringLiteral("/dynamic-playlists.png")));
    }
    // Opening a snapshot must keep the flat preview layout, even when the
    // ordinary workspace default groups albums.
    LocalTrackRow row;
    row.raw_path = "/dynamic-layout-test.flac";
    row.title = "A track";
    emit dialog->snapshotRequested(QStringLiteral("Dynamic layout"),
                                   DynamicPlaylistService::Tracks{row});
    auto* local_tab = window.currentListTab();
    QVERIFY(local_tab);
    QCOMPARE(local_tab->view_layout.presentation, ui::TrackViewPresentation::plain_columns);
    QVERIFY(!static_cast<ui::QueueTableView*>(local_tab->view)->albumGroupingEnabled());
    QVERIFY(local_tab->view->isColumnHidden(local_artwork_column));
    QPointer<DynamicPlaylistDialog> previous(dialog);
    dialog->close();
    QTRY_VERIFY(previous.isNull());
    action->trigger();
    dialog = window.findChild<DynamicPlaylistDialog*>();
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QComboBox*>(QStringLiteral("dynamic-catalog"))->count(), 10);
    dialog->close();
}

// ADR-0141: a fresh sidecar beside the file projects onto probed rows
// at sidecar provenance; a stale one projects nothing.
void BenchMainWindowTest::loudnessSidecarProjectsOntoProbedRows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto fresh_path = media.filePath(QStringLiteral("fresh.wav"));
    const auto stale_path = media.filePath(QStringLiteral("stale.wav"));
    write_wave(fresh_path, wave_sample_rate / 10U);
    write_wave(stale_path, wave_sample_rate / 10U);
    const auto write_sidecar = [](const QString& audio, const bool fresh) {
        const auto encoded = QFile::encodeName(audio);
        const std::string raw{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        auto revision = trackknife::core::observe_local_source_revision(raw);
        QVERIFY(revision.has_value());
        trackknife::metadata::LoudnessSidecar sidecar;
        sidecar.source_size = fresh ? revision->size : revision->size + 1U;
        sidecar.source_modified_seconds = revision->modification_time_seconds;
        sidecar.source_modified_nanoseconds = revision->modification_time_nanoseconds;
        sidecar.entries = {trackknife::metadata::LoudnessSidecarEntry{
            .stream_index = std::nullopt,
            .subsong_index = std::nullopt,
            .start_sample = std::nullopt,
            .end_sample = std::nullopt,
            .track_gain_db = -6.02,
            .track_peak = 1.0,
            .album_gain_db = std::nullopt,
            .album_peak = std::nullopt,
        }};
        const auto serialized = trackknife::metadata::serialize_loudness_sidecar(sidecar);
        QVERIFY(serialized.has_value());
        QFile output{QString::fromStdString(trackknife::metadata::loudness_sidecar_path(raw))};
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.write(serialized->data(), static_cast<qint64>(serialized->size()));
    };
    write_sidecar(fresh_path, true);
    write_sidecar(stale_path, false);

    BenchMainWindow window;
    window.show();
    const auto fresh_encoded = QFile::encodeName(fresh_path);
    const auto stale_encoded = QFile::encodeName(stale_path);
    window.openLocalPaths(
        {std::string{fresh_encoded.constData(), static_cast<std::size_t>(fresh_encoded.size())},
         std::string{stale_encoded.constData(), static_cast<std::size_t>(stale_encoded.size())}});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    LocalListModel* list_model = nullptr;
    QTRY_VERIFY((list_model = [&]() -> LocalListModel* {
                    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
                    return view == nullptr ? nullptr : qobject_cast<LocalListModel*>(view->model());
                }()) != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 2, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed && list_model->rows().back().probed,
                             5'000);
    const auto sidecar_value = [&](const trackknife::bench::LocalTrackRow& row,
                                   const std::string_view name) -> std::optional<std::string> {
        const auto canonical = trackknife::metadata::canonicalize_field_name(name);
        for (const auto& field : row.metadata.fields) {
            if (field.provenance == trackknife::metadata::FieldProvenance::sidecar &&
                field.canonical_name == canonical && !field.values.empty()) {
                return field.values.front();
            }
        }
        return std::nullopt;
    };
    const auto& fresh_row = list_model->rows()[0];
    const auto& stale_row = list_model->rows()[1];
    QCOMPARE(sidecar_value(fresh_row, "REPLAYGAIN_TRACK_GAIN"),
             std::optional<std::string>{"-6.02 dB"});
    QCOMPARE(sidecar_value(fresh_row, "REPLAYGAIN_TRACK_PEAK"),
             std::optional<std::string>{"1.000000"});
    QVERIFY(!sidecar_value(stale_row, "REPLAYGAIN_TRACK_GAIN").has_value());
}

void BenchMainWindowTest::replayGainScanStagesMeasuredGainsAsDrafts() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto loud_path = media.filePath(QStringLiteral("loud.wav"));
    const auto quiet_path = media.filePath(QStringLiteral("quiet.wav"));
    write_sine_wav_fixture(loud_path, 0.8);
    write_sine_wav_fixture(quiet_path, 0.2);

    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](const QString& path, const QString& title) {
        const auto encoded = QFile::encodeName(path);
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path =
                        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("TITLE", {title.toStdString()}),
                                       field("ALBUM", {"Gain Album"}),
                                       field("ALBUMARTIST", {"Band"}),
                                       field("MUSICBRAINZ_ALBUMID",
                                             {"11111111-2222-3333-4444-555555555555"})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = title,
        };
    };
    const std::vector sources{make_source(loud_path, QStringLiteral("Loud")),
                              make_source(quiet_path, QStringLiteral("Quiet"))};

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    auto* grouping = properties->findChild<QComboBox*>(QStringLiteral("bench-replaygain-grouping"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(scan != nullptr);
    QVERIFY(grouping != nullptr);
    QCOMPARE(grouping->currentIndex(), 0);
    files->selectAll();
    QTRY_VERIFY(scan->isEnabled());
    QTest::mouseClick(scan, Qt::LeftButton);

    // The measured gains stage as ordinary colored drafts with ReplayGain
    // provenance; the shared release id makes both files one programme.
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() >= 8U, 15'000);
    const auto track_gain_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN"));
    const auto track_peak_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_PEAK"));
    const auto album_gain_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_ALBUM_GAIN"));
    const auto album_peak_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_ALBUM_PEAK"));
    QVERIFY(track_gain_column.has_value());
    QVERIFY(track_peak_column.has_value());
    QVERIFY(album_gain_column.has_value());
    QVERIFY(album_peak_column.has_value());
    const auto loud_gain =
        grid_model->index(0, *track_gain_column).data(metadata_cell_values_role).toStringList();
    const auto quiet_gain =
        grid_model->index(1, *track_gain_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(loud_gain.size(), 1);
    QCOMPARE(quiet_gain.size(), 1);
    QVERIFY(loud_gain.front().endsWith(QStringLiteral(" dB")));
    // The quiet tone needs roughly 12 dB more gain than the loud one.
    const auto loud_value = loud_gain.front().chopped(3).toDouble();
    const auto quiet_value = quiet_gain.front().chopped(3).toDouble();
    QVERIFY(std::abs((quiet_value - loud_value) - 12.04) < 0.3);
    // One programme: identical album gain and peak on both rows.
    QCOMPARE(
        grid_model->index(0, *album_gain_column).data(metadata_cell_values_role).toStringList(),
        grid_model->index(1, *album_gain_column).data(metadata_cell_values_role).toStringList());
    const auto album_peak =
        grid_model->index(0, *album_peak_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(album_peak.size(), 1);
    QVERIFY(album_peak.front().startsWith(QStringLiteral("0.79")) ||
            album_peak.front().startsWith(QStringLiteral("0.80")));
    QCOMPARE(
        grid_model->index(0, *track_gain_column).data(metadata_cell_staged_source_role).toString(),
        QStringLiteral("ReplayGain"));

    // Staging inserted new columns; the file selection must survive so the
    // fields pane keeps projecting the selection instead of going blank.
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);

    // ADR-0147: the measurement exports as CSV through the status link.
    QTemporaryDir export_directory;
    QVERIFY(export_directory.isValid());
    const auto export_path = export_directory.filePath(QStringLiteral("results.csv"));
    properties->setProperty("trackknife-replaygain-export-path", export_path);
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    emit status->linkActivated(QStringLiteral("export-replaygain"));
    QFile exported{export_path};
    QVERIFY(exported.open(QIODevice::ReadOnly));
    const auto csv = QString::fromUtf8(exported.readAll());
    QVERIFY(csv.startsWith(QStringLiteral(
        "track,file,integrated_lufs,track_gain_db,track_peak,album_key,album_gain_db,"
        "album_peak,status,peak_kind")));
    QVERIFY(csv.contains(loud_gain.front()));
    // ADR-0148: the default policy exports sample peaks.
    QVERIFY(csv.contains(QStringLiteral("analyzed,sample")));
    QCOMPARE(csv.count(QLatin1Char('\n')), 3);

    // ADR-0147: the provenance view marks the unsaved scan values as
    // drafts, one row per track.
    auto* provenance_button =
        properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-provenance"));
    QVERIFY(provenance_button != nullptr);
    QVERIFY(provenance_button->isEnabled());
    provenance_button->click();
    QTableWidget* provenance_table = nullptr;
    QTRY_VERIFY((provenance_table = properties->findChild<QTableWidget*>(
                     QStringLiteral("bench-replaygain-provenance-table"))) != nullptr);
    QCOMPARE(provenance_table->rowCount(), 2);
    QCOMPARE(provenance_table->columnCount(), 7);
    QVERIFY(provenance_table->item(0, 1) != nullptr);
    QCOMPARE(provenance_table->item(0, 1)->text(),
             QStringLiteral("%1 · draft").arg(loud_gain.front()));

    // Closing would rightly demand draft confirmation; tear down directly.
    delete properties;
}

// ADR-0148: opting in routes the oversampled true peak into the
// REPLAYGAIN_*_PEAK drafts and the export names the peak kind.
void BenchMainWindowTest::replayGainScanUsesTruePeakWhenOptedIn() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("tone.wav"));
    write_sine_wav_fixture(path, 0.5);
    const auto encoded = QFile::encodeName(path);
    const std::vector sources{MetadataPropertiesSource{
        .source =
            metadata::StagedMetadataSource{
                .raw_path =
                    std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                .source_revision = std::nullopt,
                .baseline = metadata::MetadataDocument{},
            },
        .track_label = QStringLiteral("Tone"),
    }};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(scan != nullptr);
    // ADR-0185: the true-peak preference lives in Settings; the scan reads
    // it from QSettings.
    QSettings{}.setValue(QLatin1String(SettingsDialog::replaygain_true_peak_key), true);
    files->selectAll();
    QTRY_VERIFY(scan->isEnabled());
    QTest::mouseClick(scan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() >= 2U, 15'000);

    // A clean sine's true peak barely exceeds its sample peak; the draft
    // must still be the plausible linear amplitude.
    const auto peak_column = grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_PEAK"));
    QVERIFY(peak_column.has_value());
    const auto peak_values =
        grid_model->index(0, *peak_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(peak_values.size(), 1);
    const auto peak = peak_values.front().toDouble();
    QVERIFY(peak > 0.45 && peak < 0.6);

    QTemporaryDir export_directory;
    QVERIFY(export_directory.isValid());
    const auto export_path = export_directory.filePath(QStringLiteral("results.csv"));
    properties->setProperty("trackknife-replaygain-export-path", export_path);
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    emit status->linkActivated(QStringLiteral("export-replaygain"));
    QFile exported{export_path};
    QVERIFY(exported.open(QIODevice::ReadOnly));
    const auto csv = QString::fromUtf8(exported.readAll());
    QVERIFY(csv.contains(QStringLiteral("analyzed,true_peak")));

    // The setting is sticky; restore the default so later dialogs scan
    // sample peaks again.
    QSettings{}.remove(QLatin1String(SettingsDialog::replaygain_true_peak_key));
    delete properties;
}

// ADR-0149: measured Opus sources stage RFC 7845 R128 Q7.8 comments for
// tag writes; the sidecar-only policy outranks that and keeps the
// conventional -18 LUFS fields because the audio file is never touched.
void BenchMainWindowTest::replayGainScanStagesR128ForOpusTags() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("tone.opus"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("loudness-tone-opus.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::vector sources{MetadataPropertiesSource{
        .source =
            metadata::StagedMetadataSource{
                .raw_path =
                    std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                .source_revision = std::nullopt,
                .baseline = metadata::MetadataDocument{},
            },
        .track_label = QStringLiteral("Opus tone"),
    }};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(scan != nullptr);
    QSettings{}.remove(QLatin1String(SettingsDialog::replaygain_sidecar_only_key));
    files->selectAll();
    QTRY_VERIFY(scan->isEnabled());
    QTest::mouseClick(scan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() >= 1U, 15'000);

    const auto r128_column = grid_model->fieldColumn(QStringLiteral("R128_TRACK_GAIN"));
    QVERIFY(r128_column.has_value());
    const auto r128_values =
        grid_model->index(0, *r128_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(r128_values.size(), 1);
    // A strict Q7.8 integer, and one the reader lifts back into a sane
    // ReplayGain-scale gain for this quiet tone.
    bool integral = false;
    const auto q78 = r128_values.front().toInt(&integral);
    QVERIFY(integral);
    const auto lifted = q78 / 256.0 + 5.0;
    QVERIFY(lifted > -20.0 && lifted < 20.0);
    // RFC 7845 defines no peak field.
    QVERIFY(!grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_PEAK")).has_value());
    QVERIFY(!grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN")).has_value());

    // Under the sidecar-only policy the same file stages conventional
    // fields for the Trackbench-owned carrier.
    QSettings{}.setValue(QLatin1String(SettingsDialog::replaygain_sidecar_only_key), true);
    QTest::mouseClick(scan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN")).has_value(), 15'000);
    const auto conventional_column =
        grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN"));
    const auto conventional =
        grid_model->index(0, *conventional_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(conventional.size(), 1);
    QVERIFY(conventional.front().endsWith(QStringLiteral(" dB")));

    delete properties;
    QSettings{}.remove(QLatin1String(SettingsDialog::replaygain_sidecar_only_key));
}

// ADR-0152: the read-only technical summary under the file list probes
// the selected files in the background and aggregates codec, rate,
// depth, channels, bitrate, and duration; disagreement shows "mixed".
void BenchMainWindowTest::propertiesShowTechnicalSummary() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    const auto opus_path = media.filePath(QStringLiteral("tone.opus"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("loudness-tone-opus.b64"), opus_path));
    const auto make_source = [](const QString& path, const QString& label) {
        const auto encoded = QFile::encodeName(path);
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path =
                        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
                    .source_revision = std::nullopt,
                    .baseline = metadata::MetadataDocument{},
                },
            .track_label = label,
        };
    };
    const std::vector sources{make_source(flac_path, QStringLiteral("Flac")),
                              make_source(opus_path, QStringLiteral("Opus"))};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QLabel* technical = nullptr;
    QTRY_VERIFY((technical = properties->findChild<QLabel*>(
                     QStringLiteral("bench-metadata-technical"))) != nullptr);
    // The whole selection mixes a 44.1 kHz FLAC with a 48 kHz Opus file.
    QTRY_VERIFY_WITH_TIMEOUT(!technical->text().isEmpty() &&
                                 !technical->text().contains(QStringLiteral("analyzing")),
                             15'000);
    QVERIFY(technical->text().contains(QStringLiteral("2 tracks")));
    QVERIFY(technical->text().contains(QStringLiteral("mixed codecs")));
    QVERIFY(technical->text().contains(QStringLiteral("mixed rates")));
    QVERIFY(technical->text().contains(QStringLiteral("total ")));

    // A single selection shows that file's concrete values.
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    files->selectionModel()->select(files->model()->index(0, 0),
                                    QItemSelectionModel::ClearAndSelect |
                                        QItemSelectionModel::Rows);
    QTRY_VERIFY_WITH_TIMEOUT(technical->text().contains(QStringLiteral("FLAC")) &&
                                 technical->text().contains(QStringLiteral("44100 Hz")),
                             15'000);
    QVERIFY(technical->text().contains(QStringLiteral("16 bit")));
    QVERIFY(!technical->text().contains(QStringLiteral("mixed")));
    delete properties;
}

// ADR-0153: the standalone search dialog filters the current tab with
// tkq over row metadata and technicals, and keeps results as tabs.
void BenchMainWindowTest::savedSearchesCanBeManagedAndReopened() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database =
        std::filesystem::path{directory.filePath(QStringLiteral("searches.sqlite3")).toStdString()};
    LocalTrackRow first{};
    first.title = "Jazz";
    first.raw_path = "/music/first.flac";
    LocalTrackRow second{};
    second.title = "Rock";
    second.raw_path = "/music/second.flac";
    for (auto* row : {&first, &second}) {
        row->metadata.fields.push_back({.canonical_name = "title",
                                        .native_name = "TITLE",
                                        .values = {row->title},
                                        .qualifier = {},
                                        .provenance = metadata::FieldProvenance::embedded});
    }
    std::vector<LocalTrackRow> current{first, second};
    const auto access = [&current]() -> std::optional<SearchDialog::TabSnapshot> {
        return SearchDialog::TabSnapshot{QStringLiteral("Current"), current};
    };
    const auto name_dialog = [](const QString& value) {
        QTimer::singleShot(0, [value] {
            auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (prompt) {
                prompt->setTextValue(value);
                prompt->accept();
            }
        });
    };
    {
        CatalogueSource catalogues{database};
        SearchDialog dialog{catalogues, access, {}};
        dialog.show();
        auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
        auto* scope = dialog.findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
        auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
        auto* saved = dialog.findChild<QComboBox*>(QStringLiteral("bench-search-saved"));
        auto* save = dialog.findChild<QPushButton*>(QStringLiteral("bench-search-save"));
        auto* update = dialog.findChild<QPushButton*>(QStringLiteral("bench-search-update"));
        auto* rename = dialog.findChild<QPushButton*>(QStringLiteral("bench-search-rename"));
        auto* results = dialog.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
        QVERIFY(input && scope && mode && saved && save && update && rename && results);
        scope->setCurrentIndex(1);
        mode->setChecked(true);
        input->setText(QStringLiteral("title IS Jazz"));
        QTRY_VERIFY(save->isEnabled());
        QTRY_COMPARE(results->count(), 1);
        name_dialog(QStringLiteral("My jazz"));
        save->click();
        QTRY_COMPARE(saved->count(), 2);
        QCOMPARE(saved->currentText(), QStringLiteral("My jazz"));
        QVERIFY(!update->isEnabled());
        mode->setChecked(false);
        input->setText(QStringLiteral("Rock"));
        QTRY_VERIFY(update->isEnabled());
        update->click();
        QTRY_VERIFY(saved->isEnabled());
        QVERIFY(!update->isEnabled());
        name_dialog(QStringLiteral("My rock"));
        rename->click();
        QTRY_COMPARE(saved->currentText(), QStringLiteral("My rock"));
        QTRY_COMPARE(results->count(), 1);
        const auto screenshot_dir = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
        if (!screenshot_dir.isEmpty()) {
            QVERIFY(dialog.grab().save(screenshot_dir + QStringLiteral("/saved-searches.png")));
        }
        // Invalid queries cannot replace a valid saved definition.
        mode->setChecked(true);
        input->setText(QStringLiteral("AND ("));
        update->click();
        auto repository = persistence::ListRepository::open(database);
        QVERIFY(repository.has_value());
        auto definitions = repository->load_saved_searches();
        QVERIFY(definitions && definitions->size() == 1);
        QCOMPARE(definitions->front().expression, std::string{"Rock"});
        QCOMPARE(definitions->front().dialect, std::string{"words-1"});
        QCOMPARE(definitions->front().scope, persistence::SavedSearchScope::current_tab);
    }
    // A new dialog reruns the saved definition against the current tab, not old hits.
    current.push_back(second);
    CatalogueSource reopened_catalogues{database};
    SearchDialog reopened{reopened_catalogues, access, {}};
    reopened.show();
    auto* saved = reopened.findChild<QComboBox*>(QStringLiteral("bench-search-saved"));
    auto* input = reopened.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* scope = reopened.findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* mode = reopened.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = reopened.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* open = reopened.findChild<QPushButton*>(QStringLiteral("bench-search-open-tab"));
    auto* remove = reopened.findChild<QPushButton*>(QStringLiteral("bench-search-delete"));
    QVERIFY(saved && input && scope && mode && results && open && remove);
    QTRY_COMPARE(saved->count(), 2);
    saved->setCurrentIndex(1);
    QVERIFY(QMetaObject::invokeMethod(saved, "activated", Q_ARG(int, 1)));
    QCOMPARE(input->text(), QStringLiteral("Rock"));
    QCOMPARE(scope->currentIndex(), 1);
    QVERIFY(!mode->isChecked());
    QTRY_COMPARE(results->count(), 2);
    // Switching scope invalidates the old payload immediately.
    scope->setCurrentIndex(0);
    QVERIFY(!open->isEnabled());
    QCOMPARE(results->count(), 0);
    QTRY_VERIFY(reopened.findChild<QLabel*>(QStringLiteral("bench-search-status"))
                    ->text()
                    .startsWith(QStringLiteral("0 matches")));
    QTimer::singleShot(0, [] {
        if (auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            prompt->button(QMessageBox::Yes)->click();
        }
    });
    remove->click();
    QTRY_COMPARE(saved->count(), 1);
    auto repository = persistence::ListRepository::open(database);
    QVERIFY(repository && repository->load_saved_searches()->empty());
    mode->setChecked(false);
}

void BenchMainWindowTest::searchDialogFiltersTabAndOpensResults() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    const auto opus_path = media.filePath(QStringLiteral("tone.opus"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("loudness-tone-opus.b64"), opus_path));

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(
        {QFile::encodeName(flac_path).toStdString(), QFile::encodeName(opus_path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    QTRY_VERIFY(model->rows().front().probed && model->rows().back().probed);

    auto* action = window.findChild<QAction*>(QStringLiteral("action-search-dialog"));
    QVERIFY(action != nullptr);
    action->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-search-dialog"))) !=
                nullptr);
    auto* scope = dialog->findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* mode = dialog->findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = dialog->findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* open_button = dialog->findChild<QPushButton*>(QStringLiteral("bench-search-open-tab"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-search-status"));
    QVERIFY(scope && input && mode && results && open_button && status);

    scope->setCurrentIndex(1);
    mode->setChecked(true);
    input->setText(QStringLiteral("codec IS flac"));
    QTRY_COMPARE(results->count(), 1);
    input->setText(QStringLiteral("codec IS opus"));
    QTRY_VERIFY(results->count() == 1 && status->text().startsWith(QStringLiteral("1 match")));

    // Word search matches row metadata without tkq structure.
    mode->setChecked(false);
    input->setText(QStringLiteral("fixture"));
    QTRY_VERIFY(results->count() >= 1);

    // The full result set becomes an ordinary scratch tab with the rows.
    mode->setChecked(true);
    input->setText(QStringLiteral("codec IS opus"));
    QTRY_COMPARE(results->count(), 1);
    const auto tabs_before = tabs->count();
    QTRY_VERIFY(open_button->isEnabled());
    open_button->click();
    QTRY_COMPARE(tabs->count(), tabs_before + 1);
    auto* opened = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(opened != nullptr);
    auto* opened_model = qobject_cast<LocalListModel*>(opened->model());
    QVERIFY(opened_model != nullptr);
    QTRY_COMPARE(opened_model->rowCount(), 1);
    QCOMPARE(opened_model->rows().front().raw_path,
             std::string{QFile::encodeName(opus_path).constData()});

    mode->setChecked(false);
    dialog->close();
}

// A tab opened from the search dialog must show album covers like any other
// track tab: the rows carry the same files, so the shared artwork queue has
// to reach the new tab's model.
void BenchMainWindowTest::searchResultTabLoadsCovers() {
    QTemporaryDir media;
    QTemporaryDir other;
    QVERIFY(media.isValid() && other.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    const auto opus_path = other.filePath(QStringLiteral("tone.opus"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("loudness-tone-opus.b64"), opus_path));
    {
        QImage cover{32, 32, QImage::Format_RGB32};
        cover.fill(Qt::darkGreen);
        QVERIFY(cover.save(media.filePath(QStringLiteral("cover.png")), "PNG"));
    }

    BenchMainWindow window;
    window.show();
    // A local tab from a different folder keeps the window in local context
    // without priming the shared artwork cache for the indexed album.
    window.openLocalPaths({QFile::encodeName(opus_path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 1);
    QTRY_VERIFY(model->rows().front().probed);
    const auto artwork_of = [](LocalListModel* target) {
        return target->data(target->index(0, 0), static_cast<int>(ui::track_album_artwork_role))
            .value<QImage>();
    };

    {
        auto library = persistence::LocalLibrary::open(window.database_path_);
        QVERIFY(library.has_value());
        const auto root = QFile::encodeName(media.path());
        QVERIFY(
            library->add_root(std::string{root.constData(), static_cast<std::size_t>(root.size())})
                .has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(library->scan({}, progress).has_value());
    }

    auto* action = window.findChild<QAction*>(QStringLiteral("action-search-dialog"));
    QVERIFY(action != nullptr);
    action->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-search-dialog"))) !=
                nullptr);
    auto* scope = dialog->findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* mode = dialog->findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = dialog->findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* open_button = dialog->findChild<QPushButton*>(QStringLiteral("bench-search-open-tab"));
    QVERIFY(scope && input && mode && results && open_button);

    // Scope 0 is the library database: the rows never passed through a tab,
    // so their covers must be loaded for the opened result tab.
    scope->setCurrentIndex(0);
    mode->setChecked(true);
    input->setText(QStringLiteral("codec IS flac"));
    QTRY_COMPARE(results->count(), 1);
    const auto tabs_before = tabs->count();
    QTRY_VERIFY(open_button->isEnabled());
    open_button->click();
    QTRY_COMPARE(tabs->count(), tabs_before + 1);
    auto* opened = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(opened != nullptr);
    auto* opened_model = qobject_cast<LocalListModel*>(opened->model());
    QVERIFY(opened_model != nullptr);
    QTRY_COMPARE(opened_model->rowCount(), 1);
    QTRY_VERIFY(!artwork_of(opened_model).isNull());

    mode->setChecked(false);
    dialog->close();
}

void BenchMainWindowTest::searchDialogProbesMissingTechnicalsOnDemand() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    const auto encoded = QFile::encodeName(flac_path);

    LocalTrackRow row;
    row.raw_path = std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    row.title = "Restored";
    row.metadata.fields.push_back({.canonical_name = "title",
                                   .native_name = "TITLE",
                                   .values = {"Restored"},
                                   .qualifier = {},
                                   .provenance = metadata::FieldProvenance::embedded});
    row.probed = true;

    std::vector<std::pair<std::string, LocalTrackTechnicals>> reported;
    CatalogueSource catalogues{std::filesystem::path{}};
    SearchDialog dialog{catalogues,
                        [&row]() -> std::optional<SearchDialog::TabSnapshot> {
                            return SearchDialog::TabSnapshot{QStringLiteral("Fixture"), {row}};
                        },
                        [&reported](std::string path, LocalTrackTechnicals technicals) {
                            reported.emplace_back(std::move(path), technicals);
                        }};
    dialog.show();
    auto* scope = dialog.findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* results = dialog.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("bench-search-status"));
    QVERIFY(scope && input && mode && results && status);
    scope->setCurrentIndex(1);
    mode->setChecked(true);
    input->setText(QStringLiteral("samplerate GREATER 8000"));
    QTRY_COMPARE_WITH_TIMEOUT(results->count(), 1, 15'000);
    QVERIFY(status->text().contains(QStringLiteral("scanned 1 file")));
    QCOMPARE(reported.size(), 1U);
    QCOMPARE(reported.front().first, row.raw_path);
    QCOMPARE(reported.front().second.codec, std::string{"flac"});

    // Metadata-only queries never probe.
    reported.clear();
    input->setText(QStringLiteral("title HAS restored"));
    QTRY_COMPARE(results->count(), 1);
    QVERIFY(reported.empty());
    QVERIFY(!status->text().contains(QStringLiteral("scanned")));

    // ADR-0179: tab rows expose their loaded content-identity ratings to
    // rating predicates without touching the library store.
    input->setText(QStringLiteral("rating GREATER 7"));
    QTRY_COMPARE(results->count(), 0);
    row.rating = 8U;
    row.album_rating = 6U;
    input->setText(QStringLiteral("rating GREATER 7 AND albumrating EQUAL 6"));
    QTRY_COMPARE(results->count(), 1);
    input->setText(QStringLiteral("rating MISSING"));
    QTRY_COMPARE(results->count(), 0);
    mode->setChecked(false);
}

// The Server library scope translates tkq for the connected server,
// previews the result labels, refuses untranslatable queries with the
// translator's message, and opens results through the window callback.
void BenchMainWindowTest::searchPresetsAreGroupedAndPrompt() {
    CatalogueSource catalogues{std::filesystem::path{}};
    SearchDialog dialog{catalogues, {}, {}};
    dialog.show();
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* scope = dialog.findChild<QComboBox*>(QStringLiteral("bench-search-scope"));
    auto* mode = dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"));
    auto* menu = dialog.findChild<QMenu*>(QStringLiteral("bench-search-presets-menu"));
    QVERIFY(input && scope && mode && menu);
    QCOMPARE(scope->count(), 2); // Library database, current tab.
    QTRY_VERIFY(input->hasFocus());
    scope->setFocus();
    dialog.hide();
    dialog.show();
    dialog.activateWindow();
    // The offscreen platform need not reactivate a hidden native window;
    // its remembered focus target must still be the input, not the scope.
    QTRY_COMPARE(dialog.focusWidget(), input);
    const auto populate = [&] { QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow")); };
    populate();
    QCOMPARE(menu->actions().size(), 5);
    QCOMPARE(menu->actions().first()->text(), QStringLiteral("Explore"));
    auto* missing = menu->findChild<QAction*>(QStringLiteral("search-preset-missing-album"));
    QVERIFY(missing);
    missing->trigger();
    QCOMPARE(input->text(), QStringLiteral("album MISSING"));
    QVERIFY(mode->isChecked());
    auto* year = menu->findChild<QAction*>(QStringLiteral("search-preset-year"));
    QVERIFY(year);
    QTimer::singleShot(0, &dialog, [&] {
        auto* prompt = dialog.findChild<QInputDialog*>();
        QVERIFY(prompt);
        prompt->setIntValue(2001);
        prompt->accept();
    });
    year->trigger();
    QCOMPARE(input->text(), QStringLiteral("date EQUAL 2001"));
    QTimer::singleShot(0, &dialog, [&] {
        auto* prompt = dialog.findChild<QInputDialog*>();
        QVERIFY(prompt);
        prompt->reject();
    });
    year->trigger();
    QCOMPARE(input->text(), QStringLiteral("date EQUAL 2001"));
    // Every preset runs against the library itself, so none is withheld: the
    // gating only ever existed for what a remote server could translate.
    scope->setCurrentIndex(1);
    populate();
    QCOMPARE(menu->actions().size(), 5);
    QVERIFY(menu->findChild<QAction*>(QStringLiteral("search-preset-unplayed-albums")));
}

} // namespace trackknife::bench
