// SPDX-License-Identifier: GPL-3.0-only

#include "bench/file_scope_view.hpp"
#include "bench_main_window_test.hpp"
#include "workspace/artwork_session.hpp"

namespace trackknife::bench {

// ADR-0257: the tagger opens on the tags the list cached, at once, and reads
// the files behind them. A value typed meanwhile is kept when the file's own
// tags take their place; scripts and Save wait for them.
// A cover picked on this computer for an engine's files is resized here, to
// the largest size any destination keeps, before it travels: what the engine
// would shrink anyway is not sent at full size -- an original past the
// engine's line limit could not be sent at all.
void BenchMainWindowTest::coversAreResizedBeforeTheyTravel() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("scan.jpg"));
    QImage scan{2000, 1600, QImage::Format_RGB32};
    for (int y = 0; y < scan.height(); ++y) {
        for (int x = 0; x < scan.width(); ++x) {
            scan.setPixel(x, y, qRgb((x * 7) % 256, (y * 13) % 256, ((x + y) * 3) % 256));
        }
    }
    QVERIFY(scan.save(path, "JPEG", 95));
    std::vector<QSize> sent;
    FileWorkTools tools;
    tools.stage = [&sent](const std::span<const unsigned char> bytes)
        -> core::Result<metadata::ArtworkImageFile> {
        QImage arrived;
        arrived.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                             static_cast<int>(bytes.size()));
        sent.push_back(arrived.size());
        metadata::ArtworkImageFile there;
        there.raw_path = "/engine/staged.jpg";
        return there;
    };
    const auto send = [&](const metadata::ArtworkStoragePolicy& policy) {
        metadata::ArtworkWritePlanIntent intent;
        intent.kind = metadata::ArtworkWritePlanIntentKind::replace;
        intent.replacement_raw_path = QFile::encodeName(path).toStdString();
        auto staged = stageReplacements({intent}, tools, {}, policy);
        return staged && staged->front().replacement_raw_path == std::string{"/engine/staged.jpg"};
    };
    metadata::ArtworkStoragePolicy policy;
    policy.embed = true;
    policy.write_folder_image = true;
    policy.max_embedded_edge = 500U;
    policy.max_folder_edge = 1000U;
    QCOMPARE(largestCoverEdge(policy), 1000U);
    QVERIFY(send(policy));
    QCOMPARE(sent.back(), QSize(1000, 800));
    // A folder image kept as it is: the original has to travel.
    policy.max_folder_edge = 0U;
    QCOMPARE(largestCoverEdge(policy), 0U);
    QVERIFY(send(policy));
    QCOMPARE(sent.back(), QSize(2000, 1600));
    // Embedded only: the embedded limit.
    policy.write_folder_image = false;
    QVERIFY(send(policy));
    QCOMPARE(sent.back(), QSize(500, 400));
}

