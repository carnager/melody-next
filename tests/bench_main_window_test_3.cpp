// SPDX-License-Identifier: GPL-3.0-only

#include "bench_main_window_test.hpp"

namespace trackknife::bench {

// ADR-0237: a move destination is a folder on one engine's machine; the
// manager shows whose, browses that machine's folders, and saves there.
void BenchMainWindowTest::moveDestinationsArePerEngine() {
    const auto destination = [](std::string name, std::string root) {
        return persistence::SavedDestinationProfile{
            .id = core::StableId::random(),
            .profile =
                operations::DestinationProfile{.schema_version = 1U,
                                               .name = std::move(name),
                                               .root_raw_path = std::move(root),
                                               .containment_policy = {"lexical-beneath-root", 1U}}};
    };
    std::vector here{destination("Library here", "/mnt/nas/music/library")};
    std::vector there{destination("Library there", "/srv/music/library")};
    std::vector<std::string> listed_paths;
    const auto place = [](QString key, QString name,
                          std::vector<persistence::SavedDestinationProfile>& held) {
        return DestinationPlace{
            .key = std::move(key),
            .name = std::move(name),
            .load = [&held](DestinationPlace::LoadCompletion done) { done(held, {}); },
            .save =
                [&held](persistence::SavedDestinationProfile saved,
                        DestinationPlace::Completion done) {
                    std::erase_if(held, [&saved](const auto& old) { return old.id == saved.id; });
                    held.push_back(std::move(saved));
                    done({});
                },
            .remove =
                [&held](core::StableId id, DestinationPlace::Completion done) {
                    std::erase_if(held, [id](const auto& old) { return old.id == id; });
                    done({});
                },
            .folders = {},
            .copyable = {},
        };
    };
    auto local = place(QStringLiteral("local"), QStringLiteral("this computer"), here);
    auto remote = place(QStringLiteral("gemenon-id"), QStringLiteral("gemenon"), there);
    remote.folders = [&listed_paths](std::string path, EngineFolderDialog::ListingCompletion done) {
        listed_paths.push_back(path);
        const auto at = path.empty() ? std::string{"/srv/music"} : path;
        done(EngineFolderDialog::Listing{
            .path = at, .parent = std::string{"/srv"}, .folders = {"incoming", "sorted"}});
    };
    remote.copyable = [] {
        return std::vector{
            persistence::SavedDestinationProfile{
                .id = core::StableId::random(),
                .profile =
                    operations::DestinationProfile{
                        .schema_version = 1U,
                        .name = "Library here",
                        .root_raw_path = "/srv/music/sorted",
                        .containment_policy = {"lexical-beneath-root", 1U}}},
        };
    };
    const OutputProfileStore store{
        .load = [&here](OutputProfileStore::LoadCompletion done) { done({}, here, {}); },
        .save_layout = {},
        .remove_layout = {},
        .save_destination = local.save,
        .remove_destination = local.remove,
        .destinations_on = {},
        .destinations_key = {},
        .places = {local, remote},
    };
    SettingsDialog settings{nullptr, store};
    settings.showDestinationsOf(QStringLiteral("gemenon-id"));
    settings.show();
    auto* list = settings.findChild<QTreeWidget*>(QStringLiteral("bench-destination-list"));
    auto* copy = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-copy"));
    auto* browse = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-browse"));
    auto* name = settings.findChild<QLineEdit*>(QStringLiteral("bench-destination-name"));
    auto* root = settings.findChild<QLineEdit*>(QStringLiteral("bench-destination-root"));
    auto* create = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-new"));
    auto* save = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-save"));
    auto* remove = settings.findChild<QPushButton*>(QStringLiteral("bench-destination-remove"));
    auto* sections =
        settings.findChild<QTabWidget*>(QStringLiteral("bench-settings-file-operations"));
    QVERIFY(list && copy && browse && name && root && create && save && remove && sections);
    // ADR-0264: every engine's destinations in one list, under its name.
    QCOMPARE(sections->currentIndex(), 1);
    QTRY_COMPARE(list->topLevelItemCount(), 2);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("This computer"));
    QCOMPARE(list->topLevelItem(1)->text(0), QStringLiteral("Gemenon"));
    QTRY_COMPARE(list->topLevelItem(0)->childCount(), 1);
    QTRY_COMPARE(list->topLevelItem(1)->childCount(), 1);
    QCOMPARE(list->topLevelItem(1)->child(0)->text(1), QStringLiteral("/srv/music/library"));
    // Opened for an engine: its first destination selected.
    QVERIFY(list->currentItem() != nullptr);
    QCOMPARE(list->currentItem()->text(0), QStringLiteral("Library there"));
    QCOMPARE(list->currentItem()->parent(), list->topLevelItem(1));
    QCOMPARE(name->text(), QStringLiteral("Library there"));
    QVERIFY(remove->isEnabled());
    // A heading is not a destination.
    QVERIFY(!(list->topLevelItem(1)->flags() & Qt::ItemIsSelectable));

    // This computer's that lie under the engine's mount are offered.
    QTRY_VERIFY(copy->isVisible());
    QCOMPARE(copy->text(), QStringLiteral("Copy 1 from this computer"));
    QTest::mouseClick(copy, Qt::LeftButton);
    QTRY_COMPARE(there.size(), 2U);
    QVERIFY(std::ranges::any_of(there, [](const auto& saved) {
        return saved.profile.root_raw_path == "/srv/music/sorted";
    }));
    QTRY_VERIFY(!copy->isVisible());
    QCOMPARE(here.size(), 1U);

    // A new one, on the engine asked for, its folder chosen on that machine.
    QVERIFY(create->menu() != nullptr);
    QCOMPARE(create->menu()->actions().size(), 2);
    QCOMPARE(create->menu()->actions().at(1)->text(), QStringLiteral("On gemenon"));
    create->menu()->actions().at(1)->trigger();
    QVERIFY(list->currentItem() == nullptr);
    QVERIFY(!remove->isEnabled());
    name->setText(QStringLiteral("Incoming there"));
    QTest::mouseClick(browse, Qt::LeftButton);
    EngineFolderDialog* chooser = nullptr;
    QTRY_VERIFY((chooser = settings.findChild<EngineFolderDialog*>()) != nullptr);
    QCOMPARE(chooser->windowTitle(), QStringLiteral("Choose a folder on gemenon"));
    auto* folders = chooser->findChild<QListWidget*>(QStringLiteral("bench-engine-folder-list"));
    QVERIFY(folders != nullptr);
    QTRY_COMPARE(folders->count(), 2);
    folders->setCurrentRow(0);
    chooser->findChild<QPushButton*>(QStringLiteral("bench-engine-folder-choose"))->click();
    QCOMPARE(root->text(), QStringLiteral("/srv/music/incoming"));
    QTRY_VERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_COMPARE(there.size(), 3U);
    QCOMPARE(here.size(), 1U);

    QTRY_COMPARE(list->topLevelItem(1)->childCount(), 3);

    // This computer's are its own: chosen in the list, edited and removed
    // on this computer.
    QCOMPARE(list->topLevelItem(0)->childCount(), 1);
    list->setCurrentItem(list->topLevelItem(0)->child(0));
    QCOMPARE(name->text(), QStringLiteral("Library here"));
    QTRY_VERIFY(remove->isEnabled());
    QTest::mouseClick(remove, Qt::LeftButton);
    QTRY_COMPARE(here.size(), 0U);
    QCOMPARE(there.size(), 3U);
    QTRY_COMPARE(list->topLevelItem(0)->childCount(), 1); // "None yet"
    QVERIFY(!list->topLevelItem(0)->child(0)->data(0, Qt::UserRole).isValid());

    // Properties for tracks on that engine says whose destinations it offers.
    auto* properties = new MetadataPropertiesDialog(
        0U, [](std::size_t) -> std::optional<MetadataPropertiesSource> { return std::nullopt; }, {},
        {}, {}, {},
        OutputProfileStore{
            .load = [](OutputProfileStore::LoadCompletion done) { done({}, {}, {}); },
            .save_layout = {},
            .remove_layout = {},
            .save_destination = {},
            .remove_destination = {},
            .destinations_on = QStringLiteral("gemenon"),
            .destinations_key = QStringLiteral("gemenon-id"),
            .places = {}});
    auto* offered = properties->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QVERIFY(offered != nullptr);
    QCOMPARE(offered->placeholderText(), QStringLiteral("None saved on gemenon yet"));
    QCOMPARE(offered->toolTip(), QStringLiteral("Move destinations on gemenon"));
    delete properties;
}

// ADR-0237: naming layouts are global -- made here, and a copy in every
// engine, so whatever moves files through one can name them. This
// computer's engine shares the workspace: what it holds is what was saved,
// and a copy sent on connecting never lands on top of a newer one.
// ADR-0238: what the Actions popover was set to is how the next Properties
// window starts: toggles, layout, and each engine's destination.
void BenchMainWindowTest::actionChoicesAreRemembered() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("remembered.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto raw_path = QFile::encodeName(path).toStdString();
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                 .source_revision = read->source_revision,
                                                 .baseline = read->document},
        .track_label = QStringLiteral("Remembered"),
    };
    const auto layout = [](std::string name) {
        return persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile = operations::OutputLayoutProfile{.schema_version = 1U,
                                                       .name = std::move(name),
                                                       .dialect = {},
                                                       .relative_directory_expression = {},
                                                       .basename_expression = "%title%",
                                                       .sanitization_policy = {"linux", 1U}}};
    };
    const auto destination = [](std::string name) {
        return persistence::SavedDestinationProfile{
            .id = core::StableId::random(),
            .profile =
                operations::DestinationProfile{.schema_version = 1U,
                                               .name = std::move(name),
                                               .root_raw_path = "/srv/music",
                                               .containment_policy = {"lexical-beneath-root", 1U}}};
    };
    const std::vector layouts{layout("Plain"), layout("Albumartist/Date Album")};
    const std::vector destinations{destination("Incoming"), destination("Library")};
    const auto store_for = [&](const QString& engine) {
        return OutputProfileStore{
            .load =
                [&](OutputProfileStore::LoadCompletion done) { done(layouts, destinations, {}); },
            .save_layout = {},
            .remove_layout = {},
            .save_destination = {},
            .remove_destination = {},
            .destinations_on = {},
            .destinations_key = engine,
            .places = {}};
    };
    const auto open = [&](const QString& engine) {
        auto* properties = new MetadataPropertiesDialog(
            1U,
            [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
                return index == 0U ? std::optional{source} : std::nullopt;
            },
            {}, {}, {}, {}, store_for(engine),
            [] {
                return FilePublicationPlanApplier{
                    [](const operations::PreparationPlan&,
                       const operations::FilePublicationApplyProgressCallback&,
                       const core::CancellationToken&)
                        -> core::Result<operations::FilePublicationApplyResult> {
                        return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                                           .message = "not here",
                                                           .context = {}});
                    }};
            },
            {});
        properties->show();
        return properties;
    };
    const auto hidden = [](MetadataPropertiesDialog* properties, const char* name) {
        return properties->findChild<QCheckBox*>(QString::fromLatin1(name));
    };
    const auto popover_of = [](MetadataPropertiesDialog* properties) -> QFrame* {
        QTest::mouseClick(
            properties->findChild<QToolButton*>(QStringLiteral("bench-metadata-actions")),
            Qt::LeftButton);
        return properties->findChild<QFrame*>(QStringLiteral("bench-metadata-actions-popover"));
    };

    auto* first = open(QStringLiteral("gemenon-id"));
    auto* layout_combo =
        first->findChild<QComboBox*>(QStringLiteral("bench-output-layout-profile"));
    QTRY_COMPARE(layout_combo->count(), 2);
    auto* popover = popover_of(first);
    QVERIFY(popover != nullptr);
    popover->findChild<QComboBox*>(QStringLiteral("bench-actions-layout"))->setCurrentIndex(1);
    emit popover->findChild<QComboBox*>(QStringLiteral("bench-actions-layout"))->activated(1);
    popover->findChild<QComboBox*>(QStringLiteral("bench-actions-destination"))->setCurrentIndex(1);
    emit popover->findChild<QComboBox*>(QStringLiteral("bench-actions-destination"))->activated(1);
    QTRY_VERIFY(
        popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-rename-files"))->isEnabled());
    popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-rename-files"))->click();
    popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-move-files"))->click();
    QVERIFY(hidden(first, "bench-preparation-rename-files")->isChecked());
    QVERIFY(hidden(first, "bench-preparation-move-files")->isChecked());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(popover->grab().save(directory + QStringLiteral("/actions-popover.png")));
    }
    popover->close();
    delete first;

    // The next window starts as this one was left.
    auto* second = open(QStringLiteral("gemenon-id"));
    layout_combo = second->findChild<QComboBox*>(QStringLiteral("bench-output-layout-profile"));
    auto* destination_combo =
        second->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QTRY_COMPARE(layout_combo->currentText(), QStringLiteral("Albumartist/Date Album"));
    QCOMPARE(destination_combo->currentText(), QStringLiteral("Library"));
    QTRY_VERIFY(hidden(second, "bench-preparation-rename-files")->isChecked());
    QTRY_VERIFY(hidden(second, "bench-preparation-move-files")->isChecked());
    // Unticked is remembered too.
    popover = popover_of(second);
    popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-move-files"))->click();
    QVERIFY(!hidden(second, "bench-preparation-move-files")->isChecked());
    popover->close();
    delete second;

    // Another engine's destination is its own: nothing chosen there yet.
    auto* elsewhere = open(QStringLiteral("other-engine"));
    destination_combo =
        elsewhere->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QTRY_COMPARE(destination_combo->count(), 3);
    QCOMPARE(destination_combo->currentText(), QStringLiteral("Incoming"));
    QTRY_VERIFY(hidden(elsewhere, "bench-preparation-rename-files")->isChecked());
    QVERIFY(!hidden(elsewhere, "bench-preparation-move-files")->isChecked());
    delete elsewhere;
    // Left as other tests expect to find it.
    QSettings{}.remove(QStringLiteral("properties/actions"));
}

