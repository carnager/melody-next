// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

namespace trackknife::bench {

void BenchMainWindowTest::metadataStartupPresentsReconciliation() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base));
    const auto database_path = std::filesystem::path{
        QFile::encodeName(base + QStringLiteral("/lists.sqlite")).toStdString()};
    {
        auto journal = persistence::SqliteMetadataOperationJournal::open(database_path);
        QVERIFY(journal.has_value());
        const auto id = core::StableId::random();
        const core::LocalSourceRevision revision{.device = 1,
                                                 .inode = 2,
                                                 .size = 3,
                                                 .modification_time_seconds = 4,
                                                 .modification_time_nanoseconds = 5};
        const operations::MetadataOperationJournalRecord record{
            .id = id,
            .state = operations::MetadataOperationJournalState::planned,
            .source_raw_path = "/music/needs-attention.flac",
            .prepared_raw_path = "/music/.trackknife-prepared",
            .backup_raw_path = "/music/.trackknife-backup",
            .expected_revision = revision,
            .prepared_revision = std::nullopt,
            .published_revision = std::nullopt,
            .occurrence_indexes = {0U},
            .content_kind = operations::MetadataOperationContentKind::text_fields,
            .changes = {operations::MetadataOperationJournalChange{
                .field_index = 0U,
                .canonical_name = "title",
                .property_name = "TITLE",
                .original_present = true,
                .original_values = {"Old"},
                .kind = metadata::StagedMetadataPatchKind::replace_values,
                .planned_values = {"New"},
                .item_indexes = {0U},
                .exact_native_name = std::nullopt,
            }},
            .artwork = std::nullopt,
            .failure = std::nullopt,
        };
        QVERIFY(journal->create(record));
        QVERIFY(journal->transition(
            id, operations::MetadataOperationJournalTransition{
                    .expected_state = operations::MetadataOperationJournalState::planned,
                    .state = operations::MetadataOperationJournalState::needs_reconciliation,
                    .prepared_revision = std::nullopt,
                    .published_revision = std::nullopt,
                    .failure = core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Ambiguous source identity",
                                           .context = {}},
                }));
    }

    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(!window.property("trackknife-metadata-operation-running").toBool());
        QTRY_COMPARE(window.property("trackknife-metadata-reconciliation-count").toULongLong(),
                     1ULL);
        QDialog* dialog = nullptr;
        QTRY_VERIFY((dialog = window.findChild<QDialog*>(
                         QStringLiteral("bench-preparation-feedback"))) != nullptr);
        auto* table =
            dialog->findChild<QTreeWidget*>(QStringLiteral("bench-preparation-feedback-table"));
        QVERIFY(table != nullptr);
        QCOMPARE(table->topLevelItemCount(), 1);
        QVERIFY(
            table->topLevelItem(0)->text(1).contains(QStringLiteral("Ambiguous source identity")));
        dialog->close();
        QTRY_VERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
                    nullptr);
    }

    // A presented incident is acknowledged and does not reopen on later starts.
    BenchMainWindow second;
    second.show();
    QTRY_VERIFY(!second.property("trackknife-metadata-operation-running").toBool());
    QTRY_COMPARE(second.property("trackknife-metadata-reconciliation-count").toULongLong(), 1ULL);
    QVERIFY(second.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
}

void BenchMainWindowTest::filePublicationStartupPresentsReconciliation() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base));
    const auto database_path = std::filesystem::path{
        QFile::encodeName(base + QStringLiteral("/lists.sqlite")).toStdString()};
    {
        auto journal = persistence::SqliteFilePublicationJournal::open(database_path);
        QVERIFY(journal.has_value());
        const auto id = core::StableId::random();
        const core::LocalSourceRevision revision{.device = 1U,
                                                 .inode = 2U,
                                                 .size = 3U,
                                                 .modification_time_seconds = 4,
                                                 .modification_time_nanoseconds = 5};
        const operations::FilePublicationJournalRecord record{
            .id = id,
            .state = operations::FilePublicationJournalState::planned,
            .publication = operations::OutputPathPublicationKind::same_filesystem_rename,
            .source_raw_path = "/music/original.flac",
            .target_raw_path = "/music/renamed.flac",
            .prepared_raw_path = {},
            .expected_source_revision = revision,
            .prepared_revision = std::nullopt,
            .target_revision = std::nullopt,
            .occurrence_indexes = {0U},
            .planned_missing_directory_raw_paths = {},
            .reverses_journal_id = std::nullopt,
            .failure = std::nullopt,
        };
        QVERIFY(journal->create(record));
        QVERIFY(journal->transition(
            id, operations::FilePublicationJournalTransition{
                    .expected_state = operations::FilePublicationJournalState::planned,
                    .state = operations::FilePublicationJournalState::needs_reconciliation,
                    .prepared_revision = std::nullopt,
                    .target_revision = std::nullopt,
                    .failure = core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Ambiguous rename topology",
                                           .context = {}},
                }));
    }

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.property("trackknife-metadata-operation-running").toBool());
    QTRY_COMPARE(window.property("trackknife-metadata-reconciliation-count").toULongLong(), 0ULL);
    QTRY_COMPARE(window.property("trackknife-file-reconciliation-count").toULongLong(), 1ULL);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(
                     QStringLiteral("bench-preparation-feedback"))) != nullptr);
    auto* table =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-preparation-feedback-table"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->topLevelItemCount(), 1);
    QCOMPARE(table->topLevelItem(0)->text(0), QStringLiteral("/music/original.flac"));
    QVERIFY(table->topLevelItem(0)->text(1).contains(QStringLiteral("Ambiguous rename topology")));
    QVERIFY(table->topLevelItem(0)->text(1).contains(QStringLiteral("/music/renamed.flac")));
}

void BenchMainWindowTest::combinedPublicationStartupRecoversMetadataAndPath() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("combined-recovery-source.flac"));
    const auto target_path = media.filePath(QStringLiteral("combined-recovery-target.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto source_encoded = QFile::encodeName(source_path);
    const auto target_encoded = QFile::encodeName(target_path);
    const std::string source_raw{source_encoded.constData(),
                                 static_cast<std::size_t>(source_encoded.size())};
    const std::string target_raw{target_encoded.constData(),
                                 static_cast<std::size_t>(target_encoded.size())};
    const auto source_read = metadata::read_local_metadata(source_raw);
    QVERIFY(source_read.has_value());
    auto selection = metadata::StagedMetadataSelection::create({metadata::StagedMetadataSource{
        .raw_path = source_raw,
        .source_revision = source_read->source_revision,
        .baseline = source_read->document,
    }});
    QVERIFY(selection.has_value());
    const auto title = selection->field_index("title");
    QVERIFY(title.has_value());
    metadata::StagedMetadataPatchSet patches;
    QVERIFY(patches.replace_values(*selection, 0U, *title, {"Recovered combined title"}));
    const auto write_plan = metadata::revalidate_metadata_write_plan(*selection, patches);
    QVERIFY(write_plan && write_plan->ready() && write_plan->sources.size() == 1U);

    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base));
    const auto database_path = std::filesystem::path{
        QFile::encodeName(base + QStringLiteral("/lists.sqlite")).toStdString()};
    const auto operation_id = core::StableId::random();
    {
        auto repository = persistence::ListRepository::open(database_path);
        QVERIFY(repository.has_value());
        const std::vector documents{persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::scratch,
            .name = "Combined recovery",
            .pinned = false,
            .dirty = false,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = source_raw,
                .logical_reference = std::nullopt,
                .segment = std::nullopt,
                .source_selection = std::nullopt,
                .duration_ms = std::nullopt,
                .source_revision = source_read->source_revision,
                .fields = {},
            }},
        }};
        QVERIFY(repository->replace_all(documents));

        const auto prepared =
            metadata::prepare_flac_metadata_write_copy(write_plan->sources.front(), target_raw);
        QVERIFY(prepared.has_value());
        auto journal = persistence::SqliteFilePublicationJournal::open(database_path);
        QVERIFY(journal.has_value());
        const auto prepared_raw =
            operations::file_publication_prepared_path(target_path.toStdString(), operation_id)
                .native();
        QVERIFY(journal->create(operations::FilePublicationJournalRecord{
            .id = operation_id,
            .state = operations::FilePublicationJournalState::planned,
            .publication = operations::OutputPathPublicationKind::same_filesystem_rename,
            .content = operations::FilePublicationContentKind::prepared_destination_artifact,
            .source_raw_path = source_raw,
            .target_raw_path = target_raw,
            .prepared_raw_path = prepared_raw,
            .expected_source_revision = source_read->source_revision,
            .prepared_revision = std::nullopt,
            .target_revision = std::nullopt,
            .occurrence_indexes = {0U},
            .planned_missing_directory_raw_paths = {},
            .reverses_journal_id = std::nullopt,
            .failure = std::nullopt,
        }));
        QVERIFY(journal->transition(
            operation_id, operations::FilePublicationJournalTransition{
                              .expected_state = operations::FilePublicationJournalState::planned,
                              .state = operations::FilePublicationJournalState::target_prepared,
                              .prepared_revision = prepared->prepared_revision,
                              .target_revision = std::nullopt,
                              .failure = std::nullopt,
                          }));
        QVERIFY(journal->transition(
            operation_id,
            operations::FilePublicationJournalTransition{
                .expected_state = operations::FilePublicationJournalState::target_prepared,
                .state = operations::FilePublicationJournalState::target_published,
                .prepared_revision = prepared->prepared_revision,
                .target_revision = prepared->prepared_revision,
                .failure = std::nullopt,
            }));
    }
    // ADR-0237: the engine recovers its journal when it starts, as after the
    // crash this stands for.
    engine_.stop();
    QVERIFY2(engine_.start(), engine_.log().constData());

    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackknife-metadata-operation-running").toBool(),
                             5'000);
    auto* list_model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE(list_model->rowCount(), 1);
    QTRY_COMPARE(list_model->rows().front().raw_path, target_raw);
    QTRY_COMPARE(list_model->rows().front().title, std::string{"Recovered combined title"});
    QVERIFY(!QFile::exists(source_path));
    QVERIFY(QFile::exists(target_path));

    // Successful recovery is silent: no attention window, no history surface.
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) == nullptr);
    QCOMPARE(window.property("trackknife-file-reconciliation-count").toULongLong(), 0ULL);
}

void BenchMainWindowTest::folderDiscoveryAdmitsWave64() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto wave64_path = media.filePath(QStringLiteral("accepted.W64"));
    const auto ignored_path = media.filePath(QStringLiteral("ignored.txt"));
    for (const auto& path : {wave64_path, ignored_path}) {
        QFile file{path};
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("fixture"), 7);
    }
    const auto directory_encoded = QFile::encodeName(media.path());
    const std::string directory_raw{directory_encoded.constData(),
                                    static_cast<std::size_t>(directory_encoded.size())};
    const auto wave64_encoded = QFile::encodeName(wave64_path);
    const std::string wave64_raw{wave64_encoded.constData(),
                                 static_cast<std::size_t>(wave64_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({directory_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 1);
    QCOMPARE(model->rawPath(0), wave64_raw);
}

void BenchMainWindowTest::contextTransfersCreateTabs() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* source = qobject_cast<QTableView*>(tabs->widget(0));
    QVERIFY(source != nullptr);
    auto* model = qobject_cast<LocalListModel*>(source->model());
    QVERIFY(model != nullptr);
    LocalTrackRow row;
    row.raw_path = "/unmounted/é.flac";
    row.title = "Cached title";
    row.probed = true;
    model->appendRows({row, row});
    tabs->setCurrentWidget(source);
    source->selectAll();
    const auto open_menu = [&] {
        window.showTrackContextMenu(source, source->visualRect(model->index(0, 0)).center());
    };
    open_menu();
    auto* copy = window.findChild<QAction*>(QStringLiteral("action-copy-to-new-tab"));
    auto* move = window.findChild<QAction*>(QStringLiteral("action-move-to-new-tab"));
    QVERIFY(copy != nullptr);
    QVERIFY(move != nullptr);
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    copy->trigger();
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(model->rowCount(), 2);
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Copied selection"));
            dialog->accept();
        }
    });
    copy->trigger();
    QCOMPARE(tabs->count(), 2);
    auto* copied =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QCOMPARE(copied->rowCount(), 2);
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(copied->rows()[0].raw_path, row.raw_path);
    QCOMPARE(copied->rows()[1].title, row.title);
    QVERIFY(copied->rows()[0].probed);
    QVERIFY(!copied->rows()[0].source_revision.has_value());
    QVERIFY(tabs->tabText(tabs->currentIndex()).startsWith(QStringLiteral("Copied selection")));
    tabs->setCurrentWidget(source);
    open_menu();
    move = window.findChild<QAction*>(QStringLiteral("action-move-to-new-tab"));
    QVERIFY(move != nullptr);
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Moved selection"));
            dialog->accept();
        }
    });
    move->trigger();
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(model->rowCount(), 0);
    auto* moved =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QCOMPARE(moved->rowCount(), 2);
    QCOMPARE(moved->rows()[1].raw_path, row.raw_path);
}