void BenchMainWindowTest::theTaggerOpensOnCachedTagsAndReadsBehind() {
    QTemporaryDir media;
    const auto path = media.filePath(QStringLiteral("behind.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto raw_path = QFile::encodeName(path).toStdString();
    auto native = metadata::read_local_metadata(raw_path);
    QVERIFY(native);
    auto cached = native->document;
    for (auto& field : cached.fields) {
        field.native_name = field.canonical_name;
        field.provenance = metadata::FieldProvenance::cached_snapshot;
        if (field.canonical_name == "title") {
            field.values = {"As cached"};
        }
    }
    const metadata::StagedMetadataSource source{.raw_path = raw_path,
                                                .source_revision = {},
                                                .baseline = cached,
                                                .needs_metadata_capture = true};
    // The reads wait until let go, as a slow disk far away would.
    std::promise<void> release;
    auto released = release.get_future().share();
    bool let_go = false;
    const auto go = [&] {
        if (!let_go) {
            let_go = true;
            release.set_value();
        }
    };
    TaggerServices services;
    const auto local = services.tools.access;
    services.tools.access.read_many = [local, released](const std::vector<std::string>& paths,
                                                        const core::CancellationToken& token)
        -> core::Result<std::vector<core::Result<metadata::LocalMetadataRead>>> {
        released.wait();
        std::vector<core::Result<metadata::LocalMetadataRead>> read;
        for (const auto& each : paths) {
            read.push_back(local.read(each, token));
        }
        return read;
    };
    MetadataPropertiesDialog properties{
        1,
        [source](std::size_t) -> std::optional<MetadataPropertiesSource> {
            return MetadataPropertiesSource{.source = source,
                                            .track_label = QStringLiteral("Behind")};
        },
        {},
        std::move(services)};
    // Let go before the dialog is, whatever fails: it waits for its reads.
    const auto always = qScopeGuard(go);
    properties.show();
    MetadataGridModel* grid = nullptr;
    QTRY_VERIFY((grid = properties.findChild<MetadataGridModel*>()) != nullptr);
    auto* session = properties.findChild<TaggerSession*>();
    QVERIFY(session != nullptr);
    // Open before anything is read: the cached tags, provisional.
    QVERIFY(!grid->selection().source(0).source_revision);
    const auto cached_title = grid->selection().field_index("title");
    QVERIFY(cached_title);
    QCOMPARE(grid->selection().cell(0, *cached_title)->values,
             std::vector<std::string>{"As cached"});
    QVERIFY(!session->canTransform());
    QVERIFY(!session->canApply());
    QTRY_VERIFY(session->status().contains(QStringLiteral("Reading tags")));
    // A value edit is open at once.
    const std::vector<std::size_t> first{0U};
    QVERIFY(grid->replaceFieldValues(first, *cached_title, {"Typed meanwhile"}));

    go();
    QTRY_VERIFY(grid->selection().source(0).source_revision);
    const auto title = grid->selection().field_index("title");
    QVERIFY(title);
    // The file's own title now, not the cached one.
    QCOMPARE(grid->selection().cell(0, *title)->values.front(),
             native->document.first_effective_value("title").value_or(std::string{}));
    const auto* kept = grid->patches().patch(0, *title);
    QVERIFY(kept != nullptr);
    QCOMPARE(kept->values, std::vector<std::string>{"Typed meanwhile"});
    QVERIFY(session->status().contains(QStringLiteral("Tags read")));
    QTRY_VERIFY(session->canTransform());
}

void BenchMainWindowTest::musicBrainzStagesFromCachedSearchMetadata() {
    QTemporaryDir media;
    const auto path = media.filePath(QStringLiteral("cached-release.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    auto native = metadata::read_local_metadata(QFile::encodeName(path).toStdString());
    QVERIFY(native && native->document.first_effective_value("musicbrainzworkid"));
    auto cached = native->document;
    for (auto& field : cached.fields) {
        // The library index retains canonical names, not native tag spellings.
        field.native_name = field.canonical_name;
        field.provenance = metadata::FieldProvenance::cached_snapshot;
    }
    metadata::StagedMetadataSource source{.raw_path = QFile::encodeName(path).toStdString(),
                                          .source_revision = {},
                                          .baseline = cached,
                                          .needs_metadata_capture = true};
    const metadata::MetadataProposalSet proposals{
        .provider_name = "MusicBrainz",
        .provider_detail = "Matched release",
        .items = {{.item_index = 0,
                   .fields = {{.canonical_field = "musicbrainzworkid",
                               .display_field = "MUSICBRAINZ_WORKID",
                               .values = {"11111111-2222-3333-4444-555555555555"},
                               .confidence = 1.0,
                               .rationale = "Matched recording"}},
                   .artwork = {}}}};
    // Reproduce the old false-stale rejection when a cache document is used
    // directly as a native baseline.
    auto old_selection = metadata::StagedMetadataSelection::create({source}, {});
    QVERIFY(old_selection);
    MetadataGridModel old_grid{std::move(*old_selection), {QStringLiteral("Cached")}};
    auto old_preview = metadata::metadata_proposal_preview(old_grid.selection(), old_grid.patches(),
                                                           proposals, 0.5);
    QVERIFY(old_preview);
    QSignalSpy rejected{&old_grid, &MetadataGridModel::editRejected};
    QVERIFY(!old_grid.stageTransformation(*old_preview));
    QVERIFY(rejected.front().front().toString().contains(QStringLiteral("draft changed")));

    MetadataPropertiesDialog properties{
        1,
        [source](std::size_t) -> std::optional<MetadataPropertiesSource> {
            return MetadataPropertiesSource{.source = source,
                                            .track_label = QStringLiteral("Cached")};
        },
        {},
        {},
        {}};
    properties.show();
    MetadataGridModel* grid = nullptr;
    QTRY_VERIFY((grid = properties.findChild<MetadataGridModel*>()) != nullptr);
    // ADR-0257: shown on the cached tags first, then on the file's own.
    QTRY_VERIFY(grid->selection().source(0).source_revision);
    const auto preview =
        metadata::metadata_proposal_preview(grid->selection(), grid->patches(), proposals, 0.5);
    QVERIFY(preview);
    QVERIFY(grid->stageTransformation(*preview, {QStringLiteral("MusicBrainz")}));
    QCOMPARE(grid->patches().patch_count(), 1U);
    QVERIFY(grid->undo());
    QCOMPARE(grid->patches().patch_count(), 0U);
}

void BenchMainWindowTest::contextReplayGainScansAndApplies_data() {
    QTest::addColumn<bool>("cached");
    QTest::addColumn<bool>("embedded");
    // ADR-0237: done by the engine holding the files.
    QTest::newRow("imported") << false << false;
    QTest::newRow("cached-sidecar") << true << false;
    QTest::newRow("cached-embedded") << true << true;
}

void BenchMainWindowTest::contextReplayGainScansAndApplies() {
    QFETCH(bool, cached);
    QFETCH(bool, embedded);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first =
        media.filePath(embedded ? QStringLiteral("one.flac") : QStringLiteral("one.wav"));
    const auto second =
        media.filePath(embedded ? QStringLiteral("two.flac") : QStringLiteral("two.wav"));
    if (embedded) {
        const auto wav = media.filePath(QStringLiteral("tone.wav"));
        write_sine_wav_fixture(wav, 0.6);
        const auto preset = convert::find_encoder_preset("flac");
        QVERIFY(preset);
        convert::AudioConversionRequest request{};
        request.source_raw_path = QFile::encodeName(wav).toStdString();
        request.preset = *preset;
        request.metadata.fields.push_back({.canonical_name = "title",
                                           .native_name = "TITLE",
                                           .values = {"Preserve this title"},
                                           .qualifier = {},
                                           .provenance = metadata::FieldProvenance::embedded});
        for (const auto& output : {first, second}) {
            request.destination_raw_path = QFile::encodeName(output).toStdString();
            QVERIFY(convert::convert_audio_file(request));
        }
    } else {
        write_sine_wav_fixture(first, 0.6);
        write_sine_wav_fixture(second, 0.3);
    }
    const auto original = metadata::read_local_metadata(QFile::encodeName(first).toStdString());
    QVERIFY(original);

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(
        {QFile::encodeName(first).toStdString(), QFile::encodeName(second).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    QTRY_COMPARE(view->model()->rowCount(), 2);
    auto* local_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(local_model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(local_model->rows()[0].probed && local_model->rows()[1].probed, 5000);
    if (cached) {
        auto rows = local_model->rows();
        for (auto& row : rows) {
            row.source_revision.reset();
            row.metadata.fields.clear();
        }
        local_model->replaceRows(std::move(rows));
    }
    view->selectAll();

    if (cached && embedded) {
        window.findChild<QAction*>(QStringLiteral("action-track-properties"))->trigger();
        auto* properties = window.findChild<MetadataPropertiesDialog*>();
        QVERIFY(properties);
        MetadataGridModel* grid = nullptr;
        QTRY_VERIFY((grid = properties->findChild<MetadataGridModel*>()) != nullptr);
        QVERIFY(grid->selection().source(0).source_revision.has_value());
        QCOMPARE(grid->selection().source(0).baseline, original->document);
        delete properties;
        tabs->setCurrentWidget(view);
        view->selectAll();
    }

    // This build's engine does file work; once it has said so, the dialog
    // goes through it -- unless it is taken as an older one.
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto* action = window.findChild<QAction*>(QStringLiteral("action-replaygain-dialog"));
    QVERIFY(action != nullptr);
    action->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-replaygain-dialog"))) !=
                nullptr);
    QCOMPARE(dialog->property("trackknife-file-work").toString(), QStringLiteral("engine"));
    auto* grouping =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-replaygain-dialog-grouping"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-replaygain-dialog-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-replaygain-dialog-status"));
    QVERIFY(grouping != nullptr && run != nullptr && status != nullptr);
    grouping->setCurrentIndex(3);
    run->click();
    QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 30'000);
    const auto problems = dialog->findChild<QPlainTextEdit*>();
    QVERIFY2(status->text().startsWith(QStringLiteral("Saved ReplayGain tags to 2")),
             qPrintable(status->text() + QStringLiteral(" · ") +
                        (problems ? problems->toPlainText() : QString{})));

    // Unwritable WAVs landed in sidecars with the measured track gains.
    for (const auto& path : {first, second}) {
        const auto encoded = QFile::encodeName(path);
        if (embedded) {
            const auto reread = metadata::read_local_metadata(encoded.toStdString());
            QVERIFY(reread);
            QVERIFY(reread->document.first_effective_value("replaygaintrackgain"));
            for (const auto& field : original->document.effective_fields()) {
                if (!field.canonical_name.starts_with("replaygain")) {
                    QCOMPARE(reread->document.effective_values(field.canonical_name), field.values);
                }
            }
            continue;
        }
        const auto sidecar = metadata::read_loudness_sidecar(
            std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
        QVERIFY(sidecar.has_value());
        QVERIFY(sidecar->has_value());
        QCOMPARE((*sidecar)->entries.size(), 1U);
        QVERIFY((*sidecar)->entries.front().track_gain_db.has_value());
    }
    dialog->close();
}

void BenchMainWindowTest::replayGainScanPreservesLogicalSources_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("cue") << QStringLiteral("cue");
    QTest::newRow("chapter") << QStringLiteral("chapter");
    QTest::newRow("subsong") << QStringLiteral("subsong");
}

void BenchMainWindowTest::replayGainScanPreservesLogicalSources() {
    QFETCH(QString, kind);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QString input;
    if (kind == QStringLiteral("cue")) {
        write_sine_wav_fixture(media.filePath(QStringLiteral("disc.wav")), 0.8, 0.2);
        input = media.filePath(QStringLiteral("disc.cue"));
        QFile cue{input};
        QVERIFY(cue.open(QIODevice::WriteOnly));
        cue.write("TITLE \"Gain Album\"\nFILE \"disc.wav\" WAVE\n"
                  "TRACK 01 AUDIO\nTITLE \"Loud\"\nINDEX 01 00:00:00\n"
                  "TRACK 02 AUDIO\nTITLE \"Quiet\"\nINDEX 01 00:01:00\n");
    } else {
        input = media.filePath(kind == QStringLiteral("chapter") ? QStringLiteral("disc.mka")
                                                                 : QStringLiteral("disc.mod"));
        QVERIFY(materialize_audio_fixture(kind == QStringLiteral("chapter")
                                              ? QStringLiteral("loudness-chapters-mka.b64")
                                              : QStringLiteral("two-subsongs-mod.b64"),
                                          input));
    }
    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(input).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr && properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 1);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);

    std::vector<loudness::LoudnessScanItem> reference_items;
    for (const auto& row : model->rows()) {
        reference_items.push_back({.item_index = reference_items.size(),
                                   .raw_path = row.raw_path,
                                   .selection = row.selection,
                                   .range = row.segment,
                                   .album_key = "album"});
    }
    const auto reference = loudness::scan_loudness(reference_items);
    QVERIFY(reference.has_value());
    QCOMPARE(reference->analyzed_track_count(), 2U);
    QVERIFY(reference->tracks[0].loudness->measurable());
    QVERIFY(reference->tracks[1].loudness->measurable());
    // The fixture must distinguish the two logical signals.
    QVERIFY(std::abs(reference->tracks[0].loudness->track_gain_db() -
                     reference->tracks[1].loudness->track_gain_db()) > 0.1);

    view->selectAll();
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    auto* properties = window.findChild<MetadataPropertiesDialog*>();
    QVERIFY(properties != nullptr);
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    auto* grouping = properties->findChild<QComboBox*>(QStringLiteral("bench-replaygain-grouping"));
    QVERIFY(scan != nullptr && grouping != nullptr);
    grouping->setCurrentIndex(2);
    files->selectAll();
    QTRY_VERIFY(scan->isEnabled());
    scan->click();
    QTRY_VERIFY_WITH_TIMEOUT(grid->patches().patch_count() >= 8U, 15'000);
    const auto gain = grid->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN"));
    const auto peak = grid->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_PEAK"));
    const auto album = grid->fieldColumn(QStringLiteral("REPLAYGAIN_ALBUM_GAIN"));
    QVERIFY(gain && peak && album);
    for (int row = 0; row < 2; ++row) {
        const auto values = [&](const int column) {
            return grid->index(row, column).data(metadata_cell_values_role).toStringList().front();
        };
        const auto& expected = *reference->tracks[static_cast<std::size_t>(row)].loudness;
        QVERIFY(std::abs(values(*gain).chopped(3).toDouble() - expected.track_gain_db()) < 0.011);
        QVERIFY(std::abs(values(*peak).toDouble() - expected.sample_peak) < 0.000002);
        QVERIFY(std::abs(values(*album).chopped(3).toDouble() -
                         *reference->albums.front().album_gain_db()) < 0.011);
    }
    QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
    QVERIFY(grid->selection().source(0U).logical_track);
    QVERIFY(grid->selection().source(1U).logical_track);

    // A rescan of only the second row must retain its Properties index,
    // decoder selection, and nonzero range start after draft undo.
    QVERIFY(grid->undo());
    QCOMPARE(grid->patches().patch_count(), 0U);
    files->selectRow(1);
    grouping->setCurrentIndex(3);
    QTRY_VERIFY(scan->isEnabled());
    scan->click();
    QTRY_COMPARE_WITH_TIMEOUT(grid->patches().patch_count(), 2U, 15'000);
    QVERIFY(grid->index(0, *gain).data(metadata_cell_values_role).toStringList().isEmpty());
    const auto second_gain = grid->index(1, *gain).data(metadata_cell_values_role).toStringList();
    QCOMPARE(second_gain.size(), 1);
    QVERIFY(std::abs(second_gain.front().chopped(3).toDouble() -
                     reference->tracks[1].loudness->track_gain_db()) < 0.011);
    delete properties;
}

void BenchMainWindowTest::metadataRetrySkipsSavedFiles_data() {
    QTest::addColumn<bool>("change_source");
    QTest::addColumn<bool>("stopped");
    QTest::newRow("failed-then-saved") << false << false;
    QTest::newRow("stopped-then-saved") << false << true;
    QTest::newRow("changed-file-blocked") << true << false;
}

void BenchMainWindowTest::metadataRetrySkipsSavedFiles() {
    QFETCH(bool, change_source);
    QFETCH(bool, stopped);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    std::vector<std::string> paths;
    for (int index = 0; index < 2; ++index) {
        const auto path = media.filePath(QStringLiteral("retry-%1.flac").arg(index));
        QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
        paths.push_back(QFile::encodeName(path).toStdString());
        const auto read = metadata::read_local_metadata(paths.back());
        QVERIFY(read.has_value());
        sources.push_back({.source = {.raw_path = paths.back(),
                                      .source_revision = read->source_revision,
                                      .baseline = read->document},
                           .track_label = path});
    }
    std::array<int, 2> attempts{};
    std::vector<operations::MetadataApplyResult> outcomes;
    const auto database = std::filesystem::path{
        QFile::encodeName(media.filePath(QStringLiteral("retry.sqlite3"))).toStdString()};
    auto* dialog = new MetadataPropertiesDialog(
        sources.size(),
        [sources](std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return sources.at(index);
        },
        {},
        [&, database] {
            return MetadataWritePlanApplier{
                [&, database](const metadata::MetadataWritePlan& plan,
                              const operations::MetadataApplyProgressCallback& progress,
                              const core::CancellationToken& cancellation)
                    -> core::Result<operations::MetadataApplyResult> {
                    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
                    if (!journal) {
                        return std::unexpected(journal.error());
                    }
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&](const metadata::MetadataWritePlanSource& source,
                            const core::CancellationToken& token)
                            -> core::Result<operations::MetadataCommitResult> {
                            const auto index = source.raw_path == paths[0] ? 0U : 1U;
                            ++attempts[index];
                            if (index == 1U && attempts[index] == 1) {
                                return std::unexpected(
                                    core::Error{.code = stopped ? core::ErrorCode::cancelled
                                                                : core::ErrorCode::io,
                                                .message = "injected temporary failure",
                                                .context = {}});
                            }
                            return operations::commit_metadata_source(
                                source, *journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                token);
                        },
                        {}, {}, progress, cancellation,
                        operations::MetadataApplyOptions{.maximum_parallelism = 1U});
                }};
        },
        [&](const operations::MetadataApplyResult& result) { outcomes.push_back(result); });
    dialog->setArtworkMutationServices([] { return ArtworkWritePlanApplier{}; }, {});
    dialog->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    const auto title = grid->ensureField(QStringLiteral("TITLE"));
    QVERIFY(title.has_value());
    const std::vector<std::size_t> indexes{0U, 1U};
    QVERIFY(grid->replaceFieldValues(indexes, title->field_index, {"Retried title"}));
    auto* sections = dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-sections"));
    QVERIFY(sections != nullptr);
    sections->setCurrentIndex(1);
    auto* covers = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    QVERIFY(covers != nullptr);
    QTRY_COMPARE(covers->model()->rowCount(), 2);
    covers->selectAll();
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-remove"));
    QVERIFY(remove != nullptr);
    QTRY_VERIFY(remove->isEnabled());
    remove->click();
    auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(apply != nullptr);
    QTRY_VERIFY(apply->isEnabled());
    apply->click();
    QTRY_COMPARE(outcomes.size(), 1U);
    QCOMPARE(outcomes.front().committed_source_count(), 1U);
    QFile first{QString::fromStdString(paths[0])};
    QVERIFY(first.open(QIODevice::ReadOnly));
    const auto saved_bytes = first.readAll();
    first.close();
    if (change_source) {
        QFile changed{QString::fromStdString(paths[1])};
        QVERIFY(changed.open(QIODevice::Append));
        QCOMPARE(changed.write("external-change"), 15);
        changed.close();
    }
    QPushButton* retry = nullptr;
    QTRY_VERIFY((retry = dialog->findChild<QPushButton*>(
                     QStringLiteral("bench-preparation-retry"))) != nullptr);
    QPointer guard{dialog};
    retry->click();
    QTRY_COMPARE(outcomes.size(), 2U);
    QCOMPARE(attempts[0], 1);
    QCOMPARE(attempts[1], 2);
    QVERIFY(first.open(QIODevice::ReadOnly));
    QCOMPARE(first.readAll(), saved_bytes);
    if (change_source) {
        QCOMPARE(outcomes.back().failed_source_count(), 1U);
        QCOMPARE(outcomes.back().sources.front().issue->code, core::ErrorCode::conflict);
        QVERIFY(!guard.isNull());
        delete dialog;
    } else {
        QCOMPARE(outcomes.back().committed_source_count(), 1U);
        QVERIFY(window_test::closeTaggerAfterWrite(guard));
        const auto art = metadata::read_local_artwork_inventory(paths[1]);
        QVERIFY(art && art->items.empty());
        const auto tags = metadata::read_local_metadata(paths[1]);
        QVERIFY(tags.has_value());
        QCOMPARE(tags->document.first_effective_value("title"),
                 std::optional<std::string>{"Retried title"});
    }
}

void BenchMainWindowTest::metadataApplyCombinesTagsAndArtwork_data() {
    QTest::addColumn<bool>("save_tags");
    QTest::newRow("tags-and-covers") << true;
    QTest::newRow("covers-with-tags-disabled") << false;
}

void BenchMainWindowTest::metadataServiceSettingsAndCompactPages() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.findChild<LocalLibraryPanel*>());
    auto* action = window.findChild<QAction*>(QStringLiteral("action-settings"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    dialog->showPage(SettingsDialog::Page::metadata_services);
    auto* key = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-acoustid-key"));
    auto* reveal = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-acoustid-show"));
    QVERIFY(key && reveal);
    auto* link = dialog->findChild<QLabel*>(QStringLiteral("bench-settings-acoustid-link"));
    QVERIFY(link && link->openExternalLinks());
    QVERIFY(link->text().contains(QStringLiteral("https://acoustid.org/new-application")));
    // ADR-0220: which engine serves the library and owns playback, settable
    // rather than hand-edited -- "is an engine in use" was otherwise
    // unanswerable from the UI.
    auto* engine_socket =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-socket"));
    QVERIFY(engine_socket);
    // The remote engine: none, until one is named. This computer's engine
    // (the fixture's) is not configured here (ADR-0227).
    QVERIFY(engine_socket->text().isEmpty());
    engine_socket->setText(QStringLiteral("/run/user/1000/melodyd.sock"));
    // ADR-0286: one password for every engine -- none of its own to give.
    QVERIFY(dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-token")) ==
            nullptr);

    auto* lastfm = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-lastfm-key"));
    QVERIFY(lastfm);
    QCOMPARE(lastfm->echoMode(), QLineEdit::Password);
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    key->setText(QStringLiteral("  test-client-key  "));
    reveal->setChecked(true);
    QCOMPARE(key->echoMode(), QLineEdit::Normal);
    reveal->setChecked(false);
    dialog->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), static_cast<QWidget*>(dialog));
    key->setFocus();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(key));
    QTest::keyClick(key, Qt::Key_Tab);
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(reveal));
    auto* note = dialog->findChild<QLabel*>(QStringLiteral("bench-settings-save-note"));
    // Pages that wait for Save say nothing about it; the button does.
    QVERIFY(note->isHidden());
    auto* pages = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-pages"));
    auto* stack = dialog->findChild<QStackedWidget*>(QStringLiteral("bench-settings-stack"));
    QVERIFY(pages && stack);
    dialog->resize(720, 480);
    QCoreApplication::processEvents();
    QVERIFY(dialog->width() <= 720);
    QVERIFY(dialog->height() <= 480);
    for (int page = 0; page < pages->count(); ++page) {
        pages->setCurrentRow(page);
        auto* scroll = qobject_cast<QScrollArea*>(stack->currentWidget());
        QVERIFY(scroll && scroll->widgetResizable());
    }
    dialog->showPage(SettingsDialog::Page::naming);
    QVERIFY(note->text().contains(QStringLiteral("immediately")));
    QVERIFY(!note->isHidden());
    // Every page is titled, and a number field is not stretched across it.
    QCOMPARE(dialog->findChildren<QLabel*>(QStringLiteral("bench-settings-page-title")).size(),
             pages->count());
    auto* capacity = dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-capacity"));
    QVERIFY(capacity != nullptr && capacity->maximumWidth() <= 260);
    dialog->showPage(SettingsDialog::Page::metadata_services);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-metadata-small.png")));
    }
    QPointer<SettingsDialog> lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(QSettings{}.value(QLatin1String(SettingsDialog::library_engine_socket_key)).toString(),
             QStringLiteral("/run/user/1000/melodyd.sock"));
    QCOMPARE(QSettings{}.value(QLatin1String(SettingsDialog::acoustid_client_key)).toString(),
             QStringLiteral("test-client-key"));
    action->trigger();
    dialog = window.findChild<SettingsDialog*>();
    key = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-acoustid-key"));
    QCOMPARE(key->text(), QStringLiteral("test-client-key"));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    key->clear();
    lifetime = dialog;
    dialog->reject();
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(QSettings{}.value(QLatin1String(SettingsDialog::acoustid_client_key)).toString(),
             QStringLiteral("test-client-key"));
    action->trigger();
    dialog = window.findChild<SettingsDialog*>();
    dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-acoustid-key"))->clear();
    lifetime = dialog;
    dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
        ->button(QDialogButtonBox::Save)
        ->click();
    QTRY_VERIFY(lifetime.isNull());
    QVERIFY(
        QSettings{}.value(QLatin1String(SettingsDialog::acoustid_client_key)).toString().isEmpty());
    // The old preamp shortcut opens the shared Settings page instead of a separate editor.
    window.findChild<QAction*>(QStringLiteral("action-local-replaygain-preamp"))->trigger();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    pages = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-pages"));
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("Playback"));
    QVERIFY(!window.findChild<QDialog*>(QStringLiteral("bench-rg-preamp-dialog")));
    dialog->reject();
}

