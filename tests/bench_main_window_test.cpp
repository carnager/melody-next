// SPDX-License-Identifier: GPL-3.0-only

#include "bench/animated_panel_dock.hpp"
#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/catalogue_source.hpp"
#include "bench/convert_dialog.hpp"
#include "bench/cover_thumbnail.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/dynamic_playlist_service.hpp"
#include "bench/lastfm_service.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/local_list_model.hpp"
#include "bench/metadata_grid_model.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "bench/musicbrainz_track_match_widget.hpp"
#include "bench/playback_tab_widget.hpp"
#include "bench/playlist_transfer_bar.hpp"
#include "bench/replaygain_dialog.hpp"
#include "bench/search_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/track_list_find_bar.hpp"
#include "test_engine.hpp"

#include <signal.h>

#include "trackknife/audio/local_audition.hpp"
#include "trackknife/convert/convert.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/persistence/operation_journal.hpp"
#include "uicommon/command_palette.hpp"
#include "uicommon/line_slider.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/panel_layout.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"
#include <QClipboard>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHostAddress>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLocale>
#include <QMimeData>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QSysInfo>
#include <QTcpServer>

#include <QAbstractItemModelTester>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDataStream>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringListModel>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::bench {

namespace {
// ADR-0221: playback anchors are entry identities now, so the white-box
// assertions below resolve them to a row instead of reading .row() off a
// persistent index. Same question, asked explicitly. Templated because
// BenchMainWindow::ListTab is private to the window and its friend test class.
template <typename Tab> int row_of(const Tab* tab, const core::StableId& entry) {
    return tab == nullptr ? -1 : tab->model->rowOfEntry(entry, -1);
}
} // namespace

namespace {

constexpr std::uint32_t wave_sample_rate = 44'100U;
// The live progression test reaches the user's default PipeWire sink. Keep its
// non-silent probe roughly 32 dB below the previous level so it is detectable
// by the decoder without being an intrusive test sound.
constexpr std::int32_t test_wave_peak = 256;

void append_u16(std::vector<unsigned char>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<unsigned char>& bytes, const std::uint32_t value) {
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        bytes.push_back(static_cast<unsigned char>((value >> (byte * 8U)) & 0xFFU));
    }
}

// Writes a real 16-bit mono PCM WAV with a non-silent deterministic pattern so
// playback has actual audio to drain.
void write_wave(const QString& path, const std::uint32_t frames) {
    std::vector<unsigned char> bytes;
    bytes.reserve(44U + frames * 2U);
    const auto data_bytes = frames * 2U;
    for (const char character : {'R', 'I', 'F', 'F'}) {
        bytes.push_back(static_cast<unsigned char>(character));
    }
    append_u32(bytes, 36U + data_bytes);
    for (const char character : {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}) {
        bytes.push_back(static_cast<unsigned char>(character));
    }
    append_u32(bytes, 16U);
    append_u16(bytes, 1U);
    append_u16(bytes, 1U);
    append_u32(bytes, wave_sample_rate);
    append_u32(bytes, wave_sample_rate * 2U);
    append_u16(bytes, 2U);
    append_u16(bytes, 16U);
    for (const char character : {'d', 'a', 't', 'a'}) {
        bytes.push_back(static_cast<unsigned char>(character));
    }
    append_u32(bytes, data_bytes);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const auto sample = static_cast<std::int32_t>((frame * 37U) % static_cast<std::uint32_t>(
                                                                          2 * test_wave_peak + 1)) -
                            test_wave_peak;
        append_u16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(sample)));
    }
    std::ofstream output{QFile::encodeName(path).toStdString(), std::ios::binary};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
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

} // namespace

class BenchMainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void cleanup();
    void transportUsesStackedNowPlayingAndCompactDeviceButton();
    void activePlaybackTabRemainsMarkedWhileBrowsing();
    void activeTabAccentSurvivesThemeTextColor();
    void followPlaybackAndJumpRespectBrowsing();
    void commandPaletteFindsAndRunsRegisteredActions();
    void commandPaletteTracksAvailabilityAndLifetime();
    void localListeningColumnsLoadRefreshAndRespectAuthority();
    void localListeningCacheIsBoundedAndRejectsStaleResults();
    void shortcutSettingsValidateSaveAndCancel();
    void playbackBufferProfilesPersistAndExposeDiagnostics();
    void statusBarSummarizesTrackSelection();
    void committedMetadataRefreshesDuplicatesAndPreservesCueOverlay();
    void metadataReadyPlanAppliesAndRefreshesHistory();
    void metadataApplyCancellationPreservesDraftForFreshPreview();
    void metadataDialogLayoutsPersistAsynchronously();
    void metadataFieldLayoutsLoadFilterAndPersist();
    void metadataGridDisplaysUnicodePaths();
    void metadataGridReusesExactNativeFieldWithoutInvalidIndexes();
    void metadataTransformationChainPreviewsAndStagesOneUndo();
    void metadataCapturePatternSavesReloadsAndStagesAllFields();
    void preparationSidePanelEditsReusableOutputProfiles();
    void pathOnlyPreparationUsesActualTagsAndAppliesReviewedPlan();
    void combinedTagAndRenameReviewReachesPreparationApply();
    void metadataSuggestionsStageSelectionConsistency();
    void musicBrainzMatchingHandlesUnequalCounts_data();
    void musicBrainzMatchingHandlesUnequalCounts();
    void musicBrainzIdentifyStagesChosenVersion_data();
    void musicBrainzIdentifyStagesChosenVersion();
    void musicBrainzFingerprintScanRanksAndStages();
    void replayGainScanStagesMeasuredGainsAsDrafts();
    void replayGainScanUsesTruePeakWhenOptedIn();
    void replayGainScanStagesR128ForOpusTags();
    void propertiesShowTechnicalSummary();
    void savedSearchesCanBeManagedAndReopened();
    void searchDialogFiltersTabAndOpensResults();
    void searchResultTabLoadsCovers();
    void searchDialogProbesMissingTechnicalsOnDemand();
    void searchPresetsAreGroupedAndPrompt();
    void musicBrainzStagesFromCachedSearchMetadata();
    void contextReplayGainScansAndApplies_data();
    void contextReplayGainScansAndApplies();
    void loudnessSidecarProjectsOntoProbedRows();
    void desktopNotificationsNotifyBackgroundTrackChanges();
    void upNextPreservesNormalPlayback_data();
    void upNextPreservesNormalPlayback();
    void aRemoteEnginePlaysItsOwnTabs();
    void aRemoteTabGetsTagsAndCoversFromItsEngine();
    void aRestoredRemoteTabGetsItsCovers();
    void theDeviceMenuChoosesAnOutputAgent();
    void upNextEditingAndPersistence();
    void upNextPanelAnimationAndSettings();
    void upNextMultiSelectionEdits();
    void muteRestoresLocalVolumeAcrossBrowsing();
    void lastFmSettingsAndTrackActions();
    void dynamicPlaylistsShareRulesAndRecommendationMatching();
    void dynamicPlaylistCatalogAndEditor();
    void dynamicPlaylistRetainsChangesDuringRefresh();
    void dynamicResultSelectionSurvivesRefresh();
    void dynamicResultActionsUseTheirOwnSources();
    void currentTabHistoryPreservesOccurrences();
    void lastFmRefreshSelectsFreshTracksFromLargerPool();
    void replayGainScanPreservesLogicalSources_data();
    void replayGainScanPreservesLogicalSources();
    void convertDialogPlansAndConvertsSelection();
    void convertDialogAppliesPermanentReplayGain();
    void propertiesFileListLivesInTheTaggerWindow();
    void selectionActionsFollowTheActiveTab();
    void closingATabReturnsToThePreviousOne();
    void localArtworkSurvivesInvalidation();
    void folderBookmarksRevealTreePaths();
    void folderBookmarksMigrateFromLibraryRoots();
    void artworkFetchesCoverArtFromArchiveAndAddsFront();
    void metadataRetrySkipsSavedFiles_data();
    void metadataRetrySkipsSavedFiles();
    void coverPolicyRoundTrip();
    void playbackSettingsApplyLiveAndCancel();
    void librarySettingsManageFoldersWithoutScanning();
    void metadataServiceSettingsAndCompactPages();
    void coverThumbnailAppliesPolicy_data();
    void coverThumbnailAppliesPolicy();
    void metadataApplyCombinesTagsAndArtwork_data();
    void metadataApplyCombinesTagsAndArtwork();
    void artworkArchivePickerAddsChosenImageWithItsRole();
    void automaticScriptsStageOnOpen();
    void metadataStartupPresentsReconciliation();
    void filePublicationStartupPresentsReconciliation();
    void combinedPublicationStartupRecoversMetadataAndPath();
    void folderDiscoveryAdmitsWave64();
    void contextMenusTargetSelectionsListsAndFolders();
    void contextTransfersCreateTabs();
    void tabBarDropsTransferLocalRows();
    void panelLayoutPersistsAndPreservesFutureState();
    void trackViewLayoutMatchesGroupedQueueAndPersists();
    void localReorderPreservesVisibleRowGeometry();
    void localListUndoRestoresOccurrencesAndFreshMetadata();
    void localListHistoryBranchesAndBounds();
    void crossTabMoveUndoIsOneTransaction();
    void localListUndoActionsRespectAuthorityAndTextEditing();
    void localListOrderingActionsRespectAuthorityAndPersist();
    void portablePlaylistImportsPreserveAuthorityAndPersist();
    void trackListFindActionsFollowActiveTab();
    void noncontiguousLocalReorderPreservesOccurrences();
    void persistsPinnedDuplicatedAndDirtyTabs();
    void richMetadataValuesAndIdentitiesSurviveListRestart();
    void metadataPropertiesFileSelectionDrivesIndividualAndBulkEdits();
    void metadataFieldReviewPreservesDraftAndSelectionScope();
    void metadataPropertiesArtworkSectionShowsProvenanceAndCapabilities();
    void metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes();
    void cueSheetsExpandIntoPersistentSegmentRows();
    void containerChaptersExpandIntoPersistentSegmentRows();
    void codecNativeSubsongsExpandAndPersistDecoderSelections();
    void autoAdvancesOncePerFinishedTrack();
    void localPlaybackModesPersistAndStayLocal();
    void localPlaybackModesAdvance_data();
    void localPlaybackModesAdvance();
    void localTrackRatingsPersistByContentIdentity();

  private:
    QTemporaryDir settings_directory_;
    testing::TestEngine engine_;
};

void BenchMainWindowTest::initTestCase() {
    QVERIFY(settings_directory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("trackknife-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory_.path());
    QStandardPaths::setTestModeEnabled(true);
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
}

// ADR-0226: every window runs against an engine, as the application does.
void BenchMainWindowTest::init() { QVERIFY2(engine_.start(), engine_.log().constData()); }

void BenchMainWindowTest::cleanup() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    engine_.stop();
    QSettings settings;
    settings.clear();
    settings.sync();
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
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

void BenchMainWindowTest::localListeningColumnsLoadRefreshAndRespectAuthority() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* tab = window.currentListTab();
    QVERIFY(tab);
    LocalTrackRow row;
    row.raw_path = std::string{"/history/raw-"} + char(0xff) + ".flac";
    row.source_revision = core::LocalSourceRevision{.device = 10, .inode = 11};
    row.title = "History fixture";
    row.probed = true;
    auto unknown = row;
    unknown.source_revision.reset();
    auto logical = row;
    logical.segment = formats::SampleRange{0, 48000};
    tab->model->replaceRows({row, row, logical, unknown});
    auto* model = tab->model;
    const auto plays = [&](int n) {
        return model->index(n, local_play_count_column).data().toString();
    };
    QVERIFY(tab->view->isColumnHidden(local_play_count_column));
    QVERIFY(tab->view->isColumnHidden(local_last_played_column));
    const auto hidden_layout = window.captureTrackViewLayout(*tab);
    QCOMPARE(hidden_layout.columns.back().id, QStringLiteral("last-played"));
    QCOMPARE(hidden_layout.columns.back().width, 170);
    QVERIFY(model->listening_cache_.isEmpty());
    window.setTrackColumnVisible(QStringLiteral("play-count"), true);
    window.setTrackColumnVisible(QStringLiteral("last-played"), true);
    QVERIFY(!tab->view->isColumnHidden(local_play_count_column));
    QCOMPARE(tab->view->columnWidth(local_last_played_column), 170);
    QTRY_COMPARE(plays(0), QStringLiteral("0"));
    QTRY_COMPARE(model->index(0, local_last_played_column).data().toString(),
                 QStringLiteral("Never"));
    QCOMPARE(plays(3), QStringLiteral("—"));
    QVERIFY(model->index(0, local_play_count_column)
                .data(Qt::TextAlignmentRole)
                .value<Qt::Alignment>()
                .testFlag(Qt::AlignRight));
    persistence::ListItem source;
    source.source = persistence::ListSource::local;
    source.source_reference = row.raw_path;
    source.source_revision = row.source_revision;
    const qint64 played_at = 1700000000000;
    bool saved = false;
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    tab->view->selectRow(1);
    window.persistence_->recordLocalListen(source, core::StableId::random(), played_at,
                                           [&](const QString& error) {
                                               QVERIFY2(error.isEmpty(), qPrintable(error));
                                               saved = true;
                                           });
    QTRY_VERIFY(saved);
    QTRY_COMPARE(plays(0), QStringLiteral("1"));
    QTRY_COMPARE(plays(1), QStringLiteral("1"));
    QTRY_COMPARE(plays(2), QStringLiteral("0"));
    QCOMPARE(model->index(0, local_last_played_column).data().toString(),
             QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(played_at), QLocale::ShortFormat));
    QCOMPARE(resets.count(), 0);
    QCOMPARE(tab->view->selectionModel()->selectedRows().front().row(), 1);
    const auto restored_layout = ui::deserializeTrackViewLayout(
        ui::serializeTrackViewLayout(window.captureTrackViewLayout(*tab)), trackColumnIds());
    QVERIFY(restored_layout);
    window.applyTrackViewLayout(*tab, *restored_layout);
    QVERIFY(!tab->view->isColumnHidden(local_last_played_column));
    window.copyTrackViewLayoutToAllTabs();
    QVERIFY(window.track_column_actions_.value(QStringLiteral("play-count"))->isVisible());
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(directory + QStringLiteral("/listening-history.png")));
    }
}

