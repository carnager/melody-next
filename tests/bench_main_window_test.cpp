// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

namespace trackknife::bench {

void BenchMainWindowTest::initTestCase() {
    QVERIFY(home_directory_.isValid());
    qputenv("HOME", QFile::encodeName(home_directory_.path()));
    QVERIFY(settings_directory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("trackknife-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory_.path());
    QStandardPaths::setTestModeEnabled(true);
    // ADR-0287: the engine keeps its state in the test's own folder, as the
    // window opens it: not the shared default, nor its socket.
    qputenv("MELODY_STATE_DIR",
            QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
}

// ADR-0226: every window runs against an engine, as the application does.
// OK Computer with its bonus disc read as one album numbered 1 to 12 and
// then 1 to 15 again. Each disc's first row now names it -- but only in an
// album of more than one: a single disc says nothing.
void BenchMainWindowTest::discsOfAnAlbumAreNamed() {
    const auto track = [](const char* album, const char* disc, const char* subtitle,
                          const char* title) {
        LocalTrackRow row;
        row.raw_path = std::string{"/music/"} + album + "/" + disc + "/" + title + ".flac";
        row.title = title;
        row.artist = "Radiohead";
        row.album = album;
        row.date = "1997";
        const auto field = [&row](const char* name, const char* value) {
            if (*value != 0) {
                row.metadata.fields.push_back(
                    metadata::MetadataField{.canonical_name = name,
                                            .native_name = QByteArray{name}.toUpper().toStdString(),
                                            .values = {value},
                                            .qualifier = {},
                                            .provenance = metadata::FieldProvenance::embedded});
            }
        };
        field("discnumber", disc);
        field("discsubtitle", subtitle);
        row.probed = true;
        return row;
    };
    LocalListModel model;
    model.replaceRows({
        track("OK Computer", "1/2", "", "Airbag"),
        track("OK Computer", "1/2", "", "Paranoid Android"),
        track("OK Computer", "02/2", "B-sides", "Polyethylene"),
        track("OK Computer", "02/2", "B-sides", "Pearly"),
        track("The Bends", "1", "", "Planet Telex"),
        track("The Bends", "1", "", "The Bends"),
    });
    const auto disc = [&model](const int row) {
        return model.index(row, 0).data(ui::track_disc_start_role).toString();
    };
    QCOMPARE(disc(0), QStringLiteral("Disc 1"));
    QCOMPARE(disc(1), QString{});
    QCOMPARE(disc(2), QStringLiteral("Disc 2 · B-sides"));
    QCOMPARE(disc(3), QString{});
    QCOMPARE(disc(4), QString{});
    QCOMPARE(disc(5), QString{});
}

void BenchMainWindowTest::init() { QVERIFY2(engine_.start(), engine_.log().constData()); }

void BenchMainWindowTest::cleanup() {
    // A modifier left held turns the next case's clicks into Shift- or
    // Ctrl-clicks, so a case's outcome would hang on which case ran before
    // it. Released here, and the case that left it fails.
    const auto held = QGuiApplication::keyboardModifiers();
    if (held != Qt::NoModifier) {
        QWidget sink;
        releaseHeldModifiers(&sink);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    engine_.stop();
    QSettings settings;
    settings.clear();
    settings.sync();
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
    QDir{QStandardPaths::writableLocation(QStandardPaths::CacheLocation)}.removeRecursively();
    QCOMPARE(held, Qt::NoModifier);
}

void BenchMainWindowTest::commandPaletteFindsAndRunsRegisteredActions() {
    BenchMainWindow window;
    window.show();

    auto* command = window.findChild<QAction*>(QStringLiteral("action-command-palette"));
    QVERIFY(command != nullptr);
    QCOMPARE(command->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    // Parameter choices and transient actions must not leak into command discovery.
    QAction device(QStringLiteral("caprica"), &window);
    device.setObjectName(QStringLiteral("action-output-123"));
    QAction rating(QStringLiteral("1"), &window);
    rating.setObjectName(QStringLiteral("action-local-rate-1"));
    command->trigger();

    auto* palette = window.findChild<QDialog*>(QStringLiteral("command-palette"));
    QVERIFY(palette != nullptr);
    auto* filter = palette->findChild<QLineEdit*>(QStringLiteral("command-filter"));
    auto* results = palette->findChild<QListWidget*>(QStringLiteral("command-results"));
    auto* run = palette->findChild<QPushButton*>(QStringLiteral("command-run"));
    QVERIFY(filter != nullptr);
    QVERIFY(results != nullptr);
    QVERIFY(run != nullptr);
    filter->setText(QStringLiteral("caprica"));
    QCOMPARE(results->count(), 0);
    filter->setText(QStringLiteral("queue-rate"));
    QCOMPARE(results->count(), 0);
    QVERIFY(palette->findChild<QKeySequenceEdit*>() == nullptr);
    command->trigger();
    QCOMPARE(window.findChildren<ui::CommandPalette*>().size(), 1);

    filter->setText(QStringLiteral("Settings"));
    QCOMPARE(results->count(), 1);
    QVERIFY(results->currentItem()->text().startsWith(QStringLiteral("Settings")));
    run->click();
    QTRY_VERIFY(window.findChild<SettingsDialog*>() != nullptr);
    QVERIFY(!palette->isVisible());
}

void BenchMainWindowTest::commandPaletteTracksAvailabilityAndLifetime() {
    QAction first{QStringLiteral("Open files")};
    first.setObjectName(QStringLiteral("action-open-files"));
    auto second = std::make_unique<QAction>(QStringLiteral("Open folder"));
    second->setObjectName(QStringLiteral("action-open-folder"));
    second->setEnabled(false);
    QSignalSpy triggered{&first, &QAction::triggered};
    ui::CommandPalette palette({&first, &first, second.get()});
    palette.show();
    auto* filter = palette.findChild<QLineEdit*>(QStringLiteral("command-filter"));
    auto* results = palette.findChild<QListWidget*>(QStringLiteral("command-results"));
    auto* run = palette.findChild<QPushButton*>(QStringLiteral("command-run"));
    QCOMPARE(results->count(), 1);
    second->setEnabled(true);
    QCOMPARE(results->count(), 2);
    QTest::keyClick(filter, Qt::Key_Down);
    QCOMPARE(results->currentRow(), 1);
    QVERIFY(run->isEnabled());
    second->setEnabled(false);
    QCOMPARE(results->count(), 1);
    QCOMPARE(results->currentRow(), 0);
    second->setEnabled(true);
    QCOMPARE(results->count(), 2);
    second.reset();
    QCOMPARE(results->count(), 1);
    filter->setText(QStringLiteral("files open"));
    QCOMPARE(results->count(), 1);
    first.setEnabled(false);
    QCOMPARE(results->count(), 0);
    QVERIFY(!run->isEnabled());
    QTest::keyClick(filter, Qt::Key_Return);
    QCOMPARE(triggered.count(), 0);
    first.setEnabled(true);
    QCOMPARE(results->count(), 1);
    first.setVisible(false);
    QCOMPARE(results->count(), 0);
    QVERIFY(!run->isEnabled());
    first.setVisible(true);
    QTest::keyClick(filter, Qt::Key_Return);
    QCOMPARE(triggered.count(), 1);
    QVERIFY(!palette.isVisible());
}

// ADR-0282: the play count and last played columns are the list's engine's
// history of each file -- asked of the engine that holds the files, as a
// view reading history asks; a file it cannot see is unknown, not unplayed.
void BenchMainWindowTest::localListeningColumnsLoadRefreshAndRespectAuthority() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab);
    QTemporaryDir music;
    const auto heard = music.filePath(QStringLiteral("heard.flac"));
    {
        QFile file{heard};
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not really audio");
    }
    LocalTrackRow row;
    // As an engine's list gives its rows: a path, no revision.
    row.raw_path = heard.toStdString();
    row.title = "History fixture";
    row.probed = true;
    auto gone = row;
    gone.raw_path = music.filePath(QStringLiteral("gone.flac")).toStdString();
    tab->model->replaceRows({row, gone});
    auto* model = tab->model;
    const auto plays = [&](int n) {
        return model->index(n, local_play_count_column).data().toString();
    };
    QVERIFY(tab->view->isColumnHidden(local_play_count_column));
    const auto hidden_layout = window.captureTrackViewLayout(*tab);
    QCOMPARE(hidden_layout.columns.back().id, QStringLiteral("last-played"));
    QCOMPARE(hidden_layout.columns.back().width, 170);
    window.setTrackColumnVisible(QStringLiteral("play-count"), true);
    window.setTrackColumnVisible(QStringLiteral("last-played"), true);
    QVERIFY(!tab->view->isColumnHidden(local_play_count_column));
    QCOMPARE(tab->view->columnWidth(local_last_played_column), 170);
    QTRY_COMPARE(plays(0), QStringLiteral("0"));
    QCOMPARE(model->index(0, local_last_played_column).data().toString(), QStringLiteral("Never"));
    QCOMPARE(plays(1), QStringLiteral("—"));
    QVERIFY(model->index(0, local_play_count_column)
                .data(Qt::TextAlignmentRole)
                .value<Qt::Alignment>()
                .testFlag(Qt::AlignRight));

    // A listen recorded: the file is asked for again, and what the engine
    // says now is drawn, the selection kept.
    tab->view->selectRow(1);
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    model->forgetHistory({row.raw_path});
    QTRY_VERIFY(model->rows()[0].history_loaded);
    const qint64 played_at = 1700000000000;
    model->rows_[0].history_requested = true;
    QSignalSpy drawn(model, &QAbstractItemModel::dataChanged);
    model->applyHistory({model->rows()[0].entry_id}, {{3, played_at, 0, 3, played_at, 0}});
    QCOMPARE(plays(0), QStringLiteral("3"));
    QCOMPARE(model->index(0, local_last_played_column).data().toString(),
             QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(played_at), QLocale::ShortFormat));
    QVERIFY(std::ranges::any_of(drawn, [](const QList<QVariant>& signal) {
        return signal.at(0).toModelIndex().column() == local_play_count_column;
    }));
    QCOMPARE(resets.count(), 0);
    QCOMPARE(tab->view->selectionModel()->selectedRows().front().row(), 1);
    const auto restored_layout = ui::deserializeTrackViewLayout(
        ui::serializeTrackViewLayout(window.captureTrackViewLayout(*tab)), trackColumnIds());
    QVERIFY(restored_layout);
    window.applyTrackViewLayout(*tab, *restored_layout);
    QVERIFY(!tab->view->isColumnHidden(local_last_played_column));
    window.copyTrackViewLayoutToAllTabs();
    QVERIFY(window.track_column_actions_.value(QStringLiteral("play-count"))->isVisible());
}

void BenchMainWindowTest::dynamicPlaylistRetainsChangesDuringRefresh() {
    using Service = DynamicPlaylistService;
    std::vector<Service::Completion> pending;
    DynamicPlaylistDialog dialog(
        QStringLiteral("local"), {},
        [&](const EngineKey&, engine::DynamicSelection, core::CancellationToken,
            Service::Completion done) { pending.push_back(std::move(done)); });
    dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    auto* refresh = dialog.findChild<QPushButton*>(QStringLiteral("dynamic-refresh"));
    auto* stop = dialog.findChild<QPushButton*>(QStringLiteral("dynamic-stop"));
    refresh->click();
    QCOMPARE(pending.size(), 1U);
    dialog.libraryChanged();
    dialog.libraryChanged();
    LocalTrackRow stale;
    stale.raw_path = "/stale.flac";
    auto complete = std::move(pending.front());
    complete(Service::Tracks{std::vector<LocalTrackRow>{stale}});
    QCOMPARE(dialog.view()->model()->rowCount(), 0);
    QTRY_COMPARE(pending.size(), 2U);
    LocalTrackRow fresh;
    fresh.raw_path = "/fresh.flac";
    complete = std::move(pending.back());
    complete(Service::Tracks{std::vector<LocalTrackRow>{fresh}});
    QTRY_COMPARE(dialog.view()->model()->rowCount(), 1);
    QCOMPARE(qobject_cast<LocalListModel*>(dialog.view()->model())->rows().front().raw_path,
             fresh.raw_path);
    dialog.libraryChanged();
    stop->click();
    QTest::qWait(600);
    QCOMPARE(pending.size(), 2U);
}

void BenchMainWindowTest::dynamicResultSelectionSurvivesRefresh() {
    using Service = DynamicPlaylistService;
    {
        Service::Completion pending;
        DynamicPlaylistDialog dialog(QStringLiteral("local"), {},
                                     [&](const EngineKey&, engine::DynamicSelection,
                                         core::CancellationToken,
                                         Service::Completion done) { pending = std::move(done); });
        dialog.setAttribute(Qt::WA_DeleteOnClose, false);
        dialog.show();
        auto* refresh = dialog.findChild<QPushButton*>(QStringLiteral("dynamic-refresh"));
        const auto deliver = [&](const std::vector<std::string>& paths) {
            std::vector<LocalTrackRow> rows;
            for (const auto& path : paths) {
                LocalTrackRow row;
                row.raw_path = path;
                rows.push_back(row);
            }
            pending(Service::Tracks{std::move(rows)});
        };
        refresh->click();
        QVERIFY(pending);
        deliver({"a", "b", "a"});
        auto* view = dialog.view();
        QTRY_COMPARE(view->model()->rowCount(), 3);
        view->selectRow(2);
        view->setCurrentIndex(view->model()->index(2, ui::track_title_column));
        QSignalSpy played(&dialog, &DynamicPlaylistDialog::playRequested);
        refresh->click();
        QCOMPARE(view->model()->rowCount(), 3);
        deliver({"a", "a", "b"});
        QTRY_COMPARE(view->currentIndex().row(), 1);
        QCOMPARE(view->selectionModel()->selectedRows().size(), 1);
        QCOMPARE(view->selectionModel()->selectedRows().front().row(), 1);
        QCOMPARE(played.count(), 0);
        QVERIFY(view->dragEnabled());
        QVERIFY(!view->acceptDrops());
        QCOMPARE(view->defaultDropAction(), Qt::CopyAction);
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Return);
        QCOMPARE(played.count(), 1);
        QCOMPARE(played.front().front().toInt(), 1);
        std::vector<std::string> many;
        for (int i = 0; i < 80; ++i)
            many.push_back("track-" + std::to_string(i));
        refresh->click();
        deliver(many);
        QTRY_COMPARE(view->model()->rowCount(), 80);
        view->scrollTo(view->model()->index(40, ui::track_title_column),
                       QAbstractItemView::PositionAtTop);
        const auto anchor = view->indexAt(QPoint{1, 1}).data(ui::track_source_role).toString();
        QVERIFY(!anchor.isEmpty());
        many.insert(many.begin(), "new-track");
        refresh->click();
        deliver(many);
        QTRY_COMPARE(view->model()->rowCount(), 81);
        QTRY_COMPARE(view->indexAt(QPoint{1, 1}).data(ui::track_source_role).toString(), anchor);
    }
}

void BenchMainWindowTest::dynamicResultActionsUseTheirOwnSources() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    window.tabs_->setCurrentWidget(window.list_tabs_.front()->view);
    window.showDynamicPlaylists();
    auto* dialog = window.findChild<DynamicPlaylistDialog*>();
    QVERIFY(dialog);
    LocalTrackRow row;
    row.raw_path = "/dynamic/source.flac";
    row.title = "Dynamic source";
    auto* service = dialog->findChild<DynamicPlaylistService*>();
    QVERIFY(service);
    emit service->finished(DynamicPlaylistService::Tracks{std::vector{row}}, 0, {});
    auto* view = dialog->view();
    view->selectRow(0);
    QMenu menu;
    window.addUpNextActions(&menu, view);
    QVERIFY(!menu.actions().empty());
    menu.actions().front()->trigger();
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    QCOMPARE(window.playback_.requests.pending().front().source.raw_path, row.raw_path);
    auto* target = window.currentListTab();
    QVERIFY(target);
    const auto count = target->model->rowCount();
    const auto id = QString::fromStdString(target->document.id.to_string());
    QVERIFY(!window.transferRows(view, {0}, id, true, -1));
    QVERIFY(window.transferRows(view, {0}, id, false, -1));
    QCOMPARE(target->model->rowCount(), count + 1);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    LocalTrackRow other = row;
    other.raw_path = "/dynamic/other.flac";
    target->model->replaceRows({row, other, row});
    window.playback_.anchors.document = document_identity(id);
    window.playback_.row = 2;
    window.playback_.anchors.source = target->model->source(2);
    dialog->setProperty("playback-context", id);
    model->replaceRows({row, row, other});
    emit dialog->resultsChanged();
    QVERIFY(!model->index(0, 0).data(ui::track_current_role).toBool());
    QVERIFY(model->index(1, 0).data(ui::track_current_role).toBool());
    const auto reader =
        window.selectionSourceReader(model, {QPersistentModelIndex{model->index(0, 0)}});
    model->replaceRows({});
    QVERIFY(reader(0));
    QCOMPARE(reader(0)->source.raw_path, row.raw_path);
    dialog->close();
}

// ADR-0257/0259: a row the engine described carries the library's tags and
// the file's revision; the tagger still reads the file, as it reads any row
// whose tags are the library's. A row read from the file is not read again.
void BenchMainWindowTest::cachedRowsAreReadEvenWithARevision() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    persistence::LibraryTrackSnapshot snapshot;
    snapshot.raw_path = "/music/Deaf Radio/01.flac";
    snapshot.facts.fields["musicbrainzalbumid"].push_back({"731084e3", {}});
    auto described = cached_library_row(std::move(snapshot));
    described.source_revision = core::LocalSourceRevision{.device = 1, .inode = 2};
    LocalTrackRow read;
    read.raw_path = "/music/Quicksand/01.flac";
    read.probed = true;
    read.source_revision = core::LocalSourceRevision{.device = 1, .inode = 3};
    read.metadata.fields.push_back(
        metadata::MetadataField{.canonical_name = "musicbrainzalbumid",
                                .native_name = "MUSICBRAINZ_ALBUMID",
                                .values = {"dd034d22"},
                                .qualifier = {},
                                .provenance = metadata::FieldProvenance::embedded});
    LocalTrackRow unread = described;
    unread.raw_path = "/music/Other/01.flac";
    unread.source_revision.reset();
    tab->model->replaceRows({described, read, unread});
    const auto reader =
        window.selectionSourceReader(tab->model, {QPersistentModelIndex{tab->model->index(0, 0)},
                                                  QPersistentModelIndex{tab->model->index(1, 0)},
                                                  QPersistentModelIndex{tab->model->index(2, 0)}});
    const auto first = reader(0U);
    const auto second = reader(1U);
    const auto third = reader(2U);
    QVERIFY(first && second && third);
    // ADR-0268: a row read from the file is read again too -- the file may
    // have changed since, through another tagger -- so every one is read.
    QVERIFY(first->source.needs_metadata_capture && !first->source.source_revision);
    QVERIFY(second->source.needs_metadata_capture && !second->source.source_revision);
    QVERIFY(third->source.needs_metadata_capture && !third->source.source_revision);
}