// Files from a file manager dropped on empty space in the tab bar make a new
// tab of their own, named after the folder; dropped on a tab, they join it.
void BenchMainWindowTest::filesDroppedOnTheTabBarMakeATab() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto album = media.filePath(QStringLiteral("Some Album"));
    QVERIFY(QDir{}.mkpath(album));
    write_wave(album + QStringLiteral("/01.wav"), wave_sample_rate);
    write_wave(album + QStringLiteral("/02.wav"), wave_sample_rate);
    const auto single = media.filePath(QStringLiteral("single.wav"));
    write_wave(single, wave_sample_rate);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    const auto before = tabs->count();
    const auto drop_files = [&](const QString& path, const QPoint position) {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter{dragPosition(position), Qt::CopyAction, &mime, Qt::LeftButton,
                              Qt::NoModifier};
        window.handleTabTrackDrop(nullptr, &enter, position);
        if (!enter.isAccepted()) {
            return false;
        }
        QDropEvent drop{QPointF{position}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
        window.handleTabTrackDrop(nullptr, &drop, position);
        return drop.isAccepted();
    };
    const QPoint empty{tabs->tabBar()->tabRect(tabs->count() - 1).right() + 12,
                       tabs->tabBar()->height() / 2};
    QVERIFY(drop_files(album, empty));
    QCOMPARE(tabs->count(), before + 1);
    auto* made = window.currentListTab();
    QVERIFY(made != nullptr && EngineKey::of(made->document).isLocal());
    QCOMPARE(QString::fromStdString(made->document.name), QStringLiteral("Some Album"));
    QTRY_COMPARE(made->model->rowCount(), 2);

    // Onto a tab: into that one, no new tab.
    auto* first = window.list_tabs_.front().get();
    const auto first_rows = first->model->rowCount();
    QVERIFY(drop_files(single, tabs->tabBar()->tabRect(tabs->indexOf(first->view)).center()));
    QCOMPARE(tabs->count(), before + 1);
    QTRY_COMPARE(first->model->rowCount(), first_rows + 1);
}

void BenchMainWindowTest::tabBarDropsTransferLocalRows() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* source = qobject_cast<QTableView*>(tabs->widget(0));
    auto* model = qobject_cast<LocalListModel*>(source->model());
    LocalTrackRow row;
    row.raw_path = "/unmounted/track.flac";
    row.probed = true;
    model->appendRows({row, row});
    tabs->setCurrentWidget(source);
    source->selectAll();
    QMimeData mime;
    // Synthetic Qt drop events have no source; supply the real source view to
    // the handler used by the tab-bar event filter.
    const auto send_drop = [&](QTableView* from, const QPoint position,
                               const Qt::KeyboardModifiers modifiers, const bool commit) {
        QDragEnterEvent enter{dragPosition(position), Qt::CopyAction | Qt::MoveAction, &mime,
                              Qt::LeftButton, modifiers};
        window.handleTabTrackDrop(from, &enter, position);
        if (!enter.isAccepted() || !commit)
            return enter.isAccepted();
        QDropEvent drop{QPointF{position}, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                        modifiers};
        window.handleTabTrackDrop(from, &drop, position);
        return drop.isAccepted() &&
               drop.dropAction() ==
                   (modifiers.testFlag(Qt::ControlModifier) ? Qt::CopyAction : Qt::MoveAction);
    };
    const auto empty_position = [&] {
        return QPoint{tabs->tabBar()->tabRect(tabs->count() - 1).right() + 12,
                      tabs->tabBar()->height() / 2};
    };
    // Onto its own tab is not a transfer.
    QVERIFY(!send_drop(source, tabs->tabBar()->tabRect(0).center(), Qt::NoModifier, true));
    QVERIFY(!send_drop(source, QPoint{20, tabs->tabBar()->height() + 20}, Qt::NoModifier, true));
    QVERIFY(send_drop(source, empty_position(), Qt::ControlModifier, false));
    QCOMPARE(tabs->count(), 1);
    QVERIFY(send_drop(source, empty_position(), Qt::ControlModifier, true));
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(model->rowCount(), 2);
    auto* copied_view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* copied = qobject_cast<LocalListModel*>(copied_view->model());
    QCOMPARE(copied->rowCount(), 2);
    QCOMPARE(copied->rows()[0].raw_path, row.raw_path);
    QVERIFY(send_drop(source, empty_position(), Qt::NoModifier, true));
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(model->rowCount(), 0);
    auto* moved_view = qobject_cast<QTableView*>(tabs->currentWidget());
    QCOMPARE(moved_view->model()->rowCount(), 2);
    moved_view->selectAll();
    QVERIFY(send_drop(moved_view, tabs->tabBar()->tabRect(tabs->indexOf(copied_view)).center(),
                      Qt::NoModifier, true));
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(copied->rowCount(), 4);
    QCOMPARE(moved_view->model()->rowCount(), 0);
}