void BenchMainWindowTest::localListeningCacheIsBoundedAndRejectsStaleResults() {
    QTemporaryDir temporary;
    const auto database = std::filesystem::path{temporary.path().toStdString()} / "history.sqlite";
    ui::ListPersistenceService service(database);
    bool ready = false;
    service.initialize([&](auto, const QString& error) {
        QVERIFY2(error.isEmpty(), qPrintable(error));
        ready = true;
    });
    QTRY_VERIFY(ready);
    LocalTrackRow row;
    row.raw_path = "/history/a.flac";
    row.source_revision = core::LocalSourceRevision{.device = 10, .inode = 20};
    LocalListModel model;
    model.setListeningHistoryService(&service);
    model.replaceRows(std::vector<LocalTrackRow>(10000, row));
    const auto value = [&](int n) {
        return model.index(n, local_play_count_column).data().toString();
    };
    for (int n = 0; n < 10000; ++n)
        static_cast<void>(value(n));
    QVERIFY(model.listening_pending_.size() <= 64);
    QVERIFY(model.listening_cache_.size() <= 512);
    model.dispatchListeningHistory();
    QVERIFY(model.listening_busy_);
    // Change rows while the worker owns the detached request; its reply must not
    // populate a different source that happens to occupy the same row number.
    row.source_revision.reset();
    model.replaceRows({row});
    QTRY_VERIFY(!model.listening_busy_);
    QVERIFY(model.listening_cache_.isEmpty());
    QCOMPARE(value(0), QStringLiteral("—"));
    row.source_revision = core::LocalSourceRevision{.device = 10, .inode = 21};
    model.replaceRows({row});
    QTRY_COMPARE(value(0), QStringLiteral("0"));
    // Simulate a tall viewport repainting only the invalidated region. Rows
    // beyond the first admission batch must not remain stuck on Loading.
    model.replaceRows(std::vector<LocalTrackRow>(100, row));
    const auto repaint =
        QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                         [&](const QModelIndex& first, const QModelIndex& last) {
                             for (int n = first.row(); n <= std::min(last.row(), 99); ++n)
                                 static_cast<void>(value(n));
                         });
    for (int n = 0; n < 100; ++n)
        static_cast<void>(value(n));
    QTRY_VERIFY(model.listening_cache_.object(99) && model.listening_cache_.object(99)->loaded);
    QObject::disconnect(repaint);
    model.replaceRows({row});
    // An unavailable database is not an unplayed track.
    ui::ListPersistenceService unopened(database);
    model.setListeningHistoryService(&unopened);
    QTRY_COMPARE(value(0), QStringLiteral("—"));
    QVERIFY(model.index(0, local_play_count_column)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("not initialized")));
}