// Each Manage link opens what it names: the naming layouts, or the move
// destinations of the tracks' engine -- also when Settings is already open.
void BenchMainWindowTest::actionsLinksOpenTheirOwnSettings() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("linked.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(path).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    window.findChild<QAction*>(QStringLiteral("action-track-properties"))->trigger();
    auto* properties = window.findChild<MetadataPropertiesDialog*>();
    QVERIFY(properties != nullptr);
    const auto follow = [&](const char* link) {
        QTest::mouseClick(
            properties->findChild<QToolButton*>(QStringLiteral("bench-metadata-actions")),
            Qt::LeftButton);
        QFrame* popover = nullptr;
        QTRY_VERIFY((popover = properties->findChild<QFrame*>(
                         QStringLiteral("bench-metadata-actions-popover"))) != nullptr);
        auto* label = popover->findChild<QLabel*>(QString::fromLatin1(link));
        QVERIFY(label != nullptr);
        emit label->linkActivated(QStringLiteral("#"));
    };
    const auto shown_tab = [&window] {
        auto* settings = window.findChild<SettingsDialog*>();
        auto* sections =
            settings
                ? settings->findChild<QTabWidget*>(QStringLiteral("bench-settings-file-operations"))
                : nullptr;
        return sections != nullptr && settings->isVisible() ? sections->currentIndex() : -1;
    };
    follow("bench-actions-manage-destinations");
    QTRY_COMPARE(shown_tab(), 1);
    QVERIFY(window.findChild<SettingsDialog*>()->findChild<QTreeWidget*>(
                QStringLiteral("bench-destination-list")) != nullptr);
    // Settings still open: the other link switches it to the layouts.
    follow("bench-actions-manage-layouts");
    QTRY_COMPARE(shown_tab(), 0);
    follow("bench-actions-manage-destinations");
    QTRY_COMPARE(shown_tab(), 1);
    window.findChild<SettingsDialog*>()->reject();
}