void BenchMainWindowTest::contextMenusTargetSelectionsListsAndFolders() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<std::string> raw_paths;
    for (int index = 0; index < 3; ++index) {
        const auto path = media.filePath(QStringLiteral("context-%1.wav").arg(index));
        write_wave(path, wave_sample_rate / 10U);
        const auto encoded = QFile::encodeName(path);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }

    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* duplicate = window.findChild<QAction*>(QStringLiteral("action-duplicate-tab"));
    auto* track_menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    auto* folder_menu = window.findChild<QMenu*>(QStringLiteral("bench-folder-context-menu"));
    auto* folder_view = window.findChild<QTreeView*>(QStringLiteral("bench-folder-tree"));
    auto* folder_model = window.findChild<ui::LocalFolderTreeModel*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(duplicate != nullptr);
    QVERIFY(track_menu != nullptr);
    QVERIFY(folder_menu != nullptr);
    QVERIFY(folder_view != nullptr);
    QVERIFY(folder_model != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    window.openLocalPaths(raw_paths);
    auto* source = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(source != nullptr);
    QTRY_COMPARE(source->model()->rowCount(), 3);

    duplicate->trigger();
    QCOMPARE(tabs->count(), 2);
    auto* destination = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(destination != nullptr);
    QCOMPARE(destination->model()->rowCount(), 3);
    tabs->setCurrentWidget(source);

    source->selectionModel()->select(source->model()->index(0, 0),
                                     QItemSelectionModel::ClearAndSelect |
                                         QItemSelectionModel::Rows);
    source->selectionModel()->select(source->model()->index(1, 0),
                                     QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const auto selected_position = source->visualRect(source->model()->index(1, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, selected_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 2);
    auto* copy_menu = track_menu->findChild<QMenu*>(QStringLiteral("bench-track-copy-menu"));
    auto* move_menu = track_menu->findChild<QMenu*>(QStringLiteral("bench-track-move-menu"));
    auto* play = window.findChild<QAction*>(QStringLiteral("action-play-selected-track"));
    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    auto* remove = window.findChild<QAction*>(QStringLiteral("action-remove-selected-tracks"));
    QVERIFY(copy_menu != nullptr);
    QVERIFY(move_menu != nullptr);
    QVERIFY(play != nullptr);
    QVERIFY(properties != nullptr);
    QVERIFY(remove != nullptr);
    auto* convert = window.findChild<QAction*>(QStringLiteral("action-convert-files"));
    QVERIFY(convert != nullptr);
    QVERIFY(play->isEnabled());
    QVERIFY(properties->isEnabled());
    QVERIFY(convert->isEnabled());
    QMenu* tools = nullptr;
    for (auto* action : track_menu->actions())
        if (action->text() == QStringLiteral("Tools"))
            tools = action->menu();
    QVERIFY(tools != nullptr);
    QVERIFY(tools->actions().contains(properties));
    QVERIFY(tools->actions().contains(convert));
    QVERIFY(!track_menu->actions().contains(properties));
    QVERIFY(!track_menu->actions().contains(convert));
    QVERIFY(remove->isEnabled());
    QCOMPARE(copy_menu->actions().size(), 3);
    QCOMPARE(move_menu->actions().size(), 3);
    copy_menu->actions().back()->trigger();
    QCOMPARE(destination->model()->rowCount(), 5);

    const auto unselected_position = source->visualRect(source->model()->index(2, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, unselected_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(source->selectionModel()->selectedRows().front().row(), 2);

    const auto album_header_position =
        QPoint{source->visualRect(source->model()->index(0, 0)).center().x(),
               source->visualRect(source->model()->index(0, 0)).top() + 5};
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, album_header_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 3);
    track_menu->close();

    const auto directory_encoded = QFile::encodeName(media.path());
    const std::string directory_raw{directory_encoded.constData(),
                                    static_cast<std::size_t>(directory_encoded.size())};
    folder_model->addRoot(directory_raw);
    QModelIndex root;
    for (int row = 0; row < folder_model->rowCount(); ++row) {
        if (folder_model->rawPath(folder_model->index(row, 0)) == directory_raw) {
            root = folder_model->index(row, 0);
        }
    }
    QVERIFY(root.isValid());
    folder_view->scrollTo(root);
    const auto root_position = folder_view->visualRect(root).center();
    QVERIFY(QMetaObject::invokeMethod(folder_view, "customContextMenuRequested",
                                      Qt::DirectConnection, Q_ARG(QPoint, root_position)));
    auto* add_folder = window.findChild<QAction*>(QStringLiteral("action-folder-add-to-list"));
    auto* toggle_folder =
        window.findChild<QAction*>(QStringLiteral("action-folder-toggle-expanded"));
    QVERIFY(add_folder != nullptr);
    QVERIFY(toggle_folder != nullptr);
    QCOMPARE(add_folder->text(), QStringLiteral("Add folder to current list"));
    QCOMPARE(toggle_folder->text(), QStringLiteral("Expand"));
    toggle_folder->trigger();
    QVERIFY(folder_view->isExpanded(root));
    add_folder->trigger();
    QTRY_COMPARE(source->model()->rowCount(), 6);
}

void BenchMainWindowTest::panelLayoutPersistsAndPreservesFutureState() {
    constexpr auto layout_key = "workspace/panel-layout-v1";
    const auto root_widget = [](BenchMainWindow& window) {
        auto* host = window.findChild<QWidget*>(QStringLiteral("bench-panel-layout-host"));
        return host != nullptr && host->layout() != nullptr && host->layout()->count() == 1
                   ? host->layout()->itemAt(0)->widget()
                   : nullptr;
    };

    {
        BenchMainWindow window;
        window.show();
        auto* edit = window.findChild<QAction*>(QStringLiteral("action-edit-panel-layout"));
        auto* vertical = window.findChild<QAction*>(QStringLiteral("action-layout-top-bottom"));
        auto* tabbed = window.findChild<QAction*>(QStringLiteral("action-layout-tabbed"));
        auto* swap = window.findChild<QAction*>(QStringLiteral("action-layout-swap-panels"));
        auto* track_tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(edit != nullptr);
        QVERIFY(vertical != nullptr);
        QVERIFY(tabbed != nullptr);
        QVERIFY(swap != nullptr);
        QVERIFY(track_tabs != nullptr);
        QTRY_COMPARE(track_tabs->count(), 1);
        QCOMPARE(edit->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+L")));

        auto* initial_split = qobject_cast<QSplitter*>(root_widget(window));
        QVERIFY(initial_split != nullptr);
        QCOMPARE(initial_split->orientation(), Qt::Horizontal);
        QCOMPARE(initial_split->widget(0)->objectName(), QStringLiteral("bench-panel-folders"));
        QCOMPARE(initial_split->widget(1)->objectName(), QStringLiteral("bench-track-area"));
        QTRY_VERIFY(initial_split->sizes().at(0) < initial_split->sizes().at(1));
        QVERIFY(!vertical->isEnabled());

        edit->trigger();
        QVERIFY(edit->isChecked());
        QVERIFY(vertical->isEnabled());
        vertical->trigger();
        auto* vertical_split = qobject_cast<QSplitter*>(root_widget(window));
        QVERIFY(vertical_split != nullptr);
        QCOMPARE(vertical_split->orientation(), Qt::Vertical);

        tabbed->trigger();
        auto* panel_tabs = qobject_cast<QTabWidget*>(root_widget(window));
        QVERIFY(panel_tabs != nullptr);
        QCOMPARE(panel_tabs->objectName(), QStringLiteral("bench-panel-layout-tabs"));
        QCOMPARE(panel_tabs->count(), 2);
        QCOMPARE(panel_tabs->tabText(0), QStringLiteral("Sources"));
        QCOMPARE(panel_tabs->tabText(1), QStringLiteral("Track Lists"));
        QCOMPARE(panel_tabs->currentIndex(), 1);

        swap->trigger();
        panel_tabs = qobject_cast<QTabWidget*>(root_widget(window));
        QVERIFY(panel_tabs != nullptr);
        QCOMPARE(panel_tabs->tabText(0), QStringLiteral("Track Lists"));
        QCOMPARE(panel_tabs->tabText(1), QStringLiteral("Sources"));
        QCOMPARE(panel_tabs->currentIndex(), 0);
        QVERIFY(window.close());
    }

    {
        BenchMainWindow restored;
        restored.show();
        auto* panel_tabs = qobject_cast<QTabWidget*>(root_widget(restored));
        QVERIFY(panel_tabs != nullptr);
        QCOMPARE(panel_tabs->tabText(0), QStringLiteral("Track Lists"));
        QCOMPARE(panel_tabs->tabText(1), QStringLiteral("Sources"));
        QCOMPARE(panel_tabs->currentIndex(), 0);

        QSettings settings;
        QString error;
        const auto decoded = ui::deserializePanelLayout(
            settings.value(QString::fromLatin1(layout_key)).toByteArray(),
            {QStringLiteral("folders"), QStringLiteral("track-lists")}, &error);
        QVERIFY2(decoded.has_value(), qPrintable(error));
        QCOMPARE(decoded->root.kind, ui::PanelLayoutNodeKind::tabs);

        auto* reset = restored.findChild<QAction*>(QStringLiteral("action-reset-panel-layout"));
        QVERIFY(reset != nullptr);
        reset->trigger();
        auto* reset_split = qobject_cast<QSplitter*>(root_widget(restored));
        QVERIFY(reset_split != nullptr);
        QCOMPARE(reset_split->orientation(), Qt::Horizontal);
        QCOMPARE(reset_split->widget(0)->objectName(), QStringLiteral("bench-panel-folders"));
        QVERIFY(restored.close());
    }

    const QByteArray future_layout{
        R"({"schema":99,"root":{"kind":"panel","panel":"future"},"future":{"keep":true}})"};
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(layout_key), future_layout);
        settings.sync();
    }
    {
        BenchMainWindow fallback;
        fallback.show();
        auto* fallback_split = qobject_cast<QSplitter*>(root_widget(fallback));
        QVERIFY(fallback_split != nullptr);
        QCOMPARE(fallback_split->orientation(), Qt::Horizontal);
        QVERIFY(fallback.statusBar()->currentMessage().contains(QStringLiteral("preserved")));
        QVERIFY(fallback.close());
    }
    QSettings settings;
    QCOMPARE(settings.value(QString::fromLatin1(layout_key)).toByteArray(), future_layout);
}

void BenchMainWindowTest::localReorderPreservesVisibleRowGeometry() {
    LocalListModel model;
    const auto make_row = [](std::string path) {
        LocalTrackRow result;
        result.raw_path = std::move(path);
        result.title = result.raw_path;
        result.artist = "Artist";
        result.album = "Album";
        return result;
    };
    model.replaceRows(
        {make_row("/music/one.flac"), make_row("/music/two.flac"), make_row("/music/three.flac")});
    QSignalSpy reset{&model, &QAbstractItemModel::modelReset};
    QSignalSpy moved{&model, &QAbstractItemModel::rowsMoved};
    ui::QueueTableView view{nullptr};
    view.setModel(&model);
    view.setItemDelegate(new ui::QueueItemDelegate(&view));
    view.verticalHeader()->setDefaultSectionSize(22);
    view.verticalHeader()->setMinimumSectionSize(18);
    view.setAlbumGroupingEnabled(true);
    view.resize(640, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    model.reorderRows({0}, 3);
    QCoreApplication::processEvents();

    QCOMPARE(reset.count(), 0);
    QCOMPARE(moved.count(), 1);
    QCOMPARE(model.rows()[0].raw_path, std::string{"/music/two.flac"});
    QCOMPARE(model.rows()[1].raw_path, std::string{"/music/three.flac"});
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/one.flac"});
    QCOMPARE(view.verticalHeader()->count(), model.rowCount());
    for (int row_index = 0; row_index < model.rowCount(); ++row_index) {
        QVERIFY(!view.isRowHidden(row_index));
        QVERIFY(view.rowHeight(row_index) >= 18);
        QVERIFY(!view.visualRect(model.index(row_index, local_title_column)).isEmpty());
    }
}

void BenchMainWindowTest::noncontiguousLocalReorderPreservesOccurrences() {
    LocalListModel model;
    const auto make_row = [](std::string path) {
        LocalTrackRow result;
        result.raw_path = std::move(path);
        result.title = result.raw_path;
        return result;
    };
    model.replaceRows({make_row("/music/one.flac"), make_row("/music/two.flac"),
                       make_row("/music/three.flac"), make_row("/music/four.flac")});
    std::vector<std::string> rows_seen_before_move;
    connect(&model, &QAbstractItemModel::layoutAboutToBeChanged, &model, [&] {
        for (const auto& row : model.rows()) {
            rows_seen_before_move.push_back(row.raw_path);
        }
    });
    QSignalSpy reset{&model, &QAbstractItemModel::modelReset};
    QAbstractItemModelTester tester{&model, QAbstractItemModelTester::FailureReportingMode::QtTest};
    const QPersistentModelIndex playing{model.index(2, local_title_column)};
    model.setCurrentSource(model.source(2), 2);

    model.reorderRows({0, 2}, 4);

    QCOMPARE(reset.count(), 0);
    QCOMPARE(rows_seen_before_move,
             (std::vector<std::string>{"/music/one.flac", "/music/two.flac", "/music/three.flac",
                                       "/music/four.flac"}));
    QCOMPARE(model.rows()[0].raw_path, std::string{"/music/two.flac"});
    QCOMPARE(model.rows()[1].raw_path, std::string{"/music/four.flac"});
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/one.flac"});
    QCOMPARE(model.rows()[3].raw_path, std::string{"/music/three.flac"});
    QCOMPARE(playing.row(), 3);
    QVERIFY(model.index(3, 0).data(ui::track_current_role).toBool());
    QVERIFY(model.undo());
    QCOMPARE(playing.row(), 2);
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/three.flac"});
    QVERIFY(model.redo());
    QCOMPARE(playing.row(), 3);
    QCOMPARE(reset.count(), 0);
}

void BenchMainWindowTest::trackListFindActionsFollowActiveTab() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* find = window.findChild<QAction*>(QStringLiteral("action-find-in-list"));
    auto* next = window.findChild<QAction*>(QStringLiteral("action-find-next-in-list"));
    auto* previous = window.findChild<QAction*>(QStringLiteral("action-find-previous-in-list"));
    auto* bar = window.findChild<TrackListFindBar*>();
    QVERIFY(tabs && find && next && previous && bar);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    LocalTrackRow row;
    row.raw_path = "/missing/track.flac";
    row.title = "Find me";
    row.probed = true;
    model->replaceRows({row, row});
    QTRY_VERIFY(find->isEnabled());
    QVERIFY(bar->isHidden());
    view->setFocus();
    QTest::keyClick(view, Qt::Key_F, Qt::ControlModifier);
    releaseHeldModifiers(view);
    QTRY_VERIFY(!bar->isHidden());
    auto* query = bar->findChild<QLineEdit*>(QStringLiteral("bench-list-find-query"));
    QVERIFY(query != nullptr);
    QTRY_VERIFY(query->hasFocus());
    query->setText(QStringLiteral("find me"));
    QTRY_COMPARE(view->currentIndex().row(), 0);
    QTest::keyClick(query, Qt::Key_F3);
    QTRY_COMPARE(view->currentIndex().row(), 1);
    QTest::keyClick(query, Qt::Key_F3, Qt::ShiftModifier);
    releaseHeldModifiers(query);
    QTRY_COMPARE(view->currentIndex().row(), 0);
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Escape);
    QTRY_VERIFY(bar->isHidden());
    find->trigger();
    QVERIFY(!bar->isHidden());

    // Find follows the tab on screen: another list is searched on its own,
    // and the first list's cursor is left where it was.
    auto* other =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Other",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    QVERIFY(other != nullptr);
    QCOMPARE(tabs->currentWidget(), other->view);
    QVERIFY(bar->isHidden());
    QVERIFY(find->isEnabled() && next->isEnabled() && previous->isEnabled());
    LocalTrackRow elsewhere;
    elsewhere.raw_path = "/elsewhere.flac";
    elsewhere.title = "Elsewhere song";
    elsewhere.probed = true;
    other->model->replaceRows({elsewhere});
    other->view->setFocus();
    QTest::keyClick(other->view, Qt::Key_F, Qt::ControlModifier);
    releaseHeldModifiers(other->view);
    QTRY_VERIFY(query->hasFocus());
    query->setText(QStringLiteral("elsewhere song"));
    auto* status = bar->findChild<QLabel*>(QStringLiteral("bench-list-find-status"));
    QVERIFY(status != nullptr);
    QTRY_COMPARE(status->text(), QStringLiteral("Track 1 of 1"));
    QCOMPARE(other->view->currentIndex().row(), 0);
    QCOMPARE(view->currentIndex().row(), 0);
    tabs->setCurrentWidget(view);
    QVERIFY(find->isEnabled());
    QVERIFY(bar->isHidden());
    QCOMPARE(query->text(), QStringLiteral("elsewhere song"));

    view->selectionModel()->select(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(properties_action != nullptr);
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    // ADR-0221: opening the tagger no longer replaces the active tab, so the
    // list stays current and its find actions stay available.
    QTRY_VERIFY(window.findChild<MetadataPropertiesDialog*>(
                    QStringLiteral("bench-metadata-properties")) != nullptr);
    QVERIFY(qobject_cast<MetadataPropertiesDialog*>(tabs->currentWidget()) == nullptr);
    QVERIFY(find->isEnabled());
    QVERIFY(bar->isHidden());
}

void BenchMainWindowTest::localListUndoRestoresOccurrencesAndFreshMetadata() {
    LocalTrackRow whole;
    whole.raw_path = std::string{"/music/raw-"} + static_cast<char>(0xff) + ".flac";
    whole.title = "Original";
    whole.probed = true;
    const core::LocalSourceRevision before{.device = 1,
                                           .inode = 2,
                                           .size = 3,
                                           .modification_time_seconds = 4,
                                           .modification_time_nanoseconds = 5};
    auto published = before;
    published.inode = 6;
    auto relocated = published;
    relocated.inode = 7;
    whole.source_revision = before;
    auto logical = whole;
    logical.logical_reference = "cue-track-2";
    logical.segment = formats::SampleRange{.start_sample = 44100, .end_sample = 88200};
    metadata::MetadataField overlay;
    overlay.canonical_name = "title";
    overlay.native_name = "TITLE";
    overlay.values = {"Logical title"};
    overlay.provenance = metadata::FieldProvenance::sidecar;
    logical.metadata.fields.push_back(overlay);
    LocalTrackRow playing = whole;
    playing.raw_path = "/music/playing.flac";
    LocalListModel model;
    model.replaceRows({whole, logical, playing, whole});
    QAbstractItemModelTester tester{&model, QAbstractItemModelTester::FailureReportingMode::QtTest};
    const QPersistentModelIndex playback{model.index(2, 0)};
    model.setCurrentSource(model.source(2), 2);
    model.removeRowIndexes({3, 1, 0, 1, -1, 99});
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(playback.row(), 0);
    QVERIFY(model.canUndo());
    auto embedded = overlay;
    embedded.values = {"Published title"};
    embedded.provenance = metadata::FieldProvenance::embedded;
    metadata::MetadataDocument document;
    document.fields.push_back(embedded);
    // All instances of this physical file are detached, but history must still follow commits.
    const auto metadata_update = model.applyCommittedMetadata(whole.raw_path, document, published);
    QVERIFY(metadata_update.has_value());
    QCOMPARE(*metadata_update, 0U);
    const auto path_update = model.applyCommittedRelocation(whole.raw_path, "/archive/renamed.flac",
                                                            published, relocated);
    QVERIFY(path_update.has_value());
    QCOMPARE(*path_update, 0U);
    QVERIFY(model.undo());
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(playback.row(), 2);
    QVERIFY(model.index(2, 0).data(ui::track_current_role).toBool());
    QCOMPARE(model.rows()[0].raw_path, std::string{"/archive/renamed.flac"});
    QCOMPARE(model.rows()[0].title, std::string{"Published title"});
    QCOMPARE(model.rows()[0].source_revision, std::optional{relocated});
    QCOMPARE(model.rows()[1].title, std::string{"Logical title"});
    QCOMPARE(model.rows()[1].segment, logical.segment);
    QCOMPARE(model.rows()[1].logical_reference, logical.logical_reference);
    QCOMPARE(model.rows()[3], model.rows()[0]);
    QVERIFY(model.redo());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(playback.row(), 0);
    QVERIFY(model.undo());
    QCOMPARE(model.rows()[0].raw_path, std::string{"/archive/renamed.flac"});
}

void BenchMainWindowTest::localListHistoryBranchesAndBounds() {
    LocalTrackRow first;
    first.raw_path = "/music/a.flac";
    LocalTrackRow second;
    second.raw_path = "/music/b.flac";
    LocalTrackRow third;
    third.raw_path = "/music/c.flac";
    LocalListModel model;
    model.replaceRows({first, second, third});
    model.removeRowIndexes({1});
    model.reorderRows({0}, 2);
    QVERIFY(model.undo());
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, second, third}));
    QVERIFY(model.redo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third}));
    model.reorderRows({0}, 0); // A no-op preserves the redo chain.
    QVERIFY(model.canRedo());
    model.removeRowIndexes({0});
    QVERIFY(!model.canRedo());
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third}));
    model.appendRows({second});
    QCOMPARE(model.undoLabel(), QStringLiteral("Add tracks"));
    QVERIFY(model.canUndo());
    QVERIFY(!model.canRedo());
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third}));
    QVERIFY(model.redo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third, second}));
    model.replaceRows({first, second}, true);
    QCOMPARE(model.undoLabel(), QStringLiteral("Replace list contents"));
    QVERIFY(model.undo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, third, second}));
    QVERIFY(model.redo());
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{first, second}));
    model.removeRowIndexes({0});
    model.removeRowIndexes({0}, false); // Consume and cross-tab moves do not enter local history.
    QVERIFY(!model.canUndo());
    model.replaceRows({first, second});
    QSignalSpy discarded{&model, &LocalListModel::historyDiscarded};
    for (int edit = 0; edit < 101; ++edit)
        model.reorderRows({0}, 2);
    QCOMPARE(discarded.size(), 1);
    int undone = 0;
    while (model.undo())
        ++undone;
    QCOMPARE(undone, 100);
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{second, first}));
    while (model.redo()) {
    }
    QCOMPARE(model.rows(), (std::vector<LocalTrackRow>{second, first}));
}