void BenchMainWindowTest::dynamicPlaylistRetainsChangesDuringRefresh() {
    using Service = DynamicPlaylistService;
    std::vector<Service::Completion> pending;
    DynamicPlaylistDialog dialog(
        QStringLiteral("local"), QStringLiteral("Local library"),
        [&](query::CompiledTkq, core::CancellationToken, Service::Completion done) {
            pending.push_back(std::move(done));
        });
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
        DynamicPlaylistDialog dialog(QStringLiteral("local"), QStringLiteral("Test"),
                                     [&](query::CompiledTkq, core::CancellationToken,
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
    SearchDialog dialog(catalogues,
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

void BenchMainWindowTest::transportUsesStackedNowPlayingAndCompactDeviceButton() {
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
    QCOMPARE(window.menuBar()->actions().size(), 4);
    QCOMPARE(device->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QVERIFY(!device->icon().isNull());
    QVERIFY(device->menu() != nullptr);
    QCOMPARE(device->menu()->objectName(), QStringLiteral("bench-device-menu"));
    QVERIFY(!device->menu()->actions().empty());
    QCOMPARE(device->menu()->actions().front()->text(), QStringLiteral("System default"));
    QVERIFY(window.property("trackknife-player-output-available").isValid());
    QVERIFY(window.property("trackknife-player-output-suspended").isValid());
    QVERIFY(window.property("trackknife-player-default-output").isValid());
    QCOMPARE(transport->findChildren<QToolButton*>(QString{}, Qt::FindDirectChildrenOnly).size(),
             5);

    const QRect label_geometry{now_playing->mapTo(&window, QPoint{}), now_playing->size()};
    const QRect context_geometry{now_playing_context->mapTo(&window, QPoint{}),
                                 now_playing_context->size()};
    const QRect seek_geometry{seek->mapTo(&window, QPoint{}), seek->size()};
    const QRect volume_geometry{volume->mapTo(&window, QPoint{}), volume->size()};
    const QRect device_geometry{device->mapTo(&window, QPoint{}), device->size()};
    QVERIFY(label_geometry.bottom() <= context_geometry.top());
    QVERIFY(context_geometry.bottom() <= seek_geometry.top());
    QCOMPARE(label_geometry.center().x(), seek_geometry.center().x());
    QCOMPARE(context_geometry.center().x(), seek_geometry.center().x());
    QCOMPARE(volume_geometry.center().y(), seek_geometry.center().y());
    QCOMPARE(device_geometry.center().y(), seek_geometry.center().y());
    QVERIFY(header->height() <= 72);
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
    const auto image = bar.grab().toImage();
    QCOMPARE(image.pixelColor(bar.tabRect(1).center().x(), bar.tabRect(1).bottom() - 1), accent);
    QVERIFY(image.pixelColor(bar.tabRect(0).center().x(), bar.tabRect(0).bottom() - 1) != accent);
    bar.setTabData(0, false);
    bar.setTabIcon(0, QIcon{});
    QCOMPARE(colored_pixels(), 0);
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
    QVERIFY(tabs->tabBar()->tabData(index).toBool());
    QVERIFY(!tabs->tabIcon(index).isNull());
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
    QCOMPARE(status->toolTip(), QString::fromStdString(core::escape_raw_path(first_raw)));

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
    properties_action->trigger();

    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
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

    // ADR-0186: the footer Actions menu is the apply-options surface,
    // proxying the hidden state controls and the preset selectors.
    auto* actions_button =
        properties->findChild<QToolButton*>(QStringLiteral("bench-metadata-actions"));
    QVERIFY(actions_button != nullptr);
    auto* actions_menu = actions_button->menu();
    QVERIFY(actions_menu != nullptr);
    emit actions_menu->aboutToShow();
    auto* save_tags_action =
        properties->findChild<QAction*>(QStringLiteral("action-metadata-save-tags"));
    auto* rename_action =
        properties->findChild<QAction*>(QStringLiteral("action-metadata-rename-files"));
    auto* manage_layouts_action =
        properties->findChild<QAction*>(QStringLiteral("action-metadata-manage-layouts"));
    QVERIFY(save_tags_action != nullptr && rename_action != nullptr &&
            manage_layouts_action != nullptr);
    QVERIFY(save_tags_action->isChecked());
    QVERIFY(!rename_action->isEnabled());
    QSignalSpy settings_requests{properties, &MetadataPropertiesDialog::openSettingsRequested};
    manage_layouts_action->trigger();
    QCOMPARE(settings_requests.count(), 1);
    save_tags_action->setChecked(false);
    auto* save_tags =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
    QVERIFY(save_tags != nullptr);
    QVERIFY(!save_tags->isChecked());
    save_tags_action->setChecked(true);
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
    QCOMPARE(destination_combo->count(), 1);
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
    auto* layout_list = settings.findChild<QComboBox*>(QStringLiteral("bench-output-layout-list"));
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
        settings.findChild<QComboBox*>(QStringLiteral("bench-destination-list"));
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
    QTRY_COMPARE(layout_list->count(), 1);
    QCOMPARE(layout_name->text(), QStringLiteral("Albums"));
    QCOMPARE(layout_basename->text(), QStringLiteral("%tracknumber% - %title%"));
    QCOMPARE(layout_sanitization->currentData().toString(), QStringLiteral("linux"));
    QTRY_COMPARE(destination_list->count(), 1);

    auto* profile_sections =
        settings.findChild<QTabWidget*>(QStringLiteral("bench-output-profile-sections"));
    QVERIFY(profile_sections);
    QCOMPARE(profile_sections->count(), 2);
    QVERIFY(layout_directory->width() > 450);
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(settings.grab().save(directory + QStringLiteral("/settings-naming.png")));
    }
    QSignalSpy profile_changes{&settings, &SettingsDialog::outputProfilesChanged};
    QTest::mouseClick(layout_new, Qt::LeftButton);
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

    layout_list->setCurrentIndex(0);
    QCOMPARE(layout_name->text(), layout_list->currentText());
    QCOMPARE(layout_basename->text(), QStringLiteral("%tracknumber% - %title%"));
    layout_list->setCurrentIndex(1);
    QCOMPARE(layout_name->text(), QStringLiteral("Artist folders"));
    QCOMPARE(layout_basename->text(), QStringLiteral("%title%"));
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
    QTRY_COMPARE(layout_combo->count(), 2);

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
                        .notes = {},
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
    QTRY_VERIFY(closed_guard.isNull() || !closed_guard->isVisible());
}

void BenchMainWindowTest::combinedTagAndRenameReviewReachesPreparationApply() {
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
    };
    const MetadataTransformationStore transformation_store{
        .load = [](MetadataTransformationStore::LoadCompletion completion) { completion({}, {}); },
        .save = {},
        .remove = {},
    };
    bool combined_applied = false;
    std::string reviewed_target;
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store, output_store,
        [&combined_applied, &reviewed_target] {
            return FilePublicationPlanApplier{
                [&combined_applied,
                 &reviewed_target](const operations::PreparationPlan& plan,
                                   const operations::FilePublicationApplyProgressCallback& progress,
                                   const core::CancellationToken&)
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
                    combined_applied = true;
                    const auto& checked = plan.path_preflight->sources.front();
                    reviewed_target = checked.planned.target_raw_path;
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
        {});
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
    // 19 step kinds under 4 unselectable group headers; kinds are found by
    // name because the row index no longer matches the action kind.
    QCOMPARE(kind->count(), 23);
    for (const auto& kind_name : {QStringLiteral("Capitalize first character"),
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

void BenchMainWindowTest::musicBrainzIdentifyStagesChosenVersion_data() {
    QTest::addColumn<bool>("untagged");
    QTest::newRow("tagged") << false;
    QTest::newRow("untagged-manual-mapping") << true;
}

void BenchMainWindowTest::musicBrainzIdentifyStagesChosenVersion() {
    QFETCH(bool, untagged);
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto make_source = [&field, untagged](const QString& label, const QString& title,
                                                const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + label.toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = untagged ? std::vector<metadata::MetadataField>{}
                                               : std::vector{field("ALBUM", {"Alpha"}),
                                                             field("ARTIST", {"Band"}),
                                                             field("TITLE", {title.toStdString()}),
                                                             field("TRACKNUMBER",
                                                                   {track_number.toStdString()})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = label,
        };
    };
    const std::vector sources{
        make_source(untagged ? QStringLiteral("10-second") : QStringLiteral("one"),
                    QStringLiteral("One"), QStringLiteral("1")),
        make_source(untagged ? QStringLiteral("2-first") : QStringLiteral("two"),
                    QStringLiteral("Two"), QStringLiteral("2"))};

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
    int fetches = 0;
    QString search_url;
    const MusicBrainzLookupService service{
        .fetch =
            [&fetches, &search_url](const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
                ++fetches;
                if (url.contains(QStringLiteral("?query="))) {
                    search_url = QUrl::fromPercentEncoding(url.toUtf8());
                    completion(QByteArray{search_body});
                    return;
                }
                if (url.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    completion(QByteArray{lookup_body});
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
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    auto* status = properties->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(identify != nullptr);
    QVERIFY(status != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);

    // The in-app search surface opens pre-filled from the selection — no
    // MusicBrainz tags exist anywhere on these files.
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    auto* artist =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-musicbrainz-identify-artist"));
    auto* album =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-musicbrainz-identify-release"));
    auto* search =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-search"));
    auto* results =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-results"));
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-use"));
    QVERIFY(artist && album);
    QCOMPARE(artist->text(), untagged ? QString{} : QStringLiteral("Band"));
    QCOMPARE(album->text(), untagged ? QString{} : QStringLiteral("Alpha"));
    artist->setText(QStringLiteral("Band"));
    album->setText(QStringLiteral("Alpha"));
    QVERIFY(search != nullptr);
    QVERIFY(results != nullptr);
    QVERIFY(use != nullptr);

    QTest::mouseClick(search, Qt::LeftButton);
    QTRY_COMPARE(results->topLevelItemCount(), 1);
    QVERIFY(search_url.contains(QStringLiteral("artist:")));
    QVERIFY(search_url.contains(QStringLiteral("release:")));
    QVERIFY(!search_url.contains(QStringLiteral("tracks:")));
    QCOMPARE(results->topLevelItem(0)->text(1), QStringLiteral("Alpha"));
    QCOMPARE(results->topLevelItem(0)->text(2), QStringLiteral("Band"));
    QVERIFY(results->topLevelItem(0)->text(5).contains(QStringLiteral("1999-09-09")));
    QTRY_VERIFY(use->isEnabled());
    QTest::mouseClick(use, Qt::LeftButton);
    auto* stage_matches =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-stage"));
    QVERIFY(stage_matches);
    if (untagged) {
        auto* pairs =
            dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-files"));
        auto* up = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-up"));
        auto* down =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-down"));
        auto* unmatch =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-unmatch"));
        auto* sort =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-sort"));
        QVERIFY(pairs && up && down && unmatch && sort);
        auto* tracks =
            dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-tracks"));
        QVERIFY(tracks);
        QTRY_COMPARE(pairs->topLevelItemCount(), 2);
        sort->click();
        QVERIFY(pairs->topLevelItem(0)->text(0).endsWith(QStringLiteral("2-first.flac")));
        const auto first_album_track = tracks->topLevelItem(0)->text(0);
        const auto second_album_track = tracks->topLevelItem(1)->text(0);
        QVERIFY(first_album_track.contains(QStringLiteral("One")));
        QVERIFY(second_album_track.contains(QStringLiteral("Two")));
        pairs->setCurrentItem(pairs->topLevelItem(0));
        QVERIFY(!up->isEnabled());
        QVERIFY(down->isEnabled());
        QCOMPARE(tracks->currentItem(), tracks->topLevelItem(0));
        QCOMPARE(pairs->topLevelItem(0)->text(2), QStringLiteral("→ 1.1"));
        QVERIFY(pairs->topLevelItem(0)->font(2).bold());
        QCOMPARE(pairs->topLevelItem(0)->background(0), tracks->topLevelItem(0)->background(0));
        QCOMPARE(pairs->topLevelItem(0)->toolTip(0), QStringLiteral("/music/2-first.flac"));
        // Exercise the actual drop handler using this view's model-generated payload.
        std::unique_ptr<QMimeData> mime{pairs->model()->mimeData({pairs->model()->index(0, 0)})};
        const auto rect = pairs->visualItemRect(pairs->topLevelItem(1));
        const QPoint position{rect.center().x(), rect.bottom() - 1};
        QDragEnterEvent enter{position, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
        QApplication::sendEvent(pairs->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop{QPointF{position}, Qt::MoveAction, mime.get(), Qt::LeftButton,
                        Qt::NoModifier};
        QApplication::sendEvent(pairs->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QDragEnterEvent stale{position, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
        QApplication::sendEvent(pairs->viewport(), &stale);
        QVERIFY(!stale.isAccepted());
        QVERIFY(pairs->topLevelItem(1)->text(0).endsWith(QStringLiteral("2-first.flac")));
        QCOMPARE(tracks->topLevelItem(0)->text(0), first_album_track);
        QCOMPARE(tracks->topLevelItem(1)->text(0), second_album_track);
        QVERIFY(!down->isEnabled());
        pairs->setFocus();
        QTest::keyClick(pairs, Qt::Key_Up, Qt::AltModifier);
        QVERIFY(pairs->topLevelItem(0)->text(0).endsWith(QStringLiteral("2-first.flac")));
        unmatch->click();
        QCOMPARE(pairs->topLevelItemCount(), 3);
        QCOMPARE(pairs->topLevelItem(0)->text(0), QStringLiteral("No local file"));
        QVERIFY(pairs->topLevelItem(2)->text(2).contains(QStringLiteral("Unmatched")));
        QVERIFY(!unmatch->isEnabled());
        up->click();
        up->click();
        QCOMPARE(pairs->topLevelItemCount(), 3);
        // The missing slot can move too, returning the extra file to the album.
        pairs->setCurrentItem(pairs->topLevelItem(2));
        up->click();
        QCOMPARE(pairs->topLevelItemCount(), 2);
        QVERIFY(pairs->topLevelItem(0)->text(0).endsWith(QStringLiteral("2-first.flac")));
        QCOMPARE(tracks->topLevelItem(0)->text(0), first_album_track);
        QCOMPARE(tracks->topLevelItem(1)->text(0), second_album_track);
    }
    QTRY_VERIFY(stage_matches->isEnabled());
    const auto screenshot_dir = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
    if (untagged && !screenshot_dir.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshot_dir + QStringLiteral("/musicbrainz-matching.png")));
    }
    QTest::mouseClick(stage_matches, Qt::LeftButton);

    // The chosen version stages as one ordinary colored draft transaction.
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-musicbrainz-identify")) ==
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() > 0U, 5'000);
    QTRY_VERIFY(status->text().contains(QStringLiteral("MusicBrainz")));
    QCOMPARE(fetches, 2);
    if (untagged) {
        const auto title = grid_model->fieldColumn(QStringLiteral("Title"));
        QVERIFY(title);
        QCOMPARE(grid_model->index(0, *title).data(metadata_cell_values_role).toStringList(),
                 QStringList{QStringLiteral("Two")});
        QCOMPARE(grid_model->index(1, *title).data(metadata_cell_values_role).toStringList(),
                 QStringList{QStringLiteral("One")});
    }
    const auto album_id_column = grid_model->fieldColumn(QStringLiteral("MUSICBRAINZ_ALBUMID"));
    QVERIFY(album_id_column.has_value());
    QCOMPARE(
        grid_model->index(0, *album_id_column).data(metadata_cell_staged_source_role).toString(),
        QStringLiteral("MusicBrainz"));
    const auto date_column = grid_model->fieldColumn(QStringLiteral("Date"));
    QVERIFY(album_id_column.has_value());
    QVERIFY(date_column.has_value());
    for (int row = 0; row < grid_model->rowCount(); ++row) {
        QCOMPARE(
            grid_model->index(row, *album_id_column).data(metadata_cell_values_role).toStringList(),
            QStringList{QStringLiteral("11111111-2222-3333-4444-555555555555")});
        QCOMPARE(
            grid_model->index(row, *date_column).data(metadata_cell_values_role).toStringList(),
            QStringList{QStringLiteral("1999-09-09")});
    }
    delete properties;
}

void BenchMainWindowTest::musicBrainzFingerprintScanRanksAndStages() {
    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    // Only titles and track numbers — the fingerprint path must work with
    // no album, artist, or MusicBrainz tags at all.
    const auto make_source = [&field](const QString& label, const QString& title,
                                      const QString& track_number) {
        return MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = "/music/" + label.toStdString() + ".flac",
                    .source_revision = std::nullopt,
                    .baseline =
                        metadata::MetadataDocument{
                            .fields = {field("TITLE", {title.toStdString()}),
                                       field("TRACKNUMBER", {track_number.toStdString()})},
                            .unsupported_native_objects = {},
                        },
                },
            .track_label = label,
        };
    };
    const std::vector sources{
        make_source(QStringLiteral("one"), QStringLiteral("One"), QStringLiteral("1")),
        make_source(QStringLiteral("two"), QStringLiteral("Two"), QStringLiteral("2"))};

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
    static constexpr auto acoustid_one = R"json({
      "status": "ok",
      "results": [{"id": "x", "score": 0.98, "recordings": [
        {"id": "bbbb1111-0000-0000-0000-000000000001",
         "releases": [{"id": "11111111-2222-3333-4444-555555555555"}]}]}]
    })json";
    static constexpr auto acoustid_two = R"json({
      "status": "ok",
      "results": [{"id": "y", "score": 0.97, "recordings": [
        {"id": "bbbb1111-0000-0000-0000-000000000002",
         "releases": [{"id": "11111111-2222-3333-4444-555555555555"},
                      {"id": "22222222-2222-3333-4444-555555555555"}]}]}]
    })json";
    int fingerprints = 0;
    int lookups = 0;
    const MusicBrainzLookupService service{
        .fetch =
            [](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                if (url.contains(QStringLiteral("11111111-2222-3333-4444-555555555555?inc="))) {
                    completion(QByteArray{lookup_body});
                    return;
                }
                completion(std::unexpected(core::Error{
                    .code = core::ErrorCode::not_found,
                    .message = "no such release",
                    .context = {},
                }));
            },
        .fingerprint =
            [&fingerprints](const QString& file_path,
                            std::function<void(core::Result<AcoustIdFingerprint>)> completion) {
                ++fingerprints;
                completion(AcoustIdFingerprint{
                    .duration_seconds = file_path.contains(QStringLiteral("one")) ? 61U : 59U,
                    .fingerprint = QStringLiteral("AQAD-fake-") + file_path,
                });
            },
        .acoustid_lookup =
            [&lookups](const AcoustIdFingerprint& fingerprint,
                       std::function<void(core::Result<QByteArray>)> completion) {
                ++lookups;
                completion(QByteArray{fingerprint.fingerprint.contains(QStringLiteral("one"))
                                          ? acoustid_one
                                          : acoustid_two});
            },
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
    auto* identify = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-identify"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(identify != nullptr);
    files->selectAll();
    QTRY_VERIFY(identify->isEnabled());
    QTest::mouseClick(identify, Qt::LeftButton);

    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-musicbrainz-identify"))) != nullptr);
    auto* scan = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-scan"));
    auto* results =
        dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-identify-results"));
    auto* use = dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-identify-use"));
    QVERIFY(scan != nullptr && scan->isEnabled());
    QVERIFY(results != nullptr);
    QVERIFY(use != nullptr);
    QTest::mouseClick(scan, Qt::LeftButton);

    // Both files fingerprinted and looked up; the release both matched
    // ranks first with its coverage shown, and the unloadable second
    // candidate drops out instead of blocking.
    QTRY_COMPARE(results->topLevelItemCount(), 1);
    QCOMPARE(fingerprints, 2);
    QCOMPARE(lookups, 2);
    QCOMPARE(results->topLevelItem(0)->text(0), QStringLiteral("2/2 files"));
    QCOMPARE(results->topLevelItem(0)->text(1), QStringLiteral("Alpha"));

    QTRY_VERIFY(use->isEnabled());
    QTest::mouseClick(use, Qt::LeftButton);
    auto* stage_matches =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-stage"));
    QVERIFY(stage_matches);
    QTRY_VERIFY(stage_matches->isEnabled());
    QTest::mouseClick(stage_matches, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-musicbrainz-identify")) ==
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(grid_model->patches().patch_count() > 0U, 5'000);
    const auto album_column = grid_model->fieldColumn(QStringLiteral("Album"));
    QVERIFY(album_column.has_value());
    QCOMPARE(grid_model->index(0, *album_column).data(metadata_cell_values_role).toStringList(),
             QStringList{QStringLiteral("Alpha")});
    delete properties;
}

namespace {

void write_sine_wav_fixture(const QString& path, const double amplitude,
                            const std::optional<double> second_amplitude = std::nullopt) {
    constexpr int wav_rate = 44'100;
    const int frames = wav_rate * (second_amplitude ? 2 : 1);
    QFile file{path};
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QDataStream stream{&file};
    stream.setByteOrder(QDataStream::LittleEndian);
    const quint32 data_bytes = static_cast<quint32>(frames) * 4U;
    file.write("RIFF", 4);
    stream << quint32{36U + data_bytes};
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    stream << quint32{16U} << quint16{1U} << quint16{2U} << quint32{wav_rate}
           << quint32{wav_rate * 4U} << quint16{4U} << quint16{16U};
    file.write("data", 4);
    stream << data_bytes;
    for (int frame = 0; frame < frames; ++frame) {
        const auto level = frame >= wav_rate ? second_amplitude.value_or(amplitude) : amplitude;
        const auto value = level * std::sin(2.0 * 3.14159265358979 * 997.0 * frame / wav_rate);
        const auto sample = static_cast<qint16>(std::clamp(value, -1.0, 1.0) * 32'767.0);
        stream << sample << sample;
    }
}

} // namespace

void BenchMainWindowTest::closingATabReturnsToThePreviousOne() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto* origin = window.list_tabs_.front()->view;
    const auto make = [&window](const char* name) {
        return window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                           .kind = persistence::ListKind::scratch,
                                                           .name = name,
                                                           .pinned = false,
                                                           .dirty = false,
                                                           .items = {}},
                                 true);
    };
    auto* first = make("First");
    auto* second = make("Second");
    QVERIFY(first != nullptr && second != nullptr);

    // Visit the origin, then the second list: closing it goes back to the
    // origin, even though "First" sits right beside it.
    tabs->setCurrentWidget(origin);
    tabs->setCurrentWidget(second->view);
    window.closeTabAt(tabs->indexOf(second->view));
    QTRY_COMPARE(tabs->currentWidget(), static_cast<QWidget*>(origin));
    QVERIFY(tabs->indexOf(first->view) >= 0);
}

// Delete and Play act on the tab you are looking at, not on whichever list
// last held a selection.
void BenchMainWindowTest::selectionActionsFollowTheActiveTab() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_VERIFY(!window.list_tabs_.empty());
    auto& origin = *window.list_tabs_.front();
    LocalTrackRow row;
    row.raw_path = "/one.flac";
    row.title = "One";
    row.probed = true;
    origin.model->replaceRows({row});
    tabs->setCurrentWidget(origin.view);
    QCOMPARE(window.activeTrackView(), origin.view);

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
    QCOMPARE(window.activeTrackView(), other->view);

    // A selection in the list on screen is the one Delete would remove, and
    // one in a list that is not on screen is not.
    origin.view->selectRow(0);
    QVERIFY(other->view->selectionModel()->selectedRows().isEmpty());
    window.refreshSelectionActions();
    QVERIFY(!window.remove_selected_action_->isEnabled());
    tabs->setCurrentWidget(origin.view);
    window.refreshSelectionActions();
    QCOMPARE(window.activeTrackView(), origin.view);
    QVERIFY(window.remove_selected_action_->isEnabled());
}

void BenchMainWindowTest::localArtworkSurvivesInvalidation() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac = media.filePath(QStringLiteral("one.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), flac));
    {
        QImage cover{32, 32, QImage::Format_RGB32};
        cover.fill(Qt::darkRed);
        QVERIFY(cover.save(media.filePath(QStringLiteral("cover.png")), "PNG"));
    }

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({QFile::encodeName(flac).toStdString()});
    QTableView* view = nullptr;
    LocalListModel* model = nullptr;
    QTRY_VERIFY([&] {
        for (auto* candidate : window.findChildren<QTableView*>()) {
            if (qobject_cast<LocalListModel*>(candidate->model()) &&
                candidate->model()->rowCount() == 1) {
                view = candidate;
                model = qobject_cast<LocalListModel*>(candidate->model());
                return true;
            }
        }
        return false;
    }());
    QTRY_VERIFY(model->rows().front().probed);
    const auto artwork = [&] {
        return model->data(model->index(0, 0), static_cast<int>(ui::track_album_artwork_role))
            .value<QImage>();
    };
    QTRY_VERIFY(!artwork().isNull());

    window.invalidateArtwork(model->rows().front().raw_path);
    QTRY_VERIFY(!artwork().isNull());
}

void BenchMainWindowTest::convertDialogPlansAndConvertsSelection() {
    QTemporaryDir media;
    QTemporaryDir destination;
    QVERIFY(media.isValid());
    QVERIFY(destination.isValid());
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
    const auto make_item = [&field](const QString& path, const QString& title,
                                    const QString& track) {
        const auto encoded = QFile::encodeName(path);
        return ConvertDialogItem{
            .raw_path = std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())},
            .selection = {},
            .segment = {},
            .source_revision = {},
            .metadata =
                metadata::MetadataDocument{
                    .fields = {field("TITLE", {title.toStdString()}),
                               field("ALBUM", {"Converted Album"}),
                               field("TRACKNUMBER", {track.toStdString()})},
                    .unsupported_native_objects = {},
                },
            .label = title,
        };
    };
    const auto saved_layouts = std::vector{persistence::SavedOutputLayoutProfile{
        .id = core::StableId::random(),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Saved album layout",
                .dialect = {},
                .relative_directory_expression = "saved/%album%",
                .basename_expression = "%title%",
                .sanitization_policy = {"linux", 1U},
            },
    }};
    const auto saved_destinations = std::vector{persistence::SavedDestinationProfile{
        .id = core::StableId::random(),
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = "Saved root",
                .root_raw_path = destination.path().toStdString(),
                .containment_policy = {"lexical-beneath-root", 1U},
            },
    }};
    std::vector<persistence::SavedEncoderPreset> preset_catalog;
    ConvertPresetStore preset_store{
        .load =
            [&preset_catalog](ConvertPresetStore::LoadCompletion completion) {
                completion(preset_catalog, QString{});
            },
        .save =
            [&preset_catalog](persistence::SavedEncoderPreset preset,
                              ConvertPresetStore::Completion completion) {
                preset_catalog.push_back(std::move(preset));
                completion(QString{});
            },
        .remove =
            [&preset_catalog](const core::StableId id, ConvertPresetStore::Completion completion) {
                std::erase_if(preset_catalog, [&id](const auto& entry) { return entry.id == id; });
                completion(QString{});
            },
    };
    auto* dialog = new ConvertDialog(
        {make_item(loud_path, QStringLiteral("Loud"), QStringLiteral("01")),
         make_item(quiet_path, QStringLiteral("Quiet"), QStringLiteral("02"))},
        [&saved_layouts, &saved_destinations](auto completion) {
            completion(saved_layouts, saved_destinations, QString{});
        },
        preset_store);
    dialog->show();

    auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-preset"));
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-destination"));
    auto* directories =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    auto* names =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-basename-expression"));
    auto* preview = dialog->findChild<QListWidget*>(QStringLiteral("bench-convert-preview"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-status"));
    auto* problems = dialog->findChild<QPlainTextEdit*>(QStringLiteral("bench-convert-problems"));
    auto* channels = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-channels"));
    auto* gain_mode = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-gain"));
    QVERIFY(preset != nullptr && root != nullptr && directories != nullptr && names != nullptr);
    QVERIFY(preview != nullptr && run != nullptr && status != nullptr && problems != nullptr);
    QVERIFY(channels != nullptr && gain_mode != nullptr);
    // Processing choices restore from persisted settings; this test's items
    // carry no ReplayGain, so pin the gain policy off explicitly.
    channels->setCurrentIndex(channels->findData(0));
    gain_mode->setCurrentIndex(gain_mode->findData(0));
    // Problem reports scroll inside a bounded pane instead of stretching
    // the dialog; without problems it stays hidden.
    QVERIFY(!problems->isVisible());
    QVERIFY(problems->maximumHeight() <= 200);

    // The app's saved naming layouts and destination roots are offered
    // directly; picking them fills the editable fields.
    auto* layout_choice = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-layout"));
    auto* root_choice =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-destination-choice"));
    QVERIFY(layout_choice != nullptr && root_choice != nullptr);
    QTRY_COMPARE(layout_choice->count(), 2);
    QTRY_COMPARE(root_choice->count(), 2);
    layout_choice->setCurrentIndex(1);
    emit layout_choice->activated(1);
    QCOMPARE(directories->text(), QStringLiteral("saved/%album%"));
    QCOMPARE(names->text(), QStringLiteral("%title%"));
    root_choice->setCurrentIndex(1);
    emit root_choice->activated(1);
    QCOMPARE(root->text(), destination.path());
    // Hand-editing drifts the choice back to Custom.
    QTest::keyClick(names, Qt::Key_X);
    QCOMPARE(layout_choice->currentIndex(), 0);

    // A new encoder preset saves from the editor, joins the combo as a
    // selectable profile, and deletes again; built-ins are untouched.
    const auto builtin_count = preset->count();
    auto* preset_new = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-new"));
    auto* preset_export =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-export"));
    auto* preset_delete =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-delete"));
    QVERIFY(preset_export != nullptr);
    QVERIFY(preset_new != nullptr && preset_delete != nullptr);
    QVERIFY(!preset_delete->isVisible());
    QTest::mouseClick(preset_new, Qt::LeftButton);
    auto* editor = dialog->findChild<QDialog*>(QStringLiteral("bench-preset-editor"));
    QVERIFY(editor != nullptr);
    auto* editor_name = editor->findChild<QLineEdit*>(QStringLiteral("bench-preset-editor-name"));
    auto* editor_format =
        editor->findChild<QComboBox*>(QStringLiteral("bench-preset-editor-format"));
    auto* editor_bitrate =
        editor->findChild<QSpinBox*>(QStringLiteral("bench-preset-editor-bitrate"));
    auto* editor_buttons =
        editor->findChild<QDialogButtonBox*>(QStringLiteral("bench-preset-editor-buttons"));
    QVERIFY(editor_name != nullptr && editor_format != nullptr && editor_bitrate != nullptr &&
            editor_buttons != nullptr);
    QVERIFY(!editor_buttons->button(QDialogButtonBox::Save)->isEnabled());
    editor_name->setText(QStringLiteral("Phone Opus"));
    editor_format->setCurrentIndex(1);
    editor_bitrate->setValue(96);
    channels->setCurrentIndex(1);
    gain_mode->setCurrentIndex(1);
    QVERIFY(editor_buttons->button(QDialogButtonBox::Save)->isEnabled());
    QTest::mouseClick(editor_buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QTRY_COMPARE(preset_catalog.size(), 1U);
    QCOMPARE(preset_catalog.front().preset.codec_name, std::string{"libopus"});
    QCOMPARE(preset_catalog.front().preset.bit_rate, std::optional<std::int64_t>{96'000});
    // Separator + the saved preset, selected as the current choice.
    QTRY_COMPARE(preset->count(), builtin_count + 2);
    QCOMPARE(preset->currentText(), QStringLiteral("Phone Opus"));
    QTRY_VERIFY(preset_delete->isVisible());
    // Job settings restore, but permanent gain is never restored (including
    // legacy presets that still contain a gain field).
    const auto gain_key =
        QStringLiteral("convert/job-presets/%1/gain").arg(preset->currentData().toString());
    QVERIFY(!QSettings{}.contains(gain_key));
    QSettings{}.setValue(gain_key, 2);
    preset->setCurrentIndex(0);
    channels->setCurrentIndex(0);
    gain_mode->setCurrentIndex(1);
    preset->setCurrentIndex(preset->count() - 1);
    QCOMPARE(channels->currentData().toInt(), 1);
    QCOMPARE(gain_mode->currentData().toInt(), 0);
    QTest::mouseClick(preset_delete, Qt::LeftButton);
    QTRY_VERIFY(preset_catalog.empty());
    QTRY_COMPARE(preset->count(), builtin_count);
    QTRY_VERIFY(!preset_delete->isVisible());

    preset->setCurrentIndex(preset->findData(QStringLiteral("opus-192")));
    // Preset selection cannot enable permanent gain.
    gain_mode->setCurrentIndex(gain_mode->findData(0));
    root->setText(destination.path());
    directories->setText(QStringLiteral("%album%"));
    names->setText(QStringLiteral("%tracknumber% - %title%"));
    QTRY_VERIFY(preview->count() > 0 &&
                preview->item(0)->text() == QStringLiteral("Converted Album/01 - Loud.opus"));
    QCOMPARE(preview->count(), 2);
    QCOMPARE(preview->item(1)->text(), QStringLiteral("Converted Album/02 - Quiet.opus"));
    QTRY_VERIFY(run->isEnabled());

    // A colliding layout blocks Convert; the explanation scrolls in the
    // bounded problems pane while the status stays a one-line count.
    names->setText(QStringLiteral("same"));
    QTRY_VERIFY(!run->isEnabled());
    QTRY_VERIFY(problems->isVisible() &&
                problems->toPlainText().contains(QStringLiteral("target")));
    QVERIFY(status->text().contains(QStringLiteral("problem")));
    names->setText(QStringLiteral("%tracknumber% - %title%"));
    QTRY_VERIFY(run->isEnabled());
    QTRY_VERIFY(!problems->isVisible());

    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 2 of 2 files.")),
                             15'000);
    const auto album_dir = QDir{destination.path()}.filePath(QStringLiteral("Converted Album"));
    QVERIFY(QFileInfo::exists(album_dir + QStringLiteral("/01 - Loud.opus")));
    QVERIFY(QFileInfo::exists(album_dir + QStringLiteral("/02 - Quiet.opus")));
    const auto encoded = QFile::encodeName(album_dir + QStringLiteral("/02 - Quiet.opus"));
    const auto reread = metadata::read_local_metadata(
        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())});
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.first_effective_value("title"), std::optional<std::string>{"Quiet"});

    // Artwork carriage is a persisted opt-out (ADR-0131), enabled by default.
    auto* embed_artwork = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-artwork"));
    QVERIFY(embed_artwork != nullptr);
    QVERIFY(embed_artwork->isChecked());

    // The resampling and bit-depth choices reach the pipeline: FLAC at a
    // forced 96 kHz stored as dithered 16-bit.
    auto* resample = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-resample"));
    auto* bit_depth = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-bit-depth"));
    QVERIFY(resample != nullptr && bit_depth != nullptr);
    QCOMPARE(resample->currentData().toInt(), 0);
    QCOMPARE(bit_depth->currentData().toInt(), 0);
    resample->setCurrentIndex(resample->findData(96'000));
    bit_depth->setCurrentIndex(bit_depth->findData(16));
    preset->setCurrentIndex(preset->findData(QStringLiteral("flac")));
    names->setText(QStringLiteral("%tracknumber% - %title% 96k"));
    QTRY_VERIFY(preview->count() > 0 &&
                preview->item(0)->text() == QStringLiteral("Converted Album/01 - Loud 96k.flac"));
    QTRY_VERIFY(run->isEnabled());
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 2 of 2 files.")),
                             15'000);
    const auto resampled_path = album_dir + QStringLiteral("/01 - Loud 96k.flac");
    QVERIFY(QFileInfo::exists(resampled_path));
    const auto resampled_encoded = QFile::encodeName(resampled_path);
    auto resampled = formats::AudioDecoder::open(std::string{
        resampled_encoded.constData(), static_cast<std::size_t>(resampled_encoded.size())});
    QVERIFY(resampled.has_value());
    QCOMPARE(resampled->output_format().sample_rate, 96'000);
    const auto probed = formats::probe_local_media(std::string{
        resampled_encoded.constData(), static_cast<std::size_t>(resampled_encoded.size())});
    QVERIFY(probed.has_value() && probed->best_audio_stream.has_value());
    QCOMPARE(
        probed->audio_streams[static_cast<std::size_t>(*probed->best_audio_stream)].sample_format,
        std::string{"s16"});

    // Mirror mode (ADR-0132) disables the expressions and previews the
    // source structure below the inferred common root.
    auto* mirror = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-mirror"));
    auto* directory_field =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    QVERIFY(mirror != nullptr && directory_field != nullptr);
    QVERIFY(!mirror->isChecked());
    mirror->setChecked(true);
    QVERIFY(!directory_field->isEnabled());
    QTRY_VERIFY(preview->count() >= 3 && preview->item(0)->text().startsWith(
                                             QStringLiteral("Recreating full source paths")));

    // ADR-0154: mirror recreates the complete source path beneath the
    // destination, so single-album selections keep their folders too.
    const auto media_relative = QString{media.path()}.mid(1) + QStringLiteral("/loud.flac");
    QTRY_VERIFY(preview->count() >= 2 && preview->item(1)->text() == media_relative);
    mirror->setChecked(false);
    QVERIFY(directory_field->isEnabled());
    delete dialog;
}

// ADR-0173: the dialog's Gain choice permanently applies the item's
// ReplayGain metadata to the encoded PCM, exactly like the core path.
void BenchMainWindowTest::convertDialogAppliesPermanentReplayGain() {
    QSettings{}.setValue(QStringLiteral("convert/gain"), 2);
    QTemporaryDir media;
    QTemporaryDir destination;
    QVERIFY(media.isValid());
    QVERIFY(destination.isValid());
    const auto source_path = media.filePath(QStringLiteral("gained.wav"));
    write_sine_wav_fixture(source_path, 0.8);

    const auto field = [](std::string name, std::vector<std::string> values) {
        return metadata::MetadataField{
            .canonical_name = metadata::canonicalize_field_name(name),
            .native_name = std::move(name),
            .values = std::move(values),
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        };
    };
    const auto encoded_source = QFile::encodeName(source_path);
    ConvertDialogItem item{
        .raw_path = std::string{encoded_source.constData(),
                                static_cast<std::size_t>(encoded_source.size())},
        .selection = {},
        .segment = {},
        .source_revision = {},
        .metadata =
            metadata::MetadataDocument{
                .fields = {field("TITLE", {"Gained"}),
                           field("REPLAYGAIN_TRACK_GAIN", {"-12.04 dB"}),
                           field("REPLAYGAIN_TRACK_PEAK", {"0.8"})},
                .unsupported_native_objects = {},
            },
        .label = QStringLiteral("Gained"),
    };
    ConvertPresetStore preset_store{
        .load = [](ConvertPresetStore::LoadCompletion completion) { completion({}, QString{}); },
        .save = [](persistence::SavedEncoderPreset,
                   ConvertPresetStore::Completion completion) { completion(QString{}); },
        .remove = [](core::StableId,
                     ConvertPresetStore::Completion completion) { completion(QString{}); },
    };
    auto* dialog = new ConvertDialog(
        {std::move(item)}, [](auto completion) { completion({}, {}, QString{}); }, preset_store);
    dialog->show();
    auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-preset"));
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-destination"));
    auto* directories =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-directory-expression"));
    auto* names =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-convert-basename-expression"));
    auto* gain_mode = dialog->findChild<QComboBox*>(QStringLiteral("bench-convert-gain"));
    auto* run = dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-run"));
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-status"));
    auto* mirror = dialog->findChild<QCheckBox*>(QStringLiteral("bench-convert-mirror"));
    QVERIFY(preset != nullptr && root != nullptr && names != nullptr && gain_mode != nullptr);
    QVERIFY(run != nullptr && status != nullptr && directories != nullptr && mirror != nullptr);
    QCOMPARE(gain_mode->currentData().toInt(), 0);
    auto* warning = dialog->findChild<QLabel*>(QStringLiteral("bench-convert-gain-warning"));
    QVERIFY(warning);
    mirror->setChecked(false);
    preset->setCurrentIndex(preset->findData(QStringLiteral("flac")));
    root->setText(destination.path());
    directories->setText(QString{});
    names->setText(QStringLiteral("%title%"));
    gain_mode->setCurrentIndex(gain_mode->findData(1));
    QVERIFY(warning->text().contains(QStringLiteral("permanently changes the audio samples")));
    QTRY_VERIFY(run->isEnabled());
    QTimer::singleShot(0, dialog, [dialog] {
        auto* confirmation =
            dialog->findChild<QMessageBox*>(QStringLiteral("bench-convert-gain-confirmation"));
        QVERIFY(confirmation);
        QCOMPARE(confirmation->standardButton(confirmation->defaultButton()), QMessageBox::Cancel);
        confirmation->button(QMessageBox::Cancel)->click();
    });
    QTest::mouseClick(run, Qt::LeftButton);
    QVERIFY(!QFileInfo::exists(destination.filePath(QStringLiteral("Gained.flac"))));
    QVERIFY(run->isVisible());
    QTimer::singleShot(0, dialog, [dialog] {
        auto* confirmation =
            dialog->findChild<QMessageBox*>(QStringLiteral("bench-convert-gain-confirmation"));
        QVERIFY(confirmation);
        confirmation->button(QMessageBox::Yes)->click();
    });
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("Converted 1 of 1 files.")),
                             15'000);

    const auto converted =
        QFile::encodeName(QDir{destination.path()}.filePath(QStringLiteral("Gained.flac")));
    auto decoder = formats::AudioDecoder::open(
        std::string{converted.constData(), static_cast<std::size_t>(converted.size())});
    QVERIFY(decoder.has_value());
    float peak = 0.0F;
    while (true) {
        auto chunk = decoder->next_chunk();
        QVERIFY(chunk.has_value());
        if (!*chunk) {
            break;
        }
        for (const auto sample : (*chunk)->interleaved_samples) {
            peak = std::max(peak, std::abs(sample));
        }
    }
    // -12.04 dB over a 0.8 peak must land near 0.2; an untouched copy at
    // 0.8 means the dialog's gain option silently did nothing.
    QVERIFY2(peak > 0.17F && peak < 0.23F, qPrintable(QString::number(peak)));
    delete dialog;
    ConvertDialog reopened{{}};
    QCOMPARE(
        reopened.findChild<QComboBox*>(QStringLiteral("bench-convert-gain"))->currentData().toInt(),
        0);
}

// ADR-0144: notifications fire only for track changes while playing in
// the background, never retro-notify, and the menu toggle persists.
void BenchMainWindowTest::desktopNotificationsNotifyBackgroundTrackChanges() {
    DesktopNotifier notifier;
    notifier.setBackgroundOnly(true);
    QStringList sent;
    notifier.setSendOverride([&sent](const QString& summary, const QString& body) {
        sent << summary + QStringLiteral("|") + body;
    });
    MprisPlaybackState playing;
    playing.status = QStringLiteral("Playing");
    playing.title = QStringLiteral("First Song");
    playing.artist = QStringLiteral("Artist");
    playing.album = QStringLiteral("Album");
    playing.track_key = QStringLiteral("key-1");
    // Disabled: tracked but silent; enabling never retro-notifies.
    QVERIFY(!notifier.publish(playing, false));
    notifier.setEnabled(true);
    QVERIFY(!notifier.publish(playing, false));
    // A background track change notifies with a markup-escaped body.
    playing.track_key = QStringLiteral("key-2");
    playing.title = QStringLiteral("Second Song");
    playing.artist = QStringLiteral("A & B");
    QVERIFY(notifier.publish(playing, false));
    QCOMPARE(notifier.sentCount(), 1ULL);
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.front(), QStringLiteral("Second Song|A &amp; B — Album"));
    // A change while the window is active stays silent and is tracked.
    playing.track_key = QStringLiteral("key-3");
    QVERIFY(!notifier.publish(playing, true));
    QVERIFY(!notifier.publish(playing, false));
    // Pause never notifies; resuming into a new track does.
    playing.track_key = QStringLiteral("key-4");
    playing.status = QStringLiteral("Paused");
    QVERIFY(!notifier.publish(playing, false));
    playing.status = QStringLiteral("Playing");
    QVERIFY(notifier.publish(playing, false));
    QCOMPARE(notifier.sentCount(), 2ULL);
    notifier.setBackgroundOnly(false);
    playing.track_key = QStringLiteral("key-5");
    QVERIFY(notifier.publish(playing, true));
    QCOMPARE(notifier.sentCount(), 3ULL);
    QSignalSpy delivered(&notifier, &DesktopNotifier::deliveryFinished);
    notifier.setEnabled(false);
    notifier.sendTest();
    QCOMPARE(delivered.size(), 1);
    QVERIFY(delivered.front().front().toString().isEmpty());

    BenchMainWindow window;
    window.show();
    auto* action = window.findChild<QAction*>(QStringLiteral("action-desktop-notifications"));
    QVERIFY(action != nullptr);
    QVERIFY(action->isCheckable());
    QVERIFY(!action->isChecked());
    action->setChecked(true);
    QCOMPARE(QSettings{}.value(QStringLiteral("desktop/notifications"), false).toBool(), true);
    action->setChecked(false);
    QCOMPARE(QSettings{}.value(QStringLiteral("desktop/notifications"), true).toBool(), false);
}

void BenchMainWindowTest::lastFmSettingsAndTrackActions() {
    BenchMainWindow window;
    window.show();
    // The service talks to an endpoint that accepts and never answers, so no
    // request leaves this machine and no real reply races the ones fed below.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost));
    QTemporaryDir state;
    QVERIFY(state.isValid());
    delete window.lastfm_;
    window.lastfm_ =
        new LastFmService(state.filePath(QStringLiteral("lastfm.json")), &window,
                          QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(silent.serverPort())));
    const auto reply = [&window](const QString& op, const QByteArray& json) {
        emit window.lastfm_->completed(op, QJsonDocument::fromJson(json).object(), {});
    };
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::lastfm);
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("lastfm-status"));
    auto* secret = dialog->findChild<QLineEdit*>(QStringLiteral("lastfm-account-secret"));
    QVERIFY(status && secret);
    QCOMPARE(secret->echoMode(), QLineEdit::Password);
    QTRY_VERIFY(status->text().contains(QStringLiteral("Not connected")));
    auto* key = dialog->findChild<QLineEdit*>(QStringLiteral("lastfm-account-key"));
    auto* reuse = dialog->findChild<QCheckBox*>(QStringLiteral("lastfm-reuse-key"));
    auto* begin = dialog->findChild<QPushButton*>(QStringLiteral("lastfm-authorize"));
    auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("lastfm-cancel"));
    auto* poll = dialog->findChild<QTimer*>(QStringLiteral("lastfm-auth-poll"));
    auto* deadline = dialog->findChild<QTimer*>(QStringLiteral("lastfm-auth-deadline"));
    QVERIFY(key && reuse && begin && cancel && poll && deadline);
    QVERIFY(!dialog->findChild<QPushButton*>(QStringLiteral("lastfm-finish")));
    QVERIFY(reuse->isChecked());
    QVERIFY(cancel->isHidden());
    key->setText(QStringLiteral("invalid"));
    begin->click();
    QVERIFY(status->text().contains(QStringLiteral("32-character")));
    // Sharing the key with dynamic playlists is an explicit local setting.
    key->setText(QString(32, 'a'));
    secret->setText(QString(32, 'b'));
    begin->click();
    QCOMPARE(QSettings{}.value(QStringLiteral("lastfm/api-key")).toString(), QString(32, 'a'));
    auto* read_key = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-lastfm-key"));
    QCOMPARE(read_key->text(), QString(32, 'a'));
    reuse->setChecked(false);
    key->setText(QString(32, 'c'));
    begin->click();
    QCOMPARE(QSettings{}.value(QStringLiteral("lastfm/api-key")).toString(), QString(32, 'a'));
    reply(QStringLiteral("status"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(dialog->findChild<QWidget*>(QStringLiteral("lastfm-credentials"))->isHidden());
    QVERIFY(!poll->isActive());
    QVERIFY(begin->property("credentials-saved").toBool());
    // Feed deterministic replies without opening a real browser or contacting Last.fm.
    auto* page = dialog->findChild<QWidget*>(QStringLiteral("lastfm-settings"));
    page->setProperty("auth-waiting", true);
    reply(QStringLiteral("begin"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("Waiting for browser approval")));
    reply(QStringLiteral("finish"),
          QByteArray(R"({"credentials_saved":true,"authorization_pending":true})"));
    QVERIFY(poll->isActive());
    reply(QStringLiteral("finish"),
          QByteArray(
              R"({"credentials_saved":true,"connected":true,"user":"listener","enabled":true})"));
    QVERIFY(!poll->isActive());
    QVERIFY(!page->property("auth-waiting").toBool());
    QVERIFY(dialog->findChild<QCheckBox*>(QStringLiteral("lastfm-enabled"))->isChecked());
    page->setProperty("auth-waiting", true);
    poll->start();
    cancel->click();
    QVERIFY(!poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("Stopped waiting")));
    // A late reply after cancellation must not restart polling.
    reply(QStringLiteral("finish"), QByteArray(R"({"authorization_pending":true})"));
    QVERIFY(!poll->isActive());
    page->setProperty("auth-waiting", true);
    poll->start();
    QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
    QVERIFY(!poll->isActive());
    QVERIFY(status->text().contains(QStringLiteral("timed out")));
    QVERIFY(cancel->isHidden());
    dialog->close();
    LocalListModel model;
    LocalTrackRow track;
    track.artist = "Artist";
    track.title = "Title";
    track.raw_path = "/test.flac";
    model.appendRows({track});
    QTableView view;
    view.setModel(&model);
    view.selectRow(0);
    QMenu menu;
    window.addLastFmActions(&menu, &view);
    QCOMPARE(menu.actions().size(), 1);
    auto* submenu = menu.actions().first()->menu();
    QVERIFY(submenu && submenu->isEnabled());
    QCOMPARE(submenu->actions()[1]->text(), QStringLiteral("Love track"));
    QCOMPARE(submenu->actions()[2]->text(), QStringLiteral("Unlove track"));
}

void BenchMainWindowTest::muteRestoresLocalVolumeAcrossBrowsing() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.list_tabs_.empty());
    QTRY_VERIFY(window.playingOnEngine());
    auto* local = window.list_tabs_.front()->view;
    window.tabs_->setCurrentWidget(local);
    // The engine's volume: what the slider shows is what the engine reports.
    const auto engine_volume = [&window] { return window.local_playback_->state().volume_percent; };
    window.volume_->setValue(37);
    QTRY_COMPARE(engine_volume(), 37);
    QTest::mouseClick(window.mute_button_, Qt::LeftButton);
    QTRY_COMPARE(engine_volume(), 0);
    QVERIFY(window.mute_button_->isChecked());
    QVERIFY(window.mute_button_->isEnabled());
    QTest::mouseClick(window.mute_button_, Qt::LeftButton);
    QTRY_COMPARE(engine_volume(), 37);
    QVERIFY(!window.mute_button_->isChecked());
    window.volume_->setValue(0);
    QTRY_COMPARE(engine_volume(), 0);
    QVERIFY(window.mute_button_->isChecked());
    window.volume_->setValue(21);
    QTRY_COMPARE(engine_volume(), 21);
    QVERIFY(!window.mute_button_->isChecked());
}

void BenchMainWindowTest::upNextMultiSelectionEdits() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.up_next_restored_ && !window.list_tabs_.empty());
    window.tabs_->setCurrentWidget(window.list_tabs_.front()->view);
    LocalTrackRow row;
    row.raw_path = "/tmp/request.flac";
    row.title = "Duplicate request";
    window.enqueueLocalRequests({row, row, row, row, row});
    const auto original = window.up_next_display_ids_;
    QCOMPARE(original.size(), std::size_t{5});
    auto* view = window.up_next_view_;
    QCOMPARE(view->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(view->dragDropMode(), QAbstractItemView::DragDrop);
    QVERIFY(view->dragEnabled());
    QVERIFY(view->acceptDrops());
    QVERIFY(view->viewport()->acceptDrops());
    QVERIFY(view->showDropIndicator());
    QVERIFY(!view->dragDropOverwriteMode());
    QCOMPARE(view->defaultDropAction(), Qt::MoveAction);
    QVERIFY(view->model()->index(0, 0).flags().testFlag(Qt::ItemIsDragEnabled));
    const auto select = [&](std::initializer_list<int> rows) {
        view->selectionModel()->clearSelection();
        for (const auto i : rows)
            view->selectionModel()->select(view->model()->index(i, 0),
                                           QItemSelectionModel::Select | QItemSelectionModel::Rows);
    };
    select({1, 3});
    window.editUpNextSelection(4, 5);
    QCOMPARE(window.up_next_display_ids_,
             (std::vector<std::uint64_t>{original[0], original[2], original[4], original[1],
                                         original[3]}));
    QCOMPARE(view->selectionModel()->selectedRows().size(), 2);
    window.editUpNextSelection(2);
    QCOMPARE(window.up_next_display_ids_,
             (std::vector<std::uint64_t>{original[0], original[2], original[1], original[3],
                                         original[4]}));
    const auto moved = window.up_next_display_ids_;
    window.editUpNextSelection(1);
    QCOMPARE(window.playback_.requests.pending().size(), std::size_t{3});
    QVERIFY(window.playback_.requests.undo());
    window.refreshUpNext();
    QCOMPARE(window.up_next_display_ids_, moved);
    const auto revision = window.playback_.requests.revision();
    QVERIFY(!window.playback_.requests.retain({original[0], original[0]}));
    QCOMPARE(window.playback_.requests.revision(), revision);
    QCOMPARE(window.up_next_display_ids_, moved);
}