// ADR-0269: Up Next is the engine's. What another client -- melody-cli, a
// rofi script, the phone -- asks for shows in the window's Up Next, and
// what it clears goes from it.
void BenchMainWindowTest::upNextFromAnotherClientShows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("asked.wav"));
    write_wave(path, wave_sample_rate / 10U);
    const auto raw = QFile::encodeName(path).toStdString();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto client = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(client.has_value());
    // As melody-cli queues: the entry handed over, then asked for.
    const auto entry = core::StableId::random().to_string();
    QVERIFY((*client)
                ->call("playback.enqueue",
                       protocol::Json{{"entries", protocol::Json::array({protocol::Json{
                                                      {"entry", entry},
                                                      {"path", protocol::encode_raw_path(raw)},
                                                      {"title", "Asked elsewhere"},
                                                      {"group",
                                                       {{"album_artist", "Someone"},
                                                        {"artist", "Someone"},
                                                        {"album", "Elsewhere"},
                                                        {"date", "2026"}}}}})}})
                .has_value());
    QVERIFY((*client)
                ->call("playback.set_requests",
                       protocol::Json{{"entries", protocol::Json::array({entry})}})
                .has_value());
    QTRY_COMPARE_WITH_TIMEOUT(window.workspace_.playback_.requests.pending().size(), 1U, 10'000);
    const auto& shown = window.workspace_.playback_.requests.pending().front().source;
    QCOMPARE(shown.title, std::string{"Asked elsewhere"});
    QCOMPARE(shown.album, std::string{"Elsewhere"});
    QCOMPARE(shown.raw_path, raw);
    QCOMPARE(shown.entry_id.to_string(), entry);
    // Cleared elsewhere: gone here too, and not stated back.
    QVERIFY((*client)->call("playback.clear_requests").has_value());
    QTRY_VERIFY_WITH_TIMEOUT(window.workspace_.playback_.requests.pending().empty(), 10'000);
    QTest::qWait(500);
    const auto asked = (*client)->call("playback.requests");
    QVERIFY(asked.has_value());
    QVERIFY(asked->value("entries", protocol::Json::array()).empty());
}

// ADR-0268: a list's rows whose files are not there are said to be, asked
// of its engine; a file that comes back, once the engine says so, is not.
void BenchMainWindowTest::listRowsWhoseFilesAreGoneAreGreyed() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto here = media.filePath(QStringLiteral("here.wav"));
    const auto gone = media.filePath(QStringLiteral("gone.wav"));
    write_wave(here, wave_sample_rate / 10U);
    const auto here_raw = QFile::encodeName(here).toStdString();
    const auto gone_raw = QFile::encodeName(gone).toStdString();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    persistence::ListDocument document{.id = core::StableId::random(),
                                       .kind = persistence::ListKind::scratch,
                                       .name = "Gone and here",
                                       .pinned = false,
                                       .dirty = false,
                                       .items = {},
                                       .engine = {}};
    for (const auto& path : {here_raw, gone_raw}) {
        persistence::ListItem item;
        item.source = persistence::ListSource::local;
        item.source_reference = path;
        document.items.push_back(item);
    }
    auto* tab = window.addListTab(document, true);
    QVERIFY(tab != nullptr);
    const auto missing = [tab](const int row) {
        return tab->model->index(row, ui::track_title_column).data(ui::track_missing_role).toBool();
    };
    QTRY_VERIFY_WITH_TIMEOUT(missing(1), 10'000);
    QVERIFY(!missing(0));
    QVERIFY(tab->model->index(1, ui::track_title_column)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("not there")));
    // Back: once its engine says the path changed, the row is as any other.
    write_wave(gone, wave_sample_rate / 10U);
    window.workspace_.adoptLibraryChange(EngineKey::local(), {gone_raw}, {}, false);
    QTRY_VERIFY_WITH_TIMEOUT(!missing(1), 10'000);
}

// ADR-0266: where an engine keeps its undo copies, asked of it and changed
// there at once.
void BenchMainWindowTest::settingsChooseWhereUndoCopiesAreKept() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::undo);
    QVERIFY(dialog != nullptr);
    auto* place = dialog->findChild<QWidget*>(QStringLiteral("bench-undo-location"));
    QVERIFY(place != nullptr);
    auto* own = place->findChild<QRadioButton*>(QStringLiteral("bench-undo-location-engine"));
    auto* beside = place->findChild<QRadioButton*>(QStringLiteral("bench-undo-location-beside"));
    auto* kept_in = place->findChild<QLabel*>(QStringLiteral("bench-undo-location-engine-folder"));
    auto* status = place->findChild<QLabel*>(QStringLiteral("bench-undo-location-status"));
    QVERIFY(own && beside && kept_in && status);
    // By default, the engine's own folder: said, with where it is.
    QTRY_VERIFY_WITH_TIMEOUT(own->isChecked() && own->isEnabled(), 10'000);
    QVERIFY(kept_in->text().endsWith(QStringLiteral("undo")));
    // Beside each file: the engine's at once.
    beside->click();
    QTRY_VERIFY_WITH_TIMEOUT(beside->isChecked() && beside->isEnabled(), 10'000);
    QCOMPARE(status->text(), QStringLiteral("Undo copies are kept there now"));
    auto asked = window.localEngine().file_work->backup_location();
    QVERIFY(asked.has_value());
    QCOMPARE(asked->place, std::string{"beside"});
    QVERIFY(asked->kept_in.empty());
    // And back.
    own->click();
    QTRY_VERIFY_WITH_TIMEOUT(own->isChecked() && own->isEnabled(), 10'000);
    asked = window.localEngine().file_work->backup_location();
    QVERIFY(asked && asked->place == "engine" && asked->kept_in.ends_with("undo"));
    dialog->close();
}

// Settings by what they are about: writing files (rename and move,
// ReplayGain, undo) as tabs of one page, ratings on their own, and the music
// folders of each engine on Library.
void BenchMainWindowTest::settingsGroupWhatTheyAreAbout() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::replaygain);
    QVERIFY(dialog != nullptr);
    auto* pages = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-pages"));
    QStringList titles;
    for (int row = 0; row < pages->count(); ++row) {
        titles << pages->item(row)->text();
    }
    QCOMPARE(titles.mid(0, 6),
             (QStringList{QStringLiteral("General"), QStringLiteral("Playback"),
                          QStringLiteral("Engines"), QStringLiteral("File operations"),
                          QStringLiteral("Ratings"), QStringLiteral("Covers")}));
    QCOMPARE(titles, SettingsSession::pageTitles().mid(0, titles.size()));
    // A tab of File operations opens that page at the tab.
    auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("bench-settings-file-operations"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("File operations"));
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("ReplayGain"));
    QVERIFY(tabs->currentWidget()->isAncestorOf(
        dialog->findChild<QWidget*>(QStringLiteral("bench-replaygain-sidecar-only"))));
    dialog->showPage(SettingsDialog::Page::undo);
    QCOMPARE(tabs->currentWidget()->objectName(), QStringLiteral("bench-settings-undo"));
    dialog->showPage(SettingsDialog::Page::ratings);
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("Ratings"));
    dialog->showPage(SettingsDialog::Page::naming);
    QCOMPARE(tabs->currentIndex(), 0);
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("File operations"));
    auto* stack = dialog->findChild<QStackedWidget*>();
    QVERIFY(stack != nullptr);
    const auto page_of = [stack](const char* name) {
        auto* widget = stack->findChild<QWidget*>(QLatin1String(name));
        for (int index = 0; widget != nullptr && index < stack->count(); ++index) {
            if (stack->widget(index)->isAncestorOf(widget)) {
                return index;
            }
        }
        return -1;
    };
    QCOMPARE(page_of("bench-settings-ratings-in-tags"),
             static_cast<int>(SettingsDialog::Page::ratings));
    // ADR-0286: Engines -- this computer alone, holding the library, its
    // music folders on the same page.
    QCOMPARE(page_of("bench-settings-library-folders"),
             static_cast<int>(SettingsDialog::Page::engine));
    auto* engines = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-engines"));
    QVERIFY(engines != nullptr);
    QCOMPARE(engines->count(), 1);
    auto* holder =
        dialog->findChild<QRadioButton*>(QStringLiteral("bench-settings-library-holder-0"));
    QVERIFY(holder != nullptr && holder->isChecked());
    QVERIFY(holder->text().startsWith(QStringLiteral("This computer")));
    // ADR-0322: whether this computer's engine watches its library's
    // folders, as the engine itself says -- on, its default.
    auto* watch = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-library-watch-box"));
    QVERIFY(watch != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(watch->isEnabled(), 10'000);
    QVERIFY(watch->isChecked());
    dialog->close();
}

void BenchMainWindowTest::currentTabHistoryPreservesOccurrences() {
    // The engine's database, which is the application's (ADR-0226): history
    // written there is what the engine answers with.
    const auto database =
        std::filesystem::path{
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString()} /
        "lists.sqlite";
    auto repository = persistence::ListRepository::open(database);
    QVERIFY(repository);
    LocalTrackRow played, unplayed;
    played.raw_path = std::string{"/not-mounted/raw-"} + char(0xff) + ".flac";
    played.title = "Played";
    played.source_revision = core::LocalSourceRevision{.device = 1, .inode = 2};
    unplayed.raw_path = "/not-mounted/unplayed.flac";
    unplayed.title = "Unplayed";
    unplayed.source_revision = core::LocalSourceRevision{.device = 1, .inode = 3};
    persistence::ListItem source;
    source.source = persistence::ListSource::local;
    source.source_reference = played.raw_path;
    source.source_revision = played.source_revision;
    QVERIFY(repository->record_local_listen(source, core::StableId::random(), 1000));
    CatalogueSource catalogues{database};
    SearchDialog dialog(catalogues, EngineKey::local(),
                        [&]() -> std::optional<SearchDialog::TabSnapshot> {
                            return SearchDialog::TabSnapshot{QStringLiteral("Current"),
                                                             {played, unplayed, played}};
                        },
                        {});
    dialog.show();
    dialog.findChild<QComboBox*>(QStringLiteral("bench-search-scope"))->setCurrentIndex(1);
    dialog.findChild<QCheckBox*>(QStringLiteral("bench-search-query-mode"))->setChecked(true);
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("bench-search-input"));
    auto* results = dialog.findChild<QListWidget*>(QStringLiteral("bench-search-results"));
    input->setText(QStringLiteral("HISTORY(playcount) EQUAL 1"));
    QTRY_COMPARE(results->count(), 2);
    input->setText(QStringLiteral("ALL SORT HISTORY(lastplayed)"));
    QTRY_COMPARE(results->count(), 3);
    QVERIFY(results->item(0)->text().contains(QStringLiteral("Unplayed")));
    QVERIFY(results->item(1)->text().contains(QStringLiteral("Played")));
}