void BenchMainWindowTest::crossTabMoveUndoIsOneTransaction() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    tabs->setCurrentIndex(0);
    auto* source_view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* source = qobject_cast<LocalListModel*>(source_view->model());
    QVERIFY(source != nullptr);
    LocalTrackRow first;
    first.raw_path = "/music/first.flac";
    LocalTrackRow second;
    second.raw_path = "/music/second.flac";
    source->replaceRows({first, second});
    window.duplicateCurrentTab();
    QCOMPARE(tabs->count(), 2);
    auto* target_view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* target = qobject_cast<LocalListModel*>(target_view->model());
    QVERIFY(target != nullptr && target != source);
    QCOMPARE(target->rowCount(), 2);

    QVERIFY(window.transferRows(source_view, {0},
                                target_view->property("bench-document-id").toString(), true, -1));
    QCOMPARE(source->rowCount(), 1);
    QCOMPARE(target->rowCount(), 3);
    QVERIFY(window.canReplayCrossTabMove(true));
    window.replayListEdit(true);
    QCOMPARE(source->rows(), (std::vector<LocalTrackRow>{first, second}));
    QCOMPARE(target->rows(), (std::vector<LocalTrackRow>{first, second}));
    QVERIFY(window.canReplayCrossTabMove(false));
    window.replayListEdit(false);
    QCOMPARE(source->rows(), (std::vector<LocalTrackRow>{second}));
    QCOMPARE(target->rows(), (std::vector<LocalTrackRow>{first, second, first}));
}

// ADR-0247: Trackknife's own light and dark schemes, chosen in Settings,
// applied at once -- with Fusion, which paints with the palette -- and the
// system's colours back again.
// ADR-0251: the interface size is offered in Settings, stored, and read back
// before the application exists -- through the settings file alone, as the
// next start reads it -- for QT_SCALE_FACTOR; anything unusable is 100%.
void BenchMainWindowTest::interfaceSizeIsChosenForTheNextStart() {
    QSettings{}.remove(QLatin1String(interface_scale_key));
    QCOMPARE(chosenInterfaceScale(), 1.0);
    BenchMainWindow window;
    window.show();
    window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog != nullptr);
    auto* combo = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-interface-scale"));
    QVERIFY(combo != nullptr);
    QCOMPARE(combo->count(), static_cast<int>(interfaceScales().size()));
    QCOMPARE(combo->currentData().toString(), QStringLiteral("1"));
    combo->setCurrentIndex(combo->findData(QStringLiteral("1.5")));
    QPointer<SettingsDialog> lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    QSettings{}.sync();
    QCOMPARE(chosenInterfaceScale(), 1.5);
    QSettings{}.setValue(QLatin1String(interface_scale_key), QStringLiteral("7"));
    QSettings{}.sync();
    QCOMPARE(chosenInterfaceScale(), 1.0);
    QSettings{}.remove(QLatin1String(interface_scale_key));
}

// The header's pills are mixed from the palette; a palette that changes after
// the window was built -- the desktop's theme arriving or turning light, not
// a scheme chosen here -- recolours them rather than leaving dark pills on a
// light window.
void BenchMainWindowTest::headerPillsFollowAPaletteChangedLater() {
    followColorSchemes();
    QApplication::setPalette(darkPalette());
    BenchMainWindow window;
    window.resize(1000, 600);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* up_next = window.findChild<QToolButton*>(QStringLiteral("action-up-next"));
    QVERIFY(up_next != nullptr);
    const auto ground_of = [up_next] {
        const auto image = up_next->grab().toImage();
        return image.pixelColor(image.width() / 2, 3);
    };
    QVERIFY(ground_of().lightness() < 128);
    QApplication::setPalette(lightPalette());
    QCoreApplication::processEvents();
    QTRY_VERIFY(ground_of().lightness() > 128);
    ColorSchemes::instance().applyChosen();
}

// A list tab's close button, clicked, asks for its tab to close -- under
// Trackknife's own style, and without the widget set closable, as the window
// has it: QTabWidget passes its bar's request on only when it is.
void BenchMainWindowTest::listTabCloseButtonsClose() {
    followColorSchemes();
    PlaybackTabWidget tabs;
    tabs.setDocumentMode(true);
    tabs.addTab(new QWidget, QStringLiteral("First"));
    tabs.addTab(new QWidget, QStringLiteral("Second"));
    tabs.resize(700, 120);
    tabs.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabs));
    QSignalSpy requested{&tabs, &QTabWidget::tabCloseRequested};
    auto* close = tabs.tabBar()->tabButton(1, QTabBar::RightSide);
    QVERIFY(close != nullptr && close->isVisible());
    QTest::mouseClick(close, Qt::LeftButton, {}, close->rect().center());
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.front().front().toInt(), 1);
}

// The same in the window itself: the click goes to whatever is under the
// pointer there, as a hand's would.
void BenchMainWindowTest::windowListTabsCloseFromTheirButtons() {
    followColorSchemes();
    BenchMainWindow window;
    window.resize(1100, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // The restored lists first: restoring is done off the window's thread.
    QTRY_VERIFY(window.lists_restored_);
    window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                .kind = persistence::ListKind::scratch,
                                                .name = "Second",
                                                .pinned = false,
                                                .dirty = false,
                                                .items = {}},
                      true);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_VERIFY(tabs->count() >= 2);
    const auto before = tabs->count();
    auto* close = tabs->tabBar()->tabButton(tabs->count() - 1, QTabBar::RightSide);
    QVERIFY(close != nullptr && close->isVisible());
    const auto at = window.mapFromGlobal(close->mapToGlobal(close->rect().center()));
    auto* under = window.childAt(at);
    QVERIFY2(
        under == close,
        qPrintable(QStringLiteral("under the button: %1 (%2)")
                       .arg(QString::fromLatin1(under != nullptr ? under->metaObject()->className()
                                                                 : "nothing"),
                            under != nullptr ? under->objectName() : QString{})));
    // Asked for, not answered: closing an unsaved list asks first, in a box
    // a test cannot answer.
    QObject::disconnect(tabs, &QTabWidget::tabCloseRequested, &window, nullptr);
    QSignalSpy requested{tabs, &QTabWidget::tabCloseRequested};
    QTest::mouseClick(under, Qt::LeftButton, {}, under->mapFrom(&window, at));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.front().front().toInt(), before - 1);
}