void BenchMainWindowTest::upNextPanelAnimationAndSettings() {
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), false);
    QSettings{}.setValue(QStringLiteral("up-next/visible"), false);
    QSettings{}.setValue(QStringLiteral("up-next/width"), 380);
    BenchMainWindow window;
    window.resize(1320, 760);
    window.show();
    QTRY_VERIFY(window.up_next_restored_);
    auto* dock = window.up_next_dock_;
    auto* animation = dock->findChild<QVariantAnimation*>();
    QVERIFY(animation);
    auto* toggle = window.findChild<QAction*>(QStringLiteral("action-show-up-next"));
    QSettings{}.setValue(QStringLiteral("appearance/panel-animations"), true);
    toggle->trigger();
    QTRY_COMPARE(animation->state(), QAbstractAnimation::Running);
    QTest::qWait(40);
    toggle->trigger();
    QVERIFY(!toggle->isChecked());
    QTest::qWait(30);
    toggle->trigger();
    QTRY_COMPARE(animation->state(), QAbstractAnimation::Stopped);
    QVERIFY(dock->isVisible());
    QVERIFY(toggle->isChecked());
    QVERIFY(qAbs(dock->width() - 380) <= 2);
    QVERIFY(window.up_next_view_->isVisible());
    window.resizeDocks({dock}, {460}, Qt::Horizontal);
    QTRY_COMPARE(QSettings{}.value(QStringLiteral("up-next/width")).toInt(), 460);
    toggle->trigger();
    QTRY_VERIFY(!dock->isVisible());
    QCOMPARE(QSettings{}.value(QStringLiteral("up-next/width")).toInt(), 460);
    auto* dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    auto* checkbox =
        dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-panel-animations"));
    QVERIFY(checkbox && checkbox->isChecked());
    checkbox->setChecked(false);
    dialog->reject();
    QVERIFY(QSettings{}.value(QStringLiteral("appearance/panel-animations")).toBool());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.showSettingsDialog(SettingsDialog::Page::general);
    checkbox = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-panel-animations"));
    checkbox->setChecked(false);
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QVERIFY(!QSettings{}.value(QStringLiteral("appearance/panel-animations")).toBool());
    toggle->trigger();
    QVERIFY(dock->isVisible());
    QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
    toggle->trigger();
    QVERIFY(!dock->isVisible());
}

