// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

#include "workspace/tab_store.hpp"

namespace trackknife::bench {

// ADR-0282: the editor shows an expression's error and will not save the
// view until it is right; a shipped view is edited as a copy, which the list
// it was opened from is then drawn by.
void BenchMainWindowTest::listViewEditorChecksAndSavesACopy() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(static_cast<QTableView*>(tabs->currentWidget())->model());
    LocalTrackRow first;
    first.raw_path = "/music/one.flac";
    first.title = "La Femme d'Argent";
    first.artist = "Air";
    first.album = "Moon Safari";
    first.probed = true;
    auto second = first;
    second.raw_path = "/music/two.flac";
    second.title = "Sexy Boy";
    model->appendRows({first, second});
    window.chooseListView(QStringLiteral("plain-columns"));
    window.editListView(false);
    ListViewDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<ListViewDialog*>()) != nullptr);
    auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("bench-list-view-name"));
    QCOMPARE(name->text(), QStringLiteral("Plain columns (copy)"));
    auto* columns = dialog->findChild<QListWidget*>(QStringLiteral("bench-list-view-columns"));
    auto* add = dialog->findChild<QPushButton*>(QStringLiteral("bench-list-view-add-column"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    auto* save = buttons->button(QDialogButtonBox::Save);
    QVERIFY(save->isEnabled());
    // The title is chosen first: a built-in column, only its second line the
    // view's.
    QCOMPARE(columns->currentItem()->text(), QStringLiteral("Title"));
    const auto type = [](QLineEdit* field, const QString& value) {
        field->setText(value);
        emit field->textEdited(value);
    };
    const auto field = [dialog](const char* object) {
        return dialog->findChild<QLineEdit*>(QString::fromLatin1(object));
    };
    const auto note = [dialog](const char* object) {
        return dialog->findChild<QLabel*>(QString::fromLatin1(object) + QStringLiteral("-note"))
            ->text();
    };
    add->click();
    QCOMPARE(columns->currentRow(), columns->count() - 1);
    auto* display = field("bench-list-view-column-display");
    QVERIFY(display->isVisible());
    // Beneath an expression, what it gives for the list's first track.
    type(display, QStringLiteral("%artist% · %title%"));
    QCOMPARE(note("bench-list-view-column-display"), QStringLiteral("→ Air · La Femme d'Argent"));
    type(display, QStringLiteral("$if(%genre%"));
    QVERIFY(!save->isEnabled());
    QVERIFY(!note("bench-list-view-column-display").startsWith(QStringLiteral("→")));
    auto* problems = dialog->findChild<QLabel*>(QStringLiteral("bench-list-view-problems"));
    QVERIFY(problems->isVisible());
    QVERIFY(problems->text().contains(QStringLiteral("shows")));
    type(field("bench-list-view-column-title"), QStringLiteral("Codec"));
    QCOMPARE(columns->currentItem()->text(), QStringLiteral("Codec"));
    type(display, QStringLiteral("$info(codec)"));
    QVERIFY(save->isEnabled());
    QVERIFY(problems->isHidden());
    // The preview draws the view as edited.
    auto* preview = dialog->findChild<QTableView*>(QStringLiteral("bench-list-view-preview"));
    QTRY_COMPARE(preview->model()->columnCount(), local_column_count + 1);
    QCOMPARE(preview->model()->headerData(local_column_count, Qt::Horizontal).toString(),
             QStringLiteral("Codec"));
    QCOMPARE(preview->model()->rowCount(), 2);
    save->click();
    QTRY_VERIFY(window.findChild<ListViewDialog*>() == nullptr);
    const auto bound = window.currentListTab()->view_layout.view;
    QVERIFY(bound.startsWith(QStringLiteral("view-")));
    const auto copy = findListView(bound);
    QVERIFY(copy.has_value());
    QCOMPARE(copy->name, std::string{"Plain columns (copy)"});
    QCOMPARE(model->columnCount(), local_column_count + 1);
    QCOMPARE(model->headerData(local_column_count, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Codec"));
    QVERIFY(window.close());
}

// ADR-0285: a built-in column shows an expression of the view's -- plain
// columns' number, year and length are theirs -- and the editor changes one
// as it changes any column.
void BenchMainWindowTest::builtinColumnsShowTheirExpressions() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(static_cast<QTableView*>(tabs->currentWidget())->model());
    LocalTrackRow row;
    row.raw_path = "/music/one.flac";
    row.title = "La Femme d'Argent";
    row.artist = "Air";
    row.album = "Moon Safari";
    row.track_number = "3";
    row.date = "1998-01-16";
    row.duration_ms = 429'000;
    row.probed = true;
    model->appendRows({row});
    window.chooseListView(QStringLiteral("plain-columns"));
    QCOMPARE(
        model->headerData(local_track_number_column, Qt::Horizontal, Qt::DisplayRole).toString(),
        QStringLiteral("#"));
    QTRY_COMPARE(model->index(0, local_track_number_column).data().toString(),
                 QStringLiteral("03"));
    QCOMPARE(model->index(0, local_date_column).data().toString(), QStringLiteral("1998"));
    QVERIFY(model->index(0, local_date_column)
                .data(Qt::TextAlignmentRole)
                .value<Qt::Alignment>()
                .testFlag(Qt::AlignRight));
    QCOMPARE(model->index(0, local_artist_column).data().toString(), QStringLiteral("Air"));

    window.editListView(false);
    ListViewDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<ListViewDialog*>()) != nullptr);
    auto* columns = dialog->findChild<QListWidget*>(QStringLiteral("bench-list-view-columns"));
    const auto artist = columns->findItems(QStringLiteral("Artist"), Qt::MatchExactly);
    QCOMPARE(artist.size(), 1);
    columns->setCurrentItem(artist.front());
    auto* shows = dialog->findChild<QLineEdit*>(QStringLiteral("bench-list-view-column-display"));
    // What it always showed, to change.
    QCOMPARE(shows->text(), QStringLiteral("%artist%"));
    QVERIFY(dialog->findChild<QPushButton*>(QStringLiteral("bench-list-view-column-reset"))
                ->isVisible());
    shows->setText(QStringLiteral("$upper(%artist%)"));
    emit shows->textEdited(shows->text());
    QCOMPARE(
        dialog->findChild<QLabel*>(QStringLiteral("bench-list-view-column-display-note"))->text(),
        QStringLiteral("→ AIR"));
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    QTRY_VERIFY(window.findChild<ListViewDialog*>() == nullptr);
    const auto copy = findListView(window.currentListTab()->view_layout.view);
    QVERIFY(copy.has_value());
    QCOMPARE(
        std::ranges::find(copy->columns, std::string{"artist"}, &lists::ViewColumn::id)->display,
        std::string{"$upper(%artist%)"});
    // Left as it was, the title is no expression of the view's.
    QVERIFY(std::ranges::find(copy->columns, std::string{"title"}, &lists::ViewColumn::id)
                ->display.empty());
    QTRY_COMPARE(model->index(0, local_artist_column).data().toString(), QStringLiteral("AIR"));
    QVERIFY(window.close());
}

// ADR-0282: a layout saved before list views is drawn by its presentation's
// shipped view -- or, where its columns were arranged otherwise, by a copy
// arranged so, made once.
void BenchMainWindowTest::listLayoutsFromBeforeViewsMigrate() {
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        QVERIFY(window.close());
    }
    auto state = TabStore::loadState();
    QCOMPARE(state.tabs.size(), 1U);
    QByteArray layout{R"({"schema":2,"presentation":"plain-columns","columns":[)"};
    QStringList columns;
    for (const auto& spec : track_column_specs) {
        // As plain columns are, but without the date.
        const auto shown = spec.logical != local_artwork_column &&
                           spec.logical != local_date_column && spec.logical < local_rating_column;
        columns << QStringLiteral(R"({"id":"%1","width":%2,"visible":%3})")
                       .arg(QString::fromLatin1(spec.id))
                       .arg(spec.default_width)
                       .arg(shown ? QStringLiteral("true") : QStringLiteral("false"));
    }
    layout += columns.join(QLatin1Char(',')).toUtf8() + "]}";
    state.tabs.front().layout = layout;
    TabStore::saveState(state);
    {
        BenchMainWindow migrated;
        migrated.show();
        auto* tabs = migrated.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<QTableView*>(tabs->currentWidget());
        const auto bound = migrated.currentListTab()->view_layout.view;
        QVERIFY(bound.startsWith(QStringLiteral("view-")));
        QCOMPARE(findListView(bound)->name, std::string{"Plain columns (customised)"});
        QVERIFY(view->isColumnHidden(local_date_column));
        QVERIFY(!view->isColumnHidden(local_title_column));
        QVERIFY(migrated.close());
    }
    const auto custom = customListViews();
    QVERIFY(custom && custom->size() == 1U);

    // A view since retired -- the header artwork -- opens as the one in its
    // place.
    state = TabStore::loadState();
    state.tabs.front().layout = QByteArray{
        R"({"schema":3,"presentation":"albums-header-artwork","view":"albums-header-artwork",)"
        R"("columns":[]})"};
    TabStore::saveState(state);
    {
        BenchMainWindow retired;
        retired.show();
        auto* tabs = retired.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(retired.currentListTab()->view_layout.view, QStringLiteral("albums-side-artwork"));
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(retired.close());
    }
}

// ADR-0259: the window keeps which lists it has open, and a cache of their
// rows -- not the lists. Its former copy is read once, as the migration, and
// neither read nor written after; a tab whose engine is away opens from its
// cache, and one whose cache is gone from its engine.
void BenchMainWindowTest::theWindowKeepsItsTabsNotTheLists() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("kept.wav"));
    write_wave(path, wave_sample_rate / 10U);
    const auto raw_path = QFile::encodeName(path).toStdString();
    const auto database = std::filesystem::path{
        QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                          QStringLiteral("/lists.sqlite"))
            .toStdString()};
    QDir{}.mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    const auto kept_id = core::StableId::random();
    const auto scratch_id = core::StableId::random();
    {
        // As an earlier release left it: the lists in the window's database.
        auto repository = persistence::ListRepository::open(database);
        QVERIFY(repository.has_value());
        persistence::ListItem item;
        item.source = persistence::ListSource::local;
        item.source_reference = raw_path;
        item.duration_ms = 100;
        item.fields.push_back({.name = "title", .value = "Seeded"});
        const std::vector<persistence::ListDocument> seeded{
            persistence::ListDocument{.id = kept_id,
                                      .kind = persistence::ListKind::saved,
                                      .name = "Kept",
                                      .pinned = true,
                                      .dirty = false,
                                      .items = {item},
                                      .engine = {}},
            persistence::ListDocument{.id = scratch_id,
                                      .kind = persistence::ListKind::scratch,
                                      .name = "Scratch",
                                      .pinned = false,
                                      .dirty = false,
                                      .items = {},
                                      .engine = {}}};
        QVERIFY(repository->replace_all(seeded).has_value());
    }
    QVERIFY(!TabStore::hasState());
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* kept = window.tabForDocument(QString::fromStdString(kept_id.to_string()));
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->document.name, std::string{"Kept"});
        QVERIFY(kept->document.pinned);
        QCOMPARE(kept->model->rows().front().title, std::string{"Seeded"});
        QVERIFY(window.tabForDocument(QString::fromStdString(scratch_id.to_string())) != nullptr);
        window.persistNow(true);
        QVERIFY(TabStore::hasState());
        // On its engine, as ADR-0233's migration has it.
        auto other = protocol::Client::connect(protocol::Endpoint{
            .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
        QVERIFY(other.has_value());
        QTRY_VERIFY_WITH_TIMEOUT(
            (*other)->call("list.get", protocol::Json{{"id", kept_id.to_string()}}).has_value(),
            5'000);
        (*other)->close();
        QVERIFY(window.close());
    }
    {
        // Changed where the lists used to be: no longer read, so not seen.
        auto repository = persistence::ListRepository::open(database);
        QVERIFY(repository.has_value());
        auto lists = repository->load_all();
        QVERIFY(lists.has_value());
        for (auto& list : *lists) {
            if (list.id == kept_id) {
                list.name = "Changed in the old table";
            }
        }
        QVERIFY(repository->replace_all(*lists).has_value());
    }
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* kept = window.tabForDocument(QString::fromStdString(kept_id.to_string()));
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->document.name, std::string{"Kept"});
        QVERIFY(kept->document.pinned);
        QCOMPARE(kept->model->rowCount(), 1);
        // Shown last, shown first.
        window.tabs_->setCurrentWidget(kept->view);
        QTRY_VERIFY(!window.list_sync_->busy());
        window.persistNow(true);
        QVERIFY(window.close());
    }
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QCOMPARE(window.currentListTab()->document.id, kept_id);
        QVERIFY(window.close());
    }
    {
        // Nor written: the old table still says what it was changed to.
        auto repository = persistence::ListRepository::open(database);
        QVERIFY(repository.has_value());
        const auto lists = repository->load_all();
        QVERIFY(lists.has_value());
        const auto old = std::ranges::find(*lists, kept_id, &persistence::ListDocument::id);
        QVERIFY(old != lists->end());
        QCOMPARE(old->name, std::string{"Changed in the old table"});
    }

    // The engine away: the tab opens from its cache.
    engine_.stop();
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* kept = window.tabForDocument(QString::fromStdString(kept_id.to_string()));
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->model->rowCount(), 1);
        QCOMPARE(kept->model->rows().front().title, std::string{"Seeded"});
        QVERIFY(window.close());
    }
    QVERIFY2(engine_.start(), engine_.log().constData());

    // The cache gone: the tab is its engine's list again once it answers.
    QDir{QStandardPaths::writableLocation(QStandardPaths::CacheLocation)}.removeRecursively();
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* kept = window.tabForDocument(QString::fromStdString(kept_id.to_string()));
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->document.name, std::string{"Kept"});
        QTRY_COMPARE_WITH_TIMEOUT(kept->model->rowCount(), 1, 5'000);
        QCOMPARE(kept->model->rows().front().raw_path, raw_path);
        QVERIFY(window.close());
    }
}