void BenchMainWindowTest::namingLayoutsReachEveryEngine() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto client = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(client.has_value());
    const auto names = [&client] {
        std::vector<std::string> found;
        auto answer = (*client)->call("layouts.list");
        for (const auto& layout :
             answer ? answer->value("layouts", protocol::Json::array()) : protocol::Json::array()) {
            if (auto decoded = engine::wire::decode_saved_layout(layout)) {
                found.push_back(decoded->profile.name);
            }
        }
        return found;
    };
    const auto id = core::StableId::random();
    bool saved = false;
    window.buildOutputProfileStore().save_layout(
        persistence::SavedOutputLayoutProfile{
            .id = id,
            .profile = operations::OutputLayoutProfile{.schema_version = 1U,
                                                       .name = "Everywhere",
                                                       .dialect = {},
                                                       .relative_directory_expression = "%artist%",
                                                       .basename_expression = "%title%",
                                                       .sanitization_policy = {"linux", 1U}},
        },
        [&saved](const QString& error) { saved = error.isEmpty(); });
    QTRY_VERIFY(saved);
    QTRY_VERIFY_WITH_TIMEOUT(std::ranges::contains(names(), std::string{"Everywhere"}), 5'000);
    bool removed = false;
    window.buildOutputProfileStore().remove_layout(
        id, [&removed](const QString& error) { removed = error.isEmpty(); });
    QTRY_VERIFY(removed);
    QTRY_VERIFY_WITH_TIMEOUT(!std::ranges::contains(names(), std::string{"Everywhere"}), 5'000);
    (*client)->close();
}

void BenchMainWindowTest::combinedTagAndRenameReviewReachesPreparationApply_data() {
    // ADR-0237 stage 5: the rename checked and published by the engine holding
    // the file, or reaching this process's applier -- the same window, the
    // same plan.
    QTest::addColumn<bool>("through_engine");
    QTest::newRow("engine") << true;
    QTest::newRow("this-process") << false;
}

void BenchMainWindowTest::combinedTagAndRenameReviewReachesPreparationApply() {
    QFETCH(bool, through_engine);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("combined-ui-before.flac"));
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
        .track_label = QStringLiteral("Combined UI fixture"),
    };
    const std::vector layouts{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Final title",
                .dialect = {},
                .relative_directory_expression = {},
                .basename_expression = "%title%",
                .sanitization_policy = {"linux", 1U},
            },
    }};
    const OutputProfileStore output_store{
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
    const MetadataTransformationStore transformation_store{
        .load = [](MetadataTransformationStore::LoadCompletion completion) { completion({}, {}); },
        .save = {},
        .remove = {},
    };
    bool combined_applied = false;
    std::string reviewed_target;
    std::optional<operations::FilePublicationApplyResult> published;
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
        {}, {}, {}, transformation_store, output_store,
        [&combined_applied, &reviewed_target, &published, work] {
            return FilePublicationPlanApplier{
                [&combined_applied, &reviewed_target, &published,
                 work](const operations::PreparationPlan& plan,
                       const operations::FilePublicationApplyProgressCallback& progress,
                       const core::CancellationToken& cancellation)
                    -> core::Result<operations::FilePublicationApplyResult> {
                    if (!plan.ready() || !plan.metadata || !plan.path_preflight ||
                        plan.metadata->sources.size() != 1U ||
                        plan.path_preflight->sources.size() != 1U) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::invariant,
                            .message = "Expected one ready combined preparation source",
                            .context = {},
                        });
                    }
                    const auto& checked = plan.path_preflight->sources.front();
                    reviewed_target = checked.planned.target_raw_path;
                    if (work) {
                        auto applied = work->publish(plan, progress, cancellation);
                        if (applied) {
                            published = *applied;
                        }
                        combined_applied = true;
                        return applied;
                    }
                    combined_applied = true;
                    if (progress) {
                        progress(operations::FilePublicationApplyProgress{
                            .source_index = 0U,
                            .source_raw_path = checked.planned.source_raw_path,
                            .target_raw_path = checked.planned.target_raw_path,
                            .publication = checked.publication,
                            .state = operations::FilePublicationApplySourceState::committed,
                            .completed_sources = 1U,
                            .total_sources = 1U,
                            .issue = std::nullopt,
                        });
                    }
                    auto commit = operations::FilePublicationCommitResult{
                        .journal_id = core::StableId::random(),
                        .content =
                            operations::FilePublicationContentKind::prepared_destination_artifact,
                        .source_raw_path = checked.planned.source_raw_path,
                        .target_raw_path = checked.planned.target_raw_path,
                        .source_revision = checked.planned.source_revision,
                        .target_revision = checked.observed_revision,
                        .occurrence_indexes = checked.planned.item_indexes,
                        .notes = {},
                    };
                    return operations::FilePublicationApplyResult{
                        .sources = {operations::FilePublicationApplySourceResult{
                            .source_index = 0U,
                            .source_raw_path = checked.planned.source_raw_path,
                            .target_raw_path = checked.planned.target_raw_path,
                            .publication = checked.publication,
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
        {}, nullptr, {}, {}, work ? engineFileWorkTools(work) : FileWorkTools{});
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
    QVERIFY(save_tags->isChecked());
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Combined UI title"), Qt::EditRole));
    rename_files->setChecked(true);
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    // Direct apply: the combined tag-and-rename plan reaches the applier with
    // the draft title driving the new path, without any review dialog.
    QTRY_VERIFY_WITH_TIMEOUT(combined_applied, 5'000);
    QVERIFY(QString::fromStdString(reviewed_target)
                .endsWith(QStringLiteral("/Combined UI title.flac")));
    QTRY_VERIFY(closed_guard.isNull() || !closed_guard->isVisible());
    if (through_engine) {
        // Written once, at its new place, by the engine.
        QVERIFY(published && published->committed_source_count() == 1U);
        QVERIFY(!QFile::exists(path));
        const auto written = metadata::read_local_metadata(reviewed_target);
        QVERIFY(written.has_value());
        QCOMPARE(written->document.first_effective_value("title"),
                 std::optional<std::string>{"Combined UI title"});
    }
}

void BenchMainWindowTest::metadataTransformationChainPreviewsAndStagesOneUndo() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("transform.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto title = std::ranges::find(read->document.fields, std::string_view{"title"},
                                         &metadata::MetadataField::canonical_name);
    QVERIFY(title != read->document.fields.end());
    title->values = {"chain Title"};
    read->document.fields.push_back(metadata::MetadataField{
        .canonical_name = "date",
        .native_name = "DATE",
        .values = {"2024-08-30"},
        .qualifier = {},
        .provenance = metadata::FieldProvenance::embedded,
    });

    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Transformation fixture"),
    };
    std::vector<persistence::SavedMetadataTransformationChain> saved_chains;
    MetadataTransformationStore transformation_store{
        .load =
            [&saved_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(saved_chains, {});
            },
        .save =
            [&saved_chains](persistence::SavedMetadataTransformationChain chain,
                            MetadataTransformationStore::Completion completion) {
                const auto found = std::ranges::find(
                    saved_chains, chain.id, &persistence::SavedMetadataTransformationChain::id);
                if (found == saved_chains.end()) {
                    saved_chains.push_back(std::move(chain));
                } else {
                    *found = std::move(chain);
                }
                completion({});
            },
        .remove =
            [&saved_chains](core::StableId id, MetadataTransformationStore::Completion completion) {
                std::erase_if(saved_chains, [id](const auto& chain) { return chain.id == id; });
                completion({});
            },
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store);
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* transform =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    auto* redo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-redo"));
    auto* write_plan =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(transform != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(redo != nullptr);
    QVERIFY(write_plan != nullptr);
    auto* empty_script_list =
        properties->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-list"));
    auto* empty_script_status =
        properties->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-status"));
    QVERIFY(empty_script_list != nullptr);
    QVERIFY(empty_script_status != nullptr);
    QTRY_COMPARE(empty_script_list->count(), 0);
    QCOMPARE(empty_script_list->property("bench-empty-state-text").toString(),
             QStringLiteral("No saved scripts yet"));
    QTRY_VERIFY(empty_script_status->text().isEmpty());
    QVERIFY(!empty_script_status->isVisible());
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);

    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* kind =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-kind"));
    auto* target =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-transformation-target"));
    auto* target_completer = dialog->findChild<QCompleter*>(
        QStringLiteral("bench-metadata-transformation-target-completer"));
    auto* input =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-transformation-input"));
    auto* number_start =
        dialog->findChild<QSpinBox*>(QStringLiteral("bench-metadata-transformation-number-start"));
    auto* number_padding = dialog->findChild<QSpinBox*>(
        QStringLiteral("bench-metadata-transformation-number-padding"));
    auto* character_count = dialog->findChild<QSpinBox*>(
        QStringLiteral("bench-metadata-transformation-character-count"));
    auto* add =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-add"));
    auto* import_script = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-import-script"));
    auto* import_native = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-import-native"));
    auto* export_native = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-export-native"));
    auto* steps =
        dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    auto* remove =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-remove"));
    auto* stage =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-stage"));
    auto* preview_table =
        dialog->findChild<QTreeView*>(QStringLiteral("bench-metadata-transformation-table"));
    auto* preview_summary =
        dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-summary"));
    QVERIFY(kind != nullptr);
    QVERIFY(target != nullptr);
    QVERIFY(target_completer != nullptr);
    QVERIFY(input != nullptr);
    QVERIFY(number_start != nullptr);
    QVERIFY(number_padding != nullptr);
    QVERIFY(character_count != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(import_script != nullptr);
    QVERIFY(import_native != nullptr);
    QVERIFY(export_native != nullptr);
    QVERIFY(import_native->isEnabled());
    QVERIFY(!export_native->isEnabled());
    QVERIFY(steps != nullptr);
    QVERIFY(remove != nullptr);
    QVERIFY(stage != nullptr);
    QVERIFY(preview_table != nullptr);
    QVERIFY(preview_summary != nullptr);
    // 20 step kinds under 4 unselectable group headers; kinds are found by
    // name because the row index no longer matches the action kind.
    QCOMPARE(kind->count(), 24);
    for (const auto& kind_name : {QStringLiteral("Capitalize first character"),
                                  QStringLiteral("Convert rating to FMPS_RATING"),
                                  QStringLiteral("Remove exact matching values"),
                                  QStringLiteral("Replace exact matching values"),
                                  QStringLiteral("Number by selected-file order"),
                                  QStringLiteral("Keep first characters of each value"),
                                  QStringLiteral("Remove field when condition matches"),
                                  QStringLiteral("Remove listed fields (blocklist)"),
                                  QStringLiteral("Keep only listed fields (allowlist)"),
                                  QStringLiteral("Capture fields with tkcapture-1")}) {
        QVERIFY2(kind->findText(kind_name) >= 0, qPrintable(kind_name));
    }
    QVERIFY(!kind->model()->flags(kind->model()->index(0, 0)).testFlag(Qt::ItemIsSelectable));

    // The field-filter Fields box completes each comma-separated name from
    // the present fields plus the standard conventional and MusicBrainz
    // catalog, while custom names stay freely typable.
    auto* fields_completions = dialog->findChild<QStringListModel*>(
        QStringLiteral("bench-metadata-transformation-fields-completions"));
    QVERIFY(fields_completions != nullptr);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Remove listed fields (blocklist)")));
    input->setFocus(Qt::OtherFocusReason);
    input->clear();
    QTest::keyClicks(input, QStringLiteral("musicb"));
    // The selection's present spelling deduplicates the catalog entry and
    // wins the suggestion, so the box reflects the files being edited.
    QVERIFY2(fields_completions->stringList().contains(QStringLiteral("MUSICBRAINZ_TRACKID")),
             qPrintable(fields_completions->stringList().join(QStringLiteral(" | "))));
    input->clear();
    QTest::keyClicks(input, QStringLiteral("Custom Field, gen"));
    QVERIFY(fields_completions->stringList().contains(QStringLiteral("Genre")));
    QMetaObject::invokeMethod(dialog->findChild<QCompleter*>(
                                  QStringLiteral("bench-metadata-transformation-fields-completer")),
                              "activated", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("Genre")));
    QCOMPARE(input->text(), QStringLiteral("Custom Field, Genre"));
    input->clear();
    kind->setCurrentIndex(kind->findText(QStringLiteral("Capitalize first character")));

    QTimer::singleShot(0, dialog, [dialog] {
        auto* importer =
            dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-rule-script-import"));
        QVERIFY(importer != nullptr);
        auto* source_edit = importer->findChild<QPlainTextEdit*>(
            QStringLiteral("bench-metadata-rule-script-source"));
        auto* diagnostics = importer->findChild<QPlainTextEdit*>(
            QStringLiteral("bench-metadata-rule-script-diagnostics"));
        auto* replace =
            importer->findChild<QPushButton*>(QStringLiteral("bench-metadata-rule-script-replace"));
        QVERIFY(source_edit != nullptr);
        QVERIFY(diagnostics != nullptr);
        QVERIFY(replace != nullptr);
        source_edit->setPlainText(QStringLiteral(
            "$delete(comment:)\n"
            "$if($or($not(%totaldiscs%),$eq(%totaldiscs%,1)),"
            "$delete(discnumber)$delete(totaldiscs))\n"
            "$if(%originaldate%,$set(date,$left(%originaldate%,4))"
            "$set(originaldate,$left(%originaldate%,4)),$set(date,$left(%date%,4)))"));
        QVERIFY(replace->isEnabled());
        QVERIFY(diagnostics->toPlainText().contains(QStringLiteral("5 generated rules")));
        QTest::mouseClick(replace, Qt::LeftButton);
    });
    QTest::mouseClick(import_script, Qt::LeftButton);
    QCOMPARE(steps->count(), 5);
    QVERIFY(steps->item(0)->text().contains(QStringLiteral("Remove exact native field comment")));
    QVERIFY(steps->item(1)->text().contains(
        QStringLiteral("Remove exact native field discnumber when "
                       "$or($not(%totaldiscs%),$eq(%totaldiscs%,1))")));
    QVERIFY(steps->item(2)->text().contains(
        QStringLiteral("Remove exact native field totaldiscs when "
                       "$or($not(%totaldiscs%),$eq(%totaldiscs%,1))")));
    QVERIFY(steps->item(3)->text().contains(QStringLiteral("Format date as $if(%originaldate%")));
    QVERIFY(steps->item(4)->text().contains(
        QStringLiteral("Keep the first 4 characters of each value of originaldate")));
    auto* editor_tabs =
        dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-transformation-editor-tabs"));
    auto* raw_source = dialog->findChild<QPlainTextEdit*>(
        QStringLiteral("bench-metadata-transformation-raw-source"));
    auto* raw_diagnostics = dialog->findChild<QPlainTextEdit*>(
        QStringLiteral("bench-metadata-transformation-raw-diagnostics"));
    QVERIFY(editor_tabs != nullptr);
    QVERIFY(raw_source != nullptr);
    QVERIFY(raw_diagnostics != nullptr);
    QCOMPARE(editor_tabs->tabText(1), QStringLiteral("Raw script"));
    QVERIFY(raw_source->toPlainText().contains(
        QStringLiteral("$or($not(%totaldiscs%),$eq(%totaldiscs%,1))")));
    auto revised_source = raw_source->toPlainText();
    revised_source.replace(QStringLiteral("$or($not(%totaldiscs%),$eq(%totaldiscs%,1))"),
                           QStringLiteral("$eq(%totaldiscs%,1)"));
    raw_source->setPlainText(revised_source);
    QTRY_VERIFY(raw_diagnostics->toPlainText().contains(QStringLiteral("Ready")));
    QVERIFY(steps->item(1)->text().contains(QStringLiteral("$eq(%totaldiscs%,1)")));
    QVERIFY(!steps->item(1)->text().contains(QStringLiteral("$not(%totaldiscs%)")));
    QVERIFY(dialog->isWindowModified());
    QTimer::singleShot(0, dialog, [] {
        auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(confirmation != nullptr);
        auto* cancel = confirmation->button(QMessageBox::Cancel);
        QVERIFY(cancel != nullptr);
        QTest::mouseClick(cancel, Qt::LeftButton);
    });
    dialog->close();
    QVERIFY(dialog->isVisible());
    auto* save =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-save"));
    QVERIFY(save != nullptr);
    QVERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(saved_chains.size(), std::size_t{1U});
    const auto* saved_conditional =
        std::get_if<metadata::MetadataRemoveFieldIfAction>(&saved_chains.front().chain.actions[1]);
    QVERIFY(saved_conditional != nullptr);
    QCOMPARE(saved_conditional->condition, std::string{"$eq(%totaldiscs%,1)"});
    QVERIFY(!dialog->isWindowModified());
    for (auto count = 0; count < 5; ++count) {
        QTest::mouseClick(remove, Qt::LeftButton);
    }
    QCOMPARE(steps->count(), 0);

    target->setText(QStringLiteral("custom"));
    QTRY_VERIFY(target_completer->model()->rowCount() > 0);
    QCOMPARE(target_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("CUSTOM_FIELD"));
    target->clear();

    kind->setCurrentIndex(kind->findText(QStringLiteral("Capitalize first character")));
    target->setText(QStringLiteral("Album Artist"));
    QTest::mouseClick(add, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(preview_table->model() != nullptr, 5'000);
    QCOMPARE(preview_table->model()->rowCount(), 0);
    QTRY_VERIFY(preview_summary->text().contains(
        QStringLiteral("every existing Album Artist value already starts with its uppercase "
                       "form")));

    kind->setCurrentIndex(kind->findText(QStringLiteral("Capitalize first character")));
    target->setText(QStringLiteral("Title"));
    QTest::mouseClick(add, Qt::LeftButton);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Format with tkfmt-1")));
    target->setText(QStringLiteral("Comment"));
    input->setText(QStringLiteral("%artist% — %title%"));
    QTest::mouseClick(add, Qt::LeftButton);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Number by selected-file order")));
    target->setText(QStringLiteral("Track Number"));
    number_start->setValue(7);
    number_padding->setValue(2);
    QTest::mouseClick(add, Qt::LeftButton);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Keep first characters of each value")));
    target->setText(QStringLiteral("Date"));
    QCOMPARE(character_count->value(), 4);
    QTest::mouseClick(add, Qt::LeftButton);

    auto* saved =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-saved"));
    QVERIFY(saved != nullptr);
    QVERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(saved_chains.size(), std::size_t{1U});
    QCOMPARE(saved->count(), 2);
    QCOMPARE(saved->currentIndex(), 1);

    QVERIFY(export_native->isEnabled());
    dialog->close();
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    auto* script_panel =
        properties->findChild<QWidget*>(QStringLiteral("bench-metadata-side-panel"));
    auto* script_list =
        properties->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-list"));
    auto* script_status =
        properties->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-status"));
    QVERIFY(script_panel != nullptr);
    QVERIFY(script_list != nullptr);
    QVERIFY(script_status != nullptr);
    QTRY_COMPARE(script_list->count(), 1);
    QCOMPARE(script_list->currentRow(), 0);
    QCOMPARE(script_list->item(0)->checkState(), Qt::Unchecked);

    QTRY_VERIFY(transform->isEnabled());
    QCOMPARE(transform->text(), QStringLiteral("Edit selected script…"));
    QTest::mouseClick(transform, Qt::LeftButton);
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    saved = dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-saved"));
    steps = dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    stage = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-stage"));
    preview_table =
        dialog->findChild<QTreeView*>(QStringLiteral("bench-metadata-transformation-table"));
    preview_summary =
        dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-summary"));
    QVERIFY(saved != nullptr);
    QVERIFY(steps != nullptr);
    QVERIFY(stage != nullptr);
    QVERIFY(preview_table != nullptr);
    QVERIFY(preview_summary != nullptr);
    QCOMPARE(saved->count(), 2);
    QCOMPARE(saved->currentIndex(), 1);
    QCOMPARE(steps->count(), 5);
    QVERIFY(steps->item(4)->text().contains(
        QStringLiteral("Keep the first 4 characters of each value of Date")));
    QTRY_VERIFY_WITH_TIMEOUT(preview_table->model() != nullptr, 5'000);
    QTRY_COMPARE(preview_table->model()->rowCount(), 4);
    QCOMPARE(preview_table->model()->headerData(0, Qt::Horizontal).toString(),
             QStringLiteral("Field"));
    QCOMPARE(preview_table->model()->headerData(1, Qt::Horizontal).toString(),
             QStringLiteral("Old"));
    QCOMPARE(preview_table->model()->headerData(2, Qt::Horizontal).toString(),
             QStringLiteral("New"));
    QCOMPARE(preview_table->model()->index(0, 0).data().toString(), QStringLiteral("Title"));
    QCOMPARE(preview_table->model()->index(0, 2).data().toString(), QStringLiteral("Chain Title"));
    QCOMPARE(preview_table->model()->index(1, 0).data().toString(), QStringLiteral("Comment"));
    QCOMPARE(preview_table->model()->index(1, 2).data().toString(),
             QStringLiteral("First Artist; Second Artist — Chain Title"));
    QCOMPARE(preview_table->model()->index(2, 0).data().toString(), QStringLiteral("Track Number"));
    QCOMPARE(preview_table->model()->index(2, 2).data().toString(), QStringLiteral("07"));
    QCOMPARE(preview_table->model()->index(3, 0).data().toString(), QStringLiteral("Date"));
    QCOMPARE(preview_table->model()->index(3, 1).data().toString(), QStringLiteral("2024-08-30"));
    QCOMPARE(preview_table->model()->index(3, 2).data().toString(), QStringLiteral("2024"));
    const auto first_change = preview_table->model()->index(0, 0);
    QCOMPARE(preview_table->model()->rowCount(first_change), 1);
    QCOMPARE(preview_table->model()->index(0, 0, first_change).data().toString(),
             QStringLiteral("File"));
    QVERIFY(preview_table->model()
                ->index(0, 2, first_change)
                .data()
                .toString()
                .contains(QStringLiteral("step 2")));
    QVERIFY(preview_summary->text().contains(QStringLiteral("4 final cell changes")));
    QVERIFY(stage->isEnabled());
    QTest::mouseClick(stage, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);

    const auto title_column = grid_model->fieldColumn(QStringLiteral("title"));
    const auto comment_column = grid_model->fieldColumn(QStringLiteral("comment"));
    const auto track_number_column = grid_model->fieldColumn(QStringLiteral("track number"));
    const auto date_column = grid_model->fieldColumn(QStringLiteral("date"));
    QVERIFY(title_column.has_value());
    QVERIFY(comment_column.has_value());
    QVERIFY(track_number_column.has_value());
    QVERIFY(date_column.has_value());
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{4U});
    QCOMPARE(grid_model->index(0, *title_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Chain Title")}));
    QCOMPARE(grid_model->index(0, *comment_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("First Artist; Second Artist — Chain Title")}));
    QCOMPARE(
        grid_model->index(0, *track_number_column).data(metadata_cell_values_role).toStringList(),
        (QStringList{QStringLiteral("07")}));
    QCOMPARE(grid_model->index(0, *date_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("2024")}));

    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{0U});
    QTRY_VERIFY(redo->isEnabled());
    QTest::mouseClick(redo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{4U});

    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{0U});
    const auto album_column = grid_model->fieldColumn(QStringLiteral("album"));
    QVERIFY(album_column.has_value());
    const std::array automatic_items{std::size_t{0U}};
    QVERIFY(grid_model->replaceFieldValues(
        automatic_items, static_cast<std::size_t>(*album_column - 1), {"Automatic workflow"}));
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});

    QTRY_VERIFY(write_plan->isEnabled());
    QTest::mouseClick(write_plan, Qt::LeftButton);
    // Direct apply plans in the background; this fixture wires no applier, so
    // the ready plan stops at the unavailable notice and the draft survives.
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("Apply is unavailable")),
                             5'000);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});
    QTRY_VERIFY(write_plan->isEnabled());
    QTest::mouseClick(write_plan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("Apply is unavailable")),
                             5'000);
    QTRY_VERIFY(write_plan->isEnabled());
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});

    // Checking a script stages its edits immediately as one more colored
    // draft transaction — Apply stays WYSIWYG, nothing runs hidden at write
    // time.
    script_list->item(0)->setCheckState(Qt::Checked);
    QTRY_VERIFY(saved_chains.front().automatic);
    QTRY_VERIFY(script_status->text().contains(QStringLiteral("1 of 1 checked")));
    QTRY_COMPARE_WITH_TIMEOUT(grid_model->patches().patch_count(), std::size_t{5U}, 5'000);
    // The sticky footer summary names the script and offers one-click undo.
    QTRY_VERIFY(status->text().contains(QStringLiteral("staged 4 edits")));
    QVERIFY(status->text().contains(QStringLiteral("undo-automatic")));
    QCOMPARE(grid_model->index(0, *date_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("2024")}));
    // Script edits speak in italics and name their source; the hand edit
    // stays upright and unattributed.
    const auto date_index = grid_model->index(0, *date_column);
    QVERIFY(date_index.data(metadata_cell_staged_source_role)
                .toString()
                .contains(QStringLiteral("step")));
    QVERIFY(date_index.data(Qt::FontRole).value<QFont>().italic());
    QVERIFY(date_index.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Staged by")));
    const auto album_index = grid_model->index(0, *album_column);
    QVERIFY(album_index.data(metadata_cell_staged_source_role).toString().isEmpty());
    QVERIFY(!album_index.data(Qt::FontRole).value<QFont>().italic());
    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});
    delete properties;
}