void BenchMainWindowTest::metadataGridDisplaysUnicodePaths() {
    const std::vector<std::pair<std::string, QString>> cases{
        {"/music/Sangue Cássia/02-Lótus.flac",
         QStringLiteral("/music/Sangue Cássia/02-Lótus.flac")},
        {"/music/日本語/🎵.flac", QStringLiteral("/music/日本語/🎵.flac")},
        {"/music/Pétalas-" + std::string(1, static_cast<char>(0xFF)) + ".flac",
         QStringLiteral("/music/Pétalas-\\xFF.flac")},
        {"/music/\n\\xFF.flac", QStringLiteral("/music/\\x0A\\\\xFF.flac")},
        {"/music/" + std::string{"\xE2\x82"}, QStringLiteral("/music/\\xE2\\x82")},
        {"/music/" + std::string{"\xC0\xAF"}, QStringLiteral("/music/\\xC0\\xAF")},
        {"/music/\u202E.flac", QStringLiteral("/music/\\xE2\\x80\\xAE.flac")},
    };
    for (const auto& [raw_path, expected] : cases) {
        auto selection = metadata::StagedMetadataSelection::create({metadata::StagedMetadataSource{
            .raw_path = raw_path, .source_revision = std::nullopt, .baseline = {}}});
        QVERIFY(selection.has_value());
        MetadataGridModel grid{std::move(*selection), {}};
        QCOMPARE(grid.data(grid.index(0, 0), Qt::DisplayRole).toString(), expected);
        QCOMPARE(grid.data(grid.index(0, 0), Qt::ToolTipRole).toString(), expected);
        QCOMPARE(grid.trackLabel(0), expected);
    }
}

void BenchMainWindowTest::metadataGridReusesExactNativeFieldWithoutInvalidIndexes() {
    std::vector<metadata::StagedMetadataSource> sources;
    sources.reserve(20U);
    for (std::size_t item = 0U; item < 20U; ++item) {
        metadata::MetadataDocument document;
        document.fields.reserve(39U);
        for (std::size_t field = 0U; field < 39U; ++field) {
            const auto name = field == 38U ? std::string{"TEST"} : "FIELD_" + std::to_string(field);
            document.fields.push_back(metadata::MetadataField{
                .canonical_name = metadata::canonicalize_field_name(name),
                .native_name = name,
                .values = {"value"},
                .qualifier = {},
                .provenance = metadata::FieldProvenance::embedded,
            });
        }
        sources.push_back(metadata::StagedMetadataSource{
            .raw_path = "/music/" + std::to_string(item) + ".flac",
            .source_revision = std::nullopt,
            .baseline = std::move(document),
        });
    }
    auto selection = metadata::StagedMetadataSelection::create(std::move(sources));
    QVERIFY(selection.has_value());
    QCOMPARE(selection->item_count(), std::size_t{20U});
    QCOMPARE(selection->field_count(), std::size_t{39U});

    MetadataGridModel grid{std::move(*selection), {}};
    MetadataAggregateModel aggregate{&grid};
    QTableView view;
    view.setModel(&grid);
    view.setCurrentIndex(grid.index(19, 39));
    QPersistentModelIndex current{view.currentIndex()};
    QVERIFY(current.isValid());

    QSignalSpy about_to_insert{&grid, &QAbstractItemModel::columnsAboutToBeInserted};
    QSignalSpy inserted_columns{&grid, &QAbstractItemModel::columnsInserted};
    const auto inserted = aggregate.ensureField(QStringLiteral("test"));
    QVERIFY(inserted.has_value());
    QCOMPARE(*inserted, 38);
    QCOMPARE(grid.rowCount(), 20);
    QCOMPARE(grid.columnCount(), 40);
    QCOMPARE(aggregate.rowCount(), 39);
    QCOMPARE(about_to_insert.count(), 0);
    QCOMPARE(inserted_columns.count(), 0);
    QVERIFY(current.isValid());
    QCOMPARE(current.row(), 19);
    QCOMPARE(current.column(), 39);
}

void BenchMainWindowTest::ffmpegEncoderIsTheTagTagLibCallsEncoding() {
    // An MP3 converted by FFmpeg: TagLib reads its TSSE frame as ENCODING,
    // FFmpeg reports the same tag as "encoder". It is one tag, and must not
    // come back as a second, stream-only field that no edit can touch.
    metadata::MetadataDocument document;
    document.fields.push_back(
        metadata::MetadataField{.canonical_name = "encoding",
                                .native_name = "ENCODING",
                                .values = {"Lavf61.5.101"},
                                .qualifier = {},
                                .provenance = metadata::FieldProvenance::embedded});
    document.fields.push_back(
        metadata::MetadataField{.canonical_name = "encoder",
                                .native_name = "encoder",
                                .values = {"Lavf61.5.101", "Lavc61.11"},
                                .qualifier = {},
                                .provenance = metadata::FieldProvenance::stream});
    remove_shadowed_probed_metadata(document);
    QCOMPARE(document.fields.size(), std::size_t{1});
    QCOMPARE(document.fields.front().canonical_name, std::string{"encoding"});
    QCOMPARE(probed_semantic_alias("ENCODER"), std::optional<std::string_view>{"encoding"});
}

void BenchMainWindowTest::recentlyAddedComesFirstWhereAskedFor() {
    QSettings{}.remove(QStringLiteral("library/newest-first"));
    QSettings{}.remove(QStringLiteral("library/view"));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/old")));
    // One album there from the start, dated by its file: years ago.
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/old/old.flac")));
    {
        QFile old{music + QStringLiteral("/old/old.flac")};
        QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(QDateTime::fromSecsSinceEpoch(1'000'000'000),
                                QFileDevice::FileModificationTime));
    }
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto catalogue = window.localCatalogue()->open();
    QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
    persistence::LibraryScanProgress progress;
    QVERIFY(catalogue->scan({}, progress).has_value());
    // And one that arrives later.
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/new")));
    write_wave(music + QStringLiteral("/new/new.wav"), wave_sample_rate);
    QVERIFY(catalogue->scan({}, progress).has_value());

    // The quick album popup, before anything is typed: newest first.
    window.findChild<QAction*>(QStringLiteral("action-quick-album"))->trigger();
    QuickPickPopup* popup = nullptr;
    for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
        if (candidate->isVisible()) {
            popup = candidate;
        }
    }
    QVERIFY(popup != nullptr);
    QTRY_COMPARE(popup->results()->count(), 2);
    QVERIFY2(popup->results()->item(0)->text().contains(QStringLiteral("added today")),
             qPrintable(popup->results()->item(0)->text()));
    QVERIFY(popup->results()->item(1)->text().contains(QStringLiteral("years ago")));
    QVERIFY(popup->findChild<QLabel*>(QStringLiteral("bench-quick-album-status"))
                ->text()
                .startsWith(QStringLiteral("Recently added")));
    QTest::keyClick(popup->input(), Qt::Key_Escape);

    // The library panel's Recently added -- a view (ADR-0254): albums, the
    // new one first.
    window.localLibrary()->refreshLibrary();
    auto* views =
        window.localLibrary()->findChild<QComboBox*>(QStringLiteral("local-library-view"));
    QVERIFY(views != nullptr);
    const auto choose = [views](const QString& id) {
        const auto index = views->findData(id);
        QVERIFY(index >= 0);
        views->setCurrentIndex(index);
        emit views->activated(index);
    };
    choose(QStringLiteral("recently-added"));
    auto* tree = window.localLibrary()->findChild<QTreeView*>();
    QTRY_COMPARE(tree->model()->rowCount(), 2);
    const auto first = tree->model()->index(0, 0).data(library_entry_role);
    QVERIFY(first.isValid());
    QCOMPARE(first.value<persistence::LibraryEntry>().kind, persistence::LibraryEntryKind::album);
    QVERIFY(first.value<persistence::LibraryEntry>().added > tree->model()
                                                                 ->index(1, 0)
                                                                 .data(library_entry_role)
                                                                 .value<persistence::LibraryEntry>()
                                                                 .added);
    choose(QStringLiteral("artist-album"));
    QTRY_VERIFY(tree->model()->rowCount() >= 1 &&
                tree->model()
                        ->index(0, 0)
                        .data(library_entry_role)
                        .value<persistence::LibraryEntry>()
                        .kind == persistence::LibraryEntryKind::artist);
    QSettings{}.remove(QStringLiteral("library/newest-first"));
    QSettings{}.remove(QStringLiteral("library/view"));
}