void BenchMainWindowTest::persistsPinnedDuplicatedAndDirtyTabs() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("tab-state.wav"));
    write_wave(path, wave_sample_rate / 10U);
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        auto* duplicate = window.findChild<QAction*>(QStringLiteral("action-duplicate-tab"));
        auto* pin = window.findChild<QAction*>(QStringLiteral("action-pin-tab"));
        auto* save = window.findChild<QAction*>(QStringLiteral("action-save-list"));
        auto* close = window.findChild<QAction*>(QStringLiteral("action-close-tab"));
        QVERIFY(tabs != nullptr);
        QVERIFY(duplicate != nullptr);
        QVERIFY(pin != nullptr);
        QVERIFY(save != nullptr);
        QVERIFY(close != nullptr);
        QCOMPARE(duplicate->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+D")));
        QCOMPARE(pin->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+P")));
        QCOMPARE(save->shortcut(), QKeySequence::Save);
        QCOMPARE(close->shortcut(), QKeySequence(QStringLiteral("Ctrl+W")));
        QTRY_COMPARE(tabs->count(), 1);

        window.openLocalPaths({raw_path});
        auto* source = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(source != nullptr);
        QTRY_COMPARE(source->model()->rowCount(), 1);
        const auto source_index = tabs->indexOf(source);
        QVERIFY(source_index >= 0);
        QTRY_VERIFY(tabs->tabText(source_index).endsWith(QStringLiteral(" *")));
        QVERIFY(tabs->tabToolTip(source_index).contains(QStringLiteral("modified")));

        pin->trigger();
        QVERIFY(pin->isChecked());
        QVERIFY(tabs->tabToolTip(source_index).contains(QStringLiteral("pinned")));
        auto* source_close = tabs->tabBar()->tabButton(source_index, QTabBar::RightSide);
        QVERIFY(source_close != nullptr);
        QVERIFY(!source_close->isVisible());
        QVERIFY(!close->isEnabled());

        duplicate->trigger();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 1);
        auto* copied = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(copied != nullptr);
        QCOMPARE(copied->model()->rowCount(), 1);
        const auto copied_index = tabs->indexOf(copied);
        QCOMPARE(tabs->tabText(copied_index), QStringLiteral("Untitled copy *"));
        QVERIFY(tabs->tabBar()->tabButton(copied_index, QTabBar::RightSide)->isVisible());

        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setTextValue(QStringLiteral("Saved copy"));
                dialog->accept();
            }
        });
        save->trigger();
        QCOMPARE(tabs->tabText(copied_index), QStringLiteral("Saved copy"));
        QVERIFY(!tabs->tabToolTip(copied_index).contains(QStringLiteral("modified")));

        // A later edit makes the explicitly saved list dirty again. Closing
        // must offer discard and honor both answers.
        window.openLocalPaths({raw_path});
        QTRY_COMPARE(copied->model()->rowCount(), 2);
        QTRY_VERIFY(tabs->tabText(copied_index).endsWith(QStringLiteral(" *")));
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->done(QMessageBox::No);
            }
        });
        close->trigger();
        QCOMPARE(tabs->count(), 2);
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->done(QMessageBox::Yes);
            }
        });
        close->trigger();
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->currentWidget(), source);
        QVERIFY(pin->isChecked());
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    const auto local_index = tabs->currentIndex();
    QCOMPARE(tabs->tabText(local_index), QStringLiteral("Untitled *"));
    QVERIFY(tabs->tabToolTip(local_index).contains(QStringLiteral("pinned")));
    QVERIFY(tabs->tabToolTip(local_index).contains(QStringLiteral("modified")));
    QVERIFY(!tabs->tabBar()->tabButton(local_index, QTabBar::RightSide)->isVisible());
    auto* view = qobject_cast<QTableView*>(tabs->widget(local_index));
    QVERIFY(view != nullptr);
    QCOMPARE(view->model()->rowCount(), 1);
}

void BenchMainWindowTest::richMetadataValuesAndIdentitiesSurviveListRestart() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto fixture_path = media.filePath(QStringLiteral("rich-metadata.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), fixture_path));
    const auto encoded = QFile::encodeName(fixture_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_path});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* model = qobject_cast<LocalListModel*>(
            qobject_cast<QTableView*>(tabs->currentWidget())->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
        QTRY_VERIFY_WITH_TIMEOUT(model->rows().front().probed, 5'000);
        const auto& row = model->rows().front();
        QCOMPARE(row.title, std::string{"Metadata Fixture"});
        QCOMPARE(row.artist, std::string{"First Artist"});
        QCOMPARE(row.album, std::string{"Rich Metadata"});
        QCOMPARE(row.album_artist, std::string{"Album Credit"});
        QCOMPARE(row.metadata.effective_values("artist"),
                 (std::vector<std::string>{"First Artist", "Second Artist"}));
        QCOMPARE(row.metadata.effective_values("custom_field"),
                 (std::vector<std::string>{"first custom value", "second custom value"}));
        const auto identity = metadata::project_musicbrainz(row.metadata);
        QCOMPARE(identity.recording_ids,
                 (std::vector<std::string>{"11111111-1111-1111-1111-111111111111"}));
        QCOMPARE(identity.artist_ids,
                 (std::vector<std::string>{"55555555-5555-5555-5555-555555555555",
                                           "66666666-6666-6666-6666-666666666666"}));
        QVERIFY(row.source_revision.has_value());
        QCOMPARE(row.source_revision->size, 2'308U);

        // Simulate a workspace saved by the pre-ADR-0066 merge: FFmpeg's
        // generic `track` projection was cached beside TagLib's authoritative
        // native TRACKNUMBER field. Restart must discard only the redundant
        // stream projection and retain the embedded field.
        auto legacy_rows = model->rows();
        legacy_rows.front().metadata.fields.push_back(metadata::MetadataField{
            .canonical_name = "tracknumber",
            .native_name = "TRACKNUMBER",
            .values = {"3"},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        });
        legacy_rows.front().metadata.fields.push_back(metadata::MetadataField{
            .canonical_name = "track",
            .native_name = "track",
            .values = {"3"},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::stream,
        });
        legacy_rows.front().metadata.fields.push_back(metadata::MetadataField{
            .canonical_name = "album_artist",
            .native_name = "ALBUM_ARTIST",
            .values = {"Independent freeform value"},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        });
        model->replaceRows(std::move(legacy_rows));
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 1);
    const auto& row = model->rows().front();
    QCOMPARE(row.artist, std::string{"First Artist"});
    QCOMPARE(row.metadata.effective_values("artist"),
             (std::vector<std::string>{"First Artist", "Second Artist"}));
    QCOMPARE(row.metadata.effective_values("custom_field"),
             (std::vector<std::string>{"first custom value", "second custom value"}));
    QCOMPARE(metadata::project_musicbrainz(row.metadata).release_ids,
             (std::vector<std::string>{"33333333-3333-3333-3333-333333333333"}));
    QCOMPARE(row.metadata.effective_values("tracknumber"), (std::vector<std::string>{"3"}));
    QVERIFY(!row.metadata.effective_native_field("track").has_value());
    const auto freeform_album_artist = row.metadata.effective_native_field("ALBUM_ARTIST");
    QVERIFY(freeform_album_artist.has_value());
    QCOMPARE(freeform_album_artist->values,
             (std::vector<std::string>{"Independent freeform value"}));
    // The snapshot and revision are explicitly stale evidence. The write-plan
    // boundary observes the file again and compares this value before commit;
    // retaining it lets dependent-state replay distinguish an older queued
    // list save from a later external refresh.
    QVERIFY(row.source_revision.has_value());
    QCOMPARE(row.source_revision->size, 2'308U);
}

void BenchMainWindowTest::metadataFieldReviewPreservesDraftAndSelectionScope() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    for (int index = 0; index < 2; ++index) {
        const auto path = media.filePath(QStringLiteral("review-%1.flac").arg(index));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        const auto read = metadata::read_local_metadata(raw);
        QVERIFY(read.has_value());
        sources.push_back(MetadataPropertiesSource{
            .source = {.raw_path = raw,
                       .source_revision = read->source_revision,
                       .baseline = read->document},
            .track_label = QStringLiteral("Review %1").arg(index),
        });
    }
    auto dialog = std::make_unique<MetadataPropertiesDialog>(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        std::span<const std::string_view>{}, MetadataWritePlanApplierFactory{},
        MetadataApplyObserver{});
    dialog->show();
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = dialog->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* files = dialog->fileListView();
    auto* model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    auto* search = dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-field-filter"));
    auto* changed = dialog->findChild<QCheckBox*>(QStringLiteral("bench-metadata-changed-only"));
    auto* show_files = dialog->findChild<QCheckBox*>(QStringLiteral("bench-metadata-show-files"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-field-filter-status"));
    QVERIFY(model && grid && search && changed && show_files && status);
    QTRY_VERIFY(model->summaryReady() && model->draftPreviewReady());
    const auto title = model->fieldRow(QStringLiteral("title"));
    const auto artist = model->fieldRow(QStringLiteral("artist"));
    QVERIFY(title && artist);

    show_files->setChecked(true);
    QTRY_VERIFY(files->isVisible());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/tag-file-scope.png")));
    }
    // Checkboxes toggle the editing scope without clearing the other files.
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
    const auto first = grid->index(0, 0);
    const auto checkbox =
        QPoint(files->visualRect(first).left() + 14, files->visualRect(first).center().y());
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, checkbox);
    QCOMPARE(files->selectionModel()->selectedRows().size(), 1);
    QVERIFY(files->selectionModel()->isRowSelected(1));
    QTRY_COMPARE(model->selectedItemCount(), 1U);
    QTest::keyClick(files, Qt::Key_Space);
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, checkbox);
    QCOMPARE(files->selectionModel()->selectedRows().size(), 1);
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, checkbox);
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
    QTRY_COMPARE(model->selectedItemCount(), 2U);

    // Filtering is a view operation: hidden selections cannot be removed accidentally.
    fields->setCurrentIndex(model->index(*artist, 2));
    fields->selectRow(*artist);
    search->setText(QStringLiteral("TiTlE"));
    QTRY_VERIFY(fields->isRowHidden(*artist));
    QVERIFY(!fields->isRowHidden(*title));
    QVERIFY(fields->selectionModel()->selectedRows().empty());
    QVERIFY(!fields->currentIndex().isValid());
    QVERIFY(grid->patches().empty());
    search->setText(QStringLiteral("musicbrainz_albumid"));
    const auto mbid = model->fieldRow(QStringLiteral("musicbrainz_albumid"));
    QVERIFY(mbid);
    QTRY_VERIFY(!fields->isRowHidden(*mbid));
    search->clear();

    // Stage only one file, then switch the selected-file scope under the filter.
    files->selectionModel()->select(grid->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                           QItemSelectionModel::Rows);
    QTRY_COMPARE(model->selectedItemCount(), 1U);
    QTRY_VERIFY(model->summaryReady());
    QVERIFY(
        model->setData(model->index(*title, 2), QStringLiteral("Reviewed title"), Qt::EditRole));
    changed->setChecked(true);
    QTRY_VERIFY(status->text().contains(QStringLiteral("1 changed")));
    QVERIFY(!fields->isRowHidden(*title));
    QVERIFY(fields->isRowHidden(*artist));
    QVERIFY(model->index(*title, 0).data(Qt::FontRole).value<QFont>().bold());
    QVERIFY(!model->index(*title, 0).data(Qt::AccessibleDescriptionRole).toString().isEmpty());
    files->selectionModel()->select(grid->index(1, 0), QItemSelectionModel::ClearAndSelect |
                                                           QItemSelectionModel::Rows);
    QTRY_VERIFY(status->text().contains(QStringLiteral("0 changed")));
    QVERIFY(fields->isRowHidden(*title));
    QVERIFY(!grid->patches().empty());
    files->selectionModel()->select(grid->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                           QItemSelectionModel::Rows);
    QTRY_VERIFY2(
        !fields->isRowHidden(*title),
        qPrintable(QStringLiteral("%1; selected=%2; summary=%3 draft=%4; query=%5; value=%6")
                       .arg(status->text())
                       .arg(model->selectedItemCount())
                       .arg(model->summaryReady())
                       .arg(model->draftPreviewReady())
                       .arg(search->text())
                       .arg(model->index(*title, 2).data(Qt::DisplayRole).toString())));

    // Collapsing the selector preserves scope; hiding all fields preserves the draft.
    show_files->setChecked(false);
    QVERIFY(files->isHidden());
    QCOMPARE(model->selectedItemCount(), 1U);
    search->setText(QStringLiteral("no-such-field"));
    QTRY_VERIFY(fields->isRowHidden(*title));
    QVERIFY(status->text().contains(QStringLiteral("Apply includes hidden edits")));
    QCOMPARE(model->index(*title, 2).data(Qt::EditRole).toString(),
             QStringLiteral("Reviewed title"));
    search->clear();
    QTRY_VERIFY(!fields->isRowHidden(*title));
    const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
    if (!screenshot_dir.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshot_dir + QStringLiteral("/tag-field-review.png")));
    }
    QVERIFY(grid->undo());
    QTRY_VERIFY(fields->isRowHidden(*title));
    QVERIFY(!model->index(*title, 0).data(Qt::FontRole).value<QFont>().bold());
    QVERIFY(grid->redo());
    QTRY_VERIFY(!fields->isRowHidden(*title));

    // Adding a field clears the review filter so the new blank field is editable.
    auto* add = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-add-field"));
    QVERIFY(add);
    add->click();
    QInputDialog* prompt = nullptr;
    QTRY_VERIFY((prompt = dialog->findChild<QInputDialog*>()) != nullptr);
    prompt->setTextValue(QStringLiteral("REVIEW_CUSTOM"));
    prompt->accept();
    QTRY_VERIFY(model->fieldRow(QStringLiteral("REVIEW_CUSTOM")).has_value());
    const auto custom = *model->fieldRow(QStringLiteral("REVIEW_CUSTOM"));
    QTRY_VERIFY(!changed->isChecked());
    QVERIFY(!fields->isRowHidden(custom));
    QCOMPARE(fields->currentIndex().row(), custom);
    QVERIFY(grid->discardAll());
    QTRY_VERIFY(status->text().contains(QStringLiteral("0 changed")));
    QVERIFY(!fields->isRowHidden(*artist));
}