void BenchMainWindowTest::metadataCapturePatternSavesReloadsAndStagesAllFields() {
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = "/library/Alpha/Album/03. Song.flac",
                .source_revision = std::nullopt,
                .baseline = metadata::MetadataDocument{},
            },
        .track_label = QStringLiteral("Capture fixture"),
    };
    std::vector<persistence::SavedMetadataTransformationChain> saved_chains;
    MetadataTransformationStore store{
        .load =
            [&saved_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(saved_chains, {});
            },
        .save =
            [&saved_chains](persistence::SavedMetadataTransformationChain chain,
                            MetadataTransformationStore::Completion completion) {
                const auto found = std::ranges::find(
                    saved_chains, chain.id, &persistence::SavedMetadataTransformationChain::id);
                if (found == saved_chains.end()) {
                    saved_chains.push_back(std::move(chain));
                } else {
                    *found = std::move(chain);
                }
                completion({});
            },
        .remove = {},
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, store);
    properties->show();
    auto* transform =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(transform != nullptr);
    QVERIFY(grid_model != nullptr);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);

    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* kind =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-kind"));
    auto* source_kind = dialog->findChild<QComboBox*>(
        QStringLiteral("bench-metadata-transformation-capture-source"));
    auto* source_argument = dialog->findChild<QLineEdit*>(
        QStringLiteral("bench-metadata-transformation-capture-argument"));
    auto* input =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-transformation-input"));
    auto* add =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-add"));
    auto* steps =
        dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    auto* save_as = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-save-as-new"));
    QVERIFY(kind != nullptr);
    QVERIFY(source_kind != nullptr);
    QVERIFY(source_argument != nullptr);
    QVERIFY(input != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(steps != nullptr);
    QVERIFY(save_as != nullptr);
    kind->setCurrentIndex(kind->findText(QStringLiteral("Capture fields with tkcapture-1")));
    QCOMPARE(source_kind->count(), 4);
    source_kind->setCurrentIndex(0);
    QVERIFY(!source_argument->isVisible());
    input->setText(QStringLiteral("%artist%/%album%/%tracknumber%. %title%"));
    QTest::mouseClick(add, Qt::LeftButton);
    QCOMPARE(steps->count(), 1);
    QVERIFY(steps->item(0)->text().contains(QStringLiteral("Capture filename with")));
    QTest::mouseClick(save_as, Qt::LeftButton);
    QCOMPARE(saved_chains.size(), std::size_t{1U});
    const auto* saved_capture = std::get_if<metadata::MetadataCaptureValuesAction>(
        &saved_chains.front().chain.actions.front());
    QVERIFY(saved_capture != nullptr);
    QCOMPARE(saved_capture->pattern, std::string{"%artist%/%album%/%tracknumber%. %title%"});

    dialog->close();
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    auto* script_list =
        properties->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-list"));
    QVERIFY(script_list != nullptr);
    QTRY_COMPARE(script_list->count(), 1);
    script_list->setCurrentRow(0);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    steps = dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    auto* stage =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-stage"));
    auto* table =
        dialog->findChild<QTreeView*>(QStringLiteral("bench-metadata-transformation-table"));
    QVERIFY(steps != nullptr);
    QVERIFY(stage != nullptr);
    QVERIFY(table != nullptr);
    QCOMPARE(steps->count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(table->model() != nullptr, 5'000);
    QCOMPARE(table->model()->rowCount(), 4);
    QStringList fields;
    for (auto row = 0; row < table->model()->rowCount(); ++row) {
        fields.push_back(table->model()->index(row, 0).data().toString().toCaseFolded());
    }
    QCOMPARE(fields, (QStringList{QStringLiteral("artist"), QStringLiteral("album"),
                                  QStringLiteral("tracknumber"), QStringLiteral("title")}));
    QVERIFY(stage->isEnabled());
    QTest::mouseClick(stage, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{4U});
    const auto artist = grid_model->fieldColumn(QStringLiteral("artist"));
    const auto album = grid_model->fieldColumn(QStringLiteral("album"));
    const auto track = grid_model->fieldColumn(QStringLiteral("tracknumber"));
    const auto title = grid_model->fieldColumn(QStringLiteral("title"));
    QVERIFY(artist && album && track && title);
    QCOMPARE(grid_model->index(0, *artist).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Alpha")});
    QCOMPARE(grid_model->index(0, *album).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Album")});
    QCOMPARE(grid_model->index(0, *track).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("03")});
    QCOMPARE(grid_model->index(0, *title).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Song")});
    delete properties;
}

void BenchMainWindowTest::metadataSuggestionsStageSelectionConsistency() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field](const QString& label, const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + label.toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}),
                                       field("TRACKNUMBER", {track_number.toStdString()})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = label,
        };
    };
    const std::vector sources{make_source(QStringLiteral("one"), QStringLiteral("1")),
                              make_source(QStringLiteral("two"), QStringLiteral("2")),
                              make_source(QStringLiteral("three"), QStringLiteral("3"))};

    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* suggest = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-suggest"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(suggest != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(status != nullptr);
    files->selectAll();
    QTRY_VERIFY(suggest->isEnabled());
    suggest->click();

    // The internal consistency provider stages album artist and total tracks
    // for every file as one ordinary colored draft transaction.
    QTRY_COMPARE_WITH_TIMEOUT(grid_model->patches().patch_count(), std::size_t{6U}, 5'000);
    QVERIFY(status->text().contains(QStringLiteral("6 suggestions")));
    QVERIFY(status->text().contains(QStringLiteral("Selection consistency")));
    QVERIFY(aggregate_model->fieldRow(QStringLiteral("Album Artist")).has_value());
    QVERIFY(aggregate_model->fieldRow(QStringLiteral("Total Tracks")).has_value());
    const auto album_artist_column = grid_model->fieldColumn(QStringLiteral("Album Artist"));
    const auto totals_column = grid_model->fieldColumn(QStringLiteral("Total Tracks"));
    QVERIFY(album_artist_column.has_value());
    QVERIFY(totals_column.has_value());
    for (int row = 0; row < grid_model->rowCount(); ++row) {
        QCOMPARE(grid_model->index(row, *album_artist_column)
                     .data(metadata_cell_values_role)
                     .toStringList(),
                 QStringList{QStringLiteral("Band")});
        QCOMPARE(
            grid_model->index(row, *totals_column).data(metadata_cell_values_role).toStringList(),
            QStringList{QStringLiteral("3")});
    }

    // One undo removes the whole suggestion transaction.
    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QTRY_COMPARE(grid_model->patches().patch_count(), std::size_t{0U});

    // A second run restages; files that already agree produce nothing new.
    // Undoing the staged field extension resets the grid, so reselect first.
    files->selectAll();
    QTRY_VERIFY(suggest->isEnabled());
    QTest::mouseClick(suggest, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(grid_model->patches().patch_count(), std::size_t{6U}, 5'000);
    files->selectAll();
    QTRY_VERIFY(suggest->isEnabled());
    QTest::mouseClick(suggest, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("already agree")), 5'000);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{6U});
    delete properties;
}

void BenchMainWindowTest::musicBrainzMatchingHandlesUnequalCounts_data() {
    QTest::addColumn<int>("file_count");
    QTest::newRow("missing-file") << 1;
    QTest::newRow("extra-file") << 3;
    QTest::newRow("scroll-alignment") << 120;
}

void BenchMainWindowTest::musicBrainzMatchingHandlesUnequalCounts() {
    QFETCH(int, file_count);
    musicbrainz::Release release{};
    release.id = "2f2ac1b7-1111-4f4f-8f8f-123456789abc";
    release.title = "Two discs";
    for (std::size_t disc = 1; disc <= 2; ++disc) {
        musicbrainz::ReleaseMedium medium{};
        medium.position = disc;
        medium.track_count = 1;
        musicbrainz::ReleaseTrack track{};
        track.position = 1;
        track.title = "Disc " + std::to_string(disc);
        medium.tracks.push_back(track);
        release.media.push_back(medium);
    }
    std::vector<musicbrainz::LocalTrackDescriptor> local(static_cast<std::size_t>(file_count));
    std::vector<QString> paths;
    std::vector<std::size_t> indexes;
    for (int index = 0; index < file_count; ++index) {
        paths.push_back(QStringLiteral("/music/file-%1.flac").arg(index));
        indexes.push_back(static_cast<std::size_t>(index + 10));
    }
    std::optional<metadata::MetadataProposalSet> accepted;
    std::unique_ptr<QWidget> widget{createMusicBrainzTrackMatchWidget(
        release, local, paths, indexes,
        [&accepted](metadata::MetadataProposalSet proposal) { accepted = std::move(proposal); },
        [] {}, nullptr)};
    widget->show();
    auto* rows = widget->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-files"));
    auto* up = widget->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-up"));
    auto* down = widget->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-down"));
    auto* stage = widget->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-stage"));
    auto* tracks =
        widget->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-tracks"));
    QVERIFY(rows && tracks && up && down && stage);
    QTRY_COMPARE(rows->topLevelItemCount(), std::max(file_count, 2));
    QCOMPARE(tracks->topLevelItem(0)->text(0), QStringLiteral("1.1 · Disc 1"));
    QCOMPARE(tracks->topLevelItem(1)->text(0), QStringLiteral("2.1 · Disc 2"));
    if (file_count == 1) {
        QCOMPARE(rows->topLevelItem(1)->text(0), QStringLiteral("No local file"));
        rows->setCurrentItem(rows->topLevelItem(0));
        down->click();
        QCOMPARE(rows->topLevelItem(0)->text(0), QStringLiteral("No local file"));
    } else {
        QVERIFY(rows->topLevelItem(2)->text(2).contains(QStringLiteral("Unmatched")));
        rows->setCurrentItem(rows->topLevelItem(2));
        up->click();
        QCOMPARE(rows->topLevelItem(1)->text(0), QStringLiteral("file-2.flac"));
        QCOMPARE(rows->topLevelItem(2)->text(0), QStringLiteral("file-1.flac"));
    }
    QCOMPARE(tracks->topLevelItem(1)->text(0), QStringLiteral("2.1 · Disc 2"));
    if (file_count > 3) {
        QTRY_VERIFY(rows->verticalScrollBar()->maximum() > 0);
        rows->verticalScrollBar()->setValue(rows->verticalScrollBar()->maximum());
        QCOMPARE(tracks->verticalScrollBar()->value(), rows->verticalScrollBar()->value());
    }
    QVERIFY(!accepted);
    stage->click();
    QVERIFY(accepted.has_value());
    QCOMPARE(accepted->items.size(), static_cast<std::size_t>(std::min(file_count, 2)));
    QCOMPARE(accepted->items.back().item_index, file_count == 1 ? 10U : 12U);
}

// ADR-0261: every album open in the tagger at once -- grouped, looked up in
// turn, and a clear match staged into the draft by itself.
void BenchMainWindowTest::identifyAlbumsGroupsLooksUpAndStages() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [](std::string path, std::vector<metadata::MetadataField> fields,
                                std::int64_t length) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = std::move(path),
                    .source_revision = std::nullopt,
                    .baseline = metadata::MetadataDocument{.fields = std::move(fields),
                                                           .unsupported_native_objects = {}},
                },
            .track_label = {},
            .duration_ms = length,
        };
    };
    constexpr auto release_id = "11111111-2222-3333-4444-555555555555";
    const std::vector sources{
        make_source("/music/Band/Alpha/01.flac",
                    {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}), field("TITLE", {"One"}),
                     field("TRACKNUMBER", {"1"}), field("MUSICBRAINZ_ALBUMID", {release_id})},
                    61'500),
        make_source("/music/Other/Beta/01.flac",
                    {field("ALBUM", {"Beta"}), field("ARTIST", {"Other"}),
                     field("TITLE", {"Something else"}), field("TRACKNUMBER", {"1"})},
                    300'000),
        make_source("/music/Band/Alpha/02.flac",
                    {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}), field("TITLE", {"Two"}),
                     field("TRACKNUMBER", {"2"}), field("MUSICBRAINZ_ALBUMID", {release_id})},
                    58'800),
        make_source("/music/Other/Beta/02.flac",
                    {field("ALBUM", {"Beta"}), field("ARTIST", {"Other"}),
                     field("TITLE", {"Different again"}), field("TRACKNUMBER", {"2"})},
                    290'000),
        make_source("/music/incoming/rip-0412/track01.flac", {}, 100'000),
    };
    static constexpr auto search_body = R"json({
      "count": 1,
      "releases": [{
        "id": "11111111-2222-3333-4444-555555555555",
        "score": 100, "title": "Alpha", "status": "Official",
        "date": "1999-09-09", "country": "DE", "track-count": 2,
        "artist-credit": [{"name": "Band",
          "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
        "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
        "media": [{"format": "CD", "track-count": 2}]
      }]
    })json";
    static constexpr auto lookup_body = R"json({
      "id": "11111111-2222-3333-4444-555555555555",
      "title": "Alpha", "status": "Official", "date": "1999-09-09", "country": "DE",
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "artist-credit": [{"name": "Band",
        "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
      "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
        {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
         "title": "One", "length": 61000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
        {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
         "title": "Two", "length": 59000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
      ]}]
    })json";
    std::vector<QString> asked;
    const MusicBrainzLookupService service{
        .fetch =
            [&asked](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                asked.push_back(url);
                QTimer::singleShot(1, [url, completion = std::move(completion)] {
                    completion(QByteArray{url.contains(QStringLiteral("?query=")) ? search_body
                                                                                  : lookup_body});
                });
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid_model != nullptr);
    auto* open =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify-albums"));
    QVERIFY(open != nullptr);
    QTRY_VERIFY(open->isEnabled());
    open->click();
    auto* dialog =
        properties->findChild<IdentifyAlbumsDialog*>(QStringLiteral("bench-identify-albums"));
    QVERIFY(dialog != nullptr);
    auto* list = dialog->findChild<QTreeWidget*>(QStringLiteral("bench-identify-albums-list"));
    auto* detail = dialog->findChild<QTextBrowser*>(QStringLiteral("bench-identify-albums-detail"));
    QVERIFY(list != nullptr && detail != nullptr);

    // Three albums, each saying how it was grouped; looked up at once, one
    // left out meanwhile.
    QCOMPARE(list->topLevelItemCount(), 3);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("Band — Alpha"));
    QCOMPARE(list->topLevelItem(2)->text(0), QStringLiteral("rip-0412"));
    for (const auto& [row, basis] :
         {std::pair{0, "grouped by release id"}, std::pair{1, "grouped by tags"},
          std::pair{2, "grouped by folder"}}) {
        list->setCurrentItem(list->topLevelItem(row));
        QVERIFY2(detail->toPlainText().contains(QLatin1String(basis)),
                 qPrintable(detail->toPlainText()));
    }
    list->topLevelItem(2)->setCheckState(0, Qt::Unchecked);

    // The one with an id by its id; the other by a search.
    auto* session = dialog->session();
    QTRY_COMPARE_WITH_TIMEOUT(session->count(AlbumBatchSession::State::staged), std::size_t{1U},
                              5'000);
    QTRY_COMPARE(session->count(AlbumBatchSession::State::no_match), std::size_t{1U});
    QCOMPARE(session->count(AlbumBatchSession::State::left_out), std::size_t{1U});
    QCOMPARE(std::ranges::count_if(
                 asked, [](const QString& url) { return url.contains(QStringLiteral("?query=")); }),
             1);
    QVERIFY(std::ranges::any_of(asked, [](const QString& url) {
        return url.contains(QStringLiteral("release/11111111-2222-3333-4444-555555555555"));
    }));
    QVERIFY(list->topLevelItem(0)->text(2).startsWith(QStringLiteral("Matched")));
    QVERIFY(list->topLevelItem(0)->text(2).endsWith(QStringLiteral("staged")));
    QCOMPARE(list->topLevelItem(1)->text(2), QStringLiteral("No match"));

    // Staged into the tagger's draft: the release's id on both of its files.
    QVERIFY(grid_model->patches().patch_count() > 0U);
    list->setCurrentItem(list->topLevelItem(0));
    QVERIFY(detail->toPlainText().contains(QStringLiteral("Paired by track number")));
    QVERIFY(detail->toPlainText().contains(QStringLiteral("2 files, 2 tracks, 2 paired")));

    if (const auto screenshot_dir = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !screenshot_dir.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshot_dir + QStringLiteral("/identify-albums.png")));
    }
    // Narrowed to what matched.
    dialog->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-filter-2"))->click();
    QCOMPARE(list->topLevelItemCount(), 1);
    // Its draft is not written: gone with it, without asking.
    delete properties;

    // An album across disc folders is one album; split, it is one a folder,
    // and merged, one again.
    const std::vector discs{
        make_source("/music/Band/Alpha/CD 1/01.flac",
                    {field("ALBUM", {"Alpha"}), field("ARTIST", {"Band"}), field("TITLE", {"One"})},
                    61'000),
        make_source(
            "/music/Band/Alpha/CD 2/01.flac",
            {field("ALBUM", {"Alpha (CD 2)"}), field("ARTIST", {"Band"}), field("TITLE", {"Two"})},
            59'000)};
    auto* split_dialog = new MetadataPropertiesDialog(
        discs.size(),
        [discs](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < discs.size() ? std::optional{discs[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    split_dialog->show();
    auto* open_split =
        split_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify-albums"));
    QTRY_VERIFY(open_split->isEnabled());
    open_split->click();
    auto* albums_dialog =
        split_dialog->findChild<IdentifyAlbumsDialog*>(QStringLiteral("bench-identify-albums"));
    auto* albums =
        albums_dialog->findChild<QTreeWidget*>(QStringLiteral("bench-identify-albums-list"));
    auto* split =
        albums_dialog->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-split"));
    QCOMPARE(albums->topLevelItemCount(), 1);
    QVERIFY(albums->topLevelItem(0)->text(1).contains(QStringLiteral("2 folders")));
    albums->setCurrentItem(albums->topLevelItem(0));
    QVERIFY(split->isVisibleTo(albums_dialog));
    split->click();
    QCOMPARE(albums->topLevelItemCount(), 2);
    const auto order = albums_dialog->session()->order();
    QCOMPARE(order.size(), 2U);
    albums_dialog->session()->merge(order[1], order[0]);
    QCOMPARE(albums->topLevelItemCount(), 1);
    QCOMPARE(albums_dialog->session()->albums()[order[0]].items.size(), 2U);
    // Changed while being looked up: looked up again, as it now is.
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const auto state = albums_dialog->session()->albums()[order[0]].state;
            return state != AlbumBatchSession::State::waiting &&
                   state != AlbumBatchSession::State::searching;
        }(),
        5'000);
    QCOMPARE(albums_dialog->session()->count(AlbumBatchSession::State::waiting), std::size_t{0U});
    delete split_dialog;
}

// ADR-0262: Move into a folder that is no saved destination, the layout's
// folders made inside it; offered again after.
void BenchMainWindowTest::applyMovesIntoAChosenFolder() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto work = std::make_shared<engine::RemoteFileWork>(
        protocol::Endpoint{.socket = QFile::encodeName(engine_.socket()).toStdString(),
                           .host = {},
                           .port = 0,
                           .token = {}});
    const auto path = media.filePath(QStringLiteral("in/one.flac"));
    QVERIFY(QDir{}.mkpath(media.filePath(QStringLiteral("in"))));
    QVERIFY(QDir{}.mkpath(media.filePath(QStringLiteral("chosen"))));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), path));
    const auto raw_path = QFile::encodeName(path).toStdString();
    const auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                 .source_revision = read->source_revision,
                                                 .baseline = read->document},
        .track_label = {},
    };
    const std::vector layouts{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile = operations::OutputLayoutProfile{.schema_version = 1U,
                                                   .name = "Artist folders",
                                                   .dialect = {},
                                                   .relative_directory_expression = "%artist%",
                                                   .basename_expression = "%title%",
                                                   .sanitization_policy = {"linux", 1U}}}};
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {},
        TaggerServices{
            .plan_applier_factory = {},
            .apply_observer = {},
            .transformation_store =
                MetadataTransformationStore{.load = [](MetadataTransformationStore::LoadCompletion
                                                           completion) { completion({}, {}); },
                                            .save = {},
                                            .remove = {}},
            .output_profile_store =
                OutputProfileStore{.load =
                                       [layouts](OutputProfileStore::LoadCompletion completion) {
                                           completion(layouts, {}, {});
                                       },
                                   .save_layout = {},
                                   .remove_layout = {},
                                   .save_destination = {},
                                   .remove_destination = {},
                                   .destinations_on = {},
                                   .destinations_key = QStringLiteral("chosen-folder-test"),
                                   .places = {}},
            .file_plan_applier_factory =
                [work] {
                    return FilePublicationPlanApplier{
                        [work](const operations::PreparationPlan& plan,
                               const operations::FilePublicationApplyProgressCallback& progress,
                               const core::CancellationToken& cancellation) {
                            return work->publish(plan, progress, cancellation);
                        }};
                },
            .file_apply_observer = {},
            .layout_store = {},
            .musicbrainz = {},
            .tools = engineFileWorkTools(work),
            .library_elsewhere = {},
            .undo = {},
        });
    const QPointer<MetadataPropertiesDialog> guard{properties};
    properties->setProperty("trackknife-move-folder",
                            QFile::encodeName(media.filePath(QStringLiteral("chosen"))));
    properties->show();
    auto* tagger = properties->findChild<TaggerSession*>();
    auto* destinations =
        properties->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    QTRY_COMPARE(destinations->count(), 1);
    QCOMPARE(destinations->itemText(0), QStringLiteral("Choose folder…"));
    destinations->setCurrentIndex(0);
    QTRY_COMPARE(destinations->count(), 2);
    QCOMPARE(destinations->itemText(0), media.filePath(QStringLiteral("chosen")));
    QCOMPARE(tagger->destinationIndex(), 0);
    tagger->chooseRename(true);
    tagger->chooseMove(true);
    QTRY_VERIFY(tagger->moveFiles() && tagger->canApply());
    properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 10'000);
    const auto moved =
        media.filePath(QStringLiteral("chosen/Trackknife Project/Fixture Tone.flac"));
    QVERIFY2(QFile::exists(moved), qPrintable(moved));
    QVERIFY(!QFile::exists(path));
    QSettings{}.remove(QStringLiteral("properties/actions"));
}