void BenchMainWindowTest::upNextEditingAndPersistence() {
    audio::RequestQueue<std::string> queue;
    QVERIFY(queue.insert({"A", "A"}, 0));
    const auto first = queue.pending()[0];
    const auto second = queue.pending()[1];
    QVERIFY(first.id != second.id);
    QVERIFY(queue.insert({"X"}, 0));
    QCOMPARE(queue.pending()[0].source, std::string{"X"});
    QVERIFY(queue.canUndo());
    QVERIFY(queue.undo());
    QCOMPARE(queue.pending().size(), 2U);
    QVERIFY(queue.move(second.id, 0));
    QCOMPARE(queue.pending()[0].id, second.id);
    queue.started(second);
    QVERIFY(!queue.canUndo());
    queue.clear();
    QVERIFY(queue.active());
    QVERIFY(queue.pending().empty());
    queue.finished();
    QVERIFY(!queue.active());
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.up_next_restored_);
    window.tabs_->setCurrentIndex(0);
    LocalTrackRow row;
    row.raw_path = std::string{"/tmp/raw-"} + char(0xff) + ".flac";
    row.title = "Request";
    window.enqueueLocalRequests({row, row});
    QCOMPARE(window.playback_.requests.pending().size(), 2U);
    QVERIFY(!window.up_next_view_->albumGroupingEnabled());
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        window.up_next_dock_->setVisible(true);
        window.resize(1320, 760);
        QTest::qWait(250);
        QVERIFY(window.grab().save(directory + QStringLiteral("/up-next.png")));
    }
    window.editUpNext(1, 0);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    bool saved = false;
    window.persistence_->loadUiState(QStringLiteral("playback/up-next/v1"),
                                     [&](QByteArray payload, QString error) {
                                         QVERIFY(error.isEmpty());
                                         QVERIFY(payload.contains("Request"));
                                         saved = true;
                                     });
    QTRY_VERIFY(saved);
    window.playback_.requests.clear();
    window.up_next_restored_ = false;
    window.restoreUpNext();
    QTRY_VERIFY(window.up_next_restored_);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    QCOMPARE(window.playback_.requests.pending()[0].source.raw_path, row.raw_path);
}