// ADR-0135: MPRIS's play order is the workspace's modes -- repeat and single
// loop the track, repeat the list, random or album random shuffle -- both
// ways; and the playing album's cover is a file, written once per album.
void BenchMainWindowTest::mprisOrderAndCoverFollowTheWorkspace() {
    BenchMainWindow window;
    auto& workspace = window.workspace_;
    workspace.setLoopStatus(QStringLiteral("Track"));
    QCOMPARE(workspace.loopStatus(), QStringLiteral("Track"));
    QVERIFY(workspace.modeTexts().repeat.checked && workspace.modeTexts().single.checked);
    workspace.setLoopStatus(QStringLiteral("Playlist"));
    QCOMPARE(workspace.loopStatus(), QStringLiteral("Playlist"));
    QVERIFY(!workspace.modeTexts().single.checked);
    workspace.setLoopStatus(QStringLiteral("None"));
    QCOMPARE(workspace.loopStatus(), QStringLiteral("None"));
    QVERIFY(!workspace.modeTexts().repeat.checked);
    workspace.setAlbumRandom(true);
    QVERIFY(workspace.shuffled());
    workspace.setShuffled(false);
    QVERIFY(!workspace.shuffled() && !workspace.modeTexts().album_random.checked);
    workspace.setShuffled(true);
    QVERIFY(workspace.shuffled() && workspace.modeTexts().random.checked);
    workspace.setShuffled(false);

    // The cover: a file per album, the last one gone when the album changes.
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& tab = *window.list_tabs_.front();
    const auto row = [](const char* album) {
        LocalTrackRow track;
        track.raw_path = std::string{"/covers/"} + album + ".flac";
        track.title = "Song";
        track.artist = "Band";
        track.album = album;
        track.probed = true;
        return track;
    };
    tab.model->replaceRows({row("First"), row("Second")});
    QImage red{40, 40, QImage::Format_RGB32};
    red.fill(Qt::red);
    tab.model->setArtwork(tab.model->groupKey(0), red);
    QImage blue{40, 40, QImage::Format_RGB32};
    blue.fill(Qt::blue);
    tab.model->setArtwork(tab.model->groupKey(1), blue);
    const auto first = workspace.desktopCoverPath(tab.model->rows()[0], EngineKey::local());
    QVERIFY(!first.isEmpty() && QFileInfo::exists(first));
    QCOMPARE(QImage{first}.pixelColor(0, 0), QColor{Qt::red});
    QCOMPARE(workspace.desktopCoverPath(tab.model->rows()[0], EngineKey::local()), first);
    const auto second = workspace.desktopCoverPath(tab.model->rows()[1], EngineKey::local());
    QVERIFY(second != first && QFileInfo::exists(second));
    QVERIFY(!QFileInfo::exists(first));
    QFile::remove(second);
}