void BenchMainWindowTest::settingsListEnginesFoundOnTheNetwork() {
    // An engine announcing itself -- under this test's own service name, set
    // by the test environment, so nothing real is listed or disturbed.
    auto announcer = discovery::Announcer::start(discovery::Advertisement{
        .instance = "lounge", .port = 6603, .txt = {{"id", "lounge-id"}, {"auth", "1"}}});
    if (!announcer) {
        QSKIP("no multicast DNS here");
    }
    BenchMainWindow window;
    window.show();
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::engine);
    auto* menu = dialog->findChild<QMenu*>(QStringLiteral("bench-settings-found-engines-menu"));
    QVERIFY(menu != nullptr);
    const auto lounge = [menu]() -> QAction* {
        for (auto* action : menu->actions()) {
            if (action->text().startsWith(QStringLiteral("lounge — "))) {
                return action;
            }
        }
        return nullptr;
    };
    QTRY_VERIFY_WITH_TIMEOUT(lounge() != nullptr, 8'000);
    QVERIFY(lounge()->text().contains(QStringLiteral(":6603")));
    QVERIFY(lounge()->text().endsWith(QStringLiteral("· password")));
    // Chosen, not typed: its address fills the field.
    lounge()->trigger();
    auto* field = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-socket"));
    QVERIFY(field->text().endsWith(QStringLiteral(":6603")));
    dialog->reject();
}

void BenchMainWindowTest::quickTrackFindsATrackByItsTitle() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    {
        auto catalogue = window.localCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr && EngineKey::of(tab->document).isLocal());
    tab->model->replaceRows({}, true);
    auto* action = window.findChild<QAction*>(QStringLiteral("action-quick-track"));
    QVERIFY(action != nullptr);
    QCOMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    action->trigger();
    QuickPickPopup* popup = nullptr;
    for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
        if (candidate->isVisible() &&
            candidate->objectName() == QStringLiteral("bench-quick-track")) {
            popup = candidate;
        }
    }
    QVERIFY(popup != nullptr);
    // Words of the title and the artist, in any order and case.
    QTest::keyClicks(popup->input(), QStringLiteral("TONE trackknife"));
    QTRY_COMPARE(popup->results()->count(), 1);
    QVERIFY(popup->results()->item(0)->text().startsWith(QStringLiteral("Fixture Tone — ")));
    QTest::keyClick(popup->input(), Qt::Key_Return);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QCOMPARE(tab->model->rows().front().title, std::string{"Fixture Tone"});
}

void BenchMainWindowTest::quickAlbumShiftEnterReplacesAndPlays() {
    MELODY_REQUIRE_REALTIME_AUDIO();
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    write_wave(music + QStringLiteral("/long.wav"), wave_sample_rate * 60U);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.playingOnEngine());
    {
        auto catalogue = window.localCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr && EngineKey::of(tab->document).isLocal());
    window.findChild<QAction*>(QStringLiteral("action-quick-album"))->trigger();
    QuickPickPopup* popup = nullptr;
    for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
        if (candidate->isVisible()) {
            popup = candidate;
        }
    }
    QVERIFY(popup != nullptr);
    QTest::keyClicks(popup->input(), QStringLiteral("unknown"));
    QTRY_COMPARE(popup->results()->count(), 1);
    QTest::keyClick(popup->input(), Qt::Key_Return, Qt::ShiftModifier);
    // Return puts the popup away before the click's Shift release.
    releaseHeldModifiers(&window);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(window.localPlayback()->state().status, QStringLiteral("playing"),
                              10'000);
    window.localPlayback()->stop();
}

void BenchMainWindowTest::quickAlbumFindsByWordsAndPutsItAway() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    persistence::LibraryEntry album;
    {
        auto catalogue = window.localCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
        persistence::LibraryQuery albums;
        albums.kind = persistence::LibraryEntryKind::album;
        const auto page = catalogue->query(albums);
        QVERIFY(page && page->entries.size() == 1U && !page->entries.front().date.empty());
        album = page->entries.front();
    }
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr && EngineKey::of(tab->document).isLocal());
    tab->model->replaceRows({}, true);
    // A word of the album and its year, in any case.
    const auto words =
        QString::fromStdString(album.album).section(QLatin1Char(' '), 0, 0).toUpper() +
        QStringLiteral(" ") + QString::fromStdString(album.date);

    const auto open = [&window]() -> QuickPickPopup* {
        window.findChild<QAction*>(QStringLiteral("action-quick-album"))->trigger();
        QuickPickPopup* popup = nullptr;
        for (auto* candidate : window.findChildren<QuickPickPopup*>()) {
            if (candidate->isVisible()) {
                popup = candidate;
            }
        }
        return popup;
    };
    auto* popup = open();
    QVERIFY(popup != nullptr);
    QCOMPARE(window.findChild<QAction*>(QStringLiteral("action-quick-album"))->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    QCOMPARE(popup->findChild<QLabel*>(QStringLiteral("bench-quick-album-scope"))->text(),
             QStringLiteral("This computer"));
    QTest::keyClicks(popup->input(), words);
    QTRY_COMPARE(popup->results()->count(), 1);
    // Enter: added to the list in front.
    const QPointer<QuickPickPopup> chosen{popup};
    QTest::keyClick(popup->input(), Qt::Key_Return);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    // Done once chosen: it closes itself.
    QTRY_VERIFY(chosen.isNull() || !chosen->isVisible());

    // Ctrl+Shift+Enter: onto the end of Up Next instead.
    const auto waiting = window.playback_.requests.pending().size();
    popup = open();
    QVERIFY(popup != nullptr);
    QTest::keyClicks(popup->input(), words);
    QTRY_COMPARE(popup->results()->count(), 1);
    QTest::keyClick(popup->input(), Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(window.playback_.requests.pending().size(), waiting + 1U);
    QCOMPARE(tab->model->rowCount(), 1);

    // Another year finds nothing, and says so.
    popup = open();
    QVERIFY(popup != nullptr);
    QTest::keyClicks(popup->input(),
                     QString::fromStdString(album.album).section(QLatin1Char(' '), 0, 0) +
                         QStringLiteral(" 1066"));
    QTRY_COMPARE(popup->findChild<QLabel*>(QStringLiteral("bench-quick-album-status"))->text(),
                 QStringLiteral("No albums match."));
    QTest::keyClick(popup->input(), Qt::Key_Escape);
}

void BenchMainWindowTest::libraryDragsIntoUpNextWithCovers() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      music + QStringLiteral("/art.flac")));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_ && window.up_next_restored_);
    {
        auto catalogue = window.localCatalogue()->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
    }
    window.localLibrary()->refreshLibrary();
    auto* tree = window.localLibrary()->findChild<QTreeView*>();
    QVERIFY(tree != nullptr);
    QTRY_VERIFY(tree->model()->rowCount() > 0 && tree->model()
                                                     ->index(0, 0)
                                                     .data(Qt::DisplayRole)
                                                     .toString()
                                                     .contains(QStringLiteral("Trackknife")));

    // Dragged from the library onto Up Next: queued with its tags.
    window.findChild<QAction*>(QStringLiteral("action-show-up-next"))->trigger();
    QTRY_VERIFY(window.up_next_view_->isVisible());
    std::unique_ptr<QMimeData> mime{tree->model()->mimeData({tree->model()->index(0, 0)})};
    QVERIFY(mime != nullptr);
    auto* viewport = window.up_next_view_->viewport();
    QDragEnterEvent enter{dragPosition(QPoint{10, 10}), Qt::CopyAction, mime.get(), Qt::LeftButton,
                          Qt::NoModifier};
    QApplication::sendEvent(viewport, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop{QPointF{10, 10}, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_COMPARE(window.playback_.requests.pending().size(), std::size_t{1});
    const auto queued = window.playback_.requests.pending().front().source;
    QCOMPARE(queued.title, std::string{"Fixture Tone"});

    // And onto the header's Up Next button: queued at the end.
    {
        auto* button = window.up_next_button_;
        QDragEnterEvent over{dragPosition(QPoint{5, 5}), Qt::CopyAction, mime.get(), Qt::LeftButton,
                             Qt::NoModifier};
        QApplication::sendEvent(button, &over);
        QVERIFY(over.isAccepted());
        QDropEvent onto{QPointF{5, 5}, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
        QApplication::sendEvent(button, &onto);
        QVERIFY(onto.isAccepted());
        QTRY_COMPARE(window.playback_.requests.pending().size(), std::size_t{2});
    }

    // No tab holds its album, yet Up Next and the header find its cover.
    const auto key = LocalListModel::groupKeyOf(queued);
    for (const auto& tab : window.list_tabs_) {
        tab->model->setArtwork(key, {});
    }
    window.artwork_cache_.remove(key);
    window.artwork_pending_.remove(key);
    window.up_next_local_model_->setArtwork(key, {});
    window.refreshUpNext();
    QTRY_VERIFY(window.up_next_local_model_->hasArtwork(key));
    window.refreshHeaderCover(QString::fromStdString(queued.entry_id.to_string()));
    QTRY_COMPARE(window.header_cover_key_, key);
}

void BenchMainWindowTest::headerShowsThePlayingAlbumsCover() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"),
                                      media.filePath(QStringLiteral("art.flac"))));
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    window.openLocalPaths({QFile::encodeName(media.path()).toStdString()});
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    QTRY_COMPARE(tab->model->rowCount(), 1);
    // The row's album as its tags say, once they are read: taken before, it
    // was the key of a row with no tags yet, which no cover ever comes for.
    QTRY_VERIFY(tab->model->hasArtwork(tab->model->groupKey(0)));
    const auto group = tab->model->groupKey(0);

    // Nothing playing: a placeholder tile the size of a cover.
    window.refreshHeaderCover({});
    QVERIFY(window.header_cover_key_.isEmpty());
    QVERIFY(!window.now_playing_cover_->pixmap().isNull());

    // Playing: the cover of that entry's album, from the tab that has it.
    window.refreshHeaderCover(
        QString::fromStdString(tab->model->rows().front().entry_id.to_string()));
    QCOMPARE(window.header_cover_key_, group);
    QCOMPARE(window.now_playing_cover_->pixmap().deviceIndependentSize().toSize(), QSize(44, 44));
}