// ADR-0237 stage 2: writing ratings into the files is an option in
// Settings, off by default, handed to the engines that keep the ratings.
void BenchMainWindowTest::ratingsInTagsIsAnEngineOption() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto client = protocol::Client::connect(protocol::Endpoint{
        .socket = engine_.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(client.has_value());
    const auto writes_tags = [&client] {
        auto answer = (*client)->call("ratings.tags");
        return answer && answer->value("write_tags", false);
    };
    QVERIFY(!writes_tags());
    const auto choose = [&window](const bool on) {
        window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
        auto* dialog = window.findChild<SettingsDialog*>();
        QVERIFY(dialog != nullptr);
        auto* option =
            dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-ratings-in-tags"));
        QVERIFY(option != nullptr);
        option->setChecked(on);
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    };
    const auto scale = [&client] {
        auto answer = (*client)->call("ratings.tags");
        return answer ? answer->value("rating_scale", std::string{}) : std::string{};
    };
    QCOMPARE(scale(), std::string{"off"});
    choose(true);
    QVERIFY(QSettings{}.value(QLatin1String(SettingsDialog::ratings_in_tags_key)).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(writes_tags(), 5'000);
    choose(false);
    QTRY_VERIFY_WITH_TIMEOUT(!writes_tags(), 5'000);

    // Other players' plain RATING tags: read on the scale chosen.
    const auto choose_scale = [&window](const QString& value) {
        window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
        auto* dialog = window.findChild<SettingsDialog*>();
        QVERIFY(dialog != nullptr);
        auto* combo =
            dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-rating-tag-scale"));
        QVERIFY(combo != nullptr);
        combo->setCurrentIndex(combo->findData(value));
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    };
    choose_scale(QStringLiteral("100"));
    QTRY_COMPARE_WITH_TIMEOUT(scale(), std::string{"100"}, 5'000);
    choose_scale(QStringLiteral("off"));
    QTRY_COMPARE_WITH_TIMEOUT(scale(), std::string{"off"}, 5'000);

    // ADR-0245: the backup tag, named in Settings, is the engine's; an
    // official name is written but warned about.
    const auto backup = [&client] {
        auto answer = (*client)->call("ratings.tags");
        return answer ? answer->value("backup_tag", std::string{"?"}) : std::string{"?"};
    };
    QCOMPARE(backup(), std::string{});
    {
        window.findChild<QAction*>(QStringLiteral("action-settings"))->trigger();
        auto* dialog = window.findChild<SettingsDialog*>();
        QVERIFY(dialog != nullptr);
        auto* writing =
            dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-ratings-in-tags"));
        auto* copy = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-rating-backup"));
        auto* name =
            dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-rating-backup-tag"));
        auto* note =
            dialog->findChild<QLabel*>(QStringLiteral("bench-settings-rating-backup-note"));
        QVERIFY(writing && copy && name && note);
        QVERIFY(!copy->isEnabled());
        writing->setChecked(true);
        QVERIFY(copy->isEnabled());
        copy->setChecked(true);
        QCOMPARE(name->text(), QStringLiteral("MELODY_RATING"));
        QVERIFY(note->isHidden());
        name->setText(QStringLiteral("COMMENT"));
        QVERIFY(!note->isHidden() && note->text().contains(QStringLiteral("official")));
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    }
    QTRY_COMPARE_WITH_TIMEOUT(backup(), std::string{"COMMENT"}, 5'000);
    (*client)->close();
}

void BenchMainWindowTest::librarySettingsManageFoldersWithoutScanning() {
    QTemporaryDir music;
    QVERIFY(music.isValid());
    const auto path = music.filePath(QStringLiteral("keep.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    BenchMainWindow window;
    window.show();
    LocalLibraryPanel* panel = nullptr;
    QTRY_VERIFY((panel = window.findChild<LocalLibraryPanel*>()));
    const auto raw_root = QFile::encodeName(music.path()).toStdString();
    panel->addRoot(raw_root);
    auto* action = window.findChild<QAction*>(QStringLiteral("action-settings"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    dialog->showPage(SettingsDialog::Page::engine);
    auto* roots = dialog->findChild<QListWidget*>(QStringLiteral("local-library-roots"));
    QVERIFY(roots);
    QTRY_COMPARE(roots->count(), 1);
    QCOMPARE(roots->item(0)->data(Qt::UserRole).toByteArray().toStdString(), raw_root);
    QVERIFY(roots->item(0)->text().contains(QStringLiteral("not scanned")));
    QVERIFY(!panel->property("scanning").toBool());
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-library.png")));
    }
    QPointer<SettingsDialog> lifetime = dialog;
    dialog->reject();
    QTRY_VERIFY(lifetime.isNull());
    auto* shortcut = panel->findChild<QToolButton*>(QStringLiteral("local-library-folders"));
    QVERIFY(shortcut);
    shortcut->click();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    auto* pages = dialog->findChild<QListWidget*>(QStringLiteral("bench-settings-pages"));
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("Engines"));
    roots = dialog->findChild<QListWidget*>(QStringLiteral("local-library-roots"));
    QTRY_COMPARE(roots->count(), 1);
    // The shortcut reuses the open Settings screen rather than making another manager.
    shortcut->click();
    QCOMPARE(window.findChildren<SettingsDialog*>().size(), 1);
    QCOMPARE(window.findChildren<QListWidget*>(QStringLiteral("local-library-roots")).size(), 1);
    roots->setCurrentRow(0);
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("local-library-folder-remove"));
    QVERIFY(remove && remove->isEnabled());
    remove->click();
    QTRY_COMPARE(roots->count(), 0);
    QVERIFY(QFile::exists(path));
    QVERIFY(!panel->property("scanning").toBool());
    lifetime = dialog;
    dialog->reject();
    QTRY_VERIFY(lifetime.isNull());
    shortcut->click();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    roots = dialog->findChild<QListWidget*>(QStringLiteral("local-library-roots"));
    QTRY_COMPARE(roots->count(), 0);
    // Closing a manager with a pending root load must not leave stale widget callbacks.
    dialog->reject();
}

void BenchMainWindowTest::playbackSettingsApplyLiveAndCancel() {
    BenchMainWindow window;
    window.show();
    auto* action = window.findChild<QAction*>(QStringLiteral("action-settings"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    dialog->showPage(SettingsDialog::Page::playback);
    auto* profile = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-buffer-profile"));
    auto* capacity = dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-capacity"));
    auto* threshold =
        dialog->findChild<QSpinBox*>(QStringLiteral("bench-settings-buffer-threshold"));
    auto* with_gain =
        dialog->findChild<QDoubleSpinBox*>(QStringLiteral("bench-settings-preamp-with"));
    auto* without_gain =
        dialog->findChild<QDoubleSpinBox*>(QStringLiteral("bench-settings-preamp-without"));
    auto* notifications =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-notifications"));
    QVERIFY(profile && capacity && threshold && with_gain && without_gain && notifications);
    QCOMPARE(profile->currentData().toString(), QStringLiteral("balanced"));
    QCOMPARE(capacity->value(), 750);
    QVERIFY(!capacity->isEnabled());
    profile->setCurrentIndex(profile->findData(QStringLiteral("resilient")));
    QCOMPARE(capacity->value(), 2000);
    QCOMPARE(threshold->value(), 250);
    profile->setCurrentIndex(profile->findData(QStringLiteral("custom")));
    QVERIFY(capacity->isEnabled());
    capacity->setValue(100);
    QCOMPARE(threshold->maximum(), 100);
    QCOMPARE(threshold->value(), 100);
    capacity->setValue(1234);
    threshold->setValue(234);
    with_gain->setValue(-3.5);
    without_gain->setValue(-6.0);
    notifications->setChecked(true);
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-playback.png")));
        dialog->showPage(SettingsDialog::Page::general);
        QVERIFY(dialog->grab().save(directory + QStringLiteral("/settings-general.png")));
    }
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QVERIFY(buttons);
    QPointer<SettingsDialog> lifetime = dialog;
    buttons->button(QDialogButtonBox::Save)->click();
    QTRY_VERIFY(lifetime.isNull());
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 1234);
    QTRY_COMPARE(window.property("trackknife-player-rg-preamp-with").toDouble(), -3.5);
    QTRY_COMPARE(window.property("trackknife-player-rg-preamp-without").toDouble(), -6.0);
    auto* notification_action =
        window.findChild<QAction*>(QStringLiteral("action-desktop-notifications"));
    QVERIFY(notification_action && notification_action->isChecked());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-buffer-custom"))->isChecked());
    QCOMPARE(QSettings{}.value(QStringLiteral("playback/buffer-start-threshold-ms")).toInt(), 234);

    action->trigger();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    profile = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-buffer-profile"));
    QCOMPARE(profile->currentData().toString(), QStringLiteral("custom"));
    with_gain = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("bench-settings-preamp-with"));
    QCOMPARE(with_gain->value(), -3.5);
    profile->setCurrentIndex(profile->findData(QStringLiteral("responsive")));
    with_gain->setValue(10);
    dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-notifications"))
        ->setChecked(false);
    lifetime = dialog;
    dialog->reject();
    QTRY_VERIFY(lifetime.isNull());
    QCOMPARE(QSettings{}.value(QStringLiteral("playback/buffer-profile")).toString(),
             QStringLiteral("custom"));
    QCOMPARE(QSettings{}.value(QStringLiteral("playback/rg-preamp-with")).toDouble(), -3.5);
    QVERIFY(notification_action->isChecked());
    QCOMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 1234);
    // Existing shortcuts and Settings read the same preference values.
    window.findChild<QAction*>(QStringLiteral("action-buffer-responsive"))->trigger();
    notification_action->setChecked(false);
    action->trigger();
    dialog = window.findChild<SettingsDialog*>();
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-buffer-profile"))
                 ->currentData()
                 .toString(),
             QStringLiteral("responsive"));
    QVERIFY(!dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-notifications"))
                 ->isChecked());
    dialog->reject();
}

// ADR-0235: the UPnP choice is saved and shown as saved when Settings opens
// again -- it was saved, and then shown off.
void BenchMainWindowTest::upnpSettingRoundTrip() {
#if MELODY_ENABLE_UPNP
    QSettings{}.remove(QStringLiteral("engine/upnp"));
    {
        SettingsDialog dialog;
        dialog.showPage(SettingsDialog::Page::engine);
        auto* upnp = dialog.findChild<QCheckBox*>(QStringLiteral("bench-settings-engine-upnp"));
        auto* buttons =
            dialog.findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
        QVERIFY(upnp != nullptr && buttons != nullptr);
        QVERIFY(upnp->isEnabled() && !upnp->isChecked());
        upnp->setChecked(true);
        buttons->button(QDialogButtonBox::Save)->click();
    }
    QVERIFY(QSettings{}.value(QStringLiteral("engine/upnp")).toBool());
    SettingsDialog reopened;
    QVERIFY(
        reopened.findChild<QCheckBox*>(QStringLiteral("bench-settings-engine-upnp"))->isChecked());
    reopened.reject();
    QSettings{}.remove(QStringLiteral("engine/upnp"));
#else
    QSKIP("built without UPnP");
#endif
}

// A save writes a list's items only when they changed since the last save:
// two kept searches of 66,841 tracks had every save -- a rating, a tab
// switch -- rewrite 3.2 million rows, and the window froze on every start.
void BenchMainWindowTest::aSaveWritesOnlyListsThatChanged() {
    const auto rows = [](const int count, const std::string& folder) {
        std::vector<LocalTrackRow> made;
        for (int index = 0; index < count; ++index) {
            LocalTrackRow row;
            row.raw_path = "/music/" + folder + "/" + std::to_string(index) + ".flac";
            row.title = "Track " + std::to_string(index);
            made.push_back(std::move(row));
        }
        return made;
    };
    const auto document = [](const std::string& name) {
        return persistence::ListDocument{.id = core::StableId::random(),
                                         .kind = persistence::ListKind::scratch,
                                         .name = name,
                                         .pinned = false,
                                         .dirty = false,
                                         .items = {}};
    };
    const auto carries_items = [](BenchMainWindow& window, const core::StableId& id) {
        for (const auto& write : window.workspace_.collectWrites()) {
            if (write.document.id == id) {
                return std::optional{write.items};
            }
        }
        return std::optional<bool>{};
    };
    core::StableId big_id;
    core::StableId small_id;
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        auto* big = window.addListTab(document("Big"), false);
        auto* small = window.addListTab(document("Small"), false);
        big_id = big->document.id;
        small_id = small->document.id;
        // Larger than one part of a restore, which is made in parallel.
        big->model->appendRows(rows(5000, "big"));
        small->model->appendRows(rows(2, "small"));
        QCOMPARE(carries_items(window, big_id), std::optional{true});
        window.persistNow(true);
        // Saved and unchanged: headers only.
        QCOMPARE(carries_items(window, big_id), std::optional{false});
        QCOMPARE(carries_items(window, small_id), std::optional{false});
        // One list edited: its items, and only its.
        small->model->appendRows(rows(1, "more"));
        QCOMPARE(carries_items(window, small_id), std::optional{true});
        QCOMPARE(carries_items(window, big_id), std::optional{false});
        // Renamed: written whole, as an engine's copy names it.
        big->document.name = "Big, renamed";
        QCOMPARE(carries_items(window, big_id), std::optional{true});
        window.persistNow(true);
        QCOMPARE(carries_items(window, big_id), std::optional{false});
    }
    // Restored as stored, and stored right: nothing to write again.
    BenchMainWindow reopened;
    reopened.show();
    QTRY_VERIFY(reopened.lists_restored_);
    auto* big = reopened.workspace_.tabForDocument(big_id);
    auto* small = reopened.workspace_.tabForDocument(small_id);
    QVERIFY(big != nullptr && small != nullptr);
    QCOMPARE(big->model->rows().size(), std::size_t{5000});
    // In order, across the parts it was restored in.
    for (const std::size_t index : {std::size_t{0}, std::size_t{2047}, std::size_t{2048},
                                    std::size_t{4096}, std::size_t{4999}}) {
        QCOMPARE(QString::fromStdString(big->model->rows()[index].raw_path),
                 QStringLiteral("/music/big/%1.flac").arg(index));
    }
    QCOMPARE(displayText(big->document.name), QStringLiteral("Big, renamed"));
    QCOMPARE(small->model->rows().size(), std::size_t{3});
    QCOMPARE(carries_items(reopened, big_id), std::optional{false});
    QCOMPARE(carries_items(reopened, small_id), std::optional{false});
    reopened.closeTabAt(reopened.tabs_->indexOf(big->view));
    reopened.closeTabAt(reopened.tabs_->indexOf(small->view));
    reopened.persistNow(true);
}

void BenchMainWindowTest::coverPolicyRoundTrip() {
    QSettings{}.setValue(QStringLiteral("convert/embed-artwork"), false);
    SettingsDialog dialog;
    dialog.showPage(SettingsDialog::Page::covers);
    dialog.show();
    auto* embed = dialog.findChild<QCheckBox*>(QStringLiteral("bench-artwork-embed"));
    auto* folder = dialog.findChild<QCheckBox*>(QStringLiteral("bench-artwork-folder-image"));
    auto* name = dialog.findChild<QComboBox*>(QStringLiteral("bench-artwork-folder-image-name"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    auto* embedded_edge =
        dialog.findChild<QSpinBox*>(QStringLiteral("bench-artwork-max-embedded-edge"));
    auto* folder_edge =
        dialog.findChild<QSpinBox*>(QStringLiteral("bench-artwork-max-folder-edge"));
    QVERIFY(embed && folder && name && buttons && embedded_edge && folder_edge);
    QVERIFY(embed->isChecked());
    QVERIFY(!folder->isChecked());
    // ADR-0317: 1000 px until another limit is chosen.
    QCOMPARE(embedded_edge->value(), 1000);
    QCOMPARE(SettingsDialog::artworkPolicy().max_embedded_edge, 1000U);
    embed->setChecked(false);
    folder->setChecked(true);
    name->setCurrentText(QStringLiteral("front.jpg"));
    embedded_edge->setValue(1000);
    folder_edge->setValue(3000);
    buttons->button(QDialogButtonBox::Save)->click();
    const auto policy = SettingsDialog::artworkPolicy();
    QVERIFY(!policy.embed && policy.write_folder_image);
    QCOMPARE(policy.max_embedded_edge, 1000U);
    QCOMPARE(policy.max_folder_edge, 3000U);
    QCOMPARE(policy.folder_image_name, std::string{"front.jpg"});
    QCOMPARE(policy.fetch_source, std::string{"coverartarchive+bandcamp"});
    SettingsDialog reopened;
    QVERIFY(!reopened.findChild<QCheckBox*>(QStringLiteral("bench-artwork-embed"))->isChecked());
    QVERIFY(
        reopened.findChild<QCheckBox*>(QStringLiteral("bench-artwork-folder-image"))->isChecked());
    QCOMPARE(reopened.findChild<QComboBox*>(QStringLiteral("bench-artwork-folder-image-name"))
                 ->currentText(),
             QStringLiteral("front.jpg"));
    QCOMPARE(
        reopened.findChild<QSpinBox*>(QStringLiteral("bench-artwork-max-folder-edge"))->value(),
        3000);
    QVERIFY(!QSettings{}.value(QStringLiteral("convert/embed-artwork")).toBool());
}

void BenchMainWindowTest::coverThumbnailAppliesPolicy_data() {
    QTest::addColumn<bool>("embed");
    QTest::newRow("embed-and-folder") << true;
    QTest::newRow("folder-only") << false;
}

void BenchMainWindowTest::coverThumbnailAppliesPolicy() {
    QFETCH(bool, embed);
    const bool save_tags = false;
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_embed_key), embed);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_key), true);
    QSettings{}.setValue(QLatin1String(SettingsDialog::artwork_folder_image_name_key),
                         QStringLiteral("folder.jpg"));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("album.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
    const auto raw = QFile::encodeName(path).toStdString();
    const auto seed_image = media.filePath(QStringLiteral("seed.jpg"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"), seed_image));
    const auto initial = metadata::read_local_artwork_inventory(raw);
    QVERIFY(initial.has_value());
    metadata::ArtworkWritePlanIntent seed{.occurrence_index = 0,
                                          .raw_media_path = raw,
                                          .expected_media_revision = initial->media_revision,
                                          .target_ordinal = 0,
                                          .expected_target_fingerprint = {},
                                          .kind = metadata::ArtworkWritePlanIntentKind::add,
                                          .replacement_raw_path =
                                              QFile::encodeName(seed_image).toStdString(),
                                          .added_role = metadata::ArtworkRole::front,
                                          .added_description = {},
                                          .replacement_embedded_source = std::nullopt};
    auto seed_plan = metadata::revalidate_artwork_write_plan({seed});
    auto seed_journal = persistence::SqliteMetadataOperationJournal::open(
        std::filesystem::path{media.filePath(QStringLiteral("seed.sqlite3")).toStdString()});
    QVERIFY(seed_plan && seed_plan->ready() && seed_journal);
    QVERIFY(operations::commit_artwork_source(
        seed_plan->sources.front(), *seed_journal,
        [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; }));
    const auto before = metadata::read_local_metadata(raw);
    QVERIFY(before.has_value());
    const MetadataPropertiesSource source{.source = {.raw_path = raw,
                                                     .source_revision = before->source_revision,
                                                     .baseline = before->document},
                                          .track_label = QStringLiteral("Combined save")};
    const std::filesystem::path database{
        media.filePath(QStringLiteral("journal.sqlite3")).toStdString()};
    std::optional<operations::MetadataApplyResult> observed;
    auto* dialog = new MetadataPropertiesDialog(
        1, [source](std::size_t) -> std::optional<MetadataPropertiesSource> { return source; }, {},
        [database] {
            return MetadataWritePlanApplier{
                [database](const metadata::MetadataWritePlan& plan,
                           const operations::MetadataApplyProgressCallback& progress,
                           const core::CancellationToken& cancellation)
                    -> core::Result<operations::MetadataApplyResult> {
                    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
                    if (!journal) {
                        return std::unexpected(journal.error());
                    }
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&journal](const metadata::MetadataWritePlanSource& item,
                                   const core::CancellationToken& token) {
                            return operations::commit_metadata_source(
                                item, *journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                token);
                        },
                        {}, {}, progress, cancellation);
                }};
        },
        [&observed](const operations::MetadataApplyResult& result) { observed = result; });
    dialog->setArtworkMutationServices([] { return ArtworkWritePlanApplier{}; }, {});
    dialog->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    const auto title = grid->ensureField(QStringLiteral("TITLE"));
    QVERIFY(title.has_value());
    const std::array items{std::size_t{0}};
    QVERIFY(grid->replaceFieldValues(items, title->field_index, {"One apply"}));
    auto* save_tags_box =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    QVERIFY(save_tags_box != nullptr);
    save_tags_box->setChecked(save_tags);
    auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-sections"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->currentIndex(), 0);
    auto* thumbnail = dialog->findChild<CoverThumbnail*>();
    auto* section = dialog->findChild<MetadataArtworkSection*>();
    auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(thumbnail && section && apply);
    QTRY_VERIFY(thumbnail->property("cover-editable").toBool());
    QTRY_VERIFY(!thumbnail->pixmap().isNull());
    QImage image(24, 24, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    const auto incoming = media.filePath(QStringLiteral("incoming.png"));
    QVERIFY(image.save(incoming));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(incoming)});
    QDragEnterEvent enter(dragPosition(QPoint(10, 10)), Qt::CopyAction, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &drop);
    QVERIFY(drop.isAccepted());
    auto intents = section->pendingIntents();
    QCOMPARE(intents.size(), embed ? 2U : 1U);
    QCOMPARE(intents.back().kind, metadata::ArtworkWritePlanIntentKind::add);
    QCOMPARE(intents.back().added_role, metadata::ArtworkRole::front);
    section->discardPendingChanges();
    if (embed) {
        section->removeFrontCover();
        QCOMPARE(section->pendingIntents().size(), 1U);
        QCOMPARE(section->pendingIntents().front().kind,
                 metadata::ArtworkWritePlanIntentKind::remove);
        section->discardPendingChanges();
    }
    QApplication::clipboard()->setImage(image);
    QTest::keyClick(thumbnail, Qt::Key_V, Qt::ControlModifier);
    releaseHeldModifiers(thumbnail);
    QTRY_VERIFY(!section->isBusy());
    QTRY_VERIFY(section->hasPendingChanges());
    QCOMPARE(section->pendingIntents().size(), embed ? 2U : 1U);
    // ADR-0317: written as a JPEG, whatever was pasted.
    const auto destination = media.filePath(QStringLiteral("folder.jpg"));
    QVERIFY(!QFile::exists(destination));
    QVERIFY(!observed.has_value());
    QTRY_VERIFY(apply->isEnabled());
    QPointer guard{dialog};
    apply->click();
    QDialog* review = nullptr;
    QTRY_VERIFY((review = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-folder-cover-review"))) != nullptr);
    QVERIFY(!observed.has_value());
    QVERIFY(!QFile::exists(destination));
    auto* review_buttons =
        review->findChild<QDialogButtonBox*>(QStringLiteral("bench-folder-cover-review-buttons"));
    QVERIFY(review_buttons != nullptr);
    QPointer<QDialog> cancelled_review{review};
    review_buttons->button(QDialogButtonBox::Cancel)->click();
    QTRY_VERIFY(cancelled_review.isNull());
    QVERIFY(!QFile::exists(destination));
    QVERIFY(section->hasPendingChanges());
    apply->click();
    QTRY_VERIFY((review = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-folder-cover-review"))) != nullptr);
    review_buttons =
        review->findChild<QDialogButtonBox*>(QStringLiteral("bench-folder-cover-review-buttons"));
    QVERIFY(review_buttons != nullptr);
    review_buttons->button(QDialogButtonBox::Save)->click();
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10000);
    QVERIFY2(observed->committed_source_count() == 1U,
             qPrintable(observed->sources.front().issue
                            ? QString::fromStdString(observed->sources.front().issue->message)
                            : QString{}));
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
    const auto after = metadata::read_local_metadata(raw);
    const auto artwork = metadata::read_local_artwork_inventory(raw);
    QVERIFY(after && artwork);
    QVERIFY(QFile::exists(destination));
    auto folder = metadata::read_artwork_image_file(QFile::encodeName(destination).toStdString());
    QVERIFY(folder.has_value());
    const auto embedded = std::ranges::find_if(artwork->items, [](const auto& item) {
        return item.provenance == metadata::ArtworkProvenance::embedded &&
               item.role == metadata::ArtworkRole::front;
    });
    QVERIFY(embedded != artwork->items.end());
    QCOMPARE(embedded->content_fingerprint == folder->content_fingerprint, embed);
    if (!embed)
        QCOMPARE(after->source_revision, before->source_revision);
    QCOMPARE(after->document.effective_values("title"),
             save_tags ? std::vector<std::string>{"One apply"}
                       : before->document.effective_values("title"));
    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
    QVERIFY(journal.has_value());
    const auto backups = journal->load_backups();
    QVERIFY(backups.has_value());
    QCOMPARE(backups->size(), embed ? 2U : 1U);
}

void BenchMainWindowTest::metadataApplyCombinesTagsAndArtwork() {
    QFETCH(bool, save_tags);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("album.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
    const auto raw = QFile::encodeName(path).toStdString();
    const auto before = metadata::read_local_metadata(raw);
    QVERIFY(before.has_value());
    const MetadataPropertiesSource source{.source = {.raw_path = raw,
                                                     .source_revision = before->source_revision,
                                                     .baseline = before->document},
                                          .track_label = QStringLiteral("Combined save")};
    const std::filesystem::path database{
        media.filePath(QStringLiteral("journal.sqlite3")).toStdString()};
    std::optional<operations::MetadataApplyResult> observed;
    auto* dialog = new MetadataPropertiesDialog(
        1, [source](std::size_t) -> std::optional<MetadataPropertiesSource> { return source; }, {},
        [database] {
            return MetadataWritePlanApplier{
                [database](const metadata::MetadataWritePlan& plan,
                           const operations::MetadataApplyProgressCallback& progress,
                           const core::CancellationToken& cancellation)
                    -> core::Result<operations::MetadataApplyResult> {
                    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
                    if (!journal) {
                        return std::unexpected(journal.error());
                    }
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&journal](const metadata::MetadataWritePlanSource& item,
                                   const core::CancellationToken& token) {
                            return operations::commit_metadata_source(
                                item, *journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                token);
                        },
                        {}, {}, progress, cancellation);
                }};
        },
        [&observed](const operations::MetadataApplyResult& result) { observed = result; });
    dialog->setArtworkMutationServices([] { return ArtworkWritePlanApplier{}; }, {});
    dialog->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    const auto title = grid->ensureField(QStringLiteral("TITLE"));
    QVERIFY(title.has_value());
    const std::array items{std::size_t{0}};
    QVERIFY(grid->replaceFieldValues(items, title->field_index, {"One apply"}));
    auto* save_tags_box =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    QVERIFY(save_tags_box != nullptr);
    save_tags_box->setChecked(save_tags);
    auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-sections"));
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(1);
    auto* covers = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-remove"));
    auto* old_save = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(covers && remove && old_save && apply);
    QTRY_COMPARE(covers->model()->rowCount(), 1);
    QVERIFY(!old_save->isVisible());
    covers->selectAll();
    QTRY_VERIFY(remove->isEnabled());
    remove->click();
    auto* pending =
        dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(pending != nullptr);
    QCOMPARE(pending->model()->rowCount(), 1);
    QVERIFY(!pending->model()->index(0, 4).data(Qt::DecorationRole).value<QImage>().isNull());
    QCOMPARE(pending->model()->index(0, 5).data().toString(), QStringLiteral("Removed"));
    QVERIFY(!observed.has_value());
    QCOMPARE(metadata::read_local_artwork_inventory(raw)->items.size(), 1U);
    QTRY_VERIFY(apply->isEnabled());
    QPointer guard{dialog};
    apply->click();
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10000);
    QVERIFY2(observed->committed_source_count() == 1U,
             qPrintable(observed->sources.front().issue
                            ? QString::fromStdString(observed->sources.front().issue->message)
                            : QString{}));
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
    const auto after = metadata::read_local_metadata(raw);
    const auto artwork = metadata::read_local_artwork_inventory(raw);
    QVERIFY(after && artwork);
    QVERIFY(artwork->items.empty());
    QCOMPARE(after->document.effective_values("title"),
             save_tags ? std::vector<std::string>{"One apply"}
                       : before->document.effective_values("title"));
    auto journal = persistence::SqliteMetadataOperationJournal::open(database);
    QVERIFY(journal.has_value());
    const auto backups = journal->load_backups();
    QVERIFY(backups.has_value());
    QCOMPARE(backups->size(), 1U);
}

// ADR-0316: with Fetch covers ticked, each album Identify applies has its
// release's front cover from the Cover Art Archive staged, as Fetch cover…
// stages one chosen: each album's for its own files.
void BenchMainWindowTest::identifyFetchesTheCoversOfTheAlbumsApplied() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto alpha_id = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const auto beta_id = QStringLiteral("44444444-2222-3333-4444-555555555555");
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    std::vector<MetadataPropertiesSource> sources;
    for (const auto& [album, release] : {std::pair{std::string{"Alpha"}, alpha_id.toStdString()},
                                         std::pair{std::string{"Beta"}, std::string{}}}) {
        QVERIFY(QDir{media.path()}.mkpath(QString::fromStdString(album)));
        const auto path = media.filePath(QString::fromStdString(album + "/01.flac"));
        QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        auto read = metadata::read_local_metadata(raw_path);
        QVERIFY(read.has_value());
        auto document = read->document;
        std::erase_if(document.fields, [](const metadata::MetadataField& each) {
            return each.canonical_name == "ALBUM" || each.canonical_name == "ARTIST" ||
                   each.canonical_name == "ALBUMARTIST" || each.canonical_name == "TITLE" ||
                   each.canonical_name == "TRACKNUMBER" ||
                   each.canonical_name == "MUSICBRAINZ_ALBUMID";
        });
        document.fields.push_back(field("ALBUM", {album}));
        document.fields.push_back(field("ARTIST", {"Band"}));
        document.fields.push_back(field("TITLE", {"One"}));
        document.fields.push_back(field("TRACKNUMBER", {"1"}));
        if (!release.empty()) {
            document.fields.push_back(field("MUSICBRAINZ_ALBUMID", {release}));
        }
        sources.push_back(MetadataPropertiesSource{
            .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                     .source_revision = read->source_revision,
                                                     .baseline = std::move(document)},
            .track_label = QString::fromStdString(album),
        });
    }
    QImage cover{16, 16, QImage::Format_ARGB32};
    cover.fill(Qt::darkCyan);
    QByteArray png;
    QBuffer buffer{&png};
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(cover.save(&buffer, "PNG"));
    buffer.close();
    const auto release_body = [](const QString& id, const char* title, const char* group) {
        return QStringLiteral(R"json({
          "id": "%1", "title": "%2", "status": "Official", "date": "1999-09-09",
          "country": "DE", "release-group": {"id": "%3"},
          "artist-credit": [{"name": "Band",
            "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
          "media": [{"position": 1, "format": "CD", "track-count": 1, "tracks": [
            {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
             "title": "One",
             "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}}
          ]}]
        })json")
            .arg(id, QLatin1String{title}, QLatin1String{group})
            .toUtf8();
    };
    // ADR-0326: Beta has nothing in the archive; its links name a Bandcamp
    // album page, whose cover is fetched instead. ADR-0332: Alpha's release
    // has nothing either, and no Bandcamp page; another release of its album
    // has a cover, which the archive gives for the release group.
    const auto listing = [] { return QByteArray{R"json({"images": []})json"}; };
    const auto alpha_group = QStringLiteral("99999999-8888-7777-6666-555555555555");
    const auto other_release = QStringLiteral("77777777-2222-3333-4444-555555555555");
    QStringList covers_asked;
    QStringList bandcamp_asked;
    QStringList groups_asked;
    const MusicBrainzLookupService service{
        .fetch =
            [&](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                const auto plain = QUrl::fromPercentEncoding(url.toUtf8());
                if (plain.contains(QStringLiteral("?query="))) {
                    completion(QStringLiteral(R"json({"count": 1, "releases": [{
                        "id": "%1", "score": 100, "title": "Beta", "status": "Official",
                        "track-count": 1,
                        "artist-credit": [{"name": "Band", "artist": {
                          "id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
                        "release-group": {"id": "88888888-8888-7777-6666-555555555555"},
                        "media": [{"format": "CD", "track-count": 1}]}]})json")
                                   .arg(beta_id)
                                   .toUtf8());
                    return;
                }
                if (plain.contains(QStringLiteral("release?release-group="))) {
                    completion(QByteArray{R"json({"release-count": 0, "releases": []})json"});
                    return;
                }
                if (plain.contains(beta_id + QStringLiteral("?inc=url-rels"))) {
                    completion(QByteArray{R"json({"id": "x", "relations": [
                        {"type": "purchase for download",
                         "url": {"resource": "http://band.bandcamp.com/album/beta"}}],
                        "release-group": {"id": "88888888-8888-7777-6666-555555555555"}})json"});
                    return;
                }
                if (plain.contains(alpha_id + QStringLiteral("?inc=url-rels"))) {
                    completion(QStringLiteral(R"json({"id": "%1", "relations": [],
                        "release-group": {"id": "%2"}})json")
                                   .arg(alpha_id, alpha_group)
                                   .toUtf8());
                    return;
                }
                if (plain.startsWith(
                        QStringLiteral("https://coverartarchive.org/release-group/"))) {
                    groups_asked << plain.section(QLatin1Char('/'), -1);
                    completion(QStringLiteral(R"json({"images": [{"id": 9, "front": true,
                        "approved": true, "types": ["Front"],
                        "image": "http://coverartarchive.org/release/%1/9.png"}],
                        "release": "https://musicbrainz.org/release/%1"})json")
                                   .arg(other_release)
                                   .toUtf8());
                    return;
                }
                if (plain == QStringLiteral("https://coverartarchive.org/release/") +
                                 other_release + QStringLiteral("/9.png")) {
                    completion(png);
                    return;
                }
                if (plain == QStringLiteral("https://band.bandcamp.com/album/beta")) {
                    bandcamp_asked << plain;
                    completion(
                        QByteArray{R"(<html><head><meta property="og:image" )"
                                   R"(content="https://f4.bcbits.com/img/a0123456789_5.jpg">)"
                                   R"(</head></html>)"});
                    return;
                }
                if (plain == QStringLiteral("https://f4.bcbits.com/img/a0123456789_10.jpg")) {
                    bandcamp_asked << plain;
                    completion(png);
                    return;
                }
                for (const auto& [id, title, group] :
                     {std::tuple{alpha_id, "Alpha", "99999999-8888-7777-6666-555555555555"},
                      std::tuple{beta_id, "Beta", "88888888-8888-7777-6666-555555555555"}}) {
                    if (plain.contains(id + QStringLiteral("?inc="))) {
                        completion(release_body(id, title, group));
                        return;
                    }
                    if (plain == QStringLiteral("https://coverartarchive.org/release/") + id) {
                        covers_asked << title;
                        completion(listing());
                        return;
                    }
                }
                completion(std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                       .message = "unexpected url",
                                                       .context = {}}));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };
    const auto database_path =
        std::filesystem::path{media.filePath(QStringLiteral("cover.sqlite3")).toStdString()};
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->setArtworkMutationServices(
        ArtworkWritePlanApplierFactory{[database_path] {
            return ArtworkWritePlanApplier{
                [database_path](const metadata::ArtworkWritePlan& plan,
                                const operations::ArtworkApplyProgressCallback& progress,
                                const core::CancellationToken& cancellation)
                    -> core::Result<operations::ArtworkApplyResult> {
                    auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
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
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                source_cancellation);
                        },
                        progress, cancellation, operations::ArtworkApplyOptions{});
                }};
        }},
        {});
    properties->show();
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid != nullptr && identify != nullptr);
    files->selectAll();
    QTRY_VERIFY_WITH_TIMEOUT(identify->isEnabled(), 10'000);
    QTest::mouseClick(identify, Qt::LeftButton);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    QPointer<QDialog> open = dialog;
    auto* fetch =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-musicbrainz-identify-fetch-covers"));
    auto* apply =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-apply"));
    auto* next = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-next"));
    QVERIFY(fetch != nullptr && apply != nullptr && next != nullptr);
    QVERIFY(fetch->isChecked());
    // Both albums seen, each previewed.
    QTRY_COMPARE_WITH_TIMEOUT(apply->text(), QStringLiteral("Apply 1 album"), 10'000);
    next->click();
    QTRY_COMPARE_WITH_TIMEOUT(apply->text(), QStringLiteral("Apply 2 albums"), 10'000);
    apply->click();
    QTRY_VERIFY_WITH_TIMEOUT(open.isNull(), 10'000);
    // Their covers, each staged for its own album's file.
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY2_WITH_TIMEOUT(
        status->text().contains(QStringLiteral("Covers fetched for 2 albums")),
        qPrintable(status->text()), 20'000);
    QCOMPARE(covers_asked.size(), 2);
    // Alpha's from another release of its album -- asked only for Alpha:
    // Beta's own Bandcamp page came first.
    QCOMPARE(groups_asked, QStringList{alpha_group});
    // Beta's from Bandcamp, at 1200 pixels.
    QCOMPARE(bandcamp_asked,
             (QStringList{QStringLiteral("https://band.bandcamp.com/album/beta"),
                          QStringLiteral("https://f4.bcbits.com/img/a0123456789_10.jpg")}));
    auto* artwork = properties->findChild<ArtworkSession*>();
    QVERIFY(artwork != nullptr);
    std::set<std::string> covered;
    for (const auto& intent : artwork->pendingIntents()) {
        if (intent.kind == metadata::ArtworkWritePlanIntentKind::add &&
            intent.added_role == metadata::ArtworkRole::front) {
            covered.insert(intent.raw_media_path);
        }
    }
    QCOMPARE(covered.size(), std::size_t{2U});
    QVERIFY(covered.contains(sources[0].source.raw_path));
    QVERIFY(covered.contains(sources[1].source.raw_path));
    delete properties;
}

// ADR-0332: files of several albums are listed under their albums' titles;
// a title selects its album's files, with Ctrl adds them.
void BenchMainWindowTest::fileListShowsTheFilesUnderTheirAlbums() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    for (const auto& [album, file] : {std::pair{std::string{"Alpha"}, std::string{"01.flac"}},
                                      std::pair{std::string{"Alpha"}, std::string{"02.flac"}},
                                      std::pair{std::string{"Beta"}, std::string{"01.flac"}}}) {
        QVERIFY(QDir{media.path()}.mkpath(QString::fromStdString(album)));
        const auto name =
            QString::fromStdString(album) + QLatin1Char('/') + QString::fromStdString(file);
        const auto path = media.filePath(name);
        QVERIFY(materialize_audio_fixture(album == "Alpha"
                                              ? QStringLiteral("art-tone-flac.b64")
                                              : QStringLiteral("rich-metadata-flac.b64"),
                                          path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        auto read = metadata::read_local_metadata(raw_path);
        QVERIFY(read.has_value());
        auto document = read->document;
        sources.push_back(MetadataPropertiesSource{
            .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                     .source_revision = read->source_revision,
                                                     .baseline = std::move(document)},
            .track_label = name,
        });
    }
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {}, {}, {});
    properties->show();
    FileScopeView* files = nullptr;
    QTRY_VERIFY((files = dynamic_cast<FileScopeView*>(properties->fileListView())) != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(files->model()->rowCount(), 3, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(files->albumHeaders().size(), std::size_t{2U}, 10'000);
    // Each album by its title and artist.
    QCOMPARE(files->albumHeaders().at(0),
             QStringLiteral("Trackbench Fixtures · Trackknife Project"));
    QVERIFY(files->albumHeaders().at(2).startsWith(QStringLiteral("Rich Metadata · ")));
    const auto selected = [files] {
        std::set<int> rows;
        for (const auto& index : files->selectionModel()->selectedRows()) {
            rows.insert(index.row());
        }
        return rows;
    };
    const auto title_of = [files](const int row) {
        return files->visualRect(files->model()->index(row, 0)).topLeft() + QPoint{20, 4};
    };
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, title_of(2));
    QCOMPARE(selected(), (std::set<int>{2}));
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::ControlModifier, title_of(0));
    QCOMPARE(selected(), (std::set<int>{0, 1, 2}));
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, title_of(0));
    QCOMPARE(selected(), (std::set<int>{0, 1}));
    // A file, below its album's title, is still itself.
    const auto second = files->visualRect(files->model()->index(1, 0));
    QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint{second.center().x(), second.center().y()});
    QCOMPARE(selected(), (std::set<int>{1}));
    delete properties;
}

// Closed while covers are being saved, the editor waits for the files to be
// safe -- and then closes by itself, not left open saying it is closing.
// A cover still being fetched is not waited for.
void BenchMainWindowTest::closingWaitsForACoverThenCloses() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("01.flac"));
    // Tagged with release 33333333-…
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const MetadataPropertiesSource source{
        .source = metadata::StagedMetadataSource{.raw_path = raw_path,
                                                 .source_revision = read->source_revision,
                                                 .baseline = read->document},
        .track_label = QStringLiteral("01"),
    };
    QImage cover{16, 16, QImage::Format_ARGB32};
    cover.fill(Qt::darkCyan);
    QByteArray png;
    QBuffer buffer{&png};
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(cover.save(&buffer, "PNG"));
    buffer.close();
    // The image is held until the test lets it go.
    std::function<void(core::Result<QByteArray>)> held;
    const MusicBrainzLookupService service{
        .fetch =
            [&held](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                if (url.startsWith(QStringLiteral("https://coverartarchive.org/release/")) &&
                    url.endsWith(QStringLiteral("/7.png"))) {
                    held = std::move(completion);
                    return;
                }
                if (url.startsWith(QStringLiteral("https://coverartarchive.org/release/"))) {
                    completion(QByteArray{R"json({"images": [{"id": 7, "front": true,
                        "approved": true, "types": ["Front"], "image":
                        "https://coverartarchive.org/release/33333333-3333-3333-3333-333333333333/7.png"}]})json"});
                    return;
                }
                completion(std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                       .message = "unexpected url",
                                                       .context = {}}));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };
    // The save is held until the test lets it go.
    auto released = std::make_shared<std::atomic_bool>(false);
    auto saving = std::make_shared<std::atomic_bool>(false);
    const auto open = [&] {
        QPointer properties = new MetadataPropertiesDialog(
            1U,
            [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
                return index == 0U ? std::optional{source} : std::nullopt;
            },
            {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
        properties->setAttribute(Qt::WA_DeleteOnClose);
        properties->setArtworkMutationServices(
            ArtworkWritePlanApplierFactory{[released, saving] {
                return ArtworkWritePlanApplier{
                    [released, saving](const metadata::ArtworkWritePlan&,
                                       const operations::ArtworkApplyProgressCallback&,
                                       const core::CancellationToken&)
                        -> core::Result<operations::ArtworkApplyResult> {
                        *saving = true;
                        while (!*released) {
                            std::this_thread::sleep_for(std::chrono::milliseconds{10});
                        }
                        return operations::ArtworkApplyResult{};
                    }};
            }},
            {});
        properties->show();
        return properties;
    };
    const auto choose = [](QWidget* properties) {
        QPushButton* fetch = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((fetch = properties->findChild<QPushButton*>(
                                      QStringLiteral("bench-metadata-cover-fetch"))) != nullptr &&
                                     fetch->isEnabled(),
                                 10'000);
        QTest::mouseClick(fetch, Qt::LeftButton);
        QVERIFY(chooseInCoverPicker(properties, QStringLiteral("Cover Art Archive"),
                                    QStringLiteral("Front")));
    };

    // Fetching: closed at once.
    auto fetching = open();
    choose(fetching);
    if (QTest::currentTestFailed()) {
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(held), 10'000);
    QVERIFY(fetching->close());
    QTRY_VERIFY(fetching.isNull());
    std::exchange(held, {})(std::unexpected(
        core::Error{.code = core::ErrorCode::cancelled, .message = "closed", .context = {}}));

    // Saving: waited for, saying so, then closed by itself.
    auto properties = open();
    choose(properties);
    if (QTest::currentTestFailed()) {
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(held), 10'000);
    std::exchange(held, {})(png);
    QPushButton* save = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((save = properties->findChild<QPushButton*>(
                                  QStringLiteral("bench-metadata-artwork-save"))) != nullptr &&
                                 save->isEnabled(),
                             10'000);
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(saving->load(), 10'000);
    QVERIFY(!properties->close());
    QVERIFY(properties->isVisible());
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY2(status->text().contains(QStringLiteral("closing once")),
                 qPrintable(status->text()));
    *released = true;
    QTRY_VERIFY_WITH_TIMEOUT(properties.isNull(), 10'000);
}

void BenchMainWindowTest::artworkFetchesCoverArtFromArchiveAndAddsFront_data() {
    // ADR-0237: a downloaded cover is handed to the engine and written by it,
    // or written by this process -- the same section, the same result.
    QTest::addColumn<bool>("through_engine");
    QTest::newRow("engine") << true;
    QTest::newRow("this-process") << false;
}

void BenchMainWindowTest::artworkFetchesCoverArtFromArchiveAndAddsFront() {
    // ADR-0317: covers are JPEGs: a colour within its loss.
    const auto jpeg_close = [](const QColor& shown, const QColor& expected) {
        return std::abs(shown.red() - expected.red()) <= 4 &&
               std::abs(shown.green() - expected.green()) <= 4 &&
               std::abs(shown.blue() - expected.blue()) <= 4;
    };
    QFETCH(bool, through_engine);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("cover-fetch.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), media_path));
    const auto encoded = QFile::encodeName(media_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto release_id = QStringLiteral("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Cover fetch fixture"),
    };

    QImage cover{16, 16, QImage::Format_ARGB32};
    cover.fill(Qt::darkCyan);
    QByteArray png_bytes;
    QBuffer png_buffer{&png_bytes};
    QVERIFY(png_buffer.open(QIODevice::WriteOnly));
    QVERIFY(cover.save(&png_buffer, "PNG"));
    png_buffer.close();
    QImage second_cover{24, 24, QImage::Format_ARGB32};
    second_cover.fill(Qt::darkMagenta);
    QByteArray second_png_bytes;
    QBuffer second_png_buffer{&second_png_bytes};
    QVERIFY(second_png_buffer.open(QIODevice::WriteOnly));
    QVERIFY(second_cover.save(&second_png_buffer, "PNG"));
    second_png_buffer.close();
    QVERIFY(second_png_bytes.size() != png_bytes.size());
    int image_serves = 0;
    static constexpr auto listing_text = R"json({
      "images": [{"id": 42, "front": true, "approved": true, "types": ["Front"],
        "image":
          "http://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/42.png"}]
    })json";
    const QByteArray listing_body{listing_text};
    int fetches = 0;
    const MusicBrainzLookupService service{
        .fetch =
            [&fetches, &image_serves, listing_body, png_bytes, second_png_bytes, release_id](
                const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                ++fetches;
                if (url == QStringLiteral("https://coverartarchive.org/release/") + release_id) {
                    completion(listing_body);
                    return;
                }
                // The listing carried an http URL; the fetch must be the
                // https upgrade.
                if (url == QStringLiteral("https://coverartarchive.org/release/"
                                          "2f2ac1b7-1111-4f4f-8f8f-123456789abc/42.png")) {
                    completion(++image_serves == 1 ? png_bytes : second_png_bytes);
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "unexpected url",
                    .context = {},
                }));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };

    const auto database_path =
        std::filesystem::path{media.filePath(QStringLiteral("cover.sqlite3")).toStdString()};
    std::optional<operations::ArtworkApplyResult> observed;
    std::optional<operations::MetadataApplyResult> tags_observed;
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
        {},
        work ? MetadataWritePlanApplierFactory{[work] {
            return MetadataWritePlanApplier{
                [work](const metadata::MetadataWritePlan& plan,
                       const operations::MetadataApplyProgressCallback& progress,
                       const core::CancellationToken& cancellation) {
                    return work->apply(plan, progress, cancellation);
                }};
        }}
             : MetadataWritePlanApplierFactory{[database_path] {
                   return MetadataWritePlanApplier{
                       [database_path](const metadata::MetadataWritePlan& plan,
                                       const operations::MetadataApplyProgressCallback& progress,
                                       const core::CancellationToken& cancellation)
                           -> core::Result<operations::MetadataApplyResult> {
                           auto opened =
                               persistence::SqliteMetadataOperationJournal::open(database_path);
                           if (!opened) {
                               return std::unexpected(std::move(opened.error()));
                           }
                           auto journal = std::move(*opened);
                           return operations::apply_metadata_write_plan(
                               plan,
                               [&journal](const metadata::MetadataWritePlanSource& source_plan,
                                          const core::CancellationToken& source_cancellation) {
                                   return operations::commit_metadata_source(
                                       source_plan, journal,
                                       [](const operations::MetadataCommitResult&)
                                           -> core::Result<void> { return {}; },
                                       source_cancellation);
                               },
                               {}, {}, progress, cancellation);
                       }};
               }},
        [&tags_observed](const operations::MetadataApplyResult& result) { tags_observed = result; },
        {}, {}, {}, {}, nullptr, {}, service, work ? engineFileWorkTools(work) : FileWorkTools{});
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
    auto* files = properties->fileListView();
    QVERIFY(files != nullptr);
    auto* grid = qobject_cast<MetadataGridModel*>(files->model());
    QVERIFY(grid != nullptr);
    const auto captured = grid->sharedSelection();
    // Stage the same provider preview used by Identify, including a new
    // release ID. The cover must be fetchable before these tags are saved.
    const metadata::MetadataProposalSet proposals{
        .provider_name = "MusicBrainz",
        .provider_detail = "Matched release",
        .items = {{.item_index = 0U,
                   .fields = {{.canonical_field =
                                   metadata::canonicalize_field_name("MUSICBRAINZ_ALBUMID"),
                               .display_field = "MUSICBRAINZ_ALBUMID",
                               .values = {release_id.toStdString()},
                               .confidence = 1.0,
                               .rationale = "Matched release"},
                              {.canonical_field = "title",
                               .display_field = "TITLE",
                               .values = {"Identified title"},
                               .confidence = 1.0,
                               .rationale = "Matched track"}},
                   .artwork = {}}}};
    const auto proposal =
        metadata::metadata_proposal_preview(grid->selection(), grid->patches(), proposals, 0.5);
    QVERIFY(proposal.has_value());
    QVERIFY(grid->stageTransformation(*proposal, {QStringLiteral("MusicBrainz")}));
    const auto draft = grid->patches().patches();
    files->selectAll();
    sections->setCurrentIndex(0);
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* fetch = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-cover-fetch"));
    QVERIFY(items != nullptr);
    QVERIFY(fetch != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 1, 5'000);

    // One unambiguous release across the selection enables the fetch; the
    // download stages an addition; Save artwork is the explicit commit.
    QTRY_VERIFY(fetch->isEnabled());
    QTest::mouseClick(fetch, Qt::LeftButton);
    QVERIFY(chooseInCoverPicker(properties, QStringLiteral("Cover Art Archive"),
                                QStringLiteral("Front")));
    auto* save = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    QVERIFY(save != nullptr);
    QTRY_VERIFY(save->isEnabled());
    auto* pending =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(pending != nullptr);
    QTRY_VERIFY(!pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    const auto preview = pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>();
    // Shown at 60 px or less, in more device pixels for HiDPI (ADR-0251).
    const auto shown = preview.deviceIndependentSize();
    QVERIFY(shown.width() <= 60.0 && shown.height() <= 60.0);
    QVERIFY(preview.devicePixelRatio() > 1.0);
    // A JPEG now (ADR-0317): the colour within its loss.
    const auto shown_colour = preview.pixelColor(0, 0);
    const QColor expected_colour{Qt::darkCyan};
    QVERIFY(std::abs(shown_colour.red() - expected_colour.red()) <= 4 &&
            std::abs(shown_colour.green() - expected_colour.green()) <= 4 &&
            std::abs(shown_colour.blue() - expected_colour.blue()) <= 4);
    QCOMPARE(pending->model()->index(0, 4).data().toString(), QStringLiteral("None"));
    QVERIFY(!observed.has_value());
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    const auto apply_issue = observed->sources.front().issue
                                 ? QString::fromStdString(observed->sources.front().issue->message)
                                 : QStringLiteral("no per-source issue");
    QVERIFY2(observed->committed_source_count() == 1U, qPrintable(apply_issue));
    QCOMPARE(fetches, 2);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 2, 5'000);
    QVERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-preparation-feedback")) ==
            nullptr);
    const auto inventory = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(inventory.has_value());
    QCOMPARE(inventory->items.size(), 2U);
    const auto added = std::ranges::find_if(inventory->items, [](const auto& item) {
        return item.role == metadata::ArtworkRole::front;
    });
    QVERIFY(added != inventory->items.end());
    // ADR-0317: the archive's PNG written as Melody writes covers.
    QCOMPARE(added->mime_type, std::string{"image/jpeg"});
    QCOMPARE(added->provenance, metadata::ArtworkProvenance::embedded);

    QCOMPARE(grid->selection().source(0).source_revision,
             std::optional{observed->sources.front().commit->published_revision});
    QCOMPARE(captured->source(0).source_revision, std::optional{read->source_revision});
    QCOMPARE(grid->patches().patches(), draft);
    QVERIFY(grid->undo());
    QCOMPARE(grid->patches().patch_count(), 0U);
    QVERIFY(grid->redo());
    QCOMPARE(grid->patches().patches(), draft);
    const auto title_column = grid->fieldColumn(QStringLiteral("title"));
    QVERIFY(title_column.has_value());
    QCOMPARE(grid->index(0, *title_column).data(metadata_cell_staged_source_role).toString(),
             QStringLiteral("MusicBrainz"));
    // Selection refreshes must also use the new revision, not put the
    // Artwork section back on the revision captured when Properties opened.
    files->clearSelection();
    files->selectAll();
    QTRY_VERIFY(fetch->isEnabled());
    sections->setCurrentIndex(0);
    auto* apply =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-changes"));
    QVERIFY(apply != nullptr);
    QTRY_VERIFY(apply->isEnabled());
    QPointer guard{properties};
    QTest::mouseClick(apply, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(tags_observed.has_value(), 5'000);
    QCOMPARE(tags_observed->committed_source_count(), 1U);
    QVERIFY(window_test::closeTaggerAfterWrite(guard));
    const auto tagged = metadata::read_local_metadata(raw_path);
    QVERIFY(tagged.has_value());
    QCOMPARE(tagged->document.effective_values("title"),
             (std::vector<std::string>{"Identified title"}));
    QCOMPARE(tagged->document.effective_values("MUSICBRAINZ_ALBUMID"),
             (std::vector<std::string>{release_id.toStdString()}));
    const auto tagged_artwork = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(tagged_artwork.has_value());
    QCOMPARE(tagged_artwork->items.size(), inventory->items.size());
    for (std::size_t index = 0U; index < inventory->items.size(); ++index) {
        QCOMPARE(tagged_artwork->items[index].content_fingerprint,
                 inventory->items[index].content_fingerprint);
    }

    // A later session fetching again replaces the existing front cover
    // instead of stacking a second front picture.
    auto second_read = metadata::read_local_metadata(raw_path);
    QVERIFY(second_read.has_value());
    const MetadataPropertiesSource second_source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = second_read->source_revision,
                .baseline = second_read->document,
            },
        .track_label = QStringLiteral("Cover fetch fixture"),
    };
    observed.reset();
    auto* second_properties = new MetadataPropertiesDialog(
        1U,
        [second_source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{second_source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    second_properties->setArtworkMutationServices(
        [database_path] {
            return ArtworkWritePlanApplier{
                [database_path](const metadata::ArtworkWritePlan& plan,
                                const operations::ArtworkApplyProgressCallback& progress,
                                const core::CancellationToken& cancellation)
                    -> core::Result<operations::ArtworkApplyResult> {
                    auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
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
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                source_cancellation);
                        },
                        progress, cancellation,
                        operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [&observed](const operations::ArtworkApplyResult& result) { observed = result; });
    second_properties->show();
    QTabWidget* second_sections = nullptr;
    QTRY_VERIFY((second_sections = second_properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    second_sections->setCurrentIndex(1);
    auto* second_items =
        second_properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* second_fetch = second_properties->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-artwork-fetch-cover"));
    QVERIFY(second_items != nullptr);
    QVERIFY(second_fetch != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(second_items->model()->rowCount(), 2, 5'000);
    QTRY_VERIFY(second_fetch->isEnabled());
    QTest::mouseClick(second_fetch, Qt::LeftButton);
    QVERIFY(chooseInCoverPicker(second_properties, QStringLiteral("Cover Art Archive"),
                                QStringLiteral("Front")));
    auto* second_save =
        second_properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    QVERIFY(second_save != nullptr);
    QTRY_VERIFY(second_save->isEnabled());
    auto* second_pending =
        second_properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(second_pending != nullptr);
    QCOMPARE(second_pending->model()->rowCount(), 2);
    QTRY_VERIFY(
        !second_pending->model()->index(1, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    QVERIFY(jpeg_close(second_pending->model()
                           ->index(1, 5)
                           .data(Qt::DecorationRole)
                           .value<QImage>()
                           .pixelColor(0, 0),
                       QColor{Qt::darkMagenta}));
    auto* second_discard = second_properties->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-artwork-discard"));
    QVERIFY(second_discard != nullptr);
    second_discard->click();
    QCOMPARE(second_pending->model()->rowCount(), 0);
    QTRY_VERIFY(second_fetch->isEnabled());
    second_fetch->click();
    QVERIFY(chooseInCoverPicker(second_properties, QStringLiteral("Cover Art Archive"),
                                QStringLiteral("Front")));
    QTRY_VERIFY(second_save->isEnabled());
    QTRY_VERIFY(
        !second_pending->model()->index(1, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    QVERIFY(jpeg_close(second_pending->model()
                           ->index(1, 5)
                           .data(Qt::DecorationRole)
                           .value<QImage>()
                           .pixelColor(0, 0),
                       QColor{Qt::darkMagenta}));
    QVERIFY(!observed.has_value());
    QTest::mouseClick(second_save, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(observed.has_value(), 10'000);
    const auto second_issue = observed->sources.front().issue
                                  ? QString::fromStdString(observed->sources.front().issue->message)
                                  : QStringLiteral("no per-source issue");
    QVERIFY2(observed->committed_source_count() == 1U, qPrintable(second_issue));
    QTRY_COMPARE_WITH_TIMEOUT(second_items->model()->rowCount(), 2, 5'000);
    const auto replaced = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(replaced.has_value());
    QCOMPARE(replaced->items.size(), 2U);
    const auto front_count = std::ranges::count_if(replaced->items, [](const auto& item) {
        return item.role == metadata::ArtworkRole::front;
    });
    QCOMPARE(front_count, 1);
    const auto new_front = std::ranges::find_if(replaced->items, [](const auto& item) {
        return item.role == metadata::ArtworkRole::front;
    });
    // The second image, as Melody writes covers (ADR-0317): a 24 px JPEG.
    QCOMPARE(new_front->mime_type, std::string{"image/jpeg"});
    QCOMPARE(new_front->width, std::optional<std::uint32_t>{24U});

    QPointer second_guard{second_properties};
    second_properties->close();
    QTRY_VERIFY(second_guard.isNull());
}

// With covers embedded in the files, a cover.jpg beside them is not the
// files' cover: the editor does not show it as theirs, and Fetch offers it.
void BenchMainWindowTest::aCoverBesideTheFilesIsOfferedNotShown() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("track.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), media_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("external-blue-jpeg.b64"),
                                      media.filePath(QStringLiteral("cover.jpg"))));
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
        .track_label = QStringLiteral("Beside fixture"),
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, {});
    // Nothing is saved here; changes only need to be possible.
    properties->setArtworkMutationServices(
        [] {
            return ArtworkWritePlanApplier{
                [](const metadata::ArtworkWritePlan&,
                   const operations::ArtworkApplyProgressCallback&,
                   const core::CancellationToken&) -> core::Result<operations::ArtworkApplyResult> {
                    return std::unexpected(core::Error{.code = core::ErrorCode::invariant,
                                                       .message = "not in this test",
                                                       .context = {}});
                }};
        },
        {});
    properties->show();
    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    MetadataArtworkSection* section = nullptr;
    QTRY_VERIFY((section = properties->findChild<MetadataArtworkSection*>()) != nullptr);
    // Watched before the Artwork page is opened, which is when it reads.
    QSignalSpy shown{section, &MetadataArtworkSection::frontCoverChanged};
    sections->setCurrentIndex(1);
    auto* fetch =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-fetch-cover"));
    QVERIFY(fetch != nullptr);
    // No release to ask the archive about: the image beside the file is
    // what makes Fetch worth pressing.
    QTRY_VERIFY_WITH_TIMEOUT(fetch->isEnabled(), 5'000);
    QVERIFY(!shown.isEmpty());
    QVERIFY2(shown.back().front().value<QImage>().isNull(),
             "the files carry no cover, so none is shown as theirs");

    QTest::mouseClick(fetch, Qt::LeftButton);
    QVERIFY(chooseInCoverPicker(properties, QStringLiteral("cover.jpg"), QStringLiteral("Front")));
    auto* pending =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(pending != nullptr);
    QTRY_COMPARE(pending->model()->rowCount(), 1);
    // The staged front is that image: blue, as the fixture is.
    QTRY_VERIFY(!pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    const auto staged = pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>();
    const auto centre = staged.pixelColor(staged.width() / 2, staged.height() / 2);
    QVERIFY2(centre.blue() > 150 && centre.red() < 100,
             qPrintable(QStringLiteral("staged image centre %1").arg(centre.name())));
    section->discardPendingChanges();
    properties->close();
}

void BenchMainWindowTest::artworkArchivePickerAddsChosenImageWithItsRole() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto media_path = media.filePath(QStringLiteral("picker.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("art-tone-flac.b64"), media_path));
    const auto encoded = QFile::encodeName(media_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto release_id = QStringLiteral("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    read->document.fields.push_back(metadata::MetadataField{
        .canonical_name = metadata::canonicalize_field_name("MUSICBRAINZ_ALBUMID"),
        .native_name = "MUSICBRAINZ_ALBUMID",
        .values = {release_id.toStdString()},
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
        .track_label = QStringLiteral("Picker fixture"),
    };

    QImage back_cover{16, 16, QImage::Format_ARGB32};
    back_cover.fill(Qt::darkGreen);
    QByteArray back_bytes;
    QBuffer back_buffer{&back_bytes};
    QVERIFY(back_buffer.open(QIODevice::WriteOnly));
    QVERIFY(back_cover.save(&back_buffer, "PNG"));
    back_buffer.close();
    static constexpr auto picker_listing = R"json({
      "images": [
        {"id": 1, "front": true, "approved": true, "types": ["Front"],
         "image":
           "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/1.png"},
        {"id": 2, "front": false, "back": true, "approved": true, "types": ["Back"],
         "comment": "tray insert",
         "image":
           "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/2.png",
         "thumbnails": {"250":
           "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/2-250.jpg"}}
      ]
    })json";
    const MusicBrainzLookupService service{
        .fetch =
            [back_bytes, release_id](const QString& url,
                                     std::function<void(core::Result<QByteArray>)> completion) {
                if (url == QStringLiteral("https://coverartarchive.org/release/") + release_id) {
                    completion(QByteArray{picker_listing});
                    return;
                }
                if (url.endsWith(QStringLiteral("/2.png")) ||
                    url.endsWith(QStringLiteral("/2-250.jpg"))) {
                    completion(back_bytes);
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "unexpected url",
                    .context = {},
                }));
            },
        .fingerprint = {},
        .acoustid_lookup = {},
    };

    const auto database_path =
        std::filesystem::path{media.filePath(QStringLiteral("picker.sqlite3")).toStdString()};
    std::optional<operations::ArtworkApplyResult> observed;
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, {}, {}, {}, {}, nullptr, {}, service);
    properties->setArtworkMutationServices(
        [database_path] {
            return ArtworkWritePlanApplier{
                [database_path](const metadata::ArtworkWritePlan& plan,
                                const operations::ArtworkApplyProgressCallback& progress,
                                const core::CancellationToken& cancellation)
                    -> core::Result<operations::ArtworkApplyResult> {
                    auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
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
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                source_cancellation);
                        },
                        progress, cancellation,
                        operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [&observed](const operations::ArtworkApplyResult& result) { observed = result; });
    properties->show();

    QTabWidget* sections = nullptr;
    QTRY_VERIFY((sections = properties->findChild<QTabWidget*>(
                     QStringLiteral("bench-metadata-sections"))) != nullptr);
    sections->setCurrentIndex(1);
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* covers =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-fetch-cover"));
    QVERIFY(items != nullptr);
    QVERIFY(covers != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 1, 5'000);
    QTRY_VERIFY(covers->isEnabled());
    QTest::mouseClick(covers, Qt::LeftButton);

    // The picker lists every archive image with its type and comment.
    QDialog* picker = nullptr;
    QTRY_VERIFY((picker = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-artwork-picker"))) != nullptr);
    auto* list =
        picker->findChild<QTreeWidget*>(QStringLiteral("bench-metadata-artwork-picker-list"));
    auto* use =
        picker->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-picker-use"));
    QVERIFY(list != nullptr);
    QVERIFY(use != nullptr);
    QTRY_COMPARE(list->topLevelItemCount(), 2);
    QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("Front"));
    QCOMPARE(list->topLevelItem(1)->text(2), QStringLiteral("Back"));
    QCOMPARE(list->topLevelItem(1)->text(3), QStringLiteral("tray insert"));
    // The scripted service already answered the thumbnail request.
    QTRY_VERIFY(!list->topLevelItem(1)->icon(0).isNull());

    list->setCurrentItem(list->topLevelItem(1));
    QTest::mouseClick(use, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-artwork-picker")) ==
                nullptr);
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
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 2, 5'000);
    const auto inventory = metadata::read_local_artwork_inventory(raw_path);
    QVERIFY(inventory.has_value());
    QCOMPARE(inventory->items.size(), 2U);
    const auto added = std::ranges::find_if(inventory->items, [](const auto& item) {
        return item.role == metadata::ArtworkRole::back;
    });
    QVERIFY(added != inventory->items.end());
    // ADR-0317: as Melody writes covers, a JPEG.
    QCOMPARE(added->mime_type, std::string{"image/jpeg"});
    QVERIFY(added->byte_size > 0U);

    QPointer guard{properties};
    properties->close();
    QTRY_VERIFY(guard.isNull());
}

void BenchMainWindowTest::automaticScriptsStageOnOpen() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = "/music/open-stage.flac",
                .source_revision = std::nullopt,
                .baseline =
                    metadata::MetadataDocument{
                        .fields = {field("TITLE", {"Song"}), field("ARTIST", {"Band"}),
                                   field("TOTALTRACKS", {"9"})},
                        .unsupported_native_objects = {},
                    },
            },
        .track_label = QStringLiteral("Open staging fixture"),
    };
    std::vector<persistence::SavedMetadataTransformationChain> saved_chains{
        persistence::SavedMetadataTransformationChain{
            .id = core::StableId::random(),
            .chain =
                metadata::MetadataTransformationChain{
                    .schema_version = 1U,
                    .name = "Library cleanup",
                    .actions = {metadata::MetadataRemoveFieldAction{
                        .target_field = "totaltracks",
                        .match_mode = metadata::MetadataFieldMatchMode::logical,
                    }},
                },
            .automatic = true,
        }};
    MetadataTransformationStore transformation_store{
        .load =
            [&saved_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(saved_chains, {});
            },
        .save = [](persistence::SavedMetadataTransformationChain,
                   MetadataTransformationStore::Completion completion) { completion({}); },
        .remove = [](core::StableId,
                     MetadataTransformationStore::Completion completion) { completion({}); },
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store);
    properties->show();

    // Picard-style: the automatic script's edits appear as ordinary colored
    // drafts the moment the files load — the grid is the write, no hidden
    // apply-time pass remains.
    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->fileListView()) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(undo != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(grid_model->patches().patch_count(), std::size_t{1U}, 5'000);
    const auto totals_column = grid_model->fieldColumn(QStringLiteral("Total Tracks"));
    QVERIFY(totals_column.has_value());
    const auto* patch =
        grid_model->patches().patch(0U, static_cast<std::size_t>(*totals_column) - 1U);
    QVERIFY(patch != nullptr);
    QCOMPARE(patch->kind, metadata::StagedMetadataPatchKind::remove_field);
    const auto totals_index = grid_model->index(0, *totals_column);
    QCOMPARE(totals_index.data(metadata_cell_staged_source_role).toString(),
             QStringLiteral("Library cleanup · step 1"));
    QVERIFY(totals_index.data(Qt::FontRole).value<QFont>().italic());

    // The staging is one ordinary undoable transaction the user can reject.
    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{0U});

    QPointer guard{properties};
    properties->close();
    QTRY_VERIFY(guard.isNull());
}

} // namespace trackknife::bench