void BenchMainWindowTest::metadataPropertiesFileSelectionDrivesIndividualAndBulkEdits() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto rich_path = media.filePath(QStringLiteral("rich.flac"));
    const auto ordinary_path = media.filePath(QStringLiteral("ordinary.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), rich_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), ordinary_path));
    const auto rich_encoded = QFile::encodeName(rich_path);
    const auto ordinary_encoded = QFile::encodeName(ordinary_path);
    const std::string rich_raw{rich_encoded.constData(),
                               static_cast<std::size_t>(rich_encoded.size())};
    const std::string ordinary_raw{ordinary_encoded.constData(),
                                   static_cast<std::size_t>(ordinary_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({rich_raw, ordinary_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr);
    QVERIFY(properties != nullptr);
    QCOMPARE(properties->shortcut(), QKeySequence(QStringLiteral("Alt+Return")));
    QCOMPARE(properties->text(), QStringLiteral("Edit tags…"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 2, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows()[0].probed && list_model->rows()[1].probed, 5'000);
    const auto ordinary_row =
        std::ranges::find(list_model->rows(), ordinary_raw, &LocalTrackRow::raw_path);
    QVERIFY(ordinary_row != list_model->rows().end());
    QCOMPARE(ordinary_row->metadata.effective_values("tracknumber"),
             (std::vector<std::string>{"3"}));
    QVERIFY(!ordinary_row->metadata.effective_native_field("track").has_value());
    const auto rich_row = std::ranges::find(list_model->rows(), rich_raw, &LocalTrackRow::raw_path);
    QVERIFY(rich_row != list_model->rows().end());
    QVERIFY(!rich_row->metadata.effective_native_field("album_artist").has_value());
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->selectionModel()->select(list_model->index(1, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties->isEnabled());
    properties->trigger();

    auto* dialog =
        window.findChild<MetadataPropertiesDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    // ADR-0221: a window, and still non-modal so the workspace stays usable
    // while edits are staged.
    QVERIFY(!dialog->isModal());
    QVERIFY(dialog->isWindow());
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<MetadataPropertiesDialog*>(tabs->currentWidget()) == nullptr);
    QCOMPARE(dialog->windowTitle(), QStringLiteral("Edit tags · 2 tracks"));
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-loading")) != nullptr);

    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->fileListView()) != nullptr);
    auto* fields = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-fields"));
    auto* summary = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-summary"));
    auto* read_only = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-metadata-buttons"));
    QVERIFY(fields != nullptr);
    QVERIFY(summary != nullptr);
    QVERIFY(read_only != nullptr);
    QVERIFY(buttons != nullptr);
    QVERIFY(dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-pages")) == nullptr);
    QVERIFY(dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-grid")) == nullptr);
    QVERIFY(dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-values")) == nullptr);
    QVERIFY2(summary->text().contains(QStringLiteral("2 of 2 files selected")),
             qPrintable(QStringLiteral("unexpected summary: %1").arg(summary->text())));
    QVERIFY(summary->text().contains(QStringLiteral("2 sources")));
    // The files are read on opening, whatever the rows knew (ADR-0268) --
    // shown first on the cached tags and then read ("Tags read"), or read
    // before the cached tags could be shown; which comes first is a race
    // (the cached path is theTaggerOpensOnCachedTagsAndReadsBehind's).
    auto* session = dialog->findChild<TaggerSession*>();
    QVERIFY(session != nullptr);
    QTRY_VERIFY(session->ready() && session->canStageProposals());
    QVERIFY2(!read_only->text().contains(QStringLiteral("cached")),
             qPrintable(QStringLiteral("unexpected status: %1").arg(read_only->text())));
    QCOMPARE(buttons->standardButtons(), QDialogButtonBox::Close);
    QVERIFY(buttons->button(QDialogButtonBox::Apply) == nullptr);
    QCOMPARE(files->selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(files->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(files->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(!files->wordWrap());
    QVERIFY(fields->editTriggers().testFlag(QAbstractItemView::EditKeyPressed));
    QVERIFY(fields->editTriggers().testFlag(QAbstractItemView::AnyKeyPressed));
    QVERIFY(!fields->wordWrap());

    auto* undo = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    auto* redo = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-redo"));
    auto* discard = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-discard"));
    auto* add_field = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-add-field"));
    auto* remove_field =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-remove-field"));
    auto* edit_values =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-edit-values"));
    auto* preview_write_plan =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(undo != nullptr);
    QVERIFY(redo != nullptr);
    QVERIFY(discard != nullptr);
    QVERIFY(add_field != nullptr);
    QVERIFY(remove_field != nullptr);
    QVERIFY(edit_values != nullptr);
    QVERIFY(preview_write_plan != nullptr);
    QVERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());
    QVERIFY(!discard->isEnabled());
    QVERIFY(!preview_write_plan->isEnabled());

    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    QVERIFY(grid_model != nullptr);
    QVERIFY(aggregate_model != nullptr);
    QCOMPARE(grid_model->rowCount(), 2);
    QCOMPARE(files->selectionModel()->selectedRows(0).size(), 2);
    QCOMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QCOMPARE(aggregate_model->columnCount(), 3);
    QCOMPARE(aggregate_model->headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Field"));
    QCOMPARE(aggregate_model->headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Original"));
    QCOMPARE(aggregate_model->headerData(2, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Draft"));
    for (auto column = 1; column < grid_model->columnCount(); ++column) {
        QVERIFY(files->isColumnHidden(column));
    }

    const auto title_column = grid_model->fieldColumn(QStringLiteral("title"));
    const auto artist_column = grid_model->fieldColumn(QStringLiteral("artist"));
    const auto date_column = grid_model->fieldColumn(QStringLiteral("date"));
    const auto genre_column = grid_model->fieldColumn(QStringLiteral("genre"));
    const auto custom_column = grid_model->fieldColumn(QStringLiteral("custom_field"));
    const auto track_number_column = grid_model->fieldColumn(QStringLiteral("tracknumber"));
    QVERIFY(title_column.has_value());
    QVERIFY(artist_column.has_value());
    QVERIFY(date_column.has_value());
    QVERIFY(genre_column.has_value());
    QVERIFY(custom_column.has_value());
    QVERIFY(track_number_column.has_value());
    QVERIFY(!grid_model->fieldColumn(QStringLiteral("track")).has_value());
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    const auto artist_row = aggregate_model->fieldRow(QStringLiteral("artist"));
    const auto date_row = aggregate_model->fieldRow(QStringLiteral("date"));
    const auto genre_row = aggregate_model->fieldRow(QStringLiteral("genre"));
    const auto custom_row = aggregate_model->fieldRow(QStringLiteral("custom_field"));
    QVERIFY(title_row.has_value());
    QVERIFY(artist_row.has_value());
    QVERIFY(date_row.has_value());
    QVERIFY(genre_row.has_value());
    QVERIFY(custom_row.has_value());
    QVERIFY(aggregate_model->index(*title_row, 1)
                .data(Qt::DisplayRole)
                .toString()
                .contains(QStringLiteral("various")));
    QVERIFY(aggregate_model->index(*date_row, 1)
                .data(Qt::DisplayRole)
                .toString()
                .contains(QStringLiteral("present on 1 of 2")));
    QCOMPARE(aggregate_model->index(*genre_row, 1).data(Qt::DisplayRole).toString(),
             QStringLiteral("—"));

    // Arbitrary fields join the same Original/Draft table. The field starts
    // missing, its inline Draft edit applies to all selected files, and
    // removing it again cancels those additions because no baseline existed.
    const auto initial_field_count = aggregate_model->rowCount();
    QTRY_VERIFY(add_field->isEnabled());
    QTest::mouseClick(add_field, Qt::LeftButton);
    QInputDialog* field_prompt = nullptr;
    QTRY_VERIFY((field_prompt = dialog->findChild<QInputDialog*>(
                     QStringLiteral("bench-metadata-add-field-dialog"))) != nullptr);
    QVERIFY(!add_field->isEnabled());
    auto* field_name =
        field_prompt->findChild<QLineEdit*>(QStringLiteral("bench-metadata-add-field-name"));
    auto* field_completer =
        field_prompt->findChild<QCompleter*>(QStringLiteral("bench-metadata-field-completer"));
    QVERIFY(field_name != nullptr);
    QVERIFY(field_completer != nullptr);
    field_prompt->setTextValue(QStringLiteral("alb art"));
    QTRY_VERIFY(field_completer->model()->rowCount() > 0);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("Album Artist"));
    field_prompt->setTextValue(QStringLiteral("mb track"));
    QTRY_VERIFY(field_completer->model()->rowCount() >= 2);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("MUSICBRAINZ_TRACKID"));
    field_prompt->setTextValue(QStringLiteral("Mood"));
    field_prompt->accept();
    QTRY_VERIFY(dialog->findChild<QInputDialog*>(
                    QStringLiteral("bench-metadata-add-field-dialog")) == nullptr);
    QTRY_COMPARE(aggregate_model->rowCount(), initial_field_count + 1);
    const auto mood_row = aggregate_model->fieldRow(QStringLiteral("mood"));
    const auto mood_column = grid_model->fieldColumn(QStringLiteral("mood"));
    QVERIFY(mood_row.has_value());
    QVERIFY(mood_column.has_value());
    QVERIFY(files->isColumnHidden(*mood_column));
    const auto mood_draft = aggregate_model->index(*mood_row, 2);
    QTRY_VERIFY(mood_draft.flags().testFlag(Qt::ItemIsEditable));
    QTRY_COMPARE(fields->currentIndex(), mood_draft);
    QTest::keyClick(fields, Qt::Key_F2);
    QLineEdit* field_editor = nullptr;
    QTRY_VERIFY((field_editor = fields->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(field_editor, QStringLiteral("Energetic"));
    QTest::keyClick(field_editor, Qt::Key_Return);
    QTRY_VERIFY(fields->findChild<QLineEdit*>() == nullptr);
    QCOMPARE(mood_draft.data(Qt::DisplayRole).toString(), QStringLiteral("Energetic"));
    QCOMPARE(grid_model->index(0, *mood_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Energetic")}));
    QCOMPARE(grid_model->index(1, *mood_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Energetic")}));
    QTRY_VERIFY(remove_field->isEnabled());
    QTest::mouseClick(remove_field, Qt::LeftButton);
    QTRY_VERIFY(!grid_model->index(0, *mood_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *mood_column).data(metadata_cell_staged_role).toBool());
    QCOMPARE(mood_draft.data(Qt::DisplayRole).toString(), QStringLiteral("—"));

    // Successful arbitrary names become session-recent completion candidates.
    QTest::mouseClick(add_field, Qt::LeftButton);
    QTRY_VERIFY((field_prompt = dialog->findChild<QInputDialog*>(
                     QStringLiteral("bench-metadata-add-field-dialog"))) != nullptr);
    field_completer =
        field_prompt->findChild<QCompleter*>(QStringLiteral("bench-metadata-field-completer"));
    QVERIFY(field_completer != nullptr);
    field_prompt->setTextValue(QStringLiteral("moo"));
    QTRY_VERIFY(field_completer->model()->rowCount() > 0);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(), QStringLiteral("Mood"));
    field_prompt->reject();
    QTRY_VERIFY(dialog->findChild<QInputDialog*>(
                    QStringLiteral("bench-metadata-add-field-dialog")) == nullptr);

    // ADR-0276: lyrics are many lines. Their editor takes Enter as a new
    // line, keeps the text on Ctrl+Enter and drops it on Escape; the row
    // shows the line breaks as marks.
    QTest::mouseClick(add_field, Qt::LeftButton);
    QTRY_VERIFY((field_prompt = dialog->findChild<QInputDialog*>(
                     QStringLiteral("bench-metadata-add-field-dialog"))) != nullptr);
    field_prompt->setTextValue(QStringLiteral("Lyrics"));
    field_prompt->accept();
    std::optional<int> lyrics_row;
    QTRY_VERIFY((lyrics_row = aggregate_model->fieldRow(QStringLiteral("lyrics"))).has_value());
    const auto lyrics_column = grid_model->fieldColumn(QStringLiteral("lyrics"));
    QVERIFY(lyrics_column.has_value());
    const auto lyrics_draft = aggregate_model->index(*lyrics_row, 2);
    QTRY_COMPARE(fields->currentIndex(), lyrics_draft);
    QTest::keyClick(fields, Qt::Key_F2);
    QPlainTextEdit* lyrics_editor = nullptr;
    QTRY_VERIFY((lyrics_editor = fields->findChild<QPlainTextEdit*>(
                     QStringLiteral("metadata-long-text-editor"))) != nullptr);
    QVERIFY(fields->findChild<QLineEdit*>() == nullptr);
    QVERIFY(lyrics_editor->height() > fields->visualRect(lyrics_draft).height() * 4);
    QTest::keyClicks(lyrics_editor, QStringLiteral("First line"));
    QTest::keyClick(lyrics_editor, Qt::Key_Return);
    QTest::keyClicks(lyrics_editor, QStringLiteral("Second line"));
    QVERIFY(fields->findChild<QPlainTextEdit*>(QStringLiteral("metadata-long-text-editor")) !=
            nullptr);
    QTest::keyClick(lyrics_editor, Qt::Key_Return, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(fields->findChild<QPlainTextEdit*>(QStringLiteral("metadata-long-text-editor")) ==
                nullptr);
    QCOMPARE(grid_model->index(0, *lyrics_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("First line\nSecond line")}));
    QCOMPARE(lyrics_draft.data(Qt::DisplayRole).toString(),
             QStringLiteral("First line ↵ Second line"));
    QTest::keyClick(fields, Qt::Key_F2);
    QTRY_VERIFY((lyrics_editor = fields->findChild<QPlainTextEdit*>(
                     QStringLiteral("metadata-long-text-editor"))) != nullptr);
    QCOMPARE(lyrics_editor->toPlainText(), QStringLiteral("First line\nSecond line"));
    QTest::keyClicks(lyrics_editor, QStringLiteral(" and more"));
    QTest::keyClick(lyrics_editor, Qt::Key_Escape);
    QTRY_VERIFY(fields->findChild<QPlainTextEdit*>(QStringLiteral("metadata-long-text-editor")) ==
                nullptr);
    QCOMPARE(grid_model->index(0, *lyrics_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("First line\nSecond line")}));
    QTRY_VERIFY(remove_field->isEnabled());
    QTest::mouseClick(remove_field, Qt::LeftButton);
    QTRY_VERIFY(!grid_model->index(0, *lyrics_column).data(metadata_cell_staged_role).toBool());

    // One selected file exposes exact values in the same lower table and limits
    // edits to that source row.
    files->selectionModel()->select(grid_model->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                                 QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    QTRY_COMPARE(aggregate_model->index(*title_row, 1).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Metadata Fixture"));
    QVERIFY(summary->text().contains(QStringLiteral("1 of 2 files selected")));
    const auto custom_draft = aggregate_model->index(*custom_row, 2);
    fields->setCurrentIndex(custom_draft);
    QTRY_VERIFY(edit_values->isEnabled());
    QTest::mouseClick(edit_values, Qt::LeftButton);

    QDialog* exact_dialog = nullptr;
    QTRY_VERIFY((exact_dialog = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-exact-values"))) != nullptr);
    auto* exact_table =
        exact_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-exact-values-table"));
    auto* exact_add =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-add"));
    auto* exact_up =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-up"));
    auto* exact_buttons = exact_dialog->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-exact-values-buttons"));
    QVERIFY(exact_table != nullptr);
    QVERIFY(exact_add != nullptr);
    QVERIFY(exact_up != nullptr);
    QVERIFY(exact_buttons != nullptr);
    QCOMPARE(exact_table->model()->rowCount(), 2);
    QCOMPARE(exact_table->model()->index(0, 0).data(Qt::EditRole).toString(),
             QStringLiteral("first custom value"));
    QVERIFY(
        exact_table->model()->setData(exact_table->model()->index(0, 0), QString{}, Qt::EditRole));
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QLineEdit* exact_editor = nullptr;
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("third; value"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    exact_table->setCurrentIndex(exact_table->model()->index(2, 0));
    QTest::mouseClick(exact_up, Qt::LeftButton);
    QTest::mouseClick(exact_buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-exact-values")) ==
                nullptr);

    const auto first_custom = grid_model->index(0, *custom_column);
    const auto second_custom = grid_model->index(1, *custom_column);
    const QStringList custom_draft_values{QString{}, QStringLiteral("third; value"),
                                          QStringLiteral("second custom value")};
    QCOMPARE(first_custom.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QCOMPARE(second_custom.data(metadata_cell_values_role).toStringList(), QStringList{});
    QCOMPARE(custom_draft.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QVERIFY(summary->text().contains(QStringLiteral("1 staged change")));

    // Planning is an explicit fresh-read operation. Native FLAC now has a
    // preservation-proven text writer, but this exact draft deliberately
    // contains an empty value that TagLib would reinterpret as deletion. The
    // blocked apply changes nothing and reports the problem compactly.
    QTRY_VERIFY(preview_write_plan->isEnabled());
    QTest::mouseClick(preview_write_plan, Qt::LeftButton);
    QVERIFY(!preview_write_plan->isEnabled());
    QDialog* feedback = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((feedback = dialog->findChild<QDialog*>(
                                  QStringLiteral("bench-preparation-feedback"))) != nullptr,
                             5'000);
    auto* feedback_summary =
        feedback->findChild<QLabel*>(QStringLiteral("bench-preparation-feedback-summary"));
    auto* feedback_table =
        feedback->findChild<QTreeWidget*>(QStringLiteral("bench-preparation-feedback-table"));
    QVERIFY(feedback_summary != nullptr);
    QVERIFY(feedback_table != nullptr);
    QCOMPARE(feedback->windowTitle(), QStringLiteral("Apply blocked"));
    QVERIFY(feedback_summary->text().contains(QStringLiteral("Nothing was changed")));
    QCOMPARE(feedback_table->topLevelItemCount(), 1);
    QVERIFY(feedback_table->topLevelItem(0)->text(0).endsWith(QStringLiteral(".flac")));
    QVERIFY(feedback_table->topLevelItem(0)->text(1).contains(
        QStringLiteral("unsupported field mapping")));
    feedback->close();
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
                nullptr);
    QTRY_VERIFY(preview_write_plan->isEnabled());

    // Switching the file scope away and back reconstructs the selected file's
    // exact draft. With both files selected, the background result preview
    // reports the real partial state instead of a staged-count placeholder.
    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(custom_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::partial));
    QCOMPARE(custom_draft.data(Qt::DisplayRole).toString(),
             QStringLiteral("(various · present on 1 of 2 files)"));
    QVERIFY(
        custom_draft.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Complete draft")));
    files->selectionModel()->select(grid_model->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                                 QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    QTRY_COMPARE(custom_draft.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(!first_custom.data(metadata_cell_staged_role).toBool());
    QCOMPARE(
        first_custom.data(metadata_cell_values_role).toStringList(),
        (QStringList{QStringLiteral("first custom value"), QStringLiteral("second custom value")}));

    // A per-file exception can also make previously mixed values converge.
    // The complete preview exposes the resulting exact common value.
    const auto second_title_values =
        grid_model->index(1, *title_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(second_title_values.size(), 1);
    const auto title_draft = aggregate_model->index(*title_row, 2);
    QVERIFY(aggregate_model->setData(title_draft, second_title_values.front(), Qt::EditRole));
    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(title_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::common));
    QCOMPARE(title_draft.data(metadata_cell_values_role).toStringList(), second_title_values);
    QCOMPARE(title_draft.data(Qt::DisplayRole).toString(), second_title_values.front());
    fields->setCurrentIndex(title_draft);
    fields->setFocus();
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(!grid_model->index(0, *title_column).data(metadata_cell_staged_role).toBool());
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(title_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::mixed));

    // Selecting both rows makes the same editor a bulk editor.
    QTRY_VERIFY(aggregate_model->index(*title_row, 1)
                    .data(Qt::DisplayRole)
                    .toString()
                    .contains(QStringLiteral("various")));
    const auto artist_draft = aggregate_model->index(*artist_row, 2);
    fields->setCurrentIndex(artist_draft);
    QTest::mouseClick(edit_values, Qt::LeftButton);
    exact_dialog = nullptr;
    QTRY_VERIFY((exact_dialog = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-exact-values"))) != nullptr);
    exact_table =
        exact_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-exact-values-table"));
    exact_add =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-add"));
    exact_buttons = exact_dialog->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-exact-values-buttons"));
    QVERIFY(exact_table != nullptr);
    QVERIFY(exact_add != nullptr);
    QVERIFY(exact_buttons != nullptr);
    QCOMPARE(exact_table->model()->rowCount(), 0);
    QVERIFY(!exact_buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("Lead Artist"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("Guest Artist"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    QTest::mouseClick(exact_buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-exact-values")) ==
                nullptr);
    const QStringList artist_draft_values{QStringLiteral("Lead Artist"),
                                          QStringLiteral("Guest Artist")};
    QCOMPARE(grid_model->index(0, *artist_column).data(metadata_cell_values_role).toStringList(),
             artist_draft_values);
    QCOMPARE(grid_model->index(1, *artist_column).data(metadata_cell_values_role).toStringList(),
             artist_draft_values);
    QCOMPARE(artist_draft.data(metadata_cell_values_role).toStringList(), artist_draft_values);
    QVERIFY(summary->text().contains(QStringLiteral("2 staged changes")));
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(!grid_model->index(0, *artist_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *artist_column).data(metadata_cell_staged_role).toBool());

    const auto genre_draft = aggregate_model->index(*genre_row, 2);
    fields->setCurrentIndex(genre_draft);
    fields->setFocus();
    QTest::keyClick(fields, Qt::Key_F2);
    QLineEdit* aggregate_editor = nullptr;
    QTRY_VERIFY((aggregate_editor = fields->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(aggregate_editor, QStringLiteral("Rock"));
    QTest::keyClick(aggregate_editor, Qt::Key_Return);
    QTRY_COMPARE(genre_draft.data(Qt::DisplayRole).toString(), QStringLiteral("Rock"));
    QCOMPARE(grid_model->index(0, *genre_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Rock")}));
    QCOMPARE(grid_model->index(1, *genre_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Rock")}));
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(!grid_model->index(0, *genre_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *genre_column).data(metadata_cell_staged_role).toBool());

    // Delete and revert are also scoped by the file list.
    const auto date_file = grid_model->index(0, *date_column)
                                   .data(metadata_cell_baseline_values_role)
                                   .toStringList()
                                   .isEmpty()
                               ? 1
                               : 0;
    const auto other_file = 1 - date_file;
    files->selectionModel()->select(grid_model->index(date_file, 0),
                                    QItemSelectionModel::ClearAndSelect |
                                        QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    const auto date_draft = aggregate_model->index(*date_row, 2);
    fields->setCurrentIndex(date_draft);
    fields->selectionModel()->select(date_draft, QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
    QTRY_VERIFY(remove_field->isEnabled());
    QTest::mouseClick(remove_field, Qt::LeftButton);
    QTRY_VERIFY(
        grid_model->index(date_file, *date_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(other_file, *date_column).data(metadata_cell_staged_role).toBool());
    QCOMPARE(date_draft.data(Qt::DisplayRole).toString(), QStringLiteral("(remove)"));
    QTest::keyClick(fields, Qt::Key_Backspace, Qt::ControlModifier);
    releaseHeldModifiers(fields);
    QTRY_VERIFY(
        !grid_model->index(date_file, *date_column).data(metadata_cell_staged_role).toBool());

    files->clearSelection();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{0U});
    QTRY_VERIFY(!add_field->isEnabled());
    QVERIFY(!remove_field->isEnabled());
    QTRY_VERIFY(!edit_values->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("0 of 2 files selected")));

    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(add_field->isEnabled());
    QVERIFY(edit_values->isEnabled());
    QVERIFY(!summary->text().contains(QStringLiteral("staged change")));
    QVERIFY(!discard->isEnabled());
    // ADR-0221: the tagger is closed as a window. Asking the tab strip to
    // close its current tab would now close a list, not the editor.
    QPointer<MetadataPropertiesDialog> lifetime = dialog;
    QVERIFY(dialog->close());
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(tabs->count(), 1);
}

void BenchMainWindowTest::metadataPropertiesArtworkSectionShowsProvenanceAndCapabilities() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("artwork.flac"));
    const auto cover_path = media.filePath(QStringLiteral("cover.jpg"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), media_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"), cover_path));

    const auto encoded = QFile::encodeName(media_path);
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
        .track_label = QStringLiteral("Artwork fixture"),
    };

    auto* properties = new MetadataPropertiesDialog(
        2U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < 2U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    QCOMPARE(sections->tabText(0), QStringLiteral("Fields"));
    QCOMPARE(sections->tabText(1), QStringLiteral("Artwork"));
    QCOMPARE(sections->currentIndex(), 0);

    auto* artwork =
        properties->findChild<QWidget*>(QStringLiteral("bench-metadata-artwork-section"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-artwork-status"));
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* issues =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-issues"));
    auto* progress =
        properties->findChild<QProgressBar*>(QStringLiteral("bench-metadata-artwork-progress"));
    QVERIFY(artwork != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(items != nullptr);
    QVERIFY(issues != nullptr);
    QVERIFY(progress != nullptr);
    QCOMPARE(items->model()->rowCount(), 0);
    QVERIFY(!progress->isVisible());
    QVERIFY(artwork->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save")) !=
            nullptr);
    QVERIFY(artwork->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-discard")) !=
            nullptr);
    for (auto* button : artwork->findChildren<QPushButton*>()) {
        QVERIFY(!button->isEnabled());
    }

    sections->setCurrentIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 2, 5'000);
    QVERIFY(status->text().contains(QStringLiteral("2 images")));

    // Thumbnails plus compact columns: File, Role, Image, Source. Exact
    // fingerprints, native types, and ordinals live in tooltips.
    QCOMPARE(items->model()->headerData(1, Qt::Horizontal).toString(), QStringLiteral("File"));
    QCOMPARE(items->model()->headerData(2, Qt::Horizontal).toString(), QStringLiteral("Role"));
    QCOMPARE(items->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Image"));
    QCOMPARE(items->model()->headerData(4, Qt::Horizontal).toString(), QStringLiteral("Source"));
    QVERIFY(items->model()->index(0, 0).data(Qt::DecorationRole).value<QImage>().isNull() == false);
    QVERIFY(items->model()->index(1, 0).data(Qt::DecorationRole).value<QImage>().isNull() == false);
    QVERIFY(
        items->model()->index(0, 1).data().toString().contains(QStringLiteral("2 occurrences")));
    QCOMPARE(items->model()->index(0, 2).data().toString(), QStringLiteral("Other"));
    QVERIFY(
        items->model()->index(0, 3).data().toString().startsWith(QStringLiteral("PNG · 64 × 64")));
    QCOMPARE(items->model()->index(0, 4).data().toString(), QStringLiteral("Embedded"));
    QCOMPARE(items->model()->index(1, 2).data().toString(), QStringLiteral("Front"));
    QVERIFY(
        items->model()->index(1, 3).data().toString().startsWith(QStringLiteral("JPEG · 8 × 6")));
    QCOMPARE(items->model()->index(1, 4).data().toString(), QStringLiteral("External"));
    QVERIFY(items->model()
                ->index(0, 3)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("SHA-256:")));
    QVERIFY(items->model()
                ->index(1, 4)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("cover.jpg")));

    // No mutation service is wired in this fixture, so the file reports as
    // view-only in the problems pane instead of a permanent capability table.
    QTRY_COMPARE(issues->model()->rowCount(), 1);
    QCOMPARE(issues->model()->index(0, 2).data().toString(), QStringLiteral("View-only"));
    QVERIFY(status->text().contains(QStringLiteral("1 view-only")));

    QPointer guard{properties};
    properties->close();
    QTRY_VERIFY(guard.isNull());
}

void BenchMainWindowTest::metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes_data() {
    // ADR-0237: the artwork shown, reviewed and written by the engine holding
    // the file, or by this process -- the same section, the same result.
    QTest::addColumn<bool>("through_engine");
    QTest::newRow("engine") << true;
    QTest::newRow("this-process") << false;
}

void BenchMainWindowTest::metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes() {
    QFETCH(bool, through_engine);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("artwork-remove.flac"));
    const auto cover_path = media.filePath(QStringLiteral("cover.jpg"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), media_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"), cover_path));
    const auto encoded = QFile::encodeName(media_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto initial = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(initial.has_value());
    const metadata::ArtworkWritePlanIntent duplicate_role{
        .occurrence_index = 0,
        .raw_media_path = raw_path,
        .expected_media_revision = initial->media_revision,
        .target_ordinal = 0,
        .expected_target_fingerprint = {},
        .kind = metadata::ArtworkWritePlanIntentKind::add,
        .replacement_raw_path = QFile::encodeName(cover_path).toStdString(),
        .added_role = initial->items.front().role,
        .added_description = {},
        .replacement_embedded_source = std::nullopt};
    const auto seed_plan = metadata::revalidate_artwork_write_plan({duplicate_role});
    QVERIFY(seed_plan && seed_plan->ready());
    auto seed_journal = persistence::SqliteMetadataOperationJournal::open(
        std::filesystem::path{media.filePath(QStringLiteral("seed.sqlite3")).toStdString()});
    QVERIFY(seed_journal.has_value());
    QVERIFY(operations::commit_artwork_source(
                seed_plan->sources.front(), *seed_journal,
                [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; })
                .has_value());
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Artwork remove fixture"),
    };
    const auto database_path =
        std::filesystem::path{media.filePath(QStringLiteral("artwork.sqlite3")).toStdString()};
    std::optional<operations::ArtworkApplyResult> observed;
    const auto work = through_engine
                          ? std::make_shared<engine::RemoteFileWork>(protocol::Endpoint{
                                .socket = QFile::encodeName(engine_.socket()).toStdString(),
                                .host = {},
                                .port = 0,
                                .token = {}})
                          : nullptr;
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, {},
        work ? engineFileWorkTools(work) : FileWorkTools{});
    properties->setArtworkMutationServices(
        work ? ArtworkWritePlanApplierFactory{[work] {
            return ArtworkWritePlanApplier{
                [work](const metadata::ArtworkWritePlan& plan,
                       const operations::ArtworkApplyProgressCallback& progress,
                       const core::CancellationToken& cancellation) {
                    return work->artwork_apply(plan, progress, cancellation);
                }};
        }}
             : ArtworkWritePlanApplierFactory{[database_path] {
                   return ArtworkWritePlanApplier{
                       [database_path](const metadata::ArtworkWritePlan& plan,
                                       const operations::ArtworkApplyProgressCallback& progress,
                                       const core::CancellationToken& cancellation)
                           -> core::Result<operations::ArtworkApplyResult> {
                           auto opened =
                               persistence::SqliteMetadataOperationJournal::open(database_path);
                           if (!opened) {
                               return std::unexpected(std::move(opened.error()));
                           }
                           auto journal = std::move(*opened);
                           return operations::apply_artwork_write_plan(
                               plan,
                               [&journal](const metadata::ArtworkWritePlanSource& source_plan,
                                          const core::CancellationToken& source_cancellation) {
                                   return operations::commit_artwork_source(
                                       source_plan, journal,
                                       [](const operations::MetadataCommitResult&)
                                           -> core::Result<void> { return {}; },
                                       source_cancellation);
                               },
                               progress, cancellation,
                               operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                       }};
               }},
        [&observed](const operations::ArtworkApplyResult& result) { observed = result; });
    properties->show();

    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    sections->setCurrentIndex(1);
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* replace =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-replace"));
    auto* remove =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-remove"));
    auto* add = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-add"));
    auto* copy = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-copy"));
    QVERIFY(items != nullptr);
    QVERIFY(replace != nullptr);
    QVERIFY(remove != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(copy != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 3, 5'000);
    QVERIFY(add->isEnabled());
    items->selectRow(2);
    QTRY_VERIFY(!replace->isEnabled());
    QVERIFY(!remove->isEnabled());
    QVERIFY(copy->isEnabled());
    items->clearSelection();
    items->selectRow(0);
    QTRY_VERIFY(replace->isEnabled());
    QTRY_VERIFY(remove->isEnabled());
    QVERIFY(!copy->isEnabled());
    items->selectAll(); // Including an external image must not block embedded removal.
    QTRY_VERIFY(remove->isEnabled());
    QTest::mouseClick(remove, Qt::LeftButton);
    auto* pending =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    auto* discard =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-discard"));
    QVERIFY(pending != nullptr);
    QVERIFY(discard != nullptr);
    QCOMPARE(pending->model()->rowCount(), 2);
    auto* undo_selected =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-undo-selected"));
    QVERIFY(undo_selected != nullptr);
    pending->selectRow(0);
    QTRY_VERIFY(undo_selected->isEnabled());
    QTest::mouseClick(undo_selected, Qt::LeftButton);
    QCOMPARE(pending->model()->rowCount(), 1);
    QTest::mouseClick(remove, Qt::LeftButton);
    QCOMPARE(pending->model()->rowCount(), 2);
    const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
    if (!screenshot_dir.isEmpty()) {
        properties->resize(1200, 850);
        QVERIFY(properties->grab().save(screenshot_dir + QStringLiteral("/artwork-pending.png")));
    }
    bool warned_about_draft = false;
    QTimer::singleShot(0, properties, [&] {
        if (auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            warned_about_draft = warning->windowTitle().contains(QStringLiteral("artwork"));
            warning->done(QMessageBox::Cancel);
        }
    });
    QTest::keyClick(properties, Qt::Key_Escape);
    QVERIFY(warned_about_draft);
    QVERIFY(properties->isVisible());
    QCOMPARE(pending->model()->rowCount(), 2);
    const auto before_save = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(before_save.has_value());
    QCOMPARE(before_save->items.size(), 3U);
    QTest::mouseClick(discard, Qt::LeftButton);
    QCOMPARE(pending->model()->rowCount(), 0);
    QTest::mouseClick(remove, Qt::LeftButton);

    // Removal is staged until Save artwork; the external cover is retained.
    auto* save = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    QVERIFY(save != nullptr);
    QTRY_VERIFY(save->isEnabled());
    QVERIFY(!observed.has_value());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    const auto apply_issue = observed->sources.front().issue
                                 ? QString::fromStdString(observed->sources.front().issue->message)
                                 : QStringLiteral("no per-source issue");
    QVERIFY2(observed->committed_source_count() == 1U, qPrintable(apply_issue));
    QCOMPARE(observed->sources.front().commit->occurrence_indexes, (std::vector<std::size_t>{0U}));
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 1, 5'000);
    QCOMPARE(items->model()->index(0, 4).data().toString(), QStringLiteral("External"));
    QVERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
            nullptr);
    const auto inventory = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(inventory.has_value());
    QCOMPARE(inventory->items.size(), 1U);
    QCOMPARE(inventory->items.front().provenance, metadata::ArtworkProvenance::external);

    // Supply a local image after removing every embedded cover.
    observed.reset();
    QTimer chooser;
    chooser.setInterval(10);
    int file_choices = 0;
    int role_choices = 0;
    connect(&chooser, &QTimer::timeout, properties, [&] {
        if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
            if (file_choices == 0) {
                // selectFile() can leave the focused filename edit empty in
                // an already-visible dialog. Enter the path as a user would.
                auto* filename = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
                QVERIFY(filename != nullptr);
                filename->setText(cover_path);
                ++file_choices;
            } else {
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        } else if (auto* role = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            role->accept();
            ++role_choices;
        }
    });
    QTimer chooser_timeout;
    chooser_timeout.setSingleShot(true);
    connect(&chooser_timeout, &QTimer::timeout, properties, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
    });
    const auto used_native_dialogs = !QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    chooser_timeout.start(5000);
    chooser.start();
    QTest::mouseClick(add, Qt::LeftButton);
    chooser_timeout.stop();
    chooser.stop();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, !used_native_dialogs);
    QCOMPARE(file_choices, 1);
    QCOMPARE(role_choices, 1);
    QCOMPARE(pending->model()->rowCount(), 1);
    auto* files = properties->fileListView();
    QVERIFY(files != nullptr);
    QVERIFY2(files->height() >= 120, "adding an artwork draft must not collapse the file list");
    QVERIFY(!observed.has_value());
    QTRY_VERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    QCOMPARE(observed->committed_source_count(), 1U);
    const auto supplied = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(supplied.has_value());
    QCOMPARE(supplied->items.size(), 2U);
    QCOMPARE(supplied->items.front().role, metadata::ArtworkRole::front);
    QCOMPARE(supplied->items.front().provenance, metadata::ArtworkProvenance::embedded);

    QPointer guard{properties};
    properties->close();
    QTRY_VERIFY(guard.isNull());
}

void BenchMainWindowTest::cueSheetsExpandIntoPersistentSegmentRows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto wave_path = media.filePath(QStringLiteral("disc.wav"));
    const auto cue_path = media.filePath(QStringLiteral("album.cue"));
    write_wave(wave_path, wave_sample_rate * 2U);
    {
        std::ofstream cue{QFile::encodeName(cue_path).toStdString(), std::ios::binary};
        cue << "REM DATE 2026\n"
               "PERFORMER \"Cue Artist\"\n"
               "TITLE \"Cue Album\"\n"
               "FILE \"disc.wav\" WAVE\n"
               "TRACK 01 AUDIO\n"
               "TITLE \"First Cue Track\"\n"
               "INDEX 01 00:00:00\n"
               "TRACK 02 AUDIO\n"
               "TITLE \"Second Cue Track\"\n"
               "INDEX 01 00:01:00\n";
    }
    const auto folder_encoded = QFile::encodeName(media.path());
    const std::string raw_folder{folder_encoded.constData(),
                                 static_cast<std::size_t>(folder_encoded.size())};
    const auto canonical_wave =
        std::filesystem::canonical(
            std::filesystem::path{QFile::encodeName(wave_path).toStdString()})
            .native();

    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_folder});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        // The referenced disc.wav is represented by its two cue tracks, not
        // by an additional whole-file row from the same folder scan.
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        QTRY_COMPARE(view->rowHeight(0), view->verticalHeader()->defaultSectionSize() +
                                             ui::QueueItemDelegate::album_header_height);
        QCOMPARE(view->rowHeight(1), view->verticalHeader()->defaultSectionSize());
        const auto& rows = model->rows();
        QCOMPARE(rows[0].raw_path, canonical_wave);
        QCOMPARE(rows[1].raw_path, canonical_wave);
        QVERIFY(rows[0].logical_reference.has_value());
        QVERIFY(rows[1].logical_reference.has_value());
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(rows[0].segment.has_value());
        QVERIFY(rows[1].segment.has_value());
        QCOMPARE(rows[0].segment->start_sample, 0);
        QCOMPARE(rows[0].segment->end_sample, std::optional<std::int64_t>{44'100});
        QCOMPARE(rows[1].segment->start_sample, 44'100);
        QCOMPARE(rows[1].segment->end_sample, std::optional<std::int64_t>{88'200});
        QCOMPARE(rows[0].title, std::string{"First Cue Track"});
        QCOMPARE(rows[1].title, std::string{"Second Cue Track"});
        QCOMPARE(rows[0].artist, std::string{"Cue Artist"});
        QCOMPARE(rows[0].album, std::string{"Cue Album"});
        QCOMPARE(rows[0].date, std::string{"2026"});
        QCOMPARE(rows[0].duration_ms, std::optional<std::int64_t>{1'000});
        QCOMPARE(rows[0].metadata.first_effective_value("title"),
                 std::optional<std::string>{"First Cue Track"});
        QCOMPARE(rows[0].metadata.first_effective_value("album_artist"),
                 std::optional<std::string>{"Cue Artist"});
        QVERIFY(rows[0].source_revision.has_value());
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    auto* restored_view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(restored_view != nullptr);
    QTRY_COMPARE(restored_view->rowHeight(0),
                 restored_view->verticalHeader()->defaultSectionSize() +
                     ui::QueueItemDelegate::album_header_height);
    QVERIFY(model->rows()[0].segment.has_value());
    QCOMPARE(model->rows()[0].segment->end_sample, std::optional<std::int64_t>{44'100});
    QVERIFY(model->rows()[1].logical_reference.has_value());
    QCOMPARE(model->rows()[0].metadata.first_effective_value("title"),
             std::optional<std::string>{"First Cue Track"});
    QCOMPARE(model->rows()[1].title, std::string{"Second Cue Track"});
}

void BenchMainWindowTest::containerChaptersExpandIntoPersistentSegmentRows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto container_path = media.filePath(QStringLiteral("chaptered.mka"));
    QVERIFY(
        materialize_audio_fixture(QStringLiteral("container-chapters-mka.b64"), container_path));
    const auto encoded = QFile::encodeName(container_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

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
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        const auto& rows = model->rows();
        QCOMPARE(rows[0].raw_path, raw_path);
        QCOMPARE(rows[1].raw_path, raw_path);
        QVERIFY(rows[0].logical_reference.has_value());
        QVERIFY(rows[1].logical_reference.has_value());
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(rows[0].segment.has_value());
        QVERIFY(rows[1].segment.has_value());
        QCOMPARE(rows[0].segment->start_sample, 0);
        QCOMPARE(rows[0].segment->end_sample, std::optional<std::int64_t>{4'800});
        QCOMPARE(rows[1].segment->start_sample, 4'800);
        QCOMPARE(rows[1].segment->end_sample, std::optional<std::int64_t>{9'600});
        QCOMPARE(rows[0].title, std::string{"First chapter"});
        QCOMPARE(rows[1].title, std::string{"Second chapter"});
        QCOMPARE(rows[0].artist, std::string{"First Artist"});
        QCOMPARE(rows[1].artist, std::string{"Album Artist"});
        QCOMPARE(rows[0].album, std::string{"Chapter Album"});
        QCOMPARE(rows[0].album_artist, std::string{"Album Artist"});
        QCOMPARE(rows[0].date, std::string{"2026"});
        QCOMPARE(rows[0].track_number, std::string{"1"});
        QCOMPARE(rows[1].track_number, std::string{"2"});
        QCOMPARE(rows[0].duration_ms, std::optional<std::int64_t>{100});
        QCOMPARE(rows[1].duration_ms, std::optional<std::int64_t>{100});
        QCOMPARE(rows[0].metadata.first_effective_value("artist"),
                 std::optional<std::string>{"First Artist"});
        QCOMPARE(rows[1].metadata.first_effective_value("artist"),
                 std::optional<std::string>{"Album Artist"});
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    QVERIFY(model->rows()[0].segment.has_value());
    QCOMPARE(model->rows()[0].segment->start_sample, 0);
    QCOMPARE(model->rows()[0].segment->end_sample, std::optional<std::int64_t>{4'800});
    QVERIFY(model->rows()[1].logical_reference.has_value());
    QCOMPARE(model->rows()[0].metadata.first_effective_value("title"),
             std::optional<std::string>{"First chapter"});
}

void BenchMainWindowTest::codecNativeSubsongsExpandAndPersistDecoderSelections() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto module_path = media.filePath(QStringLiteral("two-songs.mod"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("two-subsongs-mod.b64"), module_path));
    const auto encoded_folder = QFile::encodeName(media.path());
    const std::string raw_folder{encoded_folder.constData(),
                                 static_cast<std::size_t>(encoded_folder.size())};
    const auto encoded_path = QFile::encodeName(module_path);
    const std::string raw_path{encoded_path.constData(),
                               static_cast<std::size_t>(encoded_path.size())};

    {
        BenchMainWindow window;
        window.show();
        // Folder intake proves tracker extensions participate in bounded
        // discovery rather than working only through explicit file opens.
        window.openLocalPaths({raw_folder});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        const auto& rows = model->rows();
        for (std::size_t index = 0U; index < rows.size(); ++index) {
            QCOMPARE(rows[index].raw_path, raw_path);
            QVERIFY(rows[index].logical_reference.has_value());
            QCOMPARE(rows[index].selection.stream_index, std::optional<int>{0});
            QCOMPARE(rows[index].selection.subsong_index,
                     std::optional<int>{static_cast<int>(index)});
            QVERIFY(rows[index].segment.has_value());
            QCOMPARE(rows[index].segment->start_sample, 0);
            QCOMPARE(rows[index].segment->end_sample, std::optional<std::int64_t>{28'800});
            QCOMPARE(rows[index].title, "Subsong " + std::to_string(index + 1U));
            QCOMPARE(rows[index].album, std::string{"Trackknife subsongs"});
            QCOMPARE(rows[index].duration_ms, std::optional<std::int64_t>{600});
            QCOMPARE(rows[index].metadata.first_effective_value("title"),
                     std::optional<std::string>{"Subsong " + std::to_string(index + 1U)});
        }
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    QCOMPARE(model->rows()[0].selection.subsong_index, std::optional<int>{0});
    QCOMPARE(model->rows()[1].selection.subsong_index, std::optional<int>{1});
    QVERIFY(model->rows()[1].segment.has_value());
    QCOMPARE(model->rows()[1].segment->end_sample, std::optional<std::int64_t>{28'800});
    QCOMPARE(model->rows()[1].metadata.first_effective_value("title"),
             std::optional<std::string>{"Subsong 2"});
}

// Regression for the runaway auto-advance: the player's "ended" state names a
// finished *file*, not a finished list, and persists for several transport
// ticks while the next source loads. Each finished track must advance the
// list exactly one row, and the last row must stay ended without wrapping
// (ADR-0023).
void BenchMainWindowTest::localPlaybackModesPersistAndStayLocal() {
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_VERIFY(tabs->count() >= 1);
        tabs->setCurrentIndex(0);
        auto* repeat = window.findChild<QAction*>(QStringLiteral("action-local-repeat"));
        auto* random = window.findChild<QAction*>(QStringLiteral("action-local-random"));
        auto* single = window.findChild<QAction*>(QStringLiteral("action-local-single"));
        auto* consume = window.findChild<QAction*>(QStringLiteral("action-local-consume"));
        auto* automatic =
            window.findChild<QAction*>(QStringLiteral("action-local-replaygain-auto"));
        auto* rg = window.findChild<QToolButton*>(QStringLiteral("bench-local-replaygain"));
        QVERIFY(repeat && random && single && consume && automatic && rg);
        for (const auto* name : {"bench-local-album-random"}) {
            auto* album_mode = window.findChild<QToolButton*>(QString::fromLatin1(name));
            QVERIFY(album_mode);
            QCOMPARE(album_mode->toolButtonStyle(), Qt::ToolButtonIconOnly);
            QVERIFY(!album_mode->icon().isNull());
            QVERIFY(album_mode->defaultAction()->isCheckable());
            QCOMPARE(album_mode->defaultAction()->text(), QStringLiteral("Album shuffle"));
        }
        QVERIFY(rg->isVisible());
        QVERIFY(!repeat->isChecked());
        repeat->trigger();
        automatic->trigger();
        QTRY_COMPARE(window.property("trackknife-player-replaygain").toInt(), 2);
        random->trigger();
        QTRY_COMPARE(window.property("trackknife-player-replaygain").toInt(), 1);
        QCOMPARE(rg->text(), QStringLiteral("ReplayGain: Automatic"));
        // Icons, not letters, with one-shot marked on the icon itself.
        QVERIFY(!single->icon().isNull() && !consume->icon().isNull());
        const auto plain_single = single->icon().pixmap(22, 22).toImage();
        single->trigger();
        single->trigger();
        QCOMPARE(single->iconText(), QStringLiteral("1×"));
        QVERIFY(single->icon().pixmap(22, 22).toImage() != plain_single);
        consume->trigger();
        consume->trigger();
        QCOMPARE(consume->iconText(), QStringLiteral("C×"));
        QVERIFY(rg->isVisible());
        QVERIFY(repeat->isChecked());
        QVERIFY(random->isChecked());
        QVERIFY(automatic->isChecked());
        // ADR-0138: the preamp dialog is reachable from the ReplayGain menu.
        QVERIFY(window.findChild<QAction*>(QStringLiteral("action-local-replaygain-preamp")) !=
                nullptr);
    }
    BenchMainWindow restored;
    restored.show();
    auto* repeat = restored.findChild<QAction*>(QStringLiteral("action-local-repeat"));
    auto* random = restored.findChild<QAction*>(QStringLiteral("action-local-random"));
    auto* single = restored.findChild<QAction*>(QStringLiteral("action-local-single"));
    auto* consume = restored.findChild<QAction*>(QStringLiteral("action-local-consume"));
    QVERIFY(repeat && random && single && consume);
    QVERIFY(repeat->isChecked());
    QVERIFY(random->isChecked());
    QCOMPARE(single->iconText(), QStringLiteral("1×"));
    QCOMPARE(consume->iconText(), QStringLiteral("C×"));
    QTRY_COMPARE(restored.property("trackknife-player-replaygain").toInt(), 1);
    // ADR-0325: preamps changed in Settings reach the playing engine; a
    // window starting sends none of its own.
    {
        QSettings settings;
        settings.setValue(QStringLiteral("playback/rg-preamp-with"), 4.5);
        settings.setValue(QStringLiteral("playback/rg-preamp-without"), -3.0);
    }
    restored.workspace_.reloadPlaybackPreferences();
    QTRY_COMPARE(restored.property("trackknife-player-rg-preamp-with").toDouble(), 4.5);
    QTRY_COMPARE(restored.property("trackknife-player-rg-preamp-without").toDouble(), -3.0);
}

void BenchMainWindowTest::localPlaybackModesAdvance_data() {
    QTest::addColumn<bool>("repeat");
    QTest::addColumn<bool>("random");
    QTest::addColumn<int>("single");
    QTest::addColumn<int>("consume");
    QTest::addColumn<int>("expected_count");
    QTest::addColumn<bool>("ended");
    QTest::addColumn<int>("track_ms");
    QTest::newRow("single") << false << false << 1 << 0 << 3 << true << 700;
    QTest::newRow("single-once") << false << false << 2 << 0 << 3 << true << 700;
    QTest::newRow("repeat-single") << true << false << 1 << 0 << 3 << false << 700;
    QTest::newRow("repeat-list") << true << false << 0 << 0 << 3 << false << 700;
    QTest::newRow("consume-duplicates") << false << false << 0 << 1 << 0 << true << 700;
    QTest::newRow("consume-once") << false << false << 0 << 2 << 2 << true << 700;
    QTest::newRow("random-cycle") << false << true << 0 << 0 << 3 << true << 700;
    QTest::newRow("repeat-consume-single") << true << false << 1 << 1 << 2 << true << 700;
    QTest::newRow("consume-gapless-duplicates") << false << false << 0 << 1 << 0 << true << 1200;
    QTest::newRow("repeat-single-gapless") << true << false << 1 << 0 << 3 << false << 1200;
}

void BenchMainWindowTest::localPlaybackModesAdvance() {
    MELODY_REQUIRE_REALTIME_AUDIO();
    QFETCH(bool, repeat);
    QFETCH(bool, random);
    QFETCH(int, single);
    QFETCH(int, consume);
    QFETCH(int, expected_count);
    QFETCH(bool, ended);
    QFETCH(int, track_ms);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first = media.filePath(QStringLiteral("a.wav"));
    const auto last = media.filePath(QStringLiteral("b.wav"));
    write_wave(first, wave_sample_rate * static_cast<std::uint32_t>(track_ms) / 1000U);
    write_wave(last, wave_sample_rate * static_cast<std::uint32_t>(track_ms) / 1000U);
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(first).toStdString(),
                           QFile::encodeName(first).toStdString(),
                           QFile::encodeName(last).toStdString()});
    QTableView* view = nullptr;
    const auto find_view = [&] {
        for (auto* candidate : window.findChildren<QTableView*>()) {
            if (qobject_cast<LocalListModel*>(candidate->model()) &&
                candidate->model()->rowCount() == 3) {
                view = candidate;
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY(find_view());
    if (view == nullptr)
        QFAIL("local list view not found");
    auto* model = qobject_cast<LocalListModel*>(view->model());
    const auto trigger = [&](const QString& name, const int count) {
        auto* action = window.findChild<QAction*>(QStringLiteral("action-local-%1").arg(name));
        QVERIFY(action != nullptr);
        for (int i = 0; i < count; ++i) {
            action->trigger();
        }
    };
    trigger(QStringLiteral("repeat"), repeat ? 1 : 0);
    trigger(QStringLiteral("random"), random ? 1 : 0);
    trigger(QStringLiteral("single"), single);
    trigger(QStringLiteral("consume"), consume);
    view->selectionModel()->setCurrentIndex(
        model->index(0, 1), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTest::keyClick(view, Qt::Key_Return);
    // ADR-0226: the engine plays, so its reported status is what is watched.
    const auto status = [&window] {
        return window.property("trackknife-engine-playback").toString();
    };
    QTRY_VERIFY(status() == QStringLiteral("playing") || status() == QStringLiteral("loading"));
    QTest::qWait(track_ms * 3 + 600);
    QTRY_COMPARE(model->rowCount(), expected_count);
    if (ended) {
        QTRY_COMPARE(status(), QStringLiteral("stopped"));
    } else {
        QVERIFY(status() != QStringLiteral("stopped"));
    }
    if (single != 0 && consume == 0) {
        QVERIFY(model->index(0, 0).data(ui::track_current_role).toBool());
        QVERIFY(!model->index(1, 0).data(ui::track_current_role).toBool());
    }
    if (single == 2) {
        QVERIFY(!window.findChild<QAction*>(QStringLiteral("action-local-single"))->isChecked());
    }
    if (consume == 2) {
        QVERIFY(!window.findChild<QAction*>(QStringLiteral("action-local-consume"))->isChecked());
    }
    QVERIFY(QFile::exists(first));
    QVERIFY(QFile::exists(last));
}

void BenchMainWindowTest::localTrackRatingsPersistByContentIdentity() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("rated.wav"));
    write_wave(path, wave_sample_rate / 2U);
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(path).toStdString()});
    QTableView* view = nullptr;
    const auto find_view = [&] {
        for (auto* candidate : window.findChildren<QTableView*>()) {
            if (qobject_cast<LocalListModel*>(candidate->model()) &&
                candidate->model()->rowCount() == 1) {
                view = candidate;
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY(find_view());
    if (view == nullptr) {
        QFAIL("the list's view is found");
    }
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    // The probe projection derives the Melody-compatible content identity.
    QTRY_VERIFY(!model->rows().front().rating_hash.empty());
    QCOMPARE(model->headerData(local_rating_column, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Rating"));
    const auto track_hash = model->rows().front().rating_hash;
    const auto album_hash = model->rows().front().album_rating_hash;
    QVERIFY(!album_hash.empty());

    auto* panel = window.findChild<LocalLibraryPanel*>();
    QVERIFY(panel != nullptr);
    auto* track_menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    QVERIFY(track_menu != nullptr);
    view->selectionModel()->select(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(
        view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, view->visualRect(model->index(0, local_title_column)).center())));
    auto* rate_menu = window.findChild<QMenu*>(QStringLiteral("bench-local-rate-menu"));
    auto* album_rate_menu = window.findChild<QMenu*>(QStringLiteral("bench-local-album-rate-menu"));
    QVERIFY(rate_menu != nullptr);
    QVERIFY(album_rate_menu != nullptr);
    QVERIFY(rate_menu->isEnabled());
    QVERIFY(album_rate_menu->isEnabled());
    auto* four_stars = window.findChild<QAction*>(QStringLiteral("action-local-rate-8"));
    auto* album_three = window.findChild<QAction*>(QStringLiteral("action-local-album-rate-6"));
    QVERIFY(four_stars != nullptr);
    QVERIFY(album_three != nullptr);
    four_stars->trigger();
    album_three->trigger();
    track_menu->close();
    QCOMPARE(model->rows().front().rating, 8U);
    QCOMPARE(model->index(0, local_rating_column).data().toString(), QStringLiteral("★★★★"));

    // Both identities reach the store. It is written by the workspace, not
    // through the panel's queue, so the panel is asked until it has them.
    std::optional<std::vector<unsigned>> stored;
    const auto ask = [&] {
        panel->requestRatings({track_hash, album_hash}, [&stored](std::vector<unsigned> values) {
            stored = std::move(values);
        });
    };
    ask();
    QTRY_VERIFY_WITH_TIMEOUT(
        (stored.has_value() && *stored == std::vector<unsigned>{8U, 6U}) || (ask(), false), 10'000);
    // The store notification reloads rows, filling the album rating painted
    // over the group's cover artwork.
    QTRY_COMPARE(model->rows().front().album_rating, 6U);
    QCOMPARE(model->index(0, local_artwork_column).data(ui::track_album_rating_role).toUInt(), 6U);

    // Unrating deletes the stored value and clears the stars immediately.
    QVERIFY(QMetaObject::invokeMethod(
        view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, view->visualRect(model->index(0, local_title_column)).center())));
    auto* unrate = window.findChild<QAction*>(QStringLiteral("action-local-rate-0"));
    QVERIFY(unrate != nullptr);
    unrate->trigger();
    track_menu->close();
    QCOMPARE(model->rows().front().rating, 0U);
    QCOMPARE(model->index(0, local_rating_column).data().toString(), QString{});
    // As above, the store is asked until the workspace's write reached it.
    stored.reset();
    const auto ask_track = [&] {
        panel->requestRatings(
            {track_hash}, [&stored](std::vector<unsigned> values) { stored = std::move(values); });
    };
    ask_track();
    QTRY_VERIFY_WITH_TIMEOUT((stored.has_value() && *stored == std::vector<unsigned>{0U}) ||
                                 (ask_track(), false),
                             10'000);
}

void BenchMainWindowTest::autoAdvancesOncePerFinishedTrack() {
    MELODY_REQUIRE_REALTIME_AUDIO();
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const std::array names{QStringLiteral("a.wav"), QStringLiteral("b.wav"),
                           QStringLiteral("c.wav")};
    std::vector<std::string> raw_paths;
    for (const auto& name : names) {
        const auto path = media.filePath(name);
        write_wave(path, wave_sample_rate * 2U);
        const auto encoded = QFile::encodeName(path);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(raw_paths);

    QTableView* view = nullptr;
    const QDeadlineTimer view_deadline{5'000};
    while (view == nullptr && !view_deadline.hasExpired()) {
        const auto views = window.findChildren<QTableView*>();
        for (auto* candidate : views) {
            if (qobject_cast<LocalListModel*>(candidate->model()) != nullptr &&
                candidate->model()->rowCount() == 3) {
                view = candidate;
                break;
            }
        }
        if (view == nullptr) {
            QTest::qWait(50);
        }
    }
    QVERIFY(view != nullptr);
    if (view == nullptr) {
        return;
    }
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(std::ranges::all_of(model->rows(),
                                                 [](const LocalTrackRow& row) {
                                                     return row.probed &&
                                                            row.source_revision.has_value();
                                                 }),
                             5'000);
    const auto current = [model](const int row) {
        return model->index(row, 0).data(ui::track_current_role).toBool();
    };

    view->selectionModel()->setCurrentIndex(
        model->index(0, 1), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTest::keyClick(view, Qt::Key_Return);

    // Without a live PipeWire server the load fails and no row ever becomes
    // current; that environment cannot exercise progression.
    const auto deadline = QDeadlineTimer{3'000};
    while (!current(0) && !current(1) && !deadline.hasExpired()) {
        QTest::qWait(50);
    }
    if (!current(0) && !current(1)) {
        QSKIP("live PipeWire playback unavailable");
    }

    // History replay keeps the playing occurrence and the future gapless queue valid.
    const QPersistentModelIndex playing_before_edit{model->index(0, 0)};
    model->removeRowIndexes({2});
    QVERIFY(model->undo());
    model->reorderRows({0, 2}, 3);
    QCOMPARE(playing_before_edit.row(), 1);
    QVERIFY(model->undo());
    QCOMPARE(playing_before_edit.row(), 0);
    QVERIFY(model->redo());
    QVERIFY(model->undo());
    QVERIFY(current(0));
    QVERIFY(model->applyPermutation({2, 1, 0}, QStringLiteral("Reverse list")));
    QCOMPARE(playing_before_edit.row(), 2);
    QVERIFY(model->undo());
    QCOMPARE(playing_before_edit.row(), 0);
    QVERIFY(current(0));

    // Editing another album must not discard the playing occurrence or the
    // already queued successor. Exercise the same publication notification
    // used by tag and artwork commits while the first track is playing.
    const auto& tagged = model->rows()[2];
    QVERIFY(tagged.source_revision.has_value());
    const auto refreshed =
        model->applyCommittedMetadata(tagged.raw_path, tagged.metadata, *tagged.source_revision);
    QVERIFY(refreshed.has_value());
    QCOMPARE(*refreshed, 1U);

    // Each two-second track advances exactly one row, and transitions are
    // gapless: the player never reports "ended" (7) between tracks — that
    // state may only appear once the final track finishes.
    bool ended_between_tracks = false;
    {
        const QDeadlineTimer advance_deadline{5'000};
        int last_state = -1;
        int ticks = 0;
        while (!current(1) && !advance_deadline.hasExpired()) {
            const auto state = window.property("trackknife-player-state").toInt();
            ended_between_tracks = ended_between_tracks || state == 7;
            if (state != last_state || ++ticks % 20 == 0) {
                qInfo() << "player state" << state << "position"
                        << window.property("trackknife-player-position").toLongLong() << "buffered"
                        << window.property("trackknife-player-buffered").toLongLong() << "callbacks"
                        << window.property("trackknife-player-callbacks").toLongLong() << "output"
                        << window.property("trackknife-player-outputstate").toInt();
                last_state = state;
            }
            QTest::qWait(50);
        }
    }
    QVERIFY(current(1));
    QTest::qWait(300);
    QVERIFY(current(1));
    QVERIFY(!current(2));
    {
        const QDeadlineTimer second_deadline{5'000};
        while (!current(2) && !second_deadline.hasExpired()) {
            ended_between_tracks =
                ended_between_tracks || window.property("trackknife-player-state").toInt() == 7;
            QTest::qWait(50);
        }
    }
    QVERIFY(current(2));
    QVERIFY(!ended_between_tracks);

    // The end of the list stays ended: no wrap-around back to the first row.
    QTest::qWait(2'700);
    QVERIFY(current(2));
    QVERIFY(!current(0));
}

// ADR-0286: a tab's rows are its only copy now: kept with the window's data,
// moved there once from the cache they were kept in, and a file that cannot
// be read set aside and said -- never taken for an empty tab.
void BenchMainWindowTest::theTabStoreKeepsWhatItCannotRead() {
    const auto cache =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/tabs");
    QDir{TabStore::defaultDirectory()}.removeRecursively();
    QDir{cache}.removeRecursively();
    QVERIFY(TabStore::defaultDirectory() != cache);
    const auto id = core::StableId::random();
    persistence::ListItem item;
    item.source = persistence::ListSource::local;
    item.source_reference = "/music/kept.flac";
    {
        TabStore old{cache};
        old.writeRows(persistence::ListDocument{.id = id,
                                                .kind = persistence::ListKind::scratch,
                                                .name = "Kept",
                                                .pinned = false,
                                                .dirty = false,
                                                .items = {item},
                                                .engine = {}});
        old.flush();
    }
    QVERIFY(QFile::exists(cache + QStringLiteral("/") + QString::fromStdString(id.to_string()) +
                          QStringLiteral(".tabcache")));
    TabStore store;
    QCOMPARE(store.directory(), TabStore::defaultDirectory());
    QVERIFY(!QFile::exists(cache + QStringLiteral("/") + QString::fromStdString(id.to_string()) +
                           QStringLiteral(".tabcache")));
    const auto moved = store.readRows(id);
    QVERIFY(moved.has_value() && moved->items.size() == 1U &&
            moved->items.front().source_reference == "/music/kept.flac");
    QVERIFY(store.takeUnreadable().empty());

    // Damaged: set aside, named, and nothing read.
    const auto broken = core::StableId::random();
    const auto broken_path = TabStore::defaultDirectory() + QStringLiteral("/") +
                             QString::fromStdString(broken.to_string()) +
                             QStringLiteral(".tabcache");
    {
        QFile file{broken_path};
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a tab at all");
    }
    QVERIFY(!store.readRows(broken).has_value());
    QVERIFY(!QFile::exists(broken_path));
    const auto unreadable = store.takeUnreadable();
    QCOMPARE(unreadable.size(), std::size_t{1});
    QVERIFY(unreadable.front().startsWith(broken_path + QStringLiteral(".unreadable-")));
    QVERIFY(QFile::exists(unreadable.front()));
    // A tab with no file is just that: nothing set aside.
    QVERIFY(!store.readRows(core::StableId::random()).has_value());
    QVERIFY(store.takeUnreadable().empty());

    // Backed up and restored with the rest, the tabs restored over set aside.
    QTemporaryDir backup;
    QVERIFY(backup.isValid());
    QVERIFY(store.copyTo(backup.filePath(QStringLiteral("w.tabs"))));
    QVERIFY(QFile::exists(backup.filePath(QStringLiteral("w.tabs/") +
                                          QString::fromStdString(id.to_string()) +
                                          QStringLiteral(".tabcache"))));
    QDir{TabStore::defaultDirectory()}.removeRecursively();
    QVERIFY(TabStore::restoreFrom(backup.filePath(QStringLiteral("w.tabs"))));
    QVERIFY(QFile::exists(TabStore::defaultDirectory() + QStringLiteral("/") +
                          QString::fromStdString(id.to_string()) + QStringLiteral(".tabcache")));
    QDir{TabStore::defaultDirectory()}.removeRecursively();
}

} // namespace trackknife::bench