// The desktop -- notifications, media keys -- names what plays as the header
// does, by finding its entry in any open list: after a restart the list
// played from is not known until something plays, and it said only the file
// name.
void BenchMainWindowTest::theDesktopNamesWhatPlaysAfterARestart() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    LocalTrackRow row;
    row.raw_path = "/music/Someone/Album/01 file name.flac";
    row.title = "The Title";
    row.artist = "Someone";
    row.album = "Album";
    tab->model->appendRows({row});
    const auto entry = QString::fromStdString(tab->model->rows().front().entry_id.to_string());
    // Restarted: playing, but from no list this window knows of.
    window.workspace_.playback_.anchors = {};
    EnginePlayback::State engine;
    engine.status = QStringLiteral("playing");
    engine.entry = entry;
    engine.path = QStringLiteral("/music/Someone/Album/01 file name.flac");
    const auto state = window.workspace_.desktopStateOf(engine);
    QCOMPARE(state.title, QStringLiteral("The Title"));
    QCOMPARE(state.artist, QStringLiteral("Someone"));
    QCOMPARE(state.album, QStringLiteral("Album"));
    QCOMPARE(state.track_key, entry);
}

// The row of what plays is looked for where it was last found, several
// times a refresh: wherever the list has moved it since, or edited it, it is
// that entry's row -- and once it has left the list, none.
void BenchMainWindowTest::thePlayingRowFollowsItsList() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab != nullptr);
    std::vector<LocalTrackRow> rows;
    for (const auto* title : {"One", "Two", "Three"}) {
        LocalTrackRow row;
        row.raw_path = std::string{"/music/"} + title + ".flac";
        row.title = title;
        rows.push_back(std::move(row));
    }
    tab->model->appendRows(std::move(rows));
    QCOMPARE(tab->model->rows().size(), 3U);
    auto& workspace = window.workspace_;
    workspace.playback_.anchors.document = tab->document.id;
    const auto playing = tab->model->rows()[2].entry_id;
    const auto entry = QString::fromStdString(playing.to_string());
    const auto* found = workspace.playingRow(entry);
    QVERIFY(found != nullptr);
    QCOMPARE(found->title, std::string{"Three"});
    QCOMPARE(workspace.playingRow(entry), found);

    // Moved to the top: found there, not at the row it was.
    QVERIFY(tab->model->applyPermutation({2, 0, 1}, QStringLiteral("Move")));
    found = workspace.playingRow(entry);
    QVERIFY(found != nullptr);
    QCOMPARE(found->entry_id, playing);
    QCOMPARE(found, &tab->model->rows()[0]);
    QCOMPARE(workspace.playingRow(entry), found);

    // Another entry now where it was found: not taken for it.
    QVERIFY(tab->model->applyPermutation({1, 2, 0}, QStringLiteral("Move")));
    found = workspace.playingRow(entry);
    QVERIFY(found != nullptr);
    QCOMPARE(found->entry_id, playing);
    QCOMPARE(found->title, std::string{"Three"});

    // Gone from every list: none.
    const auto at = static_cast<int>(found - tab->model->rows().data());
    tab->model->removeRowIndexes({at});
    QCOMPARE(workspace.playingRow(entry), nullptr);
}

void BenchMainWindowTest::transportIsOneRowWithCoverAndPills() {
    BenchMainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);

    auto* now_playing = window.findChild<QLabel*>(QStringLiteral("bench-now-playing"));
    auto* now_playing_context =
        window.findChild<QLabel*>(QStringLiteral("bench-now-playing-context"));
    auto* seek = window.findChild<QSlider*>(QStringLiteral("bench-seek"));
    auto* volume = window.findChild<QSlider*>(QStringLiteral("bench-volume"));
    auto* device = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    auto* header = window.findChild<QWidget*>(QStringLiteral("bench-player-header"));
    auto* transport = window.findChild<QWidget*>(QStringLiteral("bench-transport-buttons"));

    QVERIFY(now_playing != nullptr);
    QVERIFY(now_playing_context != nullptr);
    QVERIFY(seek != nullptr);
    QVERIFY(volume != nullptr);
    QVERIFY(device != nullptr);
    QVERIFY(header != nullptr);
    QVERIFY(transport != nullptr);
    QVERIFY(window.findChild<QComboBox*>(QStringLiteral("bench-device")) == nullptr);
    QCOMPARE(now_playing->accessibleName(), QStringLiteral("Current artist and title"));
    QCOMPARE(now_playing_context->accessibleName(), QStringLiteral("Current album and date"));
    QCOMPARE(seek->accessibleName(), QStringLiteral("Playback position"));
    QVERIFY(dynamic_cast<ui::LineSlider*>(seek) != nullptr);
    QVERIFY(dynamic_cast<ui::LineSlider*>(volume) != nullptr);
    QCOMPARE(device->accessibleName(), QStringLiteral("Audio output device"));
    // The regular menu bar is the application menu — no hamburger button.
    QVERIFY(window.findChild<QToolButton*>(QStringLiteral("bench-main-menu")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-backup-workspace")) != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-restore-workspace")) != nullptr);
    QVERIFY(!window.menuBar()->isHidden());
    // File, Edit, Workspace, Playback, and Help with the language references.
    QCOMPARE(window.menuBar()->actions().size(), 5);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-reference-tkfmt")) != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-reference-scripts")) != nullptr);
    QCOMPARE(device->toolButtonStyle(), Qt::ToolButtonIconOnly);
    // Nothing to name, no chevron squeezed beside the icon.
    QVERIFY(device->findChild<QLabel*>(QStringLiteral("bench-device-chevron"))->isHidden());
    QVERIFY(!device->icon().isNull());
    QVERIFY(device->menu() != nullptr);
    QCOMPARE(device->menu()->objectName(), QStringLiteral("bench-device-menu"));
    QVERIFY(!device->menu()->actions().empty());
    QCOMPARE(device->menu()->actions().front()->text(), QStringLiteral("System default"));
    QVERIFY(window.property("trackknife-player-output-available").isValid());
    QVERIFY(window.property("trackknife-player-output-suspended").isValid());
    QVERIFY(window.property("trackknife-player-default-output").isValid());
    // Previous, play and next; stop lives in the Playback menu.
    QCOMPARE(transport->findChildren<QToolButton*>(QString{}, Qt::FindDirectChildrenOnly).size(),
             3);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-show-up-next")) != nullptr);

    // One row: cover, then what is playing, then the controls and position,
    // then where it plays and what waits -- all on one line.
    auto* cover = window.findChild<QLabel*>(QStringLiteral("bench-now-playing-cover"));
    auto* up_next = window.findChild<QToolButton*>(QStringLiteral("action-up-next"));
    auto* count = window.findChild<QLabel*>(QStringLiteral("bench-up-next-count"));
    QVERIFY(cover != nullptr && up_next != nullptr && count != nullptr);
    const auto at = [&window](const QWidget* widget) {
        return QRect{widget->mapTo(&window, QPoint{}), widget->size()};
    };
    QTRY_VERIFY(!cover->pixmap().isNull());
    QVERIFY(at(now_playing).bottom() <= at(now_playing_context).top());
    QVERIFY(at(cover).right() < at(now_playing).left());
    QVERIFY(at(now_playing).right() < at(transport).left());
    QVERIFY(at(transport).right() < at(seek).left());
    QVERIFY(at(seek).right() < at(volume).left());
    QVERIFY(at(volume).right() < at(device).left());
    QVERIFY(at(device).right() < at(up_next).left());
    for (const auto* widget :
         std::initializer_list<const QWidget*>{cover, transport, volume, device, up_next}) {
        QVERIFY(std::abs(at(widget).center().y() - at(seek).center().y()) <= 1);
    }
    QVERIFY(header->height() <= 64);
    // The count shows only when something waits.
    QVERIFY(count->isHidden());
    QCOMPARE(up_next->accessibleName(), QStringLiteral("Up Next"));
}