// ADR-0227: this computer's engine and a remote one, side by side. Local tabs
// play here and remote tabs there, never both at once; the remote library
// fills remote tabs from its own index; and nothing mixes the two.
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
    QTRY_VERIFY(window.remote_playback_ != nullptr && window.remote_playback_->active());
    auto* remote_tab = window.remoteQueueTab();
    QVERIFY(remote_tab != nullptr && remote_tab->document.remote);
    auto* sources = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    QVERIFY(sources != nullptr);
    QCOMPARE(sources->tabData(sources->count() - 1).toString(), QStringLiteral("remote"));
    // Named by what the engine calls itself -- here its machine's name, as
    // it was started without one -- rather than by its socket or address.
    QTRY_COMPARE(sources->tabText(sources->count() - 1), QSysInfo::machineHostName());

    // A folder is added to the remote's library by its path there: this
    // computer's file dialog would offer this computer's folders.
    {
        QWidget host;
        auto* folders = window.remote_library_->createFoldersWidget(&host);
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
            const auto roots = window.remote_catalogue_source_->open()->roots();
            return roots && roots->size() == 1U &&
                   roots->front().raw_path == QFile::encodeName(music).toStdString();
        }());
    }
    // The remote library fills the remote tab, from the remote's index.
    {
        persistence::LibraryScanProgress progress;
        QVERIFY(window.remote_catalogue_source_->open()->scan({}, progress).has_value());
    }
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto page = window.remote_catalogue_source_->open()->query(albums);
    QVERIFY(page && !page->entries.empty());
    window.openLocalPaths({QFile::encodeName(here).toStdString()});
    QTRY_VERIFY(window.currentListTab() != nullptr && !window.currentListTab()->document.remote &&
                window.currentListTab()->model->rowCount() == 1);
    auto* local_tab = window.currentListTab();
    emit window.remote_library_->actionRequested(page->entries, LocalLibraryAction::append);
    QTRY_COMPARE(remote_tab->model->rowCount(), 1);
    QCOMPARE(local_tab->model->rowCount(), 1);
    QVERIFY(!remote_tab->model->rows().front().title.empty());

    // Local plays here; the remote tab plays there and stops this one.
    window.playRow(*local_tab, 0);
    QTRY_COMPARE(window.local_playback_->state().status, QStringLiteral("playing"));
    window.playRow(*remote_tab, 0);
    QTRY_COMPARE(window.remote_playback_->state().status, QStringLiteral("playing"));
    // Adding to the remote tab while it plays: the rows stay as they were
    // added -- tagged, with their own identities -- rather than being traded
    // for the engine's bare paths.
    {
        emit window.remote_library_->actionRequested(page->entries, LocalLibraryAction::append);
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
        QTRY_VERIFY(!window.remote_playback_->settling());
        QTRY_COMPARE(window.remote_playback_->state().queue_size, std::size_t{1});
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
        QVERIFY(window.remote_playback_->settling());
        // A state from before the edit, as far as the window can tell.
        window.engine_queue_revision_ = 0;
        QElapsedTimer waited;
        waited.start();
        emit window.remote_playback_->changed();
        QVERIFY2(waited.elapsed() < 1'000, "the window does not wait on a busy engine");
        QCOMPARE(remote_tab->model->rowCount(), 2);
        QCOMPARE(remote_tab->model->rows().back().entry_id, extra.entry_id);
        ::kill(pid, SIGCONT);
        QTRY_VERIFY(!window.remote_playback_->settling());
        QTest::qWait(300);
        QCOMPARE(remote_tab->model->rowCount(), 2);
        QCOMPARE(remote_tab->model->rows().back().entry_id, extra.entry_id);
        QVERIFY(remote_tab->model->rows().back().probed);
        remote_tab->model->removeRowIndexes({1});
        window.markTabDirty(*remote_tab);
    }
    QTRY_COMPARE(window.local_playback_->state().status, QStringLiteral("stopped"));
    QVERIFY(window.transport_ == window.remote_playback_);
    // And back.
    window.playRow(*local_tab, 0);
    QTRY_COMPARE(window.local_playback_->state().status, QStringLiteral("playing"));
    QTRY_COMPARE(window.remote_playback_->state().status, QStringLiteral("stopped"));

    // Up Next holds one engine's asks.
    window.enqueueLocalRequests({remote_tab->model->rows().front()}, -1, true);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    window.enqueueLocalRequests({local_tab->model->rows().front()}, -1, false);
    QCOMPARE(window.playback_.requests.pending().size(), 1U);
    window.playback_.requests.clear();

    // The binding survives a restart.
    window.persistNow(false);
    QVERIFY(window.close());
    BenchMainWindow restored;
    QTRY_VERIFY(restored.lists_restored_);
    const auto remote_tabs = std::ranges::count_if(
        restored.list_tabs_, [](const auto& tab) { return tab->document.remote; });
    QCOMPARE(remote_tabs, 1);
}