// ReplayGain as an Apply action: measured first, staged, and written with
// the tag edits in the same write; an album with gain already left alone.
void BenchMainWindowTest::applyMeasuresReplayGainAndWritesOnce() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto work = std::make_shared<engine::RemoteFileWork>(
        protocol::Endpoint{.socket = QFile::encodeName(engine_.socket()).toStdString(),
                           .host = {},
                           .port = 0,
                           .token = {}});
    const auto raw = [](const QString& path) { return QFile::encodeName(path).toStdString(); };
    const auto wav = media.filePath(QStringLiteral("tone.wav"));
    write_sine_wav_fixture(wav, 0.6);
    const auto preset = convert::find_encoder_preset("flac");
    QVERIFY(preset);
    std::vector<MetadataPropertiesSource> sources;
    for (const auto& [name, album, gain] :
         {std::tuple{"a1.flac", "Measured", ""}, std::tuple{"a2.flac", "Measured", ""},
          std::tuple{"b1.flac", "Has gain", "-3.00 dB"},
          std::tuple{"b2.flac", "Has gain", "-3.00 dB"}}) {
        const auto path = media.filePath(QLatin1String(name));
        convert::AudioConversionRequest request{};
        request.source_raw_path = raw(wav);
        request.preset = *preset;
        request.destination_raw_path = raw(path);
        const auto field = [](const char* native, const char* value) {
            return metadata::MetadataField{.canonical_name =
                                               metadata::canonicalize_field_name(native),
                                           .native_name = native,
                                           .values = {value},
                                           .qualifier = {},
                                           .provenance = metadata::FieldProvenance::embedded};
        };
        request.metadata.fields = {field("ALBUM", album), field("ARTIST", "Band"),
                                   field("TITLE", name)};
        QVERIFY(convert::convert_audio_file(request));
        // Conversion writes no gains: those it has already, written after.
        if (std::string_view{gain}.size() > 0U) {
            const auto before = metadata::read_local_metadata(raw(path));
            QVERIFY(before.has_value());
            auto selection = metadata::StagedMetadataSelection::create(
                {metadata::StagedMetadataSource{.raw_path = raw(path),
                                                .source_revision = before->source_revision,
                                                .baseline = before->document}});
            QVERIFY(selection.has_value());
            metadata::StagedMetadataPatchSet patches;
            for (const auto* native : {"REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_TRACK_GAIN"}) {
                const auto canonical = metadata::canonicalize_field_name(native);
                auto index = selection->field_index(canonical);
                if (!index) {
                    auto added = selection->ensure_missing_field(canonical, native);
                    QVERIFY(added.has_value());
                    index = *added;
                }
                QVERIFY(patches.replace_values(*selection, 0U, *index, {gain}).has_value());
            }
            const auto plan = metadata::build_metadata_write_plan(
                *selection, patches, metadata::local_metadata_file_access());
            QVERIFY(plan && plan->ready());
            const auto written = work->apply(*plan, {}, {});
            QVERIFY(written && written->committed_source_count() == 1U);
        }
        const auto read = metadata::read_local_metadata(raw(path));
        QVERIFY(read.has_value());
        sources.push_back(MetadataPropertiesSource{
            .source = metadata::StagedMetadataSource{.raw_path = raw(path),
                                                     .source_revision = read->source_revision,
                                                     .baseline = read->document},
            .track_label = {},
        });
    }
    std::size_t writes = 0U;
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {},
        TaggerServices{
            .plan_applier_factory =
                [work, &writes] {
                    return MetadataWritePlanApplier{
                        [work, &writes](const metadata::MetadataWritePlan& plan,
                                        const operations::MetadataApplyProgressCallback& progress,
                                        const core::CancellationToken& cancellation) {
                            ++writes;
                            return work->apply(plan, progress, cancellation);
                        }};
                },
            .apply_observer = {},
            .transformation_store =
                MetadataTransformationStore{.load = [](MetadataTransformationStore::LoadCompletion
                                                           completion) { completion({}, {}); },
                                            .save = {},
                                            .remove = {}},
            .output_profile_store = {},
            .file_plan_applier_factory = {},
            .file_apply_observer = {},
            .layout_store = {},
            .musicbrainz = {},
            .tools = engineFileWorkTools(work),
            .library_elsewhere = {},
            .undo = {},
        });
    const QPointer<MetadataPropertiesDialog> guard{properties};
    properties->show();
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* tagger = properties->findChild<TaggerSession*>();
    QVERIFY(tagger != nullptr);
    QTRY_VERIFY(tagger->canTransform());
    const auto artist_row = aggregate_model->fieldRow(QStringLiteral("artist"));
    QVERIFY(artist_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*artist_row, 2),
                                     QStringLiteral("Renamed Band"), Qt::EditRole));
    tagger->setReplayGainGrouping(0);
    tagger->chooseReplayGain(true);
    tagger->chooseSkipExistingGain(true);
    QCOMPARE(tagger->itemsNeedingGain({0U, 1U, 2U, 3U}), (std::vector<std::size_t>{0U, 1U}));
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QTRY_VERIFY(apply->isEnabled());
    apply->click();
    // Written once, and the editor closes.
    QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 20'000);
    QCOMPARE(writes, std::size_t{1U});
    const auto value_of = [&raw, &media](const char* name, std::string_view field) {
        const auto read = metadata::read_local_metadata(raw(media.filePath(QLatin1String(name))));
        return read ? read->document.first_effective_value(field).value_or(std::string{})
                    : std::string{"unread"};
    };
    QCOMPARE(value_of("a1.flac", "artist"), std::string{"Renamed Band"});
    QVERIFY(!value_of("a1.flac", "replaygainalbumgain").empty());
    QVERIFY(!value_of("a2.flac", "replaygaintrackgain").empty());
    QCOMPARE(value_of("b1.flac", "artist"), std::string{"Renamed Band"});
    QCOMPARE(value_of("b1.flac", "replaygainalbumgain"), std::string{"-3.00 dB"});
    QSettings{}.remove(QStringLiteral("properties/actions"));
}