// ADR-0276: the line being sung is highlighted, carried on between the
// engine's reports while it plays, and a click on a line seeks there.
void BenchMainWindowTest::lyricsPanelFollowsThePosition() {
    LyricsPanel panel;
    panel.resize(300, 400);
    panel.show();
    auto* lines = panel.findChild<QListWidget*>(QStringLiteral("lyrics-lines"));
    auto* source = panel.findChild<QLabel*>(QStringLiteral("lyrics-source"));
    QVERIFY(lines != nullptr && source != nullptr);
    QCOMPARE(lines->count(), 1); // "Nothing is playing"

    metadata::Lyrics synced{
        .source = "sidecar", .synced = true, .lines = {}, .declared_length_ms = {}};
    for (const auto& [time, text] : {std::pair{1'000, "One"}, std::pair{2'000, "Two"},
                                     std::pair{3'000, ""}, std::pair{60'000, "Late"}}) {
        synced.lines.push_back({.time_ms = time, .text = text});
    }
    panel.showLyrics(synced);
    QCOMPARE(lines->count(), 4);
    QCOMPARE(source->text(), QStringLiteral("Synced · .lrc file"));
    panel.setPosition(500, false);
    QCOMPARE(panel.currentLine(), -1);
    panel.setPosition(1'200, false);
    QCOMPARE(panel.currentLine(), 0);
    QVERIFY(lines->item(0)->font().bold() && !lines->item(1)->font().bold());
    // Playing, the same report moves on by itself, refreshed as the
    // window's transport timer does.
    QTRY_VERIFY_WITH_TIMEOUT((panel.setPosition(1'900, true), panel.currentLine() == 1), 2'000);
    QVERIFY(!lines->item(0)->font().bold() && lines->item(1)->font().bold());
    // Paused, it stays; a seek far on jumps there.
    panel.setPosition(2'500, false);
    QCOMPARE(panel.currentLine(), 1);
    panel.setPosition(3'100, false);
    QCOMPARE(panel.currentLine(), 2);
    panel.setPosition(61'000, false);
    QCOMPARE(panel.currentLine(), 3);

    // A seek asked for here moves the highlight at once; reports from
    // before it, still on their way, are not believed; one that agrees is.
    panel.seekTo(1'500);
    QCOMPARE(panel.currentLine(), 0);
    panel.setPosition(61'000, false);
    QCOMPARE(panel.currentLine(), 0);
    panel.setPosition(1'550, false);
    QCOMPARE(panel.currentLine(), 0);
    // A seek made elsewhere -- far from what the clock says -- is followed.
    panel.setPosition(2'200, false);
    QCOMPARE(panel.currentLine(), 1);
    // Playing, a report a little behind the clock does not pull it back.
    panel.setPosition(2'000, true);
    QTRY_VERIFY_WITH_TIMEOUT((panel.setPosition(2'000, true), panel.position() >= 2'250), 2'000);
    const auto before_late_report = panel.position();
    panel.setPosition(before_late_report - 200, true);
    QVERIFY(panel.position() >= before_late_report);
    panel.setPosition(0, false);

    QSignalSpy seeks{&panel, &LyricsPanel::seekRequested};
    QTest::mouseClick(lines->viewport(), Qt::LeftButton, {},
                      lines->visualItemRect(lines->item(1)).center());
    QCOMPARE(seeks.size(), 1);
    QCOMPARE(seeks.front().front().toLongLong(), qint64{2'000});

    // Plain lyrics: text, nothing highlighted, a click seeks nowhere.
    panel.showLyrics({.source = "tag:LYRICS",
                      .synced = false,
                      .lines = {{.time_ms = std::nullopt, .text = "Words"}},
                      .declared_length_ms = {}});
    QCOMPARE(source->text(), QStringLiteral("Plain · LYRICS tag"));
    panel.setPosition(5'000, true);
    QCOMPARE(panel.currentLine(), -1);
    QTest::mouseClick(lines->viewport(), Qt::LeftButton, {},
                      lines->visualItemRect(lines->item(0)).center());
    QCOMPARE(seeks.size(), 1);

    // Long lyrics scroll the line sung to the middle, gliding there; a
    // scroll by hand is left alone for a moment.
    metadata::Lyrics song{
        .source = "sidecar", .synced = true, .lines = {}, .declared_length_ms = {}};
    for (int line = 0; line < 60; ++line) {
        song.lines.push_back({.time_ms = line * 1'000, .text = "Line " + std::to_string(line)});
    }
    panel.showLyrics(song);
    panel.setPosition(30'200, false);
    QCOMPARE(panel.currentLine(), 30);
    const auto centred = [&] {
        const auto rect = lines->visualItemRect(lines->item(panel.currentLine()));
        return std::abs(rect.center().y() - lines->viewport()->height() / 2) <= rect.height();
    };
    QTRY_VERIFY_WITH_TIMEOUT(centred(), 2'000);
    QWheelEvent wheel{QPointF{10, 10},   lines->viewport()->mapToGlobal(QPointF{10, 10}),
                      QPoint{},          QPoint{0, 120},
                      Qt::NoButton,      Qt::NoModifier,
                      Qt::NoScrollPhase, false};
    QApplication::sendEvent(lines->viewport(), &wheel);
    const auto scrolled_to = lines->verticalScrollBar()->value();
    panel.setPosition(33'200, false);
    QCOMPARE(panel.currentLine(), 33);
    QTest::qWait(600);
    QCOMPARE(lines->verticalScrollBar()->value(), scrolled_to);
    // A seek from here shows the line again.
    panel.seekTo(40'100);
    QTRY_VERIFY_WITH_TIMEOUT(centred(), 2'000);

    panel.showMessage(QStringLiteral("No lyrics"));
    QCOMPARE(lines->count(), 1);
    QCOMPARE(lines->item(0)->text(), QStringLiteral("No lyrics"));
    QVERIFY(source->text().isEmpty());

    // In the window: a dock, hidden at first, shown from the Playback menu.
    BenchMainWindow window;
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-lyrics"));
    auto* toggle = window.findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    QVERIFY(dock != nullptr && toggle != nullptr);
    QCOMPARE(toggle->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    QVERIFY(!QSettings{}.value(QStringLiteral("lyrics/visible"), false).toBool() ||
            dock->isVisibleTo(&window));
}

void BenchMainWindowTest::colorSchemesAreChosenAndApplied() {
    // Complete schemes: an unfocused window keeps its accent, disabled text
    // reads as disabled.
    for (const auto& palette : {lightPalette(), darkPalette()}) {
        QCOMPARE(palette.color(QPalette::Inactive, QPalette::Highlight),
                 palette.color(QPalette::Active, QPalette::Highlight));
        QVERIFY(palette.color(QPalette::Disabled, QPalette::Text) !=
                palette.color(QPalette::Active, QPalette::Text));
    }
    QVERIFY(darkPalette().color(QPalette::Window).lightness() <
            lightPalette().color(QPalette::Window).lightness());

    // Icons in the variant the ground needs, found among those installed.
    {
        QTemporaryDir icons;
        QVERIFY(icons.isValid());
        for (const auto* name : {"Glyphs-Dark", "Glyphs-Light", "breezy", "breezy-dark"}) {
            QVERIFY(QDir{icons.path()}.mkpath(QString::fromLatin1(name)));
            QFile index{icons.path() + QLatin1Char('/') + QString::fromLatin1(name) +
                        QStringLiteral("/index.theme")};
            QVERIFY(index.open(QIODevice::WriteOnly));
            index.write("[Icon Theme]\nName=x\n");
        }
        const auto paths = QIcon::themeSearchPaths();
        QIcon::setThemeSearchPaths({icons.path()});
        QCOMPARE(iconThemeVariant(QStringLiteral("Glyphs-Dark"), false),
                 QStringLiteral("Glyphs-Light"));
        QCOMPARE(iconThemeVariant(QStringLiteral("Glyphs-Light"), true),
                 QStringLiteral("Glyphs-Dark"));
        QCOMPARE(iconThemeVariant(QStringLiteral("breezy-dark"), false), QStringLiteral("breezy"));
        QCOMPARE(iconThemeVariant(QStringLiteral("breezy"), true), QStringLiteral("breezy-dark"));
        QCOMPARE(iconThemeVariant(QStringLiteral("Lonely"), true), QStringLiteral("Lonely"));
        QIcon::setThemeSearchPaths(paths);
    }

    followColorSchemes();
    BenchMainWindow window;
    window.show();
    const auto choose = [&window](const QString& scheme) {
        window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
        auto* dialog = window.findChild<SettingsDialog*>();
        QVERIFY(dialog != nullptr);
        auto* combo = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-color-scheme"));
        QVERIFY(combo != nullptr);
        combo->setCurrentIndex(combo->findData(scheme));
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    };
    choose(QStringLiteral("dark"));
    QCOMPARE(QApplication::palette().color(QPalette::Window),
             darkPalette().color(QPalette::Window));
    // ADR-0250: Trackknife's own style under every scheme, as Quick's.
    QVERIFY(qobject_cast<TrackknifeStyle*>(QApplication::style()) != nullptr);
    QVERIFY(ColorSchemes::instance().ownPalette());
    QCOMPARE(window.palette().color(QPalette::Base), darkPalette().color(QPalette::Base));

    choose(QStringLiteral("light"));
    QCOMPARE(QApplication::palette().color(QPalette::Window),
             lightPalette().color(QPalette::Window));

    choose(QStringLiteral("system"));
    QVERIFY(!ColorSchemes::instance().ownPalette());
    QVERIFY(qobject_cast<TrackknifeStyle*>(QApplication::style()) != nullptr);
    QVERIFY(QApplication::palette().color(QPalette::Window) !=
            darkPalette().color(QPalette::Window));
    QSettings{}.remove(QLatin1String(color_scheme_key));
}

// An INI file hands every value back as text, and the text "false" is true to
// anything that only asks whether a value is there: the Qt Quick window once
// showed every option left off as on. Settings come out as their type.
void BenchMainWindowTest::settingsLeftOffReadAsOff() {
    QSettings stored;
    stored.setValue(QLatin1String(SettingsKeys::ratings_in_tags_key), QStringLiteral("false"));
    stored.setValue(QLatin1String(SettingsKeys::rating_backup_key), QStringLiteral("false"));
    stored.setValue(QStringLiteral("playback/rg-preamp-with"), QStringLiteral("-3.5"));
    stored.setValue(QLatin1String(SettingsKeys::rating_tag_scale_key), QStringLiteral("10"));
    stored.sync();
    const SettingsSession session;
    const auto& values = session.values();
    const auto in_tags = values.value(QLatin1String(SettingsKeys::ratings_in_tags_key));
    QCOMPARE(in_tags.metaType(), QMetaType::fromType<bool>());
    QVERIFY(!in_tags.toBool());
    const auto backup = values.value(QLatin1String(SettingsKeys::rating_backup_key));
    QCOMPARE(backup.metaType(), QMetaType::fromType<bool>());
    QVERIFY(!backup.toBool());
    const auto preamp = values.value(QStringLiteral("playback/rg-preamp-with"));
    QCOMPARE(preamp.metaType(), QMetaType::fromType<double>());
    QCOMPARE(preamp.toDouble(), -3.5);
    QCOMPARE(values.value(QLatin1String(SettingsKeys::rating_tag_scale_key)).toString(),
             QStringLiteral("10"));
    stored.remove(QLatin1String(SettingsKeys::ratings_in_tags_key));
    stored.remove(QLatin1String(SettingsKeys::rating_backup_key));
    stored.remove(QStringLiteral("playback/rg-preamp-with"));
    stored.remove(QLatin1String(SettingsKeys::rating_tag_scale_key));
}

// Delete takes what is chosen where the keyboard is: in Up Next, its track,
// never the row chosen in the list behind it.
void BenchMainWindowTest::deleteInUpNextTakesItsTrackNotTheLists() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model);
    LocalTrackRow row;
    row.raw_path = "/unavailable/track.flac";
    row.probed = true;
    model->replaceRows({row, row, row});
    // Nothing chosen in the list: Delete still works in Up Next.
    view->clearSelection();

    row.title = "Asked for";
    window.enqueueLocalRequests({row, row});
    QCOMPARE(window.up_next_display_ids_.size(), std::size_t{2});
    window.findChild<QAction*>(QStringLiteral("action-show-up-next"))->trigger();
    QTRY_VERIFY(window.up_next_view_->isVisible());
    window.up_next_view_->setCurrentIndex(window.up_next_view_->model()->index(0, 0));
    window.up_next_view_->setFocus();
    QTRY_VERIFY(window.up_next_view_->hasFocus());
    QTest::keyClick(window.up_next_view_, Qt::Key_Delete);
    QTRY_COMPARE(window.up_next_display_ids_.size(), std::size_t{1});
    QCOMPARE(model->rowCount(), 3);

    // In the list, the list's row.
    view->setCurrentIndex(model->index(1, 0));
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(window.up_next_display_ids_.size(), std::size_t{1});
}

void BenchMainWindowTest::localListUndoActionsRespectAuthorityAndTextEditing() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model);
    auto* undo = window.findChild<QAction*>(QStringLiteral("action-undo-list-edit"));
    auto* redo = window.findChild<QAction*>(QStringLiteral("action-redo-list-edit"));
    QVERIFY(undo && redo);
    LocalTrackRow row;
    row.raw_path = "/unavailable/track.flac";
    row.probed = true;
    model->replaceRows({row, row, row});
    view->setCurrentIndex(model->index(1, 0));
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(model->rowCount(), 2);
    QVERIFY(undo->isEnabled());
    QVERIFY(undo->text().contains(QStringLiteral("Remove tracks")));
    QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(view);
    QCOMPARE(model->rowCount(), 3);
    QCOMPARE(view->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(view->selectionModel()->selectedRows().front().row(), 1);
    QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    releaseHeldModifiers(view);
    QCOMPARE(model->rowCount(), 2);
    // Undo follows the list on screen: another list has its own (empty)
    // history, and undoing there never reaches back into this one.
    auto* other =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Other",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    QVERIFY(other != nullptr);
    QCOMPARE(tabs->currentWidget(), other->view);
    QVERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());
    undo->trigger();
    QCOMPARE(model->rowCount(), 2);
    tabs->setCurrentWidget(view);
    QVERIFY(undo->isEnabled());
    auto* selector = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(selector);
    selector->setCurrentIndex(1);
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("local-library-search"));
    QVERIFY(search);
    search->setText(QStringLiteral("ab"));
    search->insert(QStringLiteral("c"));
    search->setFocus();
    QTest::keyClick(search, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(search);
    QCOMPARE(search->text(), QStringLiteral("ab"));
    QCOMPARE(model->rowCount(), 2);
    view->setFocus();
    undo->trigger();
    QCOMPARE(model->rowCount(), 3);
    QVERIFY(tabs->tabText(tabs->indexOf(view)).endsWith(QStringLiteral(" *")));
}

void BenchMainWindowTest::localListOrderingActionsRespectAuthorityAndPersist() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* model = qobject_cast<LocalListModel*>(view->model());
    auto* sort = window.findChild<QAction*>(QStringLiteral("action-sort-list-title"));
    auto* reverse = window.findChild<QAction*>(QStringLiteral("action-reverse-list"));
    auto* deduplicate = window.findChild<QAction*>(QStringLiteral("action-deduplicate-list"));
    auto* sort_menu = window.findChild<QMenu*>(QStringLiteral("bench-sort-list-menu"));
    auto* bar = window.findChild<LocalListEditBar*>();
    QVERIFY(model && sort && reverse && deduplicate && sort_menu && bar);
    QVERIFY(!reverse->isEnabled());
    LocalTrackRow first;
    first.raw_path = "/unavailable/b.flac";
    first.title = "B";
    first.probed = true;
    auto second = first;
    second.raw_path = "/unavailable/a.flac";
    second.title = "A";
    model->replaceRows({first, second, second});
    QVERIFY(reverse->isEnabled());
    QSignalSpy edits{bar, &LocalListEditBar::edited};
    sort->trigger();
    QTRY_COMPARE(edits.size(), 1);
    QCOMPARE(model->rows().front().title, std::string{"A"});
    QVERIFY(tabs->tabText(tabs->indexOf(view)).endsWith(QStringLiteral(" *")));
    reverse->trigger();
    QTRY_COMPARE(edits.size(), 2);
    QCOMPARE(model->rows().front().title, std::string{"B"});
    deduplicate->trigger();
    QTRY_COMPARE(edits.size(), 3);
    QCOMPARE(model->rowCount(), 2);
    window.findChild<QAction*>(QStringLiteral("action-undo-list-edit"))->trigger();
    QCOMPARE(model->rowCount(), 3);
    // Ordering actions follow the list on screen, and an empty one offers none.
    auto* other =
        window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                    .kind = persistence::ListKind::scratch,
                                                    .name = "Other",
                                                    .pinned = false,
                                                    .dirty = false,
                                                    .items = {}},
                          true);
    QVERIFY(other != nullptr);
    QCOMPARE(tabs->currentWidget(), other->view);
    QVERIFY(!sort_menu->isEnabled());
    QVERIFY(!reverse->isEnabled());
    QVERIFY(!deduplicate->isEnabled());
    QVERIFY(bar->isHidden());
    // Even a directly invoked action cannot use a stale local destination.
    reverse->trigger();
    QCOMPARE(model->rowCount(), 3);
    // Closed again, so the restart below restores only the list under test.
    window.closeTabAt(tabs->indexOf(other->view));
    tabs->setCurrentWidget(view);
    QVERIFY(reverse->isEnabled());
    // Custom-expression editing retains native text undo, separate from list undo.
    window.findChild<QAction*>(QStringLiteral("action-sort-list-custom"))->trigger();
    auto* expression = bar->findChild<QLineEdit*>();
    expression->setText(QStringLiteral("%title%"));
    expression->setCursorPosition(static_cast<int>(expression->text().size()));
    expression->insert(QStringLiteral("x"));
    QTest::keyClick(expression, Qt::Key_Z, Qt::ControlModifier);
    releaseHeldModifiers(expression);
    QCOMPARE(expression->text(), QStringLiteral("%title%"));
    QCOMPARE(model->rowCount(), 3);
    // Closing cancels a pending edit before the durable workspace flush.
    reverse->trigger();
    window.close();
    BenchMainWindow reopened;
    reopened.show();
    auto* restored_tabs = reopened.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(restored_tabs->count(), 1);
    auto* restored_view = qobject_cast<QTableView*>(restored_tabs->currentWidget());
    auto* restored_model = qobject_cast<LocalListModel*>(restored_view->model());
    QVERIFY(restored_model);
    QTRY_COMPARE(restored_model->rowCount(), 3);
    QCOMPARE(restored_model->rows()[0].raw_path, first.raw_path);
    QCOMPARE(restored_model->rows()[1].raw_path, second.raw_path);
    QCOMPARE(restored_model->rows()[2].raw_path, second.raw_path);
    QVERIFY(!restored_model->canUndo());
}

void BenchMainWindowTest::portablePlaylistImportsPreserveAuthorityAndPersist() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto playlist = media.filePath(QStringLiteral("Offline mix.m3u8"));
    QFile file{playlist};
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray contents{"#EXTM3U\n#EXTINF:12.345,Offline title\ngone.flac\ngone.flac\n"};
    QCOMPARE(file.write(contents), contents.size());
    file.close();
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* local_view = qobject_cast<QTableView*>(tabs->currentWidget());
        auto* local_model = qobject_cast<LocalListModel*>(local_view->model());
        QVERIFY(local_model);
        auto* import_action = window.findChild<QAction*>(QStringLiteral("action-import-m3u8"));
        auto* export_action = window.findChild<QAction*>(QStringLiteral("action-export-m3u8"));
        QVERIFY(import_action->isEnabled());
        auto* bar = window.findChild<PlaylistTransferBar*>();
        QSignalSpy completed{bar, &PlaylistTransferBar::completed};
        window.importM3u8Path(QFile::encodeName(playlist).toStdString());
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(completed[0][0].toBool());
        QCOMPARE(tabs->count(), 2);
        QVERIFY(export_action->isEnabled());
        // Imported into a list of its own, not into the one that was showing.
        QCOMPARE(local_model->rowCount(), 0);
        auto* imported_view = qobject_cast<QTableView*>(tabs->currentWidget());
        auto* imported_model = qobject_cast<LocalListModel*>(imported_view->model());
        QVERIFY(imported_model);
        QCOMPARE(imported_model->rowCount(), 2);
        QCOMPARE(imported_model->rows()[0].title, std::string{"Offline title"});
        QCOMPARE(imported_model->rows()[0].raw_path, imported_model->rows()[1].raw_path);
        window.close();
    }
    BenchMainWindow reopened;
    reopened.show();
    auto* tabs = reopened.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 2);
    LocalListModel* restored = nullptr;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == QStringLiteral("Offline mix")) {
            auto* view = qobject_cast<QTableView*>(tabs->widget(i));
            restored = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
        }
    }
    QVERIFY(restored);
    QCOMPARE(restored->rowCount(), 2);
    QCOMPARE(restored->rows()[0].title, std::string{"Offline title"});
    QCOMPARE(restored->rows()[0].duration_ms, std::optional<std::int64_t>{12345});
    QCOMPARE(restored->rows()[0].raw_path, restored->rows()[1].raw_path);
    QVERIFY(restored->rows()[0].probed);
}

void BenchMainWindowTest::trackViewLayoutMatchesGroupedQueueAndPersists() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first_path = media.filePath(QStringLiteral("first.wav"));
    const auto second_path = media.filePath(QStringLiteral("second.wav"));
    write_wave(first_path, wave_sample_rate / 10U);
    write_wave(second_path, wave_sample_rate / 10U);
    const auto first_encoded = QFile::encodeName(first_path);
    const auto second_encoded = QFile::encodeName(second_path);
    const std::string first_raw{first_encoded.constData(),
                                static_cast<std::size_t>(first_encoded.size())};
    const std::string second_raw{second_encoded.constData(),
                                 static_cast<std::size_t>(second_encoded.size())};
    QString binding;
    int persisted_title_width = 0;
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        auto* side = window.findChild<QAction*>(QStringLiteral("action-track-layout-albums-side"));
        auto* plain = window.findChild<QAction*>(QStringLiteral("action-track-layout-plain"));
        auto* date = window.findChild<QAction*>(QStringLiteral("action-track-column-date"));
        QVERIFY(tabs != nullptr);
        QVERIFY(side != nullptr);
        QVERIFY(plain != nullptr);
        QVERIFY(date != nullptr);
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        QCOMPARE(view->model()->columnCount(), local_column_count);
        QVERIFY(side->isChecked());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(view->albumGroupingEnabled());
        QVERIFY(qobject_cast<ui::QueueItemDelegate*>(view->itemDelegate()) != nullptr);
        const auto viewport_children =
            view->viewport()->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly);
        for (const auto* child : viewport_children) {
            QVERIFY2(!child->isVisible() || !child->geometry().contains(view->viewport()->rect()),
                     "A child widget must not cover Trackbench's delegate-painted track rows");
        }
        QVERIFY(!view->wordWrap());
        QCOMPARE(view->textElideMode(), Qt::ElideRight);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        window.openLocalPaths({first_raw, second_raw});
        QTRY_COMPARE(view->model()->rowCount(), 2);
        QCOMPARE(view->verticalHeader()->sectionResizeMode(0), QHeaderView::Fixed);
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(1));
        QCOMPARE(view->model()->headerData(local_artist_column, Qt::Horizontal).toString(),
                 QStringLiteral("Artist"));
        QCOMPARE(view->model()->headerData(local_track_number_column, Qt::Horizontal).toString(),
                 QStringLiteral("#"));

        LocalTrackRow singleton{
            .raw_path = "/standalone.wav",
            .logical_reference = std::nullopt,
            .selection = {},
            .segment = std::nullopt,
            .title = "Standalone",
            .artist = "Solo artist",
            .album = "One-off",
            .album_artist = {},
            .date = "2026",
            .track_number = {},
            .duration_ms = 1'000,
            .metadata = {},
            .source_revision = std::nullopt,
        };
        auto* local_model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(local_model != nullptr);
        local_model->appendRows({std::move(singleton)});
        // A lone track is an album of its own (ADR-0250): its header above it.
        QTRY_COMPARE(view->rowHeight(2),
                     view->rowHeight(1) + ui::QueueItemDelegate::album_header_height);
        QImage singleton_cover{12, 12, QImage::Format_RGB32};
        singleton_cover.fill(Qt::red);
        local_model->setArtwork(local_model->groupKey(2), singleton_cover);
        QCoreApplication::processEvents();
        // Its cover beside its header, in the cover gutter, as any album's.
        const auto artwork_rect = view->visualRect(local_model->index(2, local_artwork_column));
        const auto shows_cover = [](const QImage& render) {
            for (int y = 0; y < render.height(); ++y) {
                for (int x = 0; x < render.width(); ++x) {
                    if (render.pixelColor(x, y) == QColor(Qt::red)) {
                        return true;
                    }
                }
            }
            return false;
        };
        QVERIFY(shows_cover(view->viewport()->grab(artwork_rect).toImage()));

        view->selectionModel()->select(local_model->index(2, 0),
                                       QItemSelectionModel::ClearAndSelect |
                                           QItemSelectionModel::Rows);
        QCoreApplication::processEvents();
        // Selected or playing, the cover stays on top of the row's tint.
        QVERIFY(shows_cover(view->viewport()->grab(artwork_rect).toImage()));
        view->clearSelection();
        local_model->setCurrentSource(local_model->source(2), 2);
        QCoreApplication::processEvents();
        QVERIFY(shows_cover(view->viewport()->grab(artwork_rect).toImage()));

        plain->trigger();
        QVERIFY(plain->isChecked());
        QCOMPARE(view->albumArtworkColumn(), -1);
        QVERIFY(qobject_cast<ui::QueueItemDelegate*>(view->itemDelegate()) == nullptr);
        QVERIFY(view->isColumnHidden(local_artwork_column));
        QCOMPARE(view->rowHeight(0), view->rowHeight(1));
        QCOMPARE(view->rowHeight(0), view->verticalHeader()->defaultSectionSize());
        QVERIFY(!view->albumGroupingEnabled());
        QCOMPARE(view->verticalHeader()->sectionResizeMode(0), QHeaderView::Fixed);

        side->trigger();
        QVERIFY(view->albumGroupingEnabled());
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(1));
        auto* header = view->horizontalHeader();
        header->moveSection(header->visualIndex(local_title_column), 1);
        view->setColumnWidth(local_title_column, 333);
        // The album view leaves the date to its header; asked for, it shows.
        QVERIFY(view->isColumnHidden(local_date_column));
        date->trigger();
        QVERIFY(!view->isColumnHidden(local_date_column));
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        persisted_title_width = view->columnWidth(local_title_column);
        window.resize(1'350, 720);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        QVERIFY(view->columnWidth(local_title_column) > persisted_title_width);
        window.resize(1'100, 720);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        persisted_title_width = view->columnWidth(local_title_column);
        binding = QStringLiteral("local:%1").arg(view->property("bench-document-id").toString());
        QVERIFY(window.close());
    }

    {
        BenchMainWindow restored;
        restored.show();
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QCOMPARE(view->horizontalHeader()->logicalIndex(1), local_title_column);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        QCOMPARE(view->columnWidth(local_title_column), persisted_title_width);
        QVERIFY(!view->isColumnHidden(local_date_column));
        QVERIFY(restored.close());
    }

    const QByteArray future_layout{
        R"({"schema":99,"presentation":"future","columns":[],"keep":true})"};
    // ADR-0259: a layout this release cannot read, as a later one would keep it.
    {
        auto kept = TabStore::loadState();
        QCOMPARE(kept.tabs.size(), 1U);
        kept.tabs.front().layout = future_layout;
        TabStore::saveState(kept);
    }
    {
        BenchMainWindow fallback;
        fallback.show();
        auto* tabs = fallback.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(fallback.statusBar()->currentMessage().contains(QStringLiteral("preserved")));
        QVERIFY(fallback.close());
    }
    const auto kept = TabStore::loadState();
    // One per list tab, kept as it was.
    QCOMPARE(kept.tabs.size(), 1U);
    QCOMPARE(
        QStringLiteral("local:%1").arg(QString::fromStdString(kept.tabs.front().id.to_string())),
        binding);
    QCOMPARE(kept.tabs.front().layout, future_layout);
}

// ADR-0282: a list view of the user's own -- an expression column, rows
// grouped by an expression with a header and subgroups of their own -- and a
// header click sorting the list by a column, again the other way. Changing
// a shipped view makes a copy, which the list is then drawn by after a
// restart too.
void BenchMainWindowTest::listViewsShowExpressionsGroupAndSort() {
    auto by_label = *lists::shipped_list_view(lists::columns_view_id);
    by_label.id = "view-test";
    by_label.name = "By label";
    by_label.builtin = false;
    by_label.columns.push_back({.id = "expression-1",
                                .visible = true,
                                .title = "Label",
                                .display = "%label%",
                                .sort = {},
                                .alignment = lists::ColumnAlignment::right,
                                .expands = false,
                                .style = {},
                                .second_line = {},
                                .width = 0});
    by_label.grouping = lists::ViewGrouping::expression;
    by_label.group_key = "%label%";
    by_label.group_title = "%label%";
    by_label.group_details = "%group_tracks% tracks";
    by_label.subgroup = lists::ViewSubgroup{.key = "%discnumber%", .label = "Disc %discnumber%"};
    // ADR-0283: Interscope's rows quiet, the label itself bold.
    by_label.row_style = "$if($eq(%label%,Interscope),quiet,)";
    by_label.columns.back().style = "bold";
    QVERIFY(saveCustomListViews({by_label}).has_value());

    const auto row = [](const char* title, const char* label, const char* disc) {
        LocalTrackRow made;
        made.raw_path = std::string{"/music/"} + title + ".flac";
        made.title = title;
        made.artist = "Artist";
        made.duration_ms = 60'000;
        for (const auto& [name, value] : {std::pair{"TITLE", title}, std::pair{"LABEL", label},
                                          std::pair{"DISCNUMBER", disc}}) {
            made.metadata.fields.push_back(
                {.canonical_name = metadata::canonicalize_field_name(name),
                 .native_name = name,
                 .values = {value},
                 .qualifier = {},
                 .provenance = metadata::FieldProvenance::embedded});
        }
        made.probed = true;
        return made;
    };
    QString customised;
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        model->appendRows({row("Beta", "Warp", "1"), row("alpha", "Warp", "2"),
                           row("Gamma", "Warp", "2"), row("Delta", "Interscope", "1")});
        window.chooseListView(QStringLiteral("view-test"));

        QCOMPARE(model->columnCount(), local_column_count + 1);
        const auto label = model->columnOf(QStringLiteral("expression-1"));
        QCOMPARE(label, local_column_count);
        QCOMPARE(model->headerData(label, Qt::Horizontal, Qt::DisplayRole).toString(),
                 QStringLiteral("Label"));
        QTRY_COMPARE(model->index(0, label).data().toString(), QStringLiteral("Warp"));
        QCOMPARE(model->index(3, label).data().toString(), QStringLiteral("Interscope"));
        QCOMPARE(model->index(0, label).data(Qt::TextAlignmentRole).value<Qt::Alignment>() &
                     Qt::AlignRight,
                 Qt::AlignRight);
        QVERIFY(!view->isColumnHidden(label));

        // Grouped by label once every row is known; the groups are not
        // albums, so nothing is left to their headers.
        QTRY_VERIFY(model->expressionGroupsReady());
        QTRY_VERIFY(view->albumGroupingEnabled());
        QVERIFY(!view->property(ui::track_album_rules_property).toBool());
        const auto starts = [model] {
            QList<bool> found;
            for (int at = 0; at < model->rowCount(); ++at) {
                found << model->index(at, 0).data(ui::track_album_group_start_role).toBool();
            }
            return found;
        };
        QCOMPARE(starts(), (QList<bool>{true, false, false, true}));
        QTRY_COMPARE(model->index(0, 0).data(ui::track_group_title_role).toString(),
                     QStringLiteral("Warp"));
        QCOMPARE(model->index(0, 0).data(ui::track_group_details_role).toString(),
                 QStringLiteral("3 tracks"));
        QCOMPARE(model->index(0, 0).data(ui::track_disc_start_role).toString(),
                 QStringLiteral("Disc 1"));
        QCOMPARE(model->index(1, 0).data(ui::track_disc_start_role).toString(),
                 QStringLiteral("Disc 2"));
        QCOMPARE(model->index(2, 0).data(ui::track_disc_start_role).toString(), QString{});
        // One disc only: nothing to tell apart.
        QCOMPARE(model->index(3, 0).data(ui::track_disc_start_role).toString(), QString{});
        // Drawn as its styles say: Interscope's row quiet, in every column;
        // the label bold, in every row.
        QCOMPARE(
            model->index(3, local_title_column).data(Qt::ForegroundRole).value<QBrush>().color(),
            QGuiApplication::palette().color(QPalette::PlaceholderText));
        QVERIFY(!model->index(0, local_title_column).data(Qt::ForegroundRole).isValid());
        QVERIFY(model->index(0, label).data(Qt::FontRole).value<QFont>().weight() >=
                QFont::DemiBold);
        QVERIFY(!model->index(0, local_title_column).data(Qt::FontRole).isValid());
        // Drawn as the view says: its header above the group, not the album's.
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(2));

        const auto titles = [model] {
            QStringList found;
            for (int at = 0; at < model->rowCount(); ++at) {
                found << model->index(at, local_title_column).data().toString();
            }
            return found;
        };
        // A click on Title sorts the list by it -- natural, case ignored --
        // as an edit that can be undone; again, the other way.
        emit view->horizontalHeader()->sectionClicked(local_title_column);
        QTRY_COMPARE(titles(), (QStringList{QStringLiteral("alpha"), QStringLiteral("Beta"),
                                            QStringLiteral("Delta"), QStringLiteral("Gamma")}));
        QVERIFY(model->canUndo());
        QCOMPARE(model->undoLabel(), QStringLiteral("Sort by Title"));
        // Groups follow the list's order.
        QTRY_COMPARE(starts(), (QList<bool>{true, false, true, true}));
        emit view->horizontalHeader()->sectionClicked(local_title_column);
        QTRY_COMPARE(titles(), (QStringList{QStringLiteral("Gamma"), QStringLiteral("Delta"),
                                            QStringLiteral("Beta"), QStringLiteral("alpha")}));
        // The Label column sorts by what it shows.
        emit view->horizontalHeader()->sectionClicked(label);
        QTRY_COMPARE(model->index(0, label).data().toString(), QStringLiteral("Interscope"));

        // A shipped view is not changed: hiding one of its columns makes a
        // copy, which the list is drawn by.
        window.chooseListView(QStringLiteral("plain-columns"));
        QCOMPARE(model->columnCount(), local_column_count);
        QVERIFY(!view->isColumnHidden(local_date_column));
        window.setTrackColumnVisible(QStringLiteral("date"), false);
        auto* tab = window.currentListTab();
        customised = tab->view_layout.view;
        QVERIFY(customised.startsWith(QStringLiteral("view-")));
        const auto copy = findListView(customised);
        QVERIFY(copy.has_value());
        QCOMPARE(copy->name, std::string{"Plain columns (customised)"});
        QVERIFY(view->isColumnHidden(local_date_column));
        QVERIFY(lists::shipped_list_view(lists::columns_view_id)->columns ==
                findListView(QStringLiteral("plain-columns"))->columns);
        QVERIFY(window.close());
    }
    {
        BenchMainWindow restored;
        restored.show();
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(restored.currentListTab()->view_layout.view, customised);
        QVERIFY(view->isColumnHidden(local_date_column));
        QVERIFY(restored.close());
    }
}

// ADR-0282: a view that reads listening history has the list's history
// fetched from its engine before its rows are shown; what arrives is read by
// its expressions, and a listen the engine records is fetched again.
void BenchMainWindowTest::listViewsReadListeningHistory() {
    auto plays = *lists::shipped_list_view(lists::columns_view_id);
    plays.id = "view-plays";
    plays.name = "Plays";
    plays.builtin = false;
    plays.columns.push_back({.id = "expression-1",
                             .visible = true,
                             .title = "Plays",
                             .display = "$if(%playcount%,%playcount% plays,none)",
                             .sort = {},
                             .alignment = lists::ColumnAlignment::left,
                             .expands = false,
                             .style = {},
                             .second_line = {},
                             .width = 0});
    QVERIFY(saveCustomListViews({plays}).has_value());
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* model =
        qobject_cast<LocalListModel*>(static_cast<QTableView*>(tabs->currentWidget())->model());
    LocalTrackRow played;
    played.raw_path = "/music/played.flac";
    played.title = "Played";
    played.probed = true;
    model->appendRows({played});
    QSignalSpy wanted{model, &LocalListModel::historyWanted};
    window.chooseListView(QStringLiteral("view-plays"));
    const auto column = model->columnOf(QStringLiteral("expression-1"));
    // Asked of the engine, which knows no listens of it: none.
    QTRY_VERIFY(wanted.count() > 0);
    QTRY_COMPARE(model->index(0, column).data().toString(), QStringLiteral("none"));
    QVERIFY(model->rows().front().history_loaded);

    // History that says otherwise is read by the expression.
    model->forgetHistory({played.raw_path});
    const auto requests = model->takeHistoryRequests();
    QCOMPARE(requests.size(), 1U);
    QCOMPARE(requests.front().source.source.source_reference, played.raw_path);
    model->applyHistory({requests.front().entry},
                        {std::array<std::int64_t, 6>{7, 1'700'000'000'000, 2, 9, -1, -1}});
    QTRY_COMPARE(model->index(0, column).data().toString(), QStringLiteral("7 plays"));

    // A listen recorded: asked again, and the engine's answer shown.
    window.workspace_.adoptListen(EngineKey::local(), played.raw_path);
    QTRY_COMPARE(model->index(0, column).data().toString(), QStringLiteral("none"));
    QVERIFY(window.close());
}

// ADR-0283: a click on the mark at a header's right end folds its group to
// the header -- its rows stay in the list, not drawn -- and again unfolds
// it; the folds are kept with the tab across a restart.
void BenchMainWindowTest::groupsFoldToTheirHeaders() {
    const auto row = [](const char* title, const char* album) {
        LocalTrackRow made;
        made.raw_path = std::string{"/music/"} + album + "/" + title + ".flac";
        made.title = title;
        made.artist = "Artist";
        made.album = album;
        made.duration_ms = 60'000;
        made.probed = true;
        return made;
    };
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        auto* model = qobject_cast<LocalListModel*>(view->model());
        model->appendRows({row("One", "First"), row("Two", "First"), row("Three", "Second"),
                           row("Four", "Second")});
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(1));
        const auto open_length = view->verticalHeader()->length();
        // The mark at the header's right end.
        const QPoint mark{view->viewport()->width() - ui::QueueTableView::collapse_mark_width / 2,
                          view->rowViewportPosition(0) + 8};
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, mark);
        QVERIFY(view->groupCollapsed(0));
        QVERIFY(view->isRowHidden(1));
        QVERIFY(!view->isRowHidden(2) && !view->isRowHidden(3));
        // Shorter by its folded rows; beside the rows the header keeps its
        // cover's height.
        QVERIFY(view->verticalHeader()->length() < open_length);
        QVERIFY(view->rowHeight(0) >= ui::QueueItemDelegate::album_header_height);
        // Still in the list.
        QCOMPARE(model->rowCount(), 4);
        // A click on the rest of the header still selects the group, folded
        // rows and all.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {},
                          QPoint{view->viewport()->width() / 2, view->rowViewportPosition(0) + 8});
        QCOMPARE(view->selectionModel()->selectedRows().size(), 2);
        QVERIFY(window.close());
    }
    {
        BenchMainWindow restored;
        restored.show();
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 1);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QTRY_COMPARE(view->model()->rowCount(), 4);
        QTRY_VERIFY(view->isRowHidden(1));
        QVERIFY(!view->isRowHidden(3));
        // Every group, then none.
        view->setAllGroupsCollapsed(true);
        QVERIFY(view->isRowHidden(1) && view->isRowHidden(3));
        view->setAllGroupsCollapsed(false);
        QVERIFY(!view->isRowHidden(1) && !view->isRowHidden(3));
        QVERIFY(restored.close());
    }
}

// ADR-0284: a view of two-line rows -- the title's artist and album beneath
// it -- draws every row taller by a line, grouped or not.
void BenchMainWindowTest::rowsHaveMoreLines() {
    auto two_lines = *lists::shipped_list_view(lists::columns_view_id);
    two_lines.id = "view-two-lines";
    two_lines.name = "Two lines";
    two_lines.builtin = false;
    two_lines.row_lines = 2;
    std::ranges::find(two_lines.columns, std::string{"title"}, &lists::ViewColumn::id)
        ->second_line = "%artist% · %album%";
    auto grouped = two_lines;
    grouped.id = "view-two-lines-albums";
    grouped.name = "Two lines, albums";
    grouped.grouping = lists::ViewGrouping::album;
    grouped.artwork = lists::ViewArtwork::beside;
    QVERIFY(saveCustomListViews({two_lines, grouped}).has_value());
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
    auto* model = qobject_cast<LocalListModel*>(view->model());
    LocalTrackRow first;
    first.raw_path = "/music/one.flac";
    first.title = "One";
    first.artist = "Air";
    first.album = "Moon Safari";
    first.probed = true;
    first.rating_hash = "rating-one";
    auto second = first;
    second.raw_path = "/music/two.flac";
    second.title = "Two";
    second.rating_hash = "rating-two";
    model->appendRows({first, second});
    window.chooseListView(QStringLiteral("view-two-lines"));
    const auto tall = ui::track_row_height + ui::trackSecondLineHeight(view->font());
    QTRY_COMPARE(view->rowHeight(1), tall);
    QCOMPARE(view->property(ui::track_row_lines_property).toInt(), 2);
    QVERIFY(qobject_cast<ui::TrackLinesDelegate*>(view->itemDelegate()) != nullptr);
    QTRY_COMPARE(model->index(0, local_title_column).data(ui::track_second_line_role).toString(),
                 QStringLiteral("Air · Moon Safari"));
    QCOMPARE(model->index(0, local_length_column).data(ui::track_second_line_role).toString(),
             QString{});
    // A larger font: the second line is measured, its descenders kept
    // inside the row rather than cut off at a fixed height.
    {
        auto large = view->font();
        large.setPointSizeF(24.0);
        view->setFont(large);
        window.chooseListView(QStringLiteral("view-two-lines-albums"));
        window.chooseListView(QStringLiteral("view-two-lines"));
        const QFontMetrics beneath{ui::trackSecondLineFont(large)};
        QTRY_VERIFY(view->rowHeight(1) >= ui::track_row_height + beneath.height());
        QVERIFY(view->rowHeight(1) > ui::track_row_height + 16);
        view->setFont(QFont{});
        window.chooseListView(QStringLiteral("view-two-lines-albums"));
        window.chooseListView(QStringLiteral("view-two-lines"));
        QTRY_COMPARE(view->rowHeight(1), tall);
    }
    // A rating coming later evaluates its row again, and the second lines of
    // built-in columns are drawn anew though the view has no columns of its
    // own.
    QSignalSpy redrawn{model, &QAbstractItemModel::dataChanged};
    model->applyRatings({{QStringLiteral("rating-one"), 8U}});
    QTRY_VERIFY(std::ranges::any_of(redrawn, [](const QList<QVariant>& signal) {
        return signal.at(0).toModelIndex().row() == 0 &&
               signal.at(0).toModelIndex().column() <= local_title_column &&
               signal.at(2).value<QList<int>>().contains(ui::track_second_line_role);
    }));
    QCOMPARE(model->index(0, local_title_column).data(ui::track_second_line_role).toString(),
             QStringLiteral("Air · Moon Safari"));
    // Grouped, the header above the first row, every row a line taller.
    window.chooseListView(QStringLiteral("view-two-lines-albums"));
    QTRY_VERIFY(view->albumGroupingEnabled());
    QTRY_COMPARE(view->rowHeight(1), tall);
    QVERIFY(view->rowHeight(0) >= tall + ui::QueueItemDelegate::album_header_height);
    // Covers beside the rows: the cover column is the view's artwork, shown
    // though this copy of plain columns had it hidden.
    QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
    QVERIFY(!view->isColumnHidden(local_artwork_column));
    // And back to one line.
    window.chooseListView(QStringLiteral("plain-columns"));
    QTRY_COMPARE(view->rowHeight(1), ui::track_row_height);
    QVERIFY(window.close());
}

} // namespace trackknife::bench