// ADR-0228: an output agent registered with the engine is offered in the
// speaker menu, and choosing it plays there. A real melody-agent process, on
// the engine's socket.
void BenchMainWindowTest::theDeviceMenuChoosesAnOutputAgent() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.local_playback_ != nullptr && window.local_playback_->active());
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

    // Gone, it stays chosen and says so.
    agent.terminate();
    QVERIFY(agent.waitForFinished(5'000));
    QTRY_VERIFY_WITH_TIMEOUT(
        output_action(QStringLiteral("agent:bedside")) != nullptr &&
            output_action(QStringLiteral("agent:bedside"))->text().contains(QStringLiteral("offline")),
        10'000);
    QVERIFY(output_action(QStringLiteral("agent:bedside"))->isChecked());

    // And this computer again, if the engine has audio of its own here.
    if (auto* local = output_action(QStringLiteral("local")); local != nullptr) {
        local->trigger();
        QTRY_COMPARE(window.property("trackknife-player-output").toString(),
                     QStringLiteral("local"));
        QCOMPARE(button->text(), QString{});
    }
}

// ADR-0227: a remote tab's files are on the remote's machine and need not be
// reachable from this one, so its tags and covers come from that engine --
// however its rows got there.
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
    QTRY_VERIFY(window.remote_playback_ != nullptr && window.remote_playback_->active());
    {
        auto catalogue = window.remote_catalogue_source_->open();
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
    const auto page = window.remote_catalogue_source_->open()->query(albums);
    QVERIFY(page && page->entries.size() == 1U);
    emit window.remote_library_->actionRequested(page->entries, LocalLibraryAction::append);
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
    auto* local = window.addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                              .kind = persistence::ListKind::scratch,
                                                              .name = "Here",
                                                              .pinned = false,
                                                              .dirty = false,
                                                              .items = {},
                                                              .remote = false},
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
    QVERIFY(!window.transferRows(local->view, {local->model->rowCount() - 1}, remote_id, false,
                                 -1));
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
    QCOMPARE(scope->currentData().toString(), QStringLiteral("remote"));
    QTRY_COMPARE(dialog->focusWidget(), static_cast<QWidget*>(input));
    // From a local tab, this computer's; and back again, the remote's.
    window.tabs_->setCurrentWidget(local->view);
    window.openSearchDialog();
    QCOMPARE(scope->currentData().toString(), QStringLiteral("local"));
    window.tabs_->setCurrentWidget(tab->view);
    window.openSearchDialog();
    QCOMPARE(scope->currentData().toString(), QStringLiteral("remote"));
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

    // Last, as it retags the fixture everything above searched for.
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_remote_folder_key), music);
    QSettings{}.setValue(QLatin1String(SettingsDialog::library_remote_mount_key), mounted);
    // Tools on a remote tab work on the file where this computer sees it --
    // the mount -- and what they change reaches the remote's library without
    // waiting for a scan.
    tab->view->selectionModel()->select(tab->model->index(0, 0),
                                        QItemSelectionModel::ClearAndSelect |
                                            QItemSelectionModel::Rows);
    const auto tooled = window.remoteFileWorkRows(tab->view);
    QVERIFY(tooled && tooled->size() == 1U);
    const auto here = QFile::encodeName(mounted + QStringLiteral("/art.flac")).toStdString();
    QCOMPARE(tooled->front().raw_path, here);
    // Retagged, as the tag editor would write it, and reported as committed.
    QVERIFY(QFile::remove(art));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-long-flac.b64"), art));
    operations::MetadataCommitResult committed;
    committed.source_raw_path = here;
    committed.published_revision = *core::observe_local_source_revision(here);
    committed.document.fields.push_back({.canonical_name = "title",
                                         .native_name = "TITLE",
                                         .values = {"Metadata Fixture"},
                                         .qualifier = {},
                                         .provenance = metadata::FieldProvenance::embedded});
    window.applyCommittedMetadata(committed);
    const auto remote_art = QFile::encodeName(art).toStdString();
    QTRY_VERIFY([&] {
        const auto indexed = window.remote_catalogue_source_->open()->cached_tracks({remote_art});
        return indexed && indexed->front().facts.title == "Metadata Fixture";
    }());
    QSettings{}.remove(QLatin1String(SettingsDialog::library_remote_folder_key));
    QSettings{}.remove(QLatin1String(SettingsDialog::library_remote_mount_key));
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
            if (tab->document.remote && tab->model->rowCount() > 0) {
                return tab.get();
            }
        }
        return nullptr;
    };
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.lists_restored_);
        QTRY_VERIFY(window.remote_playback_ != nullptr && window.remote_playback_->active());
        auto catalogue = window.remote_catalogue_source_->open();
        QVERIFY(catalogue->add_root(QFile::encodeName(music).toStdString()).has_value());
        persistence::LibraryScanProgress progress;
        QVERIFY(catalogue->scan({}, progress).has_value());
        persistence::LibraryQuery albums;
        albums.kind = persistence::LibraryEntryKind::album;
        const auto page = catalogue->query(albums);
        QVERIFY(page && page->entries.size() == 1U);
        emit window.remote_library_->actionRequested(page->entries, LocalLibraryAction::append);
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
        Service service([&](query::CompiledTkq compiled, core::CancellationToken,
                            Service::Completion completion) {
            queries.push_back(std::move(compiled));
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
            Service service([](query::CompiledTkq compiled, core::CancellationToken,
                               Service::Completion completion) {
                const auto title = compiled.predicates.back().text;
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
                                    .profile = QStringLiteral("local")};
    QVERIFY(saveDynamicPlaylists(local.profile, {local}));
    auto loaded = loadDynamicPlaylists(local.profile);
    QVERIFY(loaded);
    QCOMPARE(loaded->size(), 1);
    QCOMPARE(loaded->front().query, local.query);
    QVERIFY(loadDynamicPlaylists(QStringLiteral("mpd/other"))->isEmpty());
    QVERIFY(!saveDynamicPlaylists(QStringLiteral("mpd/other"), {local}));
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
    QCOMPARE(catalog->count(), 2);
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-name"))->setText(QStringLiteral("Rock"));
    dialog->findChild<QLineEdit*>(QStringLiteral("dynamic-query"))
        ->setText(QStringLiteral("genre HAS rock"));
    dialog->findChild<QPushButton*>(QStringLiteral("dynamic-save"))->click();
    QCOMPARE(catalog->count(), 3);
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
    QCOMPARE(dialog->findChild<QComboBox*>(QStringLiteral("dynamic-catalog"))->count(), 3);
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
    QVERIFY(grid->selection().source(0).source_revision);
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

    auto* action = window.findChild<QAction*>(QStringLiteral("action-replaygain-dialog"));
    QVERIFY(action != nullptr);
    action->trigger();
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(QStringLiteral("bench-replaygain-dialog"))) !=
                nullptr);
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

void BenchMainWindowTest::folderBookmarksRevealTreePaths() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QDir{media.path()}.mkpath(QStringLiteral("artist/album"));
    const auto root_bytes = QFile::encodeName(media.path());
    const auto nested = media.filePath(QStringLiteral("artist/album"));
    const auto nested_bytes = QFile::encodeName(nested);
    {
        QSettings settings;
        settings.setValue(QStringLiteral("library/roots"), QVariantList{root_bytes});
        settings.setValue(QStringLiteral("library/bookmarks"), QVariantList{nested_bytes});
    }

    BenchMainWindow window;
    window.show();
    auto* bookmarks = window.findChild<QListWidget*>(QStringLiteral("bench-folder-bookmarks"));
    auto* folder_view = window.findChild<QTreeView*>(QStringLiteral("bench-folder-tree"));
    auto* folder_model = window.findChild<ui::LocalFolderTreeModel*>();
    QVERIFY(bookmarks != nullptr);
    QVERIFY(folder_view != nullptr);
    QVERIFY(folder_model != nullptr);
    QTRY_COMPARE(bookmarks->count(), 1);
    QCOMPARE(bookmarks->item(0)->text(), QStringLiteral("album"));
    QTRY_VERIFY(bookmarks->isVisibleTo(&window));

    // Activating the bookmark walks the lazy tree to the nested directory.
    const std::string nested_raw{nested_bytes.constData(),
                                 static_cast<std::size_t>(nested_bytes.size())};
    emit bookmarks->activated(bookmarks->model()->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(folder_view->currentIndex().isValid() &&
                                 folder_model->rawPath(folder_view->currentIndex()) == nested_raw,
                             5'000);

    // The tree is the whole filesystem now: exactly one root, "/".
    QCOMPARE(folder_model->rowCount(), 1);
    QCOMPARE(folder_model->rawPath(folder_model->index(0, 0)), std::string{"/"});

    // Bookmarking from the tree persists, deduplicates, and removal both
    // updates the list and the stored value.
    auto* add_action = window.findChild<QAction*>(QStringLiteral("action-folder-bookmark-add"));
    auto* remove_action =
        window.findChild<QAction*>(QStringLiteral("action-folder-bookmark-remove"));
    QVERIFY(add_action != nullptr);
    QVERIFY(remove_action != nullptr);
    add_action->trigger();
    QCOMPARE(bookmarks->count(), 1);
    folder_view->setCurrentIndex(folder_model->index(0, 0));
    add_action->trigger();
    QCOMPARE(bookmarks->count(), 2);
    bookmarks->setCurrentRow(0);
    remove_action->trigger();
    QCOMPARE(bookmarks->count(), 1);
    {
        const QSettings settings;
        const auto stored = settings.value(QStringLiteral("library/bookmarks")).toList();
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.front().toByteArray(), QByteArray{"/"});
    }
    {
        QSettings settings;
        settings.remove(QStringLiteral("library/bookmarks"));
    }
}

void BenchMainWindowTest::folderBookmarksMigrateFromLibraryRoots() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto migrated_bytes = QFile::encodeName(media.path());
    {
        QSettings settings;
        settings.remove(QStringLiteral("library/bookmarks"));
        settings.setValue(QStringLiteral("library/roots"), QVariantList{migrated_bytes});
    }
    BenchMainWindow window;
    window.show();
    auto* bookmarks = window.findChild<QListWidget*>(QStringLiteral("bench-folder-bookmarks"));
    QVERIFY(bookmarks != nullptr);
    // Home first, then the migrated root.
    QTRY_COMPARE(bookmarks->count(), 2);
    QCOMPARE(bookmarks->item(0)->data(Qt::UserRole).toByteArray(),
             QFile::encodeName(QDir::homePath()));
    QCOMPARE(bookmarks->item(1)->data(Qt::UserRole).toByteArray(), migrated_bytes);
    QVERIFY(!bookmarks->item(0)->icon().isNull());
    {
        QSettings settings;
        settings.remove(QStringLiteral("library/roots"));
        settings.remove(QStringLiteral("library/bookmarks"));
    }
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
                            return operations::commit_flac_metadata_source(
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
        QTRY_VERIFY(guard.isNull());
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
    // The field was shown and read back but never saved, so every TCP engine
    // refused the empty token it was sent.
    auto* engine_token =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-engine-token"));
    QVERIFY(engine_token);
    engine_token->setText(QStringLiteral("  0123abcd "));

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
    QVERIFY(note->text().contains(QStringLiteral("choose Save")));
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
    dialog->showPage(SettingsDialog::Page::metadata_services);
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
    QCOMPARE(QSettings{}.value(QLatin1String(SettingsDialog::library_engine_token_key)).toString(),
             QStringLiteral("0123abcd"));
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
    dialog->showPage(SettingsDialog::Page::library);
    auto* roots = dialog->findChild<QListWidget*>(QStringLiteral("local-library-roots"));
    QVERIFY(roots);
    QTRY_COMPARE(roots->count(), 1);
    QCOMPARE(roots->item(0)->data(Qt::UserRole).toByteArray().toStdString(), raw_root);
    QVERIFY(roots->item(0)->text().contains(QStringLiteral("not scanned")));
    QVERIFY(!panel->property("scanning").toBool());
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
    QCOMPARE(pages->currentItem()->text(), QStringLiteral("Library"));
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
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
    // No limit until one is chosen: existing behaviour is unchanged.
    QCOMPARE(embedded_edge->value(), 0);
    QCOMPARE(embedded_edge->text(), QStringLiteral("No limit"));
    QCOMPARE(SettingsDialog::artworkPolicy().max_embedded_edge, 0U);
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
    QCOMPARE(policy.fetch_source, std::string{"coverartarchive"});
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
                            return operations::commit_flac_metadata_source(
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
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
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
    QTRY_VERIFY(!section->isBusy());
    QTRY_VERIFY(section->hasPendingChanges());
    QCOMPARE(section->pendingIntents().size(), embed ? 2U : 1U);
    const auto destination = media.filePath(QStringLiteral("folder.png"));
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
    QTRY_VERIFY(guard.isNull());
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
                            return operations::commit_flac_metadata_source(
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
    QTRY_VERIFY(guard.isNull());
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

void BenchMainWindowTest::artworkFetchesCoverArtFromArchiveAndAddsFront() {
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
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {},
        [database_path] {
            return MetadataWritePlanApplier{
                [database_path](const metadata::MetadataWritePlan& plan,
                                const operations::MetadataApplyProgressCallback& progress,
                                const core::CancellationToken& cancellation)
                    -> core::Result<operations::MetadataApplyResult> {
                    auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
                    if (!opened) {
                        return std::unexpected(std::move(opened.error()));
                    }
                    auto journal = std::move(*opened);
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&journal](const metadata::MetadataWritePlanSource& source_plan,
                                   const core::CancellationToken& source_cancellation) {
                            return operations::commit_flac_metadata_source(
                                source_plan, journal,
                                [](const operations::MetadataCommitResult&) -> core::Result<void> {
                                    return {};
                                },
                                source_cancellation);
                        },
                        {}, {}, progress, cancellation);
                }};
        },
        [&tags_observed](const operations::MetadataApplyResult& result) { tags_observed = result; },
        {}, {}, {}, {}, nullptr, {}, service);
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
    auto* save = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-save"));
    QVERIFY(save != nullptr);
    QTRY_VERIFY(save->isEnabled());
    auto* pending =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-pending"));
    QVERIFY(pending != nullptr);
    QTRY_VERIFY(!pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    const auto preview = pending->model()->index(0, 5).data(Qt::DecorationRole).value<QImage>();
    QVERIFY(preview.width() <= 60 && preview.height() <= 60);
    QCOMPARE(preview.pixelColor(0, 0), QColor{Qt::darkCyan});
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
    QCOMPARE(added->mime_type, std::string{"image/png"});
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
    QTRY_VERIFY(guard.isNull());
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
    QCOMPARE(second_pending->model()
                 ->index(1, 5)
                 .data(Qt::DecorationRole)
                 .value<QImage>()
                 .pixelColor(0, 0),
             QColor{Qt::darkMagenta});
    auto* second_discard = second_properties->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-artwork-discard"));
    QVERIFY(second_discard != nullptr);
    second_discard->click();
    QCOMPARE(second_pending->model()->rowCount(), 0);
    QTRY_VERIFY(second_fetch->isEnabled());
    second_fetch->click();
    QTRY_VERIFY(second_save->isEnabled());
    QTRY_VERIFY(
        !second_pending->model()->index(1, 5).data(Qt::DecorationRole).value<QImage>().isNull());
    QCOMPARE(second_pending->model()
                 ->index(1, 5)
                 .data(Qt::DecorationRole)
                 .value<QImage>()
                 .pixelColor(0, 0),
             QColor{Qt::darkMagenta});
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
    QCOMPARE(new_front->byte_size, static_cast<std::uint64_t>(second_png_bytes.size()));

    QPointer second_guard{second_properties};
    second_properties->close();
    QTRY_VERIFY(second_guard.isNull());
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
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-covers"));
    QVERIFY(items != nullptr);
    QVERIFY(covers != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 1, 5'000);
    QTRY_VERIFY(covers->isEnabled());
    QTest::mouseClick(covers, Qt::LeftButton);

    // The picker lists every archive image with its type and comment.
    QDialog* picker = nullptr;
    QTRY_VERIFY((picker = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-artwork-archive"))) != nullptr);
    auto* list =
        picker->findChild<QTreeWidget*>(QStringLiteral("bench-metadata-artwork-archive-list"));
    auto* use =
        picker->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-archive-use"));
    QVERIFY(list != nullptr);
    QVERIFY(use != nullptr);
    QCOMPARE(list->topLevelItemCount(), 2);
    QCOMPARE(list->topLevelItem(0)->text(1), QStringLiteral("Front"));
    QCOMPARE(list->topLevelItem(1)->text(1), QStringLiteral("Back"));
    QCOMPARE(list->topLevelItem(1)->text(3), QStringLiteral("tray insert"));
    // The scripted service already answered the thumbnail request.
    QTRY_VERIFY(!list->topLevelItem(1)->icon(0).isNull());

    list->setCurrentItem(list->topLevelItem(1));
    QTest::mouseClick(use, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-artwork-archive")) ==
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
    QCOMPARE(added->mime_type, std::string{"image/png"});
    QCOMPARE(added->byte_size, static_cast<std::uint64_t>(back_bytes.size()));

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
        QDragEnterEvent enter{position, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                              modifiers};
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
        QCOMPARE(initial_split->widget(1)->objectName(), QStringLiteral("bench-tabs"));
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
    QTRY_VERIFY(!bar->isHidden());
    auto* query = bar->findChild<QLineEdit*>(QStringLiteral("bench-list-find-query"));
    QVERIFY(query != nullptr);
    QTRY_VERIFY(query->hasFocus());
    query->setText(QStringLiteral("find me"));
    QTRY_COMPARE(view->currentIndex().row(), 0);
    QTest::keyClick(query, Qt::Key_F3);
    QTRY_COMPARE(view->currentIndex().row(), 1);
    QTest::keyClick(query, Qt::Key_F3, Qt::ShiftModifier);
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
    QCOMPARE(model->rowCount(), 3);
    QCOMPARE(view->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(view->selectionModel()->selectedRows().front().row(), 1);
    QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
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
        QTRY_COMPARE(view->rowHeight(2), view->rowHeight(1));
        QImage singleton_cover{12, 12, QImage::Format_RGB32};
        singleton_cover.fill(Qt::red);
        local_model->setArtwork(local_model->groupKey(2), singleton_cover);
        QCoreApplication::processEvents();
        const auto artwork_rect = view->visualRect(local_model->index(2, local_artwork_column));
        const auto artwork_render = view->viewport()->grab(artwork_rect).toImage();
        bool found_inline_cover = false;
        auto leftmost_cover_pixel = artwork_render.width();
        for (int y = 0; y < artwork_render.height(); ++y) {
            for (int x = 0; x < artwork_render.width(); ++x) {
                if (artwork_render.pixelColor(x, y) == QColor(Qt::red)) {
                    found_inline_cover = true;
                    leftmost_cover_pixel = std::min(leftmost_cover_pixel, x);
                }
            }
        }
        QVERIFY(found_inline_cover);
        QVERIFY(leftmost_cover_pixel >= artwork_render.width() / 2);

        view->selectionModel()->select(local_model->index(2, 0),
                                       QItemSelectionModel::ClearAndSelect |
                                           QItemSelectionModel::Rows);
        QCoreApplication::processEvents();
        const auto selected_artwork_render = view->viewport()->grab(artwork_rect).toImage();
        QCOMPARE(selected_artwork_render, artwork_render);
        view->clearSelection();
        local_model->setCurrentSource(local_model->source(2), 2);
        QCoreApplication::processEvents();
        const auto active_artwork_render = view->viewport()->grab(artwork_rect).toImage();
        QCOMPARE(active_artwork_render, artwork_render);

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
        date->trigger();
        QVERIFY(view->isColumnHidden(local_date_column));
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
        QVERIFY(view->isColumnHidden(local_date_column));
        QVERIFY(restored.close());
    }

    const QByteArray future_layout{
        R"({"schema":99,"presentation":"future","columns":[],"keep":true})"};
    const auto database_path = std::filesystem::path{
        QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                          QStringLiteral("/lists.sqlite"))
            .toStdString()};
    {
        auto repository = persistence::ListRepository::open(database_path);
        QVERIFY(repository.has_value());
        const std::vector presets{persistence::TrackViewPreset{
            .binding = binding.toStdString(),
            .header_state = std::string{future_layout.constData(),
                                        static_cast<std::size_t>(future_layout.size())},
        }};
        QVERIFY(repository->replace_view_presets(presets).has_value());
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
    auto repository = persistence::ListRepository::open(database_path);
    QVERIFY(repository.has_value());
    const auto presets = repository->load_view_presets();
    QVERIFY(presets.has_value());
    // One per list tab; the MPD queue's own preset went with the queue.
    QCOMPARE(presets->size(), 1U);
    const auto stored =
        std::ranges::find(*presets, binding.toStdString(), &persistence::TrackViewPreset::binding);
    QVERIFY(stored != presets->end());
    const QByteArray stored_layout{stored->header_state.data(),
                                   static_cast<qsizetype>(stored->header_state.size())};
    QCOMPARE(stored_layout, future_layout);
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
        QCOMPARE(tabs->tabText(copied_index), QStringLiteral("Local Queue copy *"));
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
    QCOMPARE(tabs->tabText(local_index), QStringLiteral("Local Queue *"));
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
    if (const auto directory = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
    const auto screenshot_dir = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
    QVERIFY2(read_only->text().contains(QStringLiteral("No pending edits")),
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

void BenchMainWindowTest::metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes() {
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
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {});
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
    const auto screenshot_dir = qEnvironmentVariable("TRACKKNIFE_TEST_SCREENSHOT_DIR");
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
        QCOMPARE(rg->text(), QStringLiteral("RG: Automatic"));
        single->trigger();
        single->trigger();
        QCOMPARE(single->iconText(), QStringLiteral("1×"));
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
    // Persisted preamps reach the playback worker on the next start.
    {
        QSettings settings;
        settings.setValue(QStringLiteral("playback/rg-preamp-with"), 4.5);
        settings.setValue(QStringLiteral("playback/rg-preamp-without"), -3.0);
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

    // The serialized library queue proves both identities reached the store.
    std::optional<std::vector<unsigned>> stored;
    panel->requestRatings({track_hash, album_hash},
                          [&stored](std::vector<unsigned> values) { stored = std::move(values); });
    QTRY_VERIFY(stored.has_value());
    QCOMPARE(*stored, (std::vector<unsigned>{8U, 6U}));
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
    stored.reset();
    panel->requestRatings({track_hash},
                          [&stored](std::vector<unsigned> values) { stored = std::move(values); });
    QTRY_VERIFY(stored.has_value());
    QCOMPARE(*stored, (std::vector<unsigned>{0U}));
}

void BenchMainWindowTest::autoAdvancesOncePerFinishedTrack() {
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

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::BenchMainWindowTest)

#include "bench_main_window_test.moc"