// ADR-0261/0262: the staged albums written in one go as the tagger's Actions
// say -- tags only, or renamed and moved into the library folder each is in,
// ReplayGain measured first -- an album with a file changed since it was
// read left out, those not chosen kept staged with their drafts.
void BenchMainWindowTest::identifyAlbumsWritesWhatIsChosen() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto library = media.filePath(QStringLiteral("library"));
    const auto work = std::make_shared<engine::RemoteFileWork>(
        protocol::Endpoint{.socket = QFile::encodeName(engine_.socket()).toStdString(),
                           .host = {},
                           .port = 0,
                           .token = {}});
    const auto raw = [](const QString& path) {
        const auto encoded = QFile::encodeName(path);
        return std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    };
    // A tag written as the engine writes it.
    const auto retag = [&work](const std::string& raw_path,
                               const std::vector<std::pair<std::string, std::string>>& fields) {
        const auto read = metadata::read_local_metadata(raw_path);
        if (!read) {
            return false;
        }
        auto selection = metadata::StagedMetadataSelection::create({metadata::StagedMetadataSource{
            .raw_path = raw_path,
            .source_revision = read->source_revision,
            .baseline = read->document,
        }});
        if (!selection) {
            return false;
        }
        metadata::StagedMetadataPatchSet patches;
        for (const auto& [name, value] : fields) {
            const auto canonical = metadata::canonicalize_field_name(name);
            auto field = selection->field_index(canonical);
            if (!field) {
                auto added = selection->ensure_missing_field(canonical, name);
                if (!added) {
                    return false;
                }
                field = *added;
            }
            if (!patches.replace_values(*selection, 0U, *field, {value})) {
                return false;
            }
        }
        auto plan = metadata::build_metadata_write_plan(*selection, patches,
                                                        metadata::local_metadata_file_access());
        if (!plan || !plan->ready()) {
            return false;
        }
        const auto applied = work->apply(*plan, {}, {});
        return applied && applied->committed_source_count() == 1U;
    };
    // Long enough to measure loudness.
    const auto tone = media.filePath(QStringLiteral("tone.flac"));
    {
        const auto wav = media.filePath(QStringLiteral("tone.wav"));
        write_sine_wav_fixture(wav, 0.6);
        const auto preset = convert::find_encoder_preset("flac");
        QVERIFY(preset);
        convert::AudioConversionRequest request{};
        request.source_raw_path = raw(wav);
        request.preset = *preset;
        request.destination_raw_path = raw(tone);
        QVERIFY(convert::convert_audio_file(request));
    }
    std::vector<MetadataPropertiesSource> sources;
    for (const auto* artist : {"Band", "Cover", "Third"}) {
        const auto folder =
            QStringLiteral("%1/incoming/%2 - Alpha").arg(library, QLatin1String(artist));
        QVERIFY(QDir{}.mkpath(folder));
        for (const auto& [number, title, length] :
             {std::tuple{"1", "One", 61'000}, std::tuple{"2", "Two", 59'000}}) {
            const auto path = raw(QStringLiteral("%1/0%2.flac").arg(folder, QLatin1String(number)));
            QVERIFY(QFile::copy(tone, QString::fromStdString(path)));
            std::vector<std::pair<std::string, std::string>> tags{
                {"ALBUM", "Alpha"}, {"ARTIST", artist}, {"TITLE", title}, {"TRACKNUMBER", number}};
            // Cover has gain already.
            if (std::string_view{artist} == "Cover") {
                tags.emplace_back("REPLAYGAIN_ALBUM_GAIN", "-1.00 dB");
            }
            QVERIFY(retag(path, tags));
            const auto read = metadata::read_local_metadata(path);
            QVERIFY(read.has_value());
            sources.push_back(MetadataPropertiesSource{
                .source =
                    metadata::StagedMetadataSource{
                        .raw_path = path,
                        .source_revision = read->source_revision,
                        .baseline = read->document,
                    },
                .track_label = {},
                .duration_ms = length,
            });
        }
    }
    const std::map<QString, QString> ids{
        {QStringLiteral("Band"), QStringLiteral("11111111-0000-0000-0000-000000000001")},
        {QStringLiteral("Cover"), QStringLiteral("22222222-0000-0000-0000-000000000002")},
        {QStringLiteral("Third"), QStringLiteral("33333333-0000-0000-0000-000000000003")}};
    const auto release = [&ids](const QString& artist) {
        return QStringLiteral(R"json({"id": "%1", "score": 100, "title": "Alpha",
          "status": "Official", "date": "1999-09-09", "country": "DE", "track-count": 2,
          "artist-credit": [{"name": "%2",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "%2"}}],
          "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
          "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "One", "length": 61000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
            {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
             "title": "Two", "length": 59000,
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
          ]}]})json")
            .arg(ids.at(artist), artist);
    };
    const MusicBrainzLookupService service{
        .fetch =
            [&](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                const auto asked = QUrl::fromPercentEncoding(url.toUtf8());
                QString artist = QStringLiteral("Band");
                for (const auto& [name, id] : ids) {
                    if (asked.contains(name) || asked.contains(id)) {
                        artist = name;
                    }
                }
                const auto body = url.contains(QStringLiteral("?query="))
                                      ? QStringLiteral(R"({"count": 1, "releases": [%1]})")
                                            .arg(release(artist))
                                            .toUtf8()
                                      : release(artist).toUtf8();
                QTimer::singleShot(
                    1, [body, completion = std::move(completion)] { completion(body); });
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };
    const auto layout = [](std::string name, std::string folder, std::string file) {
        return persistence::SavedOutputLayoutProfile{
            .id = core::StableId::random(),
            .profile =
                operations::OutputLayoutProfile{.schema_version = 1U,
                                                .name = std::move(name),
                                                .dialect = {},
                                                .relative_directory_expression = std::move(folder),
                                                .basename_expression = std::move(file),
                                                .sanitization_policy = {"linux", 1U}}};
    };
    // The file name of one, the folders of the other.
    const std::vector layouts{
        layout("By title", "", "%title%"), layout("Artist folders", "%artist%/%album%", "%title%"),
        // As long as one in use: it may not widen the window.
        layout("Albumartist/Date Album/{CD}/Track-Title",
               "$if2(%albumartist%,%artist%)/$if(%date%,$left(%date%,4) ,)%album%$if($or($gt("
               "%totaldiscs%,1),$gt(%discnumber%,1)),/CD $num(%discnumber%,1),)",
               "$num(%tracknumber%,2)-%title%")};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {},
        TaggerServices{
            .plan_applier_factory =
                [work] {
                    return MetadataWritePlanApplier{
                        [work](const metadata::MetadataWritePlan& plan,
                               const operations::MetadataApplyProgressCallback& progress,
                               const core::CancellationToken& cancellation) {
                            return work->apply(plan, progress, cancellation);
                        }};
                },
            .apply_observer = {},
            .transformation_store =
                MetadataTransformationStore{.load = [](MetadataTransformationStore::LoadCompletion
                                                           completion) { completion({}, {}); },
                                            .save = {},
                                            .remove = {}},
            .output_profile_store =
                OutputProfileStore{.load =
                                       [layouts](OutputProfileStore::LoadCompletion completion) {
                                           completion(layouts, {}, {});
                                       },
                                   .save_layout = {},
                                   .remove_layout = {},
                                   .save_destination = {},
                                   .remove_destination = {},
                                   .destinations_on = {},
                                   .destinations_key = {},
                                   .places = {}},
            .file_plan_applier_factory =
                [work] {
                    return FilePublicationPlanApplier{
                        [work](const operations::PreparationPlan& plan,
                               const operations::FilePublicationApplyProgressCallback& progress,
                               const core::CancellationToken& cancellation) {
                            return work->publish(plan, progress, cancellation);
                        }};
                },
            .file_apply_observer = {},
            .layout_store = {},
            .musicbrainz = service,
            .tools = engineFileWorkTools(work),
            .library_elsewhere = {},
            .undo =
                [work](std::vector<operations::UndoRequest> requests,
                       const core::CancellationToken& cancellation) {
                    return work->undo(requests, {}, cancellation);
                },
        });
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* open =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify-albums"));
    QTRY_VERIFY(open->isEnabled());
    open->click();
    auto* dialog =
        properties->findChild<IdentifyAlbumsDialog*>(QStringLiteral("bench-identify-albums"));
    auto* session = dialog->session();
    QCOMPARE(session->albums().size(), 3U);
    const auto album_of = [session](const QString& artist) {
        const auto& albums = session->albums();
        return static_cast<std::size_t>(std::distance(
            albums.begin(), std::ranges::find(albums, artist, &AlbumBatchSession::Album::artist)));
    };
    const auto band = album_of(QStringLiteral("Band"));
    const auto cover = album_of(QStringLiteral("Cover"));
    const auto third = album_of(QStringLiteral("Third"));
    // Looked up at once.
    QTRY_COMPARE_WITH_TIMEOUT(session->count(AlbumBatchSession::State::staged), std::size_t{3U},
                              10'000);
    auto* list = dialog->findChild<QTreeWidget*>(QStringLiteral("bench-identify-albums-list"));
    auto* write = dialog->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-write"));
    const auto row_of = [list](const std::size_t album) {
        for (int row = 0; row < list->topLevelItemCount(); ++row) {
            if (list->topLevelItem(row)->data(0, Qt::UserRole).toInt() == static_cast<int>(album)) {
                return list->topLevelItem(row);
            }
        }
        return static_cast<QTreeWidgetItem*>(nullptr);
    };
    QTRY_COMPARE(write->text(), QStringLiteral("Write 3 albums"));
    QVERIFY2(dialog->minimumSizeHint().width() < 700,
             qPrintable(QString::number(dialog->minimumSizeHint().width())));

    // The tagger's own Actions: ReplayGain on, measured before the write.
    auto* tagger = properties->findChild<TaggerSession*>();
    QVERIFY(tagger != nullptr);
    dialog->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-actions"))->click();
    QFrame* popover = nullptr;
    QTRY_VERIFY((popover = dialog->findChild<QFrame*>(
                     QStringLiteral("bench-metadata-actions-popover"))) != nullptr);
    auto* replaygain = popover->findChild<QCheckBox*>(QStringLiteral("bench-actions-replaygain"));
    QVERIFY(!replaygain->isChecked());
    replaygain->click();
    QVERIFY(tagger->replayGainOnApply());
    popover->close();

    // Tags only, and only Band's.
    row_of(cover)->setCheckState(0, Qt::Unchecked);
    row_of(third)->setCheckState(0, Qt::Unchecked);
    QTRY_COMPARE(write->text(), QStringLiteral("Write 1 album"));
    QTRY_VERIFY(write->isEnabled());
    // The bar moves file by file, not once an album: the write tells the
    // dialog as each of Band's two files is written, not only as the album
    // is measured, starts and ends -- five changes here without them.
    QSignalSpy told{session, &AlbumBatchSession::changed};
    write->click();
    QTRY_COMPARE_WITH_TIMEOUT(session->albums()[band].state, AlbumBatchSession::State::written,
                              20'000);
    QVERIFY2(told.count() >= 5 + 2, qPrintable(QString::number(told.count())));
    QTRY_VERIFY(session->writing() == nullptr && !session->measuring());
    const auto band_file = sources[0].source.raw_path;
    const auto band_tags = metadata::read_local_metadata(band_file);
    QVERIFY(band_tags.has_value());
    const auto value_of = [](const metadata::MetadataDocument& document, std::string_view name) {
        const auto found =
            std::ranges::find(document.fields, metadata::canonicalize_field_name(name),
                              &metadata::MetadataField::canonical_name);
        return found == document.fields.end() || found->values.empty() ? std::string{}
                                                                       : found->values.front();
    };
    QCOMPARE(value_of(band_tags->document, "MUSICBRAINZ_ALBUMID"),
             ids.at(QStringLiteral("Band")).toStdString());
    QVERIFY(!value_of(band_tags->document, "REPLAYGAIN_ALBUM_GAIN").empty());
    QVERIFY(!value_of(band_tags->document, "REPLAYGAIN_TRACK_GAIN").empty());
    // Read again; Band's draft written, the others' kept.
    QTRY_VERIFY(!tagger->writingElsewhere());
    // Read again, the file list still shows only the file names.
    {
        int shown = 0;
        for (int column = 0; column < grid_model->columnCount(); ++column) {
            shown += files->isColumnHidden(column) ? 0 : 1;
        }
        QCOMPARE(shown, 1);
    }
    QVERIFY(tagger->sharedSelection()->source(0U).baseline.first_effective_value(
        "replaygainalbumgain"));
    const auto has_patches = [&grid_model](std::size_t first, std::size_t last) {
        return std::ranges::any_of(grid_model->patches().patches(), [&](const auto& patch) {
            return patch.item_index >= first && patch.item_index <= last;
        });
    };
    QVERIFY(!has_patches(0U, 1U));
    QVERIFY(has_patches(2U, 5U));
    QCOMPARE(session->albums()[cover].state, AlbumBatchSession::State::staged);
    QVERIFY(row_of(band)->text(2).startsWith(QStringLiteral("Written")));

    // Cover renamed and moved by the layout into the library's folder, its
    // gain kept; Third's file changed since it was read, so Third
    // is left out.
    row_of(cover)->setCheckState(0, Qt::Checked);
    row_of(third)->setCheckState(0, Qt::Checked);
    QTRY_COMPARE(write->text(), QStringLiteral("Write 2 albums"));
    QTRY_VERIFY(tagger->layoutsAvailable());
    tagger->selectLayout(1);
    tagger->chooseMoveFolder(raw(library));
    QCOMPARE(tagger->destinations().back().name, library);
    tagger->chooseRename(true);
    tagger->chooseMove(true);
    QVERIFY(tagger->renameFiles() && tagger->moveFiles() && tagger->moveAvailable());
    QVERIFY(retag(sources[4].source.raw_path, {{"COMMENT", "changed elsewhere"}}));
    QTRY_VERIFY(write->isEnabled());
    write->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        session->albums()[cover].state == AlbumBatchSession::State::written ||
            session->albums()[cover].note.contains(QStringLiteral("not written")),
        20'000);
    QVERIFY2(session->albums()[cover].state == AlbumBatchSession::State::written,
             qPrintable(AlbumBatchSession::stateText(session->albums()[cover]) +
                        QStringLiteral(" | ") +
                        AlbumBatchSession::stateText(session->albums()[third])));
    QTRY_VERIFY(!tagger->writingElsewhere());
    const auto moved = library + QStringLiteral("/Cover/Alpha/One.flac");
    QVERIFY2(QFile::exists(moved), qPrintable(moved));
    QVERIFY(QFile::exists(library + QStringLiteral("/Cover/Alpha/Two.flac")));
    const auto cover_tags = metadata::read_local_metadata(raw(moved));
    QVERIFY(cover_tags.has_value());
    QCOMPARE(value_of(cover_tags->document, "REPLAYGAIN_ALBUM_GAIN"), std::string{"-1.00 dB"});
    QVERIFY(!QFile::exists(QString::fromStdString(sources[2].source.raw_path)));
    QCOMPARE(tagger->sharedSelection()->source(2U).raw_path, raw(moved));
    QCOMPARE(session->albums()[third].state, AlbumBatchSession::State::staged);
    QVERIFY2(session->albums()[third].note.contains(QStringLiteral("changed since it was read")),
             qPrintable(session->albums()[third].note));
    QVERIFY(QFile::exists(QString::fromStdString(sources[4].source.raw_path)));
    QVERIFY(row_of(third)->text(2).contains(QStringLiteral("not written")));
    QVERIFY(has_patches(4U, 5U));
    QVERIFY(dialog->isVisible());

    // Third's changed file was read again, its draft kept: written now. The
    // window stays, so the Write can be taken back.
    QTRY_COMPARE(write->text(), QStringLiteral("Write 1 album"));
    QTRY_VERIFY(write->isEnabled());
    write->click();
    QTRY_COMPARE_WITH_TIMEOUT(session->albums()[third].state, AlbumBatchSession::State::written,
                              20'000);
    const auto third_moved = library + QStringLiteral("/Third/Alpha/One.flac");
    QVERIFY2(QFile::exists(third_moved), qPrintable(third_moved));
    const auto third_tags = metadata::read_local_metadata(raw(third_moved));
    QVERIFY(third_tags.has_value());
    QCOMPARE(value_of(third_tags->document, "MUSICBRAINZ_ALBUMID"),
             ids.at(QStringLiteral("Third")).toStdString());
    QVERIFY(dialog->isVisible());

    // ADR-0263: undone -- the files back where they were, as they were, and
    // the album staged again.
    auto* undo = dialog->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-undo"));
    QTRY_VERIFY(undo->isVisibleTo(dialog) && undo->isEnabled());
    undo->click();
    QTRY_VERIFY_WITH_TIMEOUT(!session->undoing() && !tagger->writingElsewhere(), 20'000);
    const auto third_original = QString::fromStdString(sources[4].source.raw_path);
    QVERIFY2(QFile::exists(third_original), qPrintable(third_original));
    QVERIFY(!QFile::exists(third_moved));
    const auto restored = metadata::read_local_metadata(sources[4].source.raw_path);
    QVERIFY(restored.has_value());
    QVERIFY(value_of(restored->document, "MUSICBRAINZ_ALBUMID").empty());
    QCOMPARE(value_of(restored->document, "ARTIST"), std::string{"Third"});
    QCOMPARE(value_of(restored->document, "COMMENT"), std::string{"changed elsewhere"});
    QTRY_COMPARE_WITH_TIMEOUT(session->albums()[third].state, AlbumBatchSession::State::staged,
                              10'000);
    QCOMPARE(tagger->sharedSelection()->source(4U).raw_path, sources[4].source.raw_path);
    QTRY_COMPARE(write->text(), QStringLiteral("Write 1 album"));
    QVERIFY(!undo->isVisibleTo(dialog));

    // Identified again: Band and Cover are tagged as their releases already,
    // so staged they have nothing to write -- said so, and not counted for
    // the Write, which is Third's alone. Renamed or moved they would count.
    tagger->chooseRename(false);
    tagger->chooseMove(false);
    dialog->close();
    QTRY_VERIFY(!dialog->isVisible());
    delete dialog;
    QTRY_VERIFY(open->isEnabled());
    open->click();
    auto* again =
        properties->findChild<IdentifyAlbumsDialog*>(QStringLiteral("bench-identify-albums"));
    QVERIFY(again != nullptr);
    auto* second = again->session();
    QTRY_COMPARE_WITH_TIMEOUT(second->count(AlbumBatchSession::State::staged), std::size_t{3U},
                              10'000);
    const auto again_of = [second](const QString& artist) {
        const auto& albums = second->albums();
        return static_cast<std::size_t>(std::distance(
            albums.begin(), std::ranges::find(albums, artist, &AlbumBatchSession::Album::artist)));
    };
    QVERIFY(second->alreadyTagged(again_of(QStringLiteral("Band"))));
    QVERIFY(second->alreadyTagged(again_of(QStringLiteral("Cover"))));
    QVERIFY(!second->alreadyTagged(again_of(QStringLiteral("Third"))));
    QCOMPARE(second->toWrite(), std::vector<std::size_t>{again_of(QStringLiteral("Third"))});
    // Shown so as it turns out, not once something else redraws the window:
    // the patches staged can turn out to change nothing a moment later.
    auto* rows_again = again->findChild<QTreeWidget*>(QStringLiteral("bench-identify-albums-list"));
    const auto state_shown = [rows_again](const std::size_t album) {
        for (int row = 0; row < rows_again->topLevelItemCount(); ++row) {
            if (rows_again->topLevelItem(row)->data(0, Qt::UserRole).toInt() ==
                static_cast<int>(album)) {
                return rows_again->topLevelItem(row)->text(2);
            }
        }
        return QString{};
    };
    QTRY_COMPARE(state_shown(again_of(QStringLiteral("Band"))),
                 QStringLiteral("Matched · already tagged so"));
    QTRY_COMPARE(state_shown(again_of(QStringLiteral("Cover"))),
                 QStringLiteral("Matched · already tagged so"));
    auto* write_again =
        again->findChild<QPushButton*>(QStringLiteral("bench-identify-albums-write"));
    QTRY_COMPARE(write_again->text(), QStringLiteral("Write 1 album"));
    auto* summary = again->findChild<QLabel*>(QStringLiteral("bench-identify-albums-summary"));
    QVERIFY(summary != nullptr);
    QVERIFY2(summary->text().startsWith(QStringLiteral("1 staged")) &&
                 summary->text().endsWith(QStringLiteral("2 already tagged")),
             qPrintable(summary->text()));

    // Edited since in the tag editor -- Band's title -- it has something to
    // write again, and says so by itself.
    auto* fields = properties->findChild<QTableView*>(QStringLiteral("bench-metadata-fields"));
    QVERIFY(fields != nullptr);
    auto* field_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    QVERIFY(field_model != nullptr);
    files->selectionModel()->select(
        QItemSelection{grid_model->index(0, 0), grid_model->index(1, 0)},
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(field_model->selectedItemCount(), 2U);
    QTRY_VERIFY(field_model->summaryReady());
    const auto title_row = field_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row);
    QVERIFY(field_model->setData(field_model->index(*title_row, 2), QStringLiteral("Edited"),
                                 Qt::EditRole));
    QTRY_VERIFY2(state_shown(again_of(QStringLiteral("Band"))).endsWith(QStringLiteral("· staged")),
                 qPrintable(state_shown(again_of(QStringLiteral("Band")))));
    QTRY_COMPARE(write_again->text(), QStringLiteral("Write 2 albums"));
    QSettings{}.remove(QStringLiteral("properties/actions"));
    delete properties;
}

} // namespace trackknife::bench