void BenchMainWindowTest::shortcutSettingsValidateSaveAndCancel() {
    QSettings{}.remove(QStringLiteral("shortcuts"));
    BenchMainWindow window;
    window.show();
    auto* jump = window.findChild<QAction*>(QStringLiteral("action-jump-to-playing"));
    QVERIFY(jump);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::shortcuts);
    auto* edit =
        dialog->findChild<QKeySequenceEdit*>(QStringLiteral("shortcut-action-jump-to-playing"));
    QVERIFY(edit);
    edit->setKeySequence(QKeySequence(QStringLiteral("Ctrl+O")));
    auto* save = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
                     ->button(QDialogButtonBox::Save);
    save->click();
    QVERIFY(dialog->isVisible());
    QVERIFY(!dialog->findChild<QLabel*>(QStringLiteral("shortcut-conflict"))->text().isEmpty());
    QCOMPARE(jump->shortcut(), QKeySequence(QStringLiteral("Ctrl+J")));
    edit->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+J")));
    save->click();
    QCOMPARE(jump->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+J")));
    QCOMPARE(QSettings{}.value(QStringLiteral("shortcuts/action-jump-to-playing")).toString(),
             QStringLiteral("Ctrl+Alt+J"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.showSettingsDialog(SettingsDialog::Page::shortcuts);
    dialog->findChild<QPushButton*>(QStringLiteral("shortcut-restore-defaults"))->click();
    dialog->reject();
    QCOMPARE(jump->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+Alt+J"))); // Cancel keeps live binding.
    {
        BenchMainWindow reopened;
        QCOMPARE(reopened.findChild<QAction*>(QStringLiteral("action-jump-to-playing"))->shortcut(),
                 QKeySequence(QStringLiteral("Ctrl+Alt+J")));
    }
    QSettings{}.remove(QStringLiteral("shortcuts"));
}

// Ctrl+L was documented, and refused to anything else as "reserved for
// library search", but nothing was bound to it. And only actions with a key
// by default could be given one: Convert, ReplayGain and the rest could not.
void BenchMainWindowTest::everyCommandTakesAKeyAndCtrlLSearchesTheLibrary() {
    QSettings{}.remove(QStringLiteral("shortcuts"));
    BenchMainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QTRY_VERIFY(window.lists_restored_);

    // From Playlists, which has no search, to the library's.
    window.local_source_tabs_->setCurrentIndex(window.playlists_tab_);
    auto* search = window.findChild<QAction*>(QStringLiteral("action-focus-library-search"));
    QVERIFY(search != nullptr);
    QCOMPARE(search->shortcut(), QKeySequence(QStringLiteral("Ctrl+L")));
    search->trigger();
    QTRY_VERIFY(QApplication::focusWidget() != nullptr);
    QCOMPARE(QApplication::focusWidget()->objectName(), QStringLiteral("local-library-search"));
    QVERIFY(window.local_source_tabs_->currentIndex() != window.playlists_tab_);

    auto* convert = window.findChild<QAction*>(QStringLiteral("action-convert-files"));
    QVERIFY(convert != nullptr && convert->shortcut().isEmpty());
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::shortcuts);
    auto* edit =
        dialog->findChild<QKeySequenceEdit*>(QStringLiteral("shortcut-action-convert-files"));
    QVERIFY(edit != nullptr);
    edit->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+C")));
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QCOMPARE(convert->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+C")));
    QSettings{}.remove(QStringLiteral("shortcuts"));
}

// Quit stops this computer's engine -- and it stays stopped. The window's
// engine connection starts its engine again when it goes away, every few
// seconds, and the event loop ran on for a moment after Quit: the engine
// came back, and the music with it.
void BenchMainWindowTest::quittingStopsTheEngineForGood() {
    // The window's own engine, as the application starts it, in place of
    // the one every other case runs against: same data directory, a socket
    // of its own. cleanup() clears the settings again.
    engine_.stop();
    QSettings settings;
    settings.remove(QLatin1String(SettingsDialog::library_local_engine_socket_key));
    settings.remove(QLatin1String(SettingsDialog::library_engine_socket_key));
    settings.sync();
    const auto program = qgetenv("MELODY_ENGINE");
    qputenv("MELODY_ENGINE", MELODY_ENGINE_BINARY);
    allowLocalEngine(true);
    const auto engine = localEngine();
    QVERIFY(engine.has_value());
    const auto restore = qScopeGuard([&] {
        static_cast<void>(stopLocalEngine(*engine));
        allowLocalEngine(false);
        qputenv("MELODY_ENGINE", program);
    });

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY2_WITH_TIMEOUT(window.localPlayback() != nullptr && window.localPlayback()->active(),
                              qPrintable(window.localCatalogue()
                                             ? window.localCatalogue()->describe()
                                             : QStringLiteral("no source")),
                              15'000);
    QVERIFY(!lockHolders(engine->state / "engine.lock").empty());

    window.quitAndStopEngine();
    QVERIFY(lockHolders(engine->state / "engine.lock").empty());
    // The loop turns on past the reconnect timer's next tick, as it does
    // while the process ends.
    QTest::qWait(5'000);
    QVERIFY2(lockHolders(engine->state / "engine.lock").empty(),
             "quitting must not leave anything that starts the engine again");
}

// ADR-0286: how this computer's engine is shared was kept in Settings and
// given as flags at every start; it is the engine's melodyd.conf now, handed
// over the first time the window meets the engine, and not again.
void BenchMainWindowTest::sharingSettingsAreHandedToTheEngineOnce() {
    engine_.stop();
    QSettings settings;
    settings.remove(QLatin1String(SettingsDialog::library_local_engine_socket_key));
    settings.remove(QLatin1String(SettingsDialog::library_engine_socket_key));
    settings.remove(QLatin1String(SettingsDialog::engine_settings_in_config_key));
    settings.setValue(QLatin1String(SettingsDialog::engine_music_root_key),
                      QStringLiteral("/srv/handed-over"));
    settings.sync();
    const auto program = qgetenv("MELODY_ENGINE");
    qputenv("MELODY_ENGINE", MELODY_ENGINE_BINARY);
    allowLocalEngine(true);
    const auto engine = localEngine();
    QVERIFY(engine.has_value());
    // A test's folder: started here, with its own melodyd.conf.
    QVERIFY(!engine->standard);
    const auto restore = qScopeGuard([&] {
        static_cast<void>(stopLocalEngine(*engine));
        allowLocalEngine(false);
        qputenv("MELODY_ENGINE", program);
        QFile::remove(QString::fromStdString((engine->state / "melodyd.conf").string()));
    });
    const auto config = [&engine] {
        QFile file{QString::fromStdString((engine->state / "melodyd.conf").string())};
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
    };
    pid_t handed_to = 0;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY_WITH_TIMEOUT(
            settings.value(QLatin1String(SettingsDialog::engine_settings_in_config_key), false)
                .toBool(),
            20'000);
        QVERIFY(config().contains(QStringLiteral("\nmusic-root = /srv/handed-over\n")));
        // Restarted to use it, and answering again.
        QTRY_VERIFY_WITH_TIMEOUT(
            window.localPlayback() != nullptr && window.localPlayback()->active(), 15'000);
        const auto holders = lockHolders(engine->state / "engine.lock");
        QCOMPARE(holders.size(), std::size_t{1U});
        handed_to = holders.front();
    }
    // Once: the next window finds it handed over, and leaves the engine be.
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
        QTest::qWait(1'000);
        const auto holders = lockHolders(engine->state / "engine.lock");
        QVERIFY(holders.size() == 1U && holders.front() == handed_to);
    }
    settings.remove(QLatin1String(SettingsDialog::engine_music_root_key));
}

// ADR-0286: a tab is the window's; only a saved playlist is on its engine,
// for the phone and the CLI to see. A tab that is none never reaches it; one
// saved is written with the tab's own id, and not while it has unsaved edits.
void BenchMainWindowTest::theWindowsListsAreOnItsEngine() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto listed = [&engine](const std::string& id) -> std::optional<protocol::Json> {
        auto all = (*engine)->call("list.all");
        if (!all) {
            return std::nullopt;
        }
        for (const auto& list : all->value("lists", protocol::Json::array())) {
            if (list.value("id", std::string{}) == id) {
                return list;
            }
        }
        return std::nullopt;
    };
    const auto rows = [](const int count) {
        std::vector<LocalTrackRow> made;
        for (int index = 0; index < count; ++index) {
            LocalTrackRow row;
            row.raw_path = "/music/list/" + std::to_string(index) + ".flac";
            row.title = "Track " + std::to_string(index);
            made.push_back(std::move(row));
        }
        return made;
    };

    // The window's own first tab: its own, not the engine's.
    QVERIFY(!window.list_tabs_.empty());
    const auto first = window.list_tabs_.front()->document.id.to_string();
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(first).has_value());

    auto* tab = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                            .kind = persistence::ListKind::scratch,
                                                            .name = "Party",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {}},
                                  true);
    const auto id = tab->document.id.to_string();
    tab->model->appendRows(rows(3));
    window.markTabDirty(*tab);
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id).has_value());

    // Saved: a saved playlist of the tab's own id, with its entries.
    tab->document.kind = persistence::ListKind::saved;
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_VERIFY(listed(id).has_value() && listed(id)->value("tracks", 0) == 3);
    QCOMPARE(listed(id)->value("name", std::string{}), std::string{"Party"});
    QCOMPARE(listed(id)->value("kind", std::string{}), std::string{"saved"});
    auto items = (*engine)->call("list.get", protocol::Json{{"id", id}});
    QVERIFY(items.has_value());
    QCOMPARE(items->at("items").at(1).value("title", std::string{}), std::string{"Track 1"});
    QCOMPARE(items->at("items").at(0).value("entry", std::string{}),
             tab->model->rows().front().entry_id.to_string());

    // Edited, unsaved: not written.
    tab->model->appendRows(rows(1));
    tab->document.dirty = true;
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QCOMPARE(listed(id)->value("tracks", 0), 3);

    // A tab that is no saved playlist, filled and closed: never on the
    // engine.
    auto* scratch =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Scratch",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    const auto scratch_id = scratch->document.id.to_string();
    scratch->model->appendRows(rows(1));
    window.persistNow(false);
    QTest::qWait(300);
    QVERIFY(!listed(scratch_id).has_value());
    window.closeTabAt(window.tabs_->indexOf(scratch->view));
    window.persistNow(false);
    QVERIFY(!listed(scratch_id).has_value());
    // Saved again -- the edits go too -- and then closed, with nothing to ask
    // about: the list stays on the engine.
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_COMPARE(listed(id)->value("tracks", 0), 4);
    window.closeTabAt(window.tabs_->indexOf(tab->view));
    QVERIFY(window.tabForDocument(QString::fromStdString(id)) == nullptr);
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(listed(id).has_value());
    (*engine)->close();
}

// ADR-0256: a list is sent whole once, in batches the engine takes, and
// then as edits; a reconnect sends nothing; playing names the list.
void BenchMainWindowTest::listsTravelAsEdits() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto summary = [&engine](const std::string& id) -> protocol::Json {
        auto all = (*engine)->call("list.all");
        for (const auto& list :
             all ? all->value("lists", protocol::Json::array()) : protocol::Json::array()) {
            if (list.value("id", std::string{}) == id) {
                return list;
            }
        }
        return protocol::Json::object();
    };
    const auto entries = [&engine](const std::string& id) {
        std::vector<std::string> found;
        auto got = (*engine)->call("list.get", protocol::Json{{"id", id}});
        for (const auto& item :
             got ? got->value("items", protocol::Json::array()) : protocol::Json::array()) {
            found.push_back(item.value("entry", std::string{}));
        }
        return found;
    };
    const auto shown = [](const LocalListModel& model) {
        std::vector<std::string> found;
        for (const auto& row : model.rows()) {
            found.push_back(row.entry_id.to_string());
        }
        return found;
    };

    // 8,000 tracks, over two megabytes of list: more than the engine takes
    // in one line, so it arrives in batches. A saved playlist: what is on an
    // engine (ADR-0286).
    auto* tab = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                            .kind = persistence::ListKind::saved,
                                                            .name = "Long",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {}},
                                  true);
    const auto id = tab->document.id.to_string();
    std::vector<LocalTrackRow> made;
    for (int index = 0; index < 8'000; ++index) {
        LocalTrackRow row;
        row.raw_path = "/music/a rather long folder name/an album of some length/" +
                       std::to_string(index) + " - a title of usual length.flac";
        row.title = "A title of usual length " + std::to_string(index);
        row.artist = "Someone";
        row.album = "An album of some length";
        row.album_artist = "Someone";
        row.date = "1999";
        row.duration_ms = 200'000;
        made.push_back(std::move(row));
    }
    tab->model->appendRows(std::move(made));
    window.markTabDirty(*tab);
    // Saved.
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_VERIFY_WITH_TIMEOUT(summary(id).value("tracks", 0) == 8'000 && !window.list_sync_->busy(),
                             30'000);
    QCOMPARE(entries(id), shown(*tab->model));
    const auto made_at = summary(id).value("revision", std::uint64_t{0});

    // Three rows dragged and saved: one edit, the list as shown.
    auto rows = tab->model->rows();
    std::rotate(rows.begin() + 5, rows.begin() + 8, rows.begin() + 101);
    tab->model->replaceRows(std::move(rows));
    window.markTabDirty(*tab);
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_VERIFY(summary(id).value("revision", std::uint64_t{0}) == made_at + 1U &&
                !window.list_sync_->busy());
    QCOMPARE(entries(id), shown(*tab->model));

    // Reconnecting compares; nothing is written.
    window.list_sync_->reconnected(window.localPlayback());
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QCOMPARE(summary(id).value("revision", std::uint64_t{0}), made_at + 1U);

    // Playing names the list: the engine's queue is its copy, entry for entry.
    window.playRow(*tab, 10);
    QTRY_VERIFY_WITH_TIMEOUT(window.localPlayback()->state().queue_size == 8'000U, 10'000);
    auto queue = (*engine)->call("playback.queue");
    QVERIFY(queue.has_value());
    const auto& held = queue->at("entries");
    QCOMPARE(held.size(), std::size_t{8'000});
    QCOMPARE(held.at(10).value("entry", std::string{}),
             tab->model->rows()[10].entry_id.to_string());
    QCOMPARE(held.at(10).at("group").value("album_artist", std::string{}), std::string{"Someone"});
    QCOMPARE(summary(id).value("revision", std::uint64_t{0}), made_at + 1U);

    // A row taken from the playing list leaves the engine's queue as well;
    // the playlist keeps it until Save.
    tab->model->removeRowIndexes({0});
    window.markTabDirty(*tab);
    QTRY_VERIFY(window.localPlayback()->state().queue_size == 7'999U);
    QTRY_VERIFY(!window.list_sync_->busy());
    QTest::qWait(300);
    QCOMPARE(tab->model->rowCount(), 7'999);
    QCOMPARE(entries(id).size(), std::size_t{8'000});

    // ADR-0286: a tab that is no saved playlist plays as long as it is: its
    // queue sent in parts, each within a line, and never a list.
    auto* scratch =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Long scratch",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    std::vector<LocalTrackRow> copied = tab->model->rows();
    for (auto& row : copied) {
        row.entry_id = core::StableId::random();
    }
    scratch->model->appendRows(std::move(copied));
    window.markTabDirty(*scratch);
    window.persistNow(false);
    window.playRow(*scratch, 7'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        window.localPlayback()->state().queue_size == 7'999U &&
            window.localPlayback()->state().entry ==
                QString::fromStdString(scratch->model->rows()[7'000].entry_id.to_string()),
        10'000);
    queue = (*engine)->call("playback.queue");
    QVERIFY(queue.has_value());
    QCOMPARE(queue->at("entries").size(), std::size_t{7'999});
    QCOMPARE(queue->at("entries").at(7'998).value("entry", std::string{}),
             scratch->model->rows().back().entry_id.to_string());
    QCOMPARE(queue->at("entries").at(5'000).at("group").value("album", std::string{}),
             std::string{"An album of some length"});
    QVERIFY(summary(scratch->document.id.to_string()).empty());
    QVERIFY2(!engine_.log().contains("refused a"), "nothing the window sent was too long");
    (*engine)->close();
}

// ADR-0233: what another client does to a list open here reaches the tab: a
// change is shown, a deletion closes it. A saved list someone else saved
// since this window read it is not overwritten on Save: the choice is asked.
void BenchMainWindowTest::anotherClientsListChangesReachTheWindow() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto revision = [&other](const std::string& id) {
        auto got = (*other)->call("list.get", protocol::Json{{"id", id}});
        return got ? got->value("revision", std::uint64_t{0}) : std::uint64_t{0};
    };

    // A saved playlist: what other clients see (ADR-0286).
    auto* tab = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                            .kind = persistence::ListKind::saved,
                                                            .name = "Shared",
                                                            .pinned = false,
                                                            .dirty = false,
                                                            .items = {}},
                                  true);
    const auto id = tab->document.id.to_string();
    const auto qid = QString::fromStdString(id);
    LocalTrackRow row;
    row.raw_path = "/music/shared/a.flac";
    row.title = "a";
    tab->model->appendRows({row});
    window.persistNow(false);
    QTRY_VERIFY(revision(id) >= 1U);
    QTRY_VERIFY(!window.list_sync_->busy());

    // Changed elsewhere: the tab shows it.
    QVERIFY((*other)->call(
        "list.save",
        protocol::Json{{"id", id},
                       {"name", "Shared, renamed"},
                       {"items", listItems({"/music/shared/a.flac", "/music/shared/b.flac"})}}));
    QTRY_COMPARE(rowPaths(*tab->model),
                 (std::vector<std::string>{"/music/shared/a.flac", "/music/shared/b.flac"}));
    QCOMPARE(displayText(tab->document.name), QStringLiteral("Shared, renamed"));
    QVERIFY(!tab->document.dirty);

    // Saved here, then saved elsewhere, then edited here and saved: asked.
    tab->document.kind = persistence::ListKind::saved;
    window.persistNow(false);
    QTRY_COMPARE(
        (*other)->call("list.get", protocol::Json{{"id", id}})->value("kind", std::string{}),
        std::string{"saved"});
    QTRY_VERIFY(!window.list_sync_->busy());
    const auto theirs = revision(id);
    QVERIFY((*other)->call("list.save",
                           protocol::Json{{"id", id},
                                          {"name", "Shared, renamed"},
                                          {"kind", "saved"},
                                          {"revision", theirs},
                                          {"items", listItems({"/music/shared/theirs.flac"})}}));
    // Their change reached this window, which had no edits of its own, so it
    // shows theirs; now edited here, unsaved.
    QTRY_COMPARE(rowPaths(*tab->model), std::vector<std::string>{"/music/shared/theirs.flac"});
    QTRY_VERIFY(!window.list_sync_->busy());
    LocalTrackRow mine;
    mine.raw_path = "/music/shared/mine.flac";
    tab->model->appendRows({mine});
    tab->document.dirty = true;
    window.persistNow(false);
    QTRY_VERIFY(!window.list_sync_->busy());
    // Theirs again, meanwhile.
    QVERIFY((*other)->call("list.save",
                           protocol::Json{{"id", id},
                                          {"name", "Shared, renamed"},
                                          {"kind", "saved"},
                                          {"items", listItems({"/music/shared/again.flac"})}}));
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QCOMPARE(rowPaths(*tab->model),
             (std::vector<std::string>{"/music/shared/theirs.flac", "/music/shared/mine.flac"}));
    // Saved here: the choice.
    tab->document.dirty = false;
    window.persistNow(false);
    QTRY_VERIFY(window.findChild<QMessageBox*>(QStringLiteral("bench-list-conflict")) != nullptr);
    auto* question = window.findChild<QMessageBox*>(QStringLiteral("bench-list-conflict"));
    question->findChild<QAbstractButton*>(QStringLiteral("bench-list-conflict-copy"))->click();
    // Mine is a list of its own; the tab shows theirs.
    QTRY_COMPARE(rowPaths(*tab->model), std::vector<std::string>{"/music/shared/again.flac"});
    const auto copy = std::ranges::find_if(window.list_tabs_, [](const auto& candidate) {
        return displayText(candidate->document.name) == QStringLiteral("Shared, renamed (mine)");
    });
    QVERIFY(copy != window.list_tabs_.end());
    QCOMPARE(rowPaths(*(*copy)->model),
             (std::vector<std::string>{"/music/shared/theirs.flac", "/music/shared/mine.flac"}));
    const auto copy_id = (*copy)->document.id.to_string();
    window.persistNow(false);
    QTRY_VERIFY((*other)->call("list.get", protocol::Json{{"id", copy_id}}).has_value());

    // Deleted elsewhere: the tab closes, with nothing asked.
    QVERIFY((*other)->call("list.delete", protocol::Json{{"id", id}}));
    QTRY_VERIFY(window.tabForDocument(qid) == nullptr);
    (*other)->close();
}

// A list changed on its engine while this window was closed: the engine's
// version is what the tab shows when the window opens again, rather than the
// window's old copy being written back over it.
void BenchMainWindowTest::aListChangedWhileClosedIsTakenUpOnOpening() {
    std::string id;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
        auto* tab =
            window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                        .kind = persistence::ListKind::saved,
                                                        .name = "Kept",
                                                        .pinned = false,
                                                        .dirty = false,
                                                        .items = {}},
                              true);
        id = tab->document.id.to_string();
        LocalTrackRow row;
        row.raw_path = "/music/kept/old.flac";
        tab->model->appendRows({row});
        window.persistNow(true);
        auto engine = protocol::Client::connect(protocol::Endpoint{
            .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
        QVERIFY(engine.has_value());
        QTRY_VERIFY((*engine)->call("list.get", protocol::Json{{"id", id}}).has_value());
        QTRY_VERIFY(!window.list_sync_->busy());
        (*engine)->close();
    }
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    QVERIFY((*other)->call("list.save",
                           protocol::Json{{"id", id},
                                          {"name", "Kept"},
                                          {"kind", "saved"},
                                          {"items", listItems({"/music/kept/new.flac"})}}));

    BenchMainWindow reopened;
    reopened.show();
    QTRY_VERIFY(reopened.lists_restored_);
    auto* tab = reopened.tabForDocument(QString::fromStdString(id));
    QVERIFY(tab != nullptr);
    QTRY_COMPARE(rowPaths(*tab->model), std::vector<std::string>{"/music/kept/new.flac"});
    QTest::qWait(300);
    QTRY_VERIFY(!reopened.list_sync_->busy());
    auto stored = (*other)->call("list.get", protocol::Json{{"id", id}});
    QVERIFY(stored.has_value());
    QCOMPARE(stored->at("items").size(), std::size_t{1});
    (*other)->close();
}

// ADR-0233: a list this window does not have open -- saved from another
// window, the CLI, the phone -- is found under its engine and opened as a
// tab, without being written back just for having been opened.
void BenchMainWindowTest::aListFromElsewhereOpensAsATab() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    auto made = (*other)->call("list.save",
                               protocol::Json{{"name", "From elsewhere"},
                                              {"kind", "saved"},
                                              {"items", listItems({"/music/elsewhere/1.flac",
                                                                   "/music/elsewhere/2.flac"})}});
    QVERIFY(made.has_value());
    const auto id = made->value("id", std::string{});

    window.findChild<QAction*>(QStringLiteral("action-open-list"))->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("bench-open-list"));
    QVERIFY(dialog != nullptr);
    auto* tree = dialog->findChild<QTreeWidget*>(QStringLiteral("bench-open-list-tree"));
    QTreeWidgetItem* found = nullptr;
    QTRY_VERIFY([&] {
        const auto items = tree->findItems(QStringLiteral("From elsewhere"),
                                           Qt::MatchExactly | Qt::MatchRecursive);
        found = items.isEmpty() ? nullptr : items.front();
        return found != nullptr;
    }());
    QCOMPARE(found->text(1), QStringLiteral("2"));
    QCOMPARE(found->parent()->text(0), QStringLiteral("This computer"));
    tree->setCurrentItem(found);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Open)->click();

    auto* tab = static_cast<BenchMainWindow::ListTab*>(nullptr);
    QTRY_VERIFY((tab = window.tabForDocument(QString::fromStdString(id))) != nullptr);
    QCOMPARE(rowPaths(*tab->model),
             (std::vector<std::string>{"/music/elsewhere/1.flac", "/music/elsewhere/2.flac"}));
    QCOMPARE(tab->document.kind, persistence::ListKind::saved);
    QVERIFY(window.tabs_->currentWidget() == tab->view);
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QCOMPARE((*other)->call("list.get", protocol::Json{{"id", id}})->value("revision", 0), 1);
    (*other)->close();
}

// ADR-0259: a list opened from its engine shows what the engine's library
// knows of each file -- every tag and the revision -- not only the few names
// the list was saved with, described after it opens.
void BenchMainWindowTest::aListFromTheEngineCarriesItsLibraryTags() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto music = media.filePath(QStringLiteral("music"));
    QVERIFY(QDir{}.mkpath(music));
    const auto file = music + QStringLiteral("/rich.flac");
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), file));
    const auto raw = QFile::encodeName(file).toStdString();
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto catalogue = window.localCatalogue()->open();
    QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
    persistence::LibraryScanProgress progress;
    QVERIFY(catalogue->scan({}, progress).has_value());

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    auto items = listItems({raw, "/music/not-indexed/outside.flac"});
    items[0]["title"] = "Saved title";
    items[1]["title"] = "Outside";
    auto made = (*other)->call(
        "list.save",
        protocol::Json{{"name", "Described"}, {"kind", "saved"}, {"items", std::move(items)}});
    QVERIFY(made.has_value());
    window.openEngineList(EngineKey::local(),
                          QString::fromStdString(made->value("id", std::string{})));
    auto* tab = static_cast<BenchMainWindow::ListTab*>(nullptr);
    QTRY_VERIFY((tab = window.tabForDocument(
                     QString::fromStdString(made->value("id", std::string{})))) != nullptr);
    QTRY_COMPARE(tab->model->rowCount(), 2);
    const auto& rows = tab->model->rows();
    // The names it was saved with first, then the library's.
    QTRY_COMPARE(rows[0].title, std::string{"Metadata Fixture"});
    QCOMPARE(rows[0].album, std::string{"Rich Metadata"});
    QStringList names;
    for (const auto& field : rows[0].metadata.fields) {
        names.append(QString::fromStdString(field.canonical_name));
    }
    // Named as the library folds them, as a search's rows are.
    QVERIFY2(names.contains(QStringLiteral("musicbrainztrackid")), qPrintable(names.join(u',')));
    QVERIFY(rows[0].source_revision.has_value());
    QCOMPARE(rows[0].source_revision->size, static_cast<std::uint64_t>(QFileInfo{file}.size()));
    QCOMPARE(rows[1].title, std::string{"Outside"});
    QVERIFY(!rows[1].source_revision.has_value());

    // Described a part at a time: the indexed file comes after a part's worth
    // of others.
    std::vector<std::string> many;
    for (int index = 0; index < 2'000; ++index) {
        many.push_back("/music/not-indexed/" + std::to_string(index) + ".flac");
    }
    many.push_back(raw);
    auto long_list = (*other)->call(
        "list.save",
        protocol::Json{{"name", "Long"}, {"kind", "saved"}, {"items", listItems(many)}});
    QVERIFY(long_list.has_value());
    const auto long_id = QString::fromStdString(long_list->value("id", std::string{}));
    window.openEngineList(EngineKey::local(), long_id);
    auto* long_tab = static_cast<BenchMainWindow::ListTab*>(nullptr);
    QTRY_VERIFY((long_tab = window.tabForDocument(long_id)) != nullptr);
    QTRY_COMPARE(long_tab->model->rowCount(), 2'001);
    QTRY_COMPARE(long_tab->model->rows().back().title, std::string{"Metadata Fixture"});
    (*other)->close();
}

// ADR-0233: a file moved from this window is followed in every list of its
// engine -- one open in no window included -- and an engine that is away at
// the time is told when it is back.
void BenchMainWindowTest::aMoveIsFollowedInListsNotOpenHere() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    const auto connect_other = [this] {
        return protocol::Client::connect(protocol::Endpoint{
            .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    };
    auto other = connect_other();
    QVERIFY(other.has_value());
    auto made = (*other)->call(
        "list.save",
        protocol::Json{{"name", "Not open here"},
                       {"kind", "saved"},
                       {"items", listItems({"/music/moving/a.flac", "/music/moving/b.flac"})}});
    QVERIFY(made.has_value());
    const auto id = made->value("id", std::string{});
    const auto paths = [&other, &id] {
        std::vector<std::string> found;
        auto got = (*other)->call("list.get", protocol::Json{{"id", id}});
        for (const auto& item :
             got ? got->value("items", protocol::Json::array()) : protocol::Json::array()) {
            found.push_back(*protocol::decode_raw_path(item.value("path", std::string{})));
        }
        return found;
    };
    const auto moved = [](const std::string& from, const std::string& to) {
        operations::FilePublicationCommitResult result;
        result.journal_id = core::StableId::random();
        result.source_raw_path = from;
        result.target_raw_path = to;
        return result;
    };

    window.applyCommittedRelocation(moved("/music/moving/a.flac", "/music/moved/a.flac"));
    QTRY_COMPARE(paths(),
                 (std::vector<std::string>{"/music/moved/a.flac", "/music/moving/b.flac"}));
    QTRY_VERIFY(!QSettings{}.contains(QStringLiteral("lists/pending-relocations")));

    // Away during the move: kept, and handed over when it is back.
    (*other)->close();
    engine_.stop();
    QTRY_VERIFY(!window.localPlayback()->active());
    window.applyCommittedRelocation(moved("/music/moving/b.flac", "/music/moved/b.flac"));
    QVERIFY(QSettings{}.contains(QStringLiteral("lists/pending-relocations")));
    QVERIFY2(engine_.start(), engine_.log().constData());
    QTRY_VERIFY_WITH_TIMEOUT(window.localPlayback()->active(), 10'000);
    other = connect_other();
    QVERIFY(other.has_value());
    QTRY_COMPARE_WITH_TIMEOUT(
        paths(), (std::vector<std::string>{"/music/moved/a.flac", "/music/moved/b.flac"}), 10'000);
    QTRY_VERIFY(!QSettings{}.contains(QStringLiteral("lists/pending-relocations")));
    (*other)->close();
}

// The library's "Add to list" puts a selection into a list of the user's
// choosing, not only the one on screen; and a folder dragged from the folder
// browser onto a list is read into it, as from a file manager.
void BenchMainWindowTest::libraryAndFoldersAddToAChosenList() {
    QTemporaryDir music_dir;
    QVERIFY(music_dir.isValid());
    const auto music = music_dir.path();
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/album")));
    write_wave(music + QStringLiteral("/album/one.wav"), wave_sample_rate);
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localLibrary() != nullptr);
    auto catalogue = window.localCatalogue()->open();
    QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
    persistence::LibraryScanProgress progress;
    QVERIFY(catalogue->scan({}, progress).has_value());

    auto* chosen = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                               .kind = persistence::ListKind::saved,
                                                               .name = "Chosen",
                                                               .pinned = false,
                                                               .dirty = false,
                                                               .items = {}},
                                     false);
    auto* current = window.currentListTab();
    QVERIFY(current != nullptr && current != chosen);
    const auto chosen_id = QString::fromStdString(chosen->document.id.to_string());
    const auto targets = window.listTargets(EngineKey::local());
    QVERIFY(std::ranges::any_of(targets, [&](const auto& target) {
        return target.first == chosen_id && target.second == QStringLiteral("Chosen");
    }));
    QVERIFY(window
                .listTargets(window.remoteEngine() != nullptr ? window.remoteEngine()->key
                                                              : EngineKey::remote())
                .empty());

    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = catalogue->query(albums);
    QVERIFY(page && !page->entries.empty());
    const auto before = current->model->rowCount();
    emit window.localLibrary()->browser().addToListRequested(page->entries, chosen_id);
    QTRY_COMPARE(chosen->model->rowCount(), 1);
    QCOMPARE(current->model->rowCount(), before);
    QTRY_VERIFY(!window.discovery_running_);

    // Or into a new list, called what it was named.
    const auto tabs_before = window.list_tabs_.size();
    emit window.localLibrary()->browser().newListRequested(page->entries, QStringLiteral("Picked"));
    QTRY_COMPARE(window.list_tabs_.size(), tabs_before + 1U);
    auto* picked = window.list_tabs_.back().get();
    QCOMPARE(picked->document.name, std::string{"Picked"});
    QTRY_COMPARE(picked->model->rowCount(), 1);
    QCOMPARE(chosen->model->rowCount(), 1);
    QTRY_VERIFY(!window.discovery_running_);

    // The folder, dragged from a file manager onto the list on screen.
    auto mime = std::make_unique<QMimeData>();
    mime->setUrls({QUrl::fromLocalFile(music + QStringLiteral("/album"))});
    auto* viewport = current->view->viewport();
    QDragEnterEvent enter{dragPosition(QPoint{10, 10}), Qt::CopyAction, mime.get(), Qt::LeftButton,
                          Qt::NoModifier};
    QApplication::sendEvent(viewport, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop{QPointF{10, 10}, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_COMPARE(current->model->rowCount(), before + 1);
    QCOMPARE(current->model->rows().back().raw_path,
             QFile::encodeName(music + QStringLiteral("/album/one.wav")).toStdString());
}

// ADR-0233: this computer's lists first, then the remote engine's -- kept
// that way whatever order tabs are opened or dragged in.
void BenchMainWindowTest::tabsAreGroupedByEngine() {
    QTemporaryDir remote_state;
    QVERIFY(remote_state.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    auto* remote_tab = window.remoteQueueTab();
    QVERIFY(remote_tab != nullptr);
    auto* local_tab =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Added later",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          false);
    auto* bar = window.tabs_->tabBar();
    const auto remote_at = [&] { return window.tabs_->indexOf(remote_tab->view); };
    const auto local_at = [&] { return window.tabs_->indexOf(local_tab->view); };
    QVERIFY2(local_at() < remote_at(), "a local list opens before the remote's group");
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(bar->grab().save(directory + QStringLiteral("/tabs-grouped-by-engine.png")));
    }
    // Dragged into the other group: back in its own.
    bar->moveTab(remote_at(), 0);
    QVERIFY2(local_at() < remote_at(), "the groups stay together");
    for (int index = 0; index < remote_at(); ++index) {
        QVERIFY(engineOfView(window.tabs_->widget(index)).isLocal());
    }
}

// ADR-0296: the library's Playlists tab -- its engine's saved lists, open
// here or not, not a tab never saved -- and a drop of files from a file
// manager on one not open yet adds to it.
void BenchMainWindowTest::theListsPanelShowsEveryListAndTakesDrops() {
    QTemporaryDir music_dir;
    QVERIFY(music_dir.isValid());
    const auto music = music_dir.path();
    QVERIFY(QDir{}.mkpath(music + QStringLiteral("/album")));
    write_wave(music + QStringLiteral("/album/one.wav"), wave_sample_rate);
    BenchMainWindow window;
    window.resize(1200, 700);
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());

    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto make = [&engine](const std::string& name) {
        const auto made = (*engine)->call(
            "list.save",
            protocol::Json{{"name", name}, {"kind", "saved"}, {"items", protocol::Json::array()}});
        return made ? QString::fromStdString(made->value("id", std::string{})) : QString{};
    };
    const auto elsewhere = make("Elsewhere");
    const auto target = make("Target");
    QVERIFY(!elsewhere.isEmpty() && !target.isEmpty());

    window.local_source_tabs_->setCurrentIndex(window.playlists_tab_);
    auto* panel = window.playlists_view_;
    QTRY_VERIFY(panel->itemFor(elsewhere) != nullptr && panel->itemFor(target) != nullptr);
    // Flat, as the library's rows: no heading over them, and how many said.
    QVERIFY(panel->itemFor(elsewhere)->parent() == nullptr);
    QCOMPARE(window.findChild<QLabel*>(QStringLiteral("bench-playlists-status"))->text(),
             QStringLiteral("2 playlists on this computer"));
    // Filtered by name.
    auto* filter = window.findChild<QLineEdit*>(QStringLiteral("bench-playlists-filter"));
    QVERIFY(filter != nullptr);
    filter->setText(QStringLiteral("targ"));
    QVERIFY(panel->itemFor(elsewhere)->isHidden() && !panel->itemFor(target)->isHidden());
    filter->clear();
    QVERIFY(!panel->itemFor(elsewhere)->isHidden());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(window.grab().save(directory + QStringLiteral("/playlists.png")));
    }
    window.local_source_tabs_->setCurrentIndex(0);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QTest::qWait(300);
        QVERIFY(window.grab().save(directory + QStringLiteral("/library.png")));
    }
    window.local_source_tabs_->setCurrentIndex(window.playlists_tab_);
    // The window's own tab, never saved, is no playlist.
    auto* own = window.currentListTab();
    QVERIFY(own != nullptr);
    QVERIFY(panel->itemFor(QString::fromStdString(own->document.id.to_string())) == nullptr);

    // Chosen: opened, and its tracks are what is shown.
    panel->setCurrentItem(panel->itemFor(elsewhere));
    QTRY_VERIFY(window.tabForDocument(elsewhere) != nullptr);
    QCOMPARE(window.currentListTab(), window.tabForDocument(elsewhere));

    // Files dropped on a playlist not open here: opened, and added to.
    QVERIFY(window.tabForDocument(target) == nullptr);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(music + QStringLiteral("/album/one.wav"))});
    QTRY_VERIFY(panel->itemFor(target) != nullptr);
    const auto at = panel->visualItemRect(panel->itemFor(target)).center();
    auto* viewport = panel->viewport();
    QDragEnterEvent enter{dragPosition(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move{dragPosition(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &move);
    QVERIFY(move.isAccepted());
    QDropEvent drop{QPointF{at}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(viewport, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_VERIFY(window.tabForDocument(target) != nullptr);
    QTRY_COMPARE(window.tabForDocument(target)->model->rowCount(), 1);
    QTRY_VERIFY(!window.discovery_running_);
    // Lists are tabs only: no pane to show them in instead.
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-lists-panel")) == nullptr);
}

// ADR-0286: a tab that is no saved playlist is the window's alone: never on
// its engine -- not after a reconnect, not on closing -- and nothing is left
// to remove there.
void BenchMainWindowTest::aScratchTabNeverReachesItsEngine() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto listed = [&engine](const std::string& id) {
        auto all = (*engine)->call("list.all");
        return all && std::ranges::any_of(all->value("lists", protocol::Json::array()),
                                          [&id](const protocol::Json& list) {
                                              return list.value("id", std::string{}) == id;
                                          });
    };
    auto* scratch =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Scratch",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    const auto id = scratch->document.id.to_string();
    LocalTrackRow row;
    row.raw_path = "/music/scratch.flac";
    scratch->model->appendRows({row});
    window.markTabDirty(*scratch);
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id));
    window.list_sync_->reconnected(window.localPlayback());
    window.persistNow(false);
    QTest::qWait(300);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id));
    // Closed with nothing to ask: its contents let go of, as Discard would.
    scratch->document.dirty = false;
    window.closeTabAt(window.tabs_->indexOf(scratch->view));
    window.persistNow(false);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id));
    QVERIFY(QSettings{}.value(QStringLiteral("lists/pending-removals")).toStringList().isEmpty());
    (*engine)->close();
}

// Deleted by another client while this window was writing it: the delete
// was taken for this window's own write and ignored, the tab stayed, and its
// next write made the list again -- on the phone, a list that would not go.
void BenchMainWindowTest::aListDeletedElsewhereWhileBeingWrittenStaysDeleted() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.localPlayback() != nullptr && window.localPlayback()->active());
    auto engine = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(engine.has_value());
    const auto listed = [&engine](const std::string& id) {
        auto all = (*engine)->call("list.all");
        return all && std::ranges::any_of(all->value("lists", protocol::Json::array()),
                                          [&id](const protocol::Json& list) {
                                              return list.value("id", std::string{}) == id;
                                          });
    };
    auto* scratch =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::saved,
                                                    .name = "Scratch",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    const auto id = scratch->document.id.to_string();
    LocalTrackRow row;
    row.raw_path = "/music/one.flac";
    scratch->model->appendRows({row});
    window.persistNow(false);
    QTRY_VERIFY(listed(id));
    QTRY_VERIFY(!window.list_sync_->busy());

    // Changed here and being written; deleted there, and told so before the
    // write has answered.
    row.raw_path = "/music/two.flac";
    scratch->model->appendRows({row});
    window.persistNow(false);
    QVERIFY(window.list_sync_->busy());
    QVERIFY((*engine)->call("list.delete", protocol::Json{{"id", id}}).has_value());
    window.list_sync_->listChanged(window.localPlayback(), QString::fromStdString(id), 0, true);

    QTRY_VERIFY(window.tabForDocument(QString::fromStdString(id)) == nullptr);
    QTRY_VERIFY(!window.list_sync_->busy());
    window.persistNow(false);
    QTRY_VERIFY(!window.list_sync_->busy());
    QVERIFY(!listed(id));
    (*engine)->close();
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest)
