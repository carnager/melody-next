// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/convert_dialog.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/local_list_model.hpp"
#include "bench/metadata_grid_model.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "bench/mpd_library_search_model.hpp"
#include "bench/playlist_transfer_bar.hpp"
#include "bench/search_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/track_list_find_bar.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "quick/mpd_search_result_model.hpp"
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
#include "trackknife/persistence/operation_journal.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/line_slider.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/panel_layout.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"

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
#include <QFile>
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
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
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
    void cleanup();
    void transportUsesStackedNowPlayingAndCompactDeviceButton();
    void unifiesMpdAndLocalAuthoritiesInOneWorkspace();
    void mpdSearchProjectsControllerResults();
    void mpdSearchResolvesCompleteAlbums();
    void mpdSearchCoversFollowLibraryRefresh();
    void mpdQueueAndLibraryMenusExposeServerActions();
    void mpdGoToArtistAlbumNavigatesLibrary();
    void mpdStoredPlaylistTabsFollowServerAuthority();
    void playbackBufferProfilesPersistAndExposeDiagnostics();
    void statusBarSummarizesTrackSelection();
    void committedMetadataRefreshesDuplicatesAndPreservesCueOverlay();
    void metadataReadyPlanAppliesAndRefreshesHistory();
    void metadataApplyCancellationPreservesDraftForFreshPreview();
    void metadataDialogLayoutsPersistAsynchronously();
    void metadataGridReusesExactNativeFieldWithoutInvalidIndexes();
    void metadataTransformationChainPreviewsAndStagesOneUndo();
    void metadataCapturePatternSavesReloadsAndStagesAllFields();
    void preparationSidePanelEditsReusableOutputProfiles();
    void pathOnlyPreparationUsesActualTagsAndAppliesReviewedPlan();
    void combinedTagAndRenameReviewReachesPreparationApply();
    void metadataSuggestionsStageSelectionConsistency();
    void musicBrainzIdentifyStagesChosenVersion_data();
    void musicBrainzIdentifyStagesChosenVersion();
    void musicBrainzFingerprintScanRanksAndStages();
    void replayGainScanStagesMeasuredGainsAsDrafts();
    void replayGainScanUsesTruePeakWhenOptedIn();
    void replayGainScanStagesR128ForOpusTags();
    void propertiesShowTechnicalSummary();
    void searchDialogFiltersTabAndOpensResults();
    void searchDialogProbesMissingTechnicalsOnDemand();
    void contextReplayGainScansAndApplies();
    void loudnessSidecarProjectsOntoProbedRows();
    void desktopNotificationsNotifyBackgroundTrackChanges();
    void replayGainScanPreservesLogicalSources_data();
    void replayGainScanPreservesLogicalSources();
    void convertDialogPlansAndConvertsSelection();
    void settingsControlStartupContextAndMusicRoot();
    void folderBookmarksRevealTreePaths();
    void folderBookmarksMigrateFromLibraryRoots();
    void artworkFetchesCoverArtFromArchiveAndAddsFront();
    void artworkArchivePickerAddsChosenImageWithItsRole();
    void automaticScriptsStageOnOpen();
    void metadataStartupPresentsReconciliation();
    void filePublicationStartupPresentsReconciliation();
    void combinedPublicationStartupRecoversMetadataAndPath();
    void folderDiscoveryAdmitsWave64();
    void contextMenusTargetSelectionsListsAndFolders();
    void panelLayoutPersistsAndPreservesFutureState();
    void trackViewLayoutMatchesGroupedQueueAndPersists();
    void localReorderPreservesVisibleRowGeometry();
    void localListUndoRestoresOccurrencesAndFreshMetadata();
    void localListHistoryBranchesAndBounds();
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

  private:
    QTemporaryDir settings_directory_;
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

void BenchMainWindowTest::cleanup() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QSettings settings;
    settings.clear();
    settings.sync();
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
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
    QTRY_COMPARE(tabs->count(), 2);

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
    QVERIFY(window.property("trackknife-player-device-generation").isValid());
    QVERIFY(window.property("trackknife-player-default-output").isValid());
    QCOMPARE(transport->findChildren<QToolButton*>(QString{}, Qt::FindDirectChildrenOnly).size(),
             4);

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

void BenchMainWindowTest::unifiesMpdAndLocalAuthoritiesInOneWorkspace() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* mpd_queue = dynamic_cast<ui::QueueTableView*>(
        window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue")));
    auto* folder_view = window.findChild<QTreeView*>(QStringLiteral("bench-folder-tree"));
    auto* mpd_library = window.findChild<QTreeView*>(QStringLiteral("bench-mpd-library"));
    auto* local_source_tabs = window.findChild<QTabBar*>(QStringLiteral("bench-local-source-tabs"));
    auto* mpd_source_tabs = window.findChild<QTabBar*>(QStringLiteral("bench-mpd-source-tabs"));
    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    auto* plain = window.findChild<QAction*>(QStringLiteral("action-track-layout-plain"));
    auto* copy_layout = window.findChild<QAction*>(QStringLiteral("action-copy-track-layout"));
    auto* buffer_menu = window.findChild<QMenu*>(QStringLiteral("bench-buffer-menu"));
    auto* device = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("bench-mpd-search"));
    auto* search_surface = window.findChild<QWidget*>(QStringLiteral("bench-mpd-search-surface"));
    auto* repeat = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-repeat"));
    auto* random = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-random"));
    auto* single = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-single"));
    auto* consume = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-consume"));
    auto* replaygain = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-replaygain"));
    QVERIFY(tabs != nullptr);
    QVERIFY(mpd_queue != nullptr);
    QVERIFY(folder_view != nullptr);
    QVERIFY(mpd_library != nullptr);
    QVERIFY(local_source_tabs != nullptr);
    QVERIFY(mpd_source_tabs != nullptr);
    QCOMPARE(local_source_tabs->tabText(0), QStringLiteral("Folders"));
    QCOMPARE(local_source_tabs->tabText(1), QStringLiteral("Library"));
    QCOMPARE(mpd_source_tabs->tabText(0), QStringLiteral("Library"));
    QCOMPARE(mpd_source_tabs->tabText(1), QStringLiteral("Playlists"));
    QVERIFY(properties != nullptr);
    QVERIFY(plain != nullptr);
    QVERIFY(copy_layout != nullptr);
    QVERIFY(buffer_menu != nullptr);
    QVERIFY(device != nullptr);
    QVERIFY(search != nullptr);
    QVERIFY(search_surface != nullptr);
    QVERIFY(repeat != nullptr);
    QVERIFY(random != nullptr);
    QVERIFY(single != nullptr);
    QVERIFY(consume != nullptr);
    QVERIFY(replaygain != nullptr);
    QCOMPARE(replaygain->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
    QVERIFY(replaygain->text().startsWith(QStringLiteral("RG: ")));
    QVERIFY(replaygain->menu() != nullptr);
    QCOMPARE(replaygain->menu()->actions().size(), 4);
    QTRY_COMPARE(tabs->count(), 2);
    auto* local_queue = dynamic_cast<ui::QueueTableView*>(tabs->currentWidget());
    QVERIFY(local_queue != nullptr);
    QVERIFY(local_queue != mpd_queue);
    QCOMPARE(mpd_queue->model()->columnCount(), local_column_count);
    QCOMPARE(local_queue->model()->columnCount(), local_column_count);
    QCOMPARE(mpd_queue->property("trackknife-hover-row").toInt(), -1);
    QCOMPARE(local_queue->property("trackknife-hover-row").toInt(), -1);
    for (int column = 0; column < local_column_count; ++column) {
        QCOMPARE(mpd_queue->model()->headerData(column, Qt::Horizontal).toString(),
                 local_queue->model()->headerData(column, Qt::Horizontal).toString());
        QCOMPARE(mpd_queue->isColumnHidden(column), local_queue->isColumnHidden(column));
        QCOMPARE(mpd_queue->horizontalHeader()->visualIndex(column),
                 local_queue->horizontalHeader()->visualIndex(column));
        QCOMPARE(mpd_queue->columnWidth(column), local_queue->columnWidth(column));
    }
    QVERIFY(qobject_cast<ui::QueueItemDelegate*>(mpd_queue->itemDelegate()) != nullptr);
    QVERIFY(qobject_cast<ui::QueueItemDelegate*>(local_queue->itemDelegate()) != nullptr);
    QVERIFY(tabs->cornerWidget(Qt::TopRightCorner) == nullptr);
    QVERIFY(
        window.findChild<QWidget*>(QStringLiteral("bench-panel-folders"))->isAncestorOf(search));
    QVERIFY(!search->isVisible());
    QVERIFY(!repeat->isVisible());
    QVERIFY(!random->isVisible());
    QVERIFY(!single->isVisible());
    QVERIFY(!consume->isVisible());

    const auto mpd_index = tabs->indexOf(mpd_queue);
    QVERIFY(mpd_index >= 0);
    QCOMPARE(tabs->tabText(mpd_index), QStringLiteral("MPD Queue"));
    auto* mpd_close = tabs->tabBar()->tabButton(mpd_index, QTabBar::RightSide);
    QVERIFY(mpd_close == nullptr || !mpd_close->isVisible());
    tabs->setCurrentWidget(mpd_queue);
    QTRY_COMPARE(window.property("trackknife-active-authority").toString(), QStringLiteral("mpd"));
    QVERIFY(mpd_source_tabs->isVisible());
    QVERIFY(!local_source_tabs->isVisible());
    QVERIFY(mpd_library->isVisible());
    QVERIFY(!folder_view->isVisible());
    QVERIFY(!properties->isEnabled());
    QVERIFY(!buffer_menu->isEnabled());
    QCOMPARE(device->accessibleName(), QStringLiteral("MPD output"));
    QVERIFY(plain->isEnabled());
    QVERIFY(search->isVisible());
    QVERIFY(repeat->isVisible());
    QVERIFY(random->isVisible());
    QVERIFY(single->isVisible());
    QVERIFY(consume->isVisible());
    QCOMPARE(single->text(), QStringLiteral("1"));
    QCOMPARE(consume->text(), QStringLiteral("C"));
    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    QVERIFY(!search_surface->isVisible());
    QVERIFY(mpd_library->isVisible());
    QVERIFY(tabs->cornerWidget(Qt::TopRightCorner) == nullptr);
    auto* sources = window.findChild<QWidget*>(QStringLiteral("bench-panel-folders"));
    QVERIFY(sources != nullptr);
    QVERIFY(sources->isAncestorOf(search));
    QVERIFY(search->geometry().right() < sources->width());
    QCOMPARE(tabs->tabBar()->maximumWidth(), QWIDGETSIZE_MAX);

    plain->trigger();
    copy_layout->trigger();
    QVERIFY(qobject_cast<ui::QueueItemDelegate*>(mpd_queue->itemDelegate()) == nullptr);
    tabs->setCurrentWidget(local_queue);
    QTRY_COMPARE(window.property("trackknife-active-authority").toString(),
                 QStringLiteral("local"));
    QVERIFY(local_source_tabs->isVisible());
    QVERIFY(!mpd_source_tabs->isVisible());
    QVERIFY(folder_view->isVisible());
    QVERIFY(!mpd_library->isVisible());
    QVERIFY(buffer_menu->isEnabled());
    QCOMPARE(device->accessibleName(), QStringLiteral("Audio output device"));
    QVERIFY(qobject_cast<ui::QueueItemDelegate*>(local_queue->itemDelegate()) == nullptr);
    QVERIFY(!search->isVisible());
    QVERIFY(!search_surface->isVisible());
    QVERIFY(!repeat->isVisible());
    QVERIFY(!random->isVisible());
    QVERIFY(!single->isVisible());
    QVERIFY(!consume->isVisible());
}

void BenchMainWindowTest::mpdSearchProjectsControllerResults() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* mpd_queue = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    auto* field = window.findChild<QLineEdit*>(QStringLiteral("bench-mpd-search"));
    auto* surface = window.findChild<QWidget*>(QStringLiteral("bench-mpd-search-surface"));
    auto* results = window.findChild<QTreeView*>(QStringLiteral("bench-mpd-search-results"));
    auto* result_model = window.findChild<quick::MpdSearchResultModel*>();
    auto* controller = window.findChild<quick::MpdProbeController*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(mpd_queue != nullptr);
    QVERIFY(field != nullptr);
    QVERIFY(surface != nullptr);
    QVERIFY(results != nullptr);
    QVERIFY(result_model != nullptr);
    QVERIFY(controller != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    tabs->setCurrentWidget(mpd_queue);

    auto* library = qobject_cast<quick::MpdQueueModel*>(controller->libraryModel());
    QVERIFY(library != nullptr);
    mpd::Metadata metadata{{
        {"Artist", "Search Artist"},
        {"AlbumArtist", "Search Artist"},
        {"Album", "Search Album"},
        {"Date", "2026"},
        {"Track", "1"},
        {"Title", "Search Result"},
    }};
    library->replaceTracks({mpd::Track{
        .uri = "search/result.flac",
        .metadata = std::move(metadata),
        .musicbrainz = {},
        .queue_id = std::nullopt,
        .queue_position = std::nullopt,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    }});
    field->setText(QStringLiteral("S"));
    QTest::qWait(220);
    QVERIFY(QMetaObject::invokeMethod(controller, "searchFinished", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("S")), Q_ARG(bool, true)));
    QTRY_VERIFY(result_model->firstResultRow() >= 0);
    // ADR-0140: Enter commits the displayed hits as a query-keyed tab;
    // recommitting the same query reuses it, and it closes like any tab.
    const auto tab_count = tabs->count();
    QTest::keyClick(field, Qt::Key_Return);
    QTRY_COMPARE(tabs->count(), tab_count + 1);
    auto* search_view = window.findChild<QTableView*>(QStringLiteral("bench-mpd-search-tab-view"));
    QVERIFY(search_view != nullptr);
    QCOMPARE(search_view->property("bench-mpd-search-query").toString(), QStringLiteral("S"));
    QCOMPARE(tabs->tabText(tabs->indexOf(search_view)), QStringLiteral("Search: S"));
    QCOMPARE(search_view->model()->rowCount(), 1);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget*>(search_view));
    tabs->setCurrentWidget(mpd_queue);
    QTest::keyClick(field, Qt::Key_Return);
    QTRY_COMPARE(tabs->currentWidget(), static_cast<QWidget*>(search_view));
    QCOMPARE(tabs->count(), tab_count + 1);
    auto* close_action = window.findChild<QAction*>(QStringLiteral("action-close-tab"));
    QVERIFY(close_action != nullptr);
    QVERIFY(close_action->isEnabled());
    close_action->trigger();
    QTRY_COMPARE(tabs->count(), tab_count);
    tabs->setCurrentWidget(mpd_queue);
    const auto first = result_model->firstResultRow();
    QCOMPARE(result_model->index(first, 1).data().toString(), QStringLiteral("Search Result"));
    QCOMPARE(result_model->index(first, 4).data(Qt::ToolTipRole).toString(),
             QStringLiteral("Append to queue (Enter)"));

    auto* tree_model = qobject_cast<MpdLibrarySearchModel*>(results->model());
    QVERIFY(tree_model);
    QCOMPARE(tree_model->index(0, 0).data().toString(), QStringLiteral("Albums"));
    QCOMPARE(tree_model->index(1, 0).data().toString(), QStringLiteral("Tracks"));
    QCOMPARE(tree_model->index(0, 0, tree_model->index(1, 0)).data().toString(),
             QStringLiteral("Search Artist — 01. Search Result"));

    QSignalSpy artwork_requests{result_model, &quick::MpdSearchResultModel::artworkRequested};
    result_model->replaceTracks({mpd::Track{
        .uri = "search/album-cover.flac",
        .metadata = mpd::Metadata{{
            {"Artist", "Cover Artist"},
            {"AlbumArtist", "Cover Artist"},
            {"Album", "Cover Album"},
            {"Date", "2026"},
            {"Track", "1"},
            {"Title", "Covered Track"},
        }},
        .musicbrainz = {},
        .queue_id = std::nullopt,
        .queue_position = std::nullopt,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    }});
    QTRY_VERIFY(!artwork_requests.isEmpty());
    const auto album_row = result_model->firstResultRow();
    QVERIFY(album_row >= 0);
    QImage cover{160, 160, QImage::Format_ARGB32_Premultiplied};
    cover.fill(Qt::blue);
    result_model->acceptArtwork(artwork_requests.front().front().toULongLong(), cover);
    QVERIFY(!result_model->index(album_row, 0).data(Qt::DecorationRole).value<QImage>().isNull());
    auto* local_tree = window.findChild<QTreeView*>(QStringLiteral("local-library-tree"));
    QVERIFY(local_tree);
    QVERIFY(dynamic_cast<ui::ServerLibraryTreeView*>(results));
    QVERIFY(dynamic_cast<ui::ServerLibraryTreeDelegate*>(results->itemDelegate()));
    QCOMPARE(results->selectionMode(), local_tree->selectionMode());
    QCOMPARE(results->indentation(), local_tree->indentation());
    QCOMPARE(results->frameShape(), local_tree->frameShape());
    QCOMPARE(results->dragDropMode(), local_tree->dragDropMode());
    QCOMPARE(results->expandsOnDoubleClick(), local_tree->expandsOnDoubleClick());
    const auto album = tree_model->index(0, 0, tree_model->index(0, 0));
    QCOMPARE(album.data().toString(), QStringLiteral("Cover Album"));
    QCOMPARE(album.data(MpdLibrarySearchModel::ArtistRole).toString(),
             QStringLiteral("Cover Artist"));

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    QTRY_VERIFY(surface->isVisible());
    results->viewport()->repaint();
    const auto album_cell = results->visualRect(album);
    QVERIFY(!album_cell.isEmpty());
    const auto album_render = results->viewport()->grab(album_cell).toImage();
    auto blue_left = album_render.width();
    auto blue_right = -1;
    auto blue_top = album_render.height();
    auto blue_bottom = -1;
    for (int y = 0; y < album_render.height(); ++y) {
        for (int x = 0; x < album_render.width(); ++x) {
            if (album_render.pixelColor(x, y) == QColor(Qt::blue)) {
                blue_left = std::min(blue_left, x);
                blue_right = std::max(blue_right, x);
                blue_top = std::min(blue_top, y);
                blue_bottom = std::max(blue_bottom, y);
            }
        }
    }
    QVERIFY(blue_right >= blue_left);
    QVERIFY(blue_bottom >= blue_top);
    const auto painted_cover_width = blue_right - blue_left + 1;
    const auto painted_cover_height = blue_bottom - blue_top + 1;
    QVERIFY(painted_cover_width <= album_render.height());
    QVERIFY(painted_cover_height <= album_render.height());
    QVERIFY(std::abs(painted_cover_width - painted_cover_height) <= 1);
    if (qEnvironmentVariableIsSet("TRACKKNIFE_SEARCH_SCREENSHOT")) {
        surface->parentWidget()->parentWidget()->grab().save(
            qEnvironmentVariable("TRACKKNIFE_SEARCH_SCREENSHOT"));
    }
    QCOMPARE(album_cell.height(), 42);
    QCOMPARE(painted_cover_width, 28);
    results->setCurrentIndex(album);
    results->setFocus();
    QVERIFY(!results->isExpanded(album));
    QTest::keyClick(results, Qt::Key_Return);
    QVERIFY(results->isExpanded(album));
    QCOMPARE(results->currentIndex(), album);
    QTest::keyClick(results, Qt::Key_Left);
    QVERIFY(!results->isExpanded(album));
    QTest::keyClick(results, Qt::Key_Right);
    QVERIFY(results->isExpanded(album));
    const auto track = tree_model->index(0, 0, tree_model->index(1, 0));
    results->selectionModel()->select(track,
                                      QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(results->selectionModel()->selectedRows().size(), 2);
    tree_model->setMore(true);
    QVERIFY(tree_model->index(1, 0, tree_model->index(1, 0))
                .data(MpdLibrarySearchModel::MoreRole)
                .toBool());
    tree_model->setMore(false);
    QCOMPARE(tree_model->rowCount(tree_model->index(1, 0)), 1);
    mpd_queue->setFocus();
    QTRY_VERIFY(surface->isVisible());
    auto* sources = window.findChild<QWidget*>(QStringLiteral("bench-panel-folders"));
    auto* tree = window.findChild<QTreeView*>(QStringLiteral("bench-mpd-library"));
    auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("bench-source-stack"));
    QVERIFY(sources && tree && stack);
    QVERIFY(sources->isAncestorOf(field));
    QVERIFY(sources->isAncestorOf(surface));
    QVERIFY(stack->currentWidget()->isAncestorOf(surface));
    QVERIFY(!tree->isVisible());
    const auto retained_count = result_model->rowCount();
    tabs->setCurrentIndex(1);
    QVERIFY(!field->isVisible());
    QVERIFY(!surface->isVisible());
    tabs->setCurrentWidget(mpd_queue);
    QVERIFY(surface->isVisible());
    QCOMPARE(field->text(), QStringLiteral("S"));
    QCOMPARE(result_model->rowCount(), retained_count);
    // Query edits remove stale actionable results immediately. A late reply
    // for the old query must not replace the new search or the browse tree.
    field->setText(QStringLiteral("Different"));
    QCOMPARE(result_model->rowCount(), 0);
    QVERIFY(QMetaObject::invokeMethod(controller, "searchFinished", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("S")), Q_ARG(bool, true)));
    QCOMPARE(result_model->rowCount(), 0);
    field->clear();
    QVERIFY(field->text().isEmpty());
    QVERIFY(stack->currentWidget()->isAncestorOf(tree));
    QVERIFY(!surface->isVisible());
    QVERIFY(tree->isVisible());
    QVERIFY(QMetaObject::invokeMethod(controller, "searchFinished", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Different")),
                                      Q_ARG(bool, true)));
    QCOMPARE(result_model->rowCount(), 0);
}

void BenchMainWindowTest::mpdSearchCoversFollowLibraryRefresh() {
    BenchMainWindow window;
    window.show();
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* model = window.findChild<quick::MpdSearchResultModel*>();
    auto* field = window.findChild<QLineEdit*>(QStringLiteral("bench-mpd-search"));
    auto* tree = window.findChild<QTreeView*>(QStringLiteral("bench-mpd-search-results"));
    QVERIFY(controller && model && field && tree);
    field->setText(QStringLiteral("Album"));
    QTest::qWait(250);
    mpd::Track track;
    track.uri = "album/01.flac";
    track.metadata = mpd::Metadata{{{"Artist", "Artist"}, {"Album", "Album"}}};
    QSignalSpy requests{model, &quick::MpdSearchResultModel::artworkRequested};
    model->replaceTracks({track});
    QVERIFY(!requests.empty());
    const auto old_token = requests.last().at(0).toULongLong();
    QImage old_cover{32, 32, QImage::Format_RGB32};
    old_cover.fill(Qt::red);
    model->acceptArtwork(old_token, old_cover);
    const QPersistentModelIndex album{tree->model()->index(0, 0, tree->model()->index(0, 0))};
    const auto color = [&] {
        const auto image = album.data(Qt::DecorationRole).value<QIcon>().pixmap(32, 32).toImage();
        return image.isNull() ? QColor{} : image.pixelColor(16, 16);
    };
    QCOMPARE(color(), QColor{Qt::red});
    const auto previous_requests = requests.size();
    QVERIFY(QMetaObject::invokeMethod(controller, "serverDatabaseChanged", Qt::DirectConnection));
    QTRY_VERIFY(requests.size() > previous_requests);
    QVERIFY(album.isValid());
    QCOMPARE(field->text(), QStringLiteral("Album"));
    const auto fresh_token = requests.last().at(0).toULongLong();
    QVERIFY(fresh_token != old_token);
    QImage fresh_cover{32, 32, QImage::Format_RGB32};
    fresh_cover.fill(Qt::blue);
    model->acceptArtwork(fresh_token, fresh_cover);
    QCOMPARE(color(), QColor{Qt::blue});
    model->acceptArtwork(old_token, old_cover);
    QCOMPARE(color(), QColor{Qt::blue});
    // Repeating the same query gets fresh artwork too.
    model->replaceTracks({track});
    QVERIFY(requests.last().at(0).toULongLong() != fresh_token);
}

void BenchMainWindowTest::mpdSearchResolvesCompleteAlbums() {
    quick::MpdSearchResultModel source;
    MpdLibrarySearchModel tree{&source};
    mpd::Track first{};
    first.uri = "album/01.flac";
    first.metadata = mpd::Metadata{
        {{"AlbumArtist", "Artist"}, {"Album", "Album"}, {"Title", "First"}, {"Track", "1"}}};
    auto second = first;
    second.uri = "album/02.flac";
    second.metadata = mpd::Metadata{
        {{"AlbumArtist", "Artist"}, {"Album", "Album"}, {"Title", "Second"}, {"Track", "2/2"}}};
    source.replaceTracks({second}); // The search matched only one track of this release.
    QSignalSpy requests{&tree, &MpdLibrarySearchModel::albumRequested};
    const auto album = tree.index(0, 0, tree.index(0, 0));
    const auto matching_track = tree.index(0, 0, tree.index(1, 0));
    QStringList resolved;
    QString error;
    int completions = 0;
    tree.resolve({matching_track, album}, [&](QStringList uris, QString problem) {
        resolved = std::move(uris);
        error = std::move(problem);
        ++completions;
    });
    QCOMPARE(completions, 0);
    QCOMPARE(requests.size(), 1);
    tree.loadAlbum(album); // Expansion joins the existing complete-release request.
    QCOMPARE(requests.size(), 1);
    tree.acceptAlbum(requests.front().front().toULongLong(), {second, first}, {});
    QCOMPARE(completions, 1);
    QVERIFY(error.isEmpty());
    QCOMPARE(resolved, QStringList({"album/01.flac", "album/02.flac"}));
    QCOMPARE(tree.rowCount(album), 2);
    QCOMPARE(tree.index(1, 0, album).data().toString(), QStringLiteral("02. Second"));
    QCOMPARE(album.data().toString(), QStringLiteral("Album (2)"));
    tree.resolve({album, tree.index(0, 0, album), matching_track},
                 [&](QStringList uris, QString problem) {
                     resolved = std::move(uris);
                     error = std::move(problem);
                     ++completions;
                 });
    QCOMPARE(completions, 2);
    QCOMPARE(resolved.size(), 2);
    QCOMPARE(requests.size(), 1);

    source.replaceTracks({second});
    tree.resolve({tree.index(0, 0, tree.index(0, 0))}, [&](QStringList uris, QString problem) {
        resolved = std::move(uris);
        error = std::move(problem);
        ++completions;
    });
    QCOMPARE(requests.size(), 2);
    const auto stale_token = requests.back().front().toULongLong();
    source.replaceTracks({});
    QCOMPARE(completions, 3);
    QVERIFY(resolved.isEmpty());
    QVERIFY(!error.isEmpty());
    tree.acceptAlbum(stale_token, {first, second}, {});
    QCOMPARE(completions, 3);
    QCOMPARE(tree.index(0, 0, tree.index(0, 0)).data().toString(), QStringLiteral("No matches"));
}

void BenchMainWindowTest::mpdQueueAndLibraryMenusExposeServerActions() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* queue = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* track_menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    auto* priority_menu = window.findChild<QMenu*>(QStringLiteral("bench-mpd-priority-menu"));
    auto* library = dynamic_cast<ui::ServerLibraryTreeView*>(
        window.findChild<QTreeView*>(QStringLiteral("bench-mpd-library")));
    auto* library_model = window.findChild<ui::ServerLibraryTreeModel*>();
    auto* library_menu = window.findChild<QMenu*>(QStringLiteral("bench-mpd-library-context-menu"));
    auto* replaygain = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-replaygain"));
    QVERIFY(tabs != nullptr);
    QVERIFY(queue != nullptr);
    QVERIFY(controller != nullptr);
    QVERIFY(track_menu != nullptr);
    QVERIFY(priority_menu != nullptr);
    QVERIFY(library != nullptr);
    QVERIFY(library_model != nullptr);
    QVERIFY(library_menu != nullptr);
    QVERIFY(replaygain != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    tabs->setCurrentWidget(queue);

    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(controller->queueModel());
    QVERIFY(queue_model != nullptr);
    queue_model->replaceTracks({mpd::Track{
        .uri = "queue/prioritized.flac",
        .metadata = mpd::Metadata{{{"Title", "Prioritized"}}},
        .musicbrainz = {},
        .queue_id = 17U,
        .queue_position = 0U,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = 192U,
        .unknown_structural_pairs = {},
    }});
    queue->scrollTo(queue_model->index(0, 0));
    const auto queue_position = queue->visualRect(queue_model->index(0, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(queue, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, queue_position)));
    QVERIFY(track_menu->actions().contains(
        window.findChild<QAction*>(QStringLiteral("action-mpd-add-next-selection"))));
    QVERIFY(track_menu->actions().contains(
        window.findChild<QAction*>(QStringLiteral("action-mpd-append-selection"))));
    QVERIFY(track_menu->actions().contains(
        window.findChild<QAction*>(QStringLiteral("action-mpd-crop-selection"))));
    QVERIFY(track_menu->actions().contains(priority_menu->menuAction()));
    auto* high = window.findChild<QAction*>(QStringLiteral("action-mpd-queue-priority-192"));
    QVERIFY(high != nullptr);
    QVERIFY(high->isChecked());
    QVERIFY(!priority_menu->isEnabled()); // Disconnected capability gate.
    track_menu->close();

    QCOMPARE(replaygain->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(replaygain->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
    QCOMPARE(replaygain->accessibleName(), QStringLiteral("MPD ReplayGain mode"));

    QSignalSpy root_requests{library_model, &ui::ServerLibraryTreeModel::rootRequested};
    library_model->reload();
    QTRY_COMPARE(root_requests.size(), 1);
    const auto token = root_requests.front().at(0).toULongLong();
    const auto tag = root_requests.front().at(1).toString();
    library_model->acceptRoot(token, tag, {QStringLiteral("Library Artist")}, {});
    const auto root = library_model->index(0, 0);
    QVERIFY(root.isValid());
    QVERIFY(root.flags().testFlag(Qt::ItemIsDragEnabled));
    QVERIFY(library->dragEnabled());
    QVERIFY(dynamic_cast<ui::ServerLibraryTreeDelegate*>(library->itemDelegate()) != nullptr);
    library->scrollTo(root);
    QTRY_VERIFY(!library->visualRect(root).isEmpty());

    QSignalSpy branch_requests{library_model, &ui::ServerLibraryTreeModel::branchRequested};
    library_model->fetchMore(root);
    QTRY_COMPARE(branch_requests.size(), 1);
    const auto branch_token = branch_requests.front().front().toULongLong();
    library_model->acceptBranch(branch_token,
                                {mpd::Track{
                                    .uri = "library/artist/album/01.flac",
                                    .metadata = mpd::Metadata{{
                                        {"Artist", "Library Artist"},
                                        {"AlbumArtist", "Library Artist"},
                                        {"Album", "Library Album"},
                                        {"Date", "2026"},
                                        {"Track", "1"},
                                        {"Title", "Library Track"},
                                    }},
                                    .musicbrainz = {},
                                    .queue_id = std::nullopt,
                                    .queue_position = std::nullopt,
                                    .duration = std::chrono::milliseconds{180'000},
                                    .last_modified = std::nullopt,
                                    .audio_format = std::nullopt,
                                    .priority = std::nullopt,
                                    .unknown_structural_pairs = {},
                                }},
                                {});
    QVERIFY(library_model->rowCount(root) > 0);
    QVERIFY(!library->isExpanded(root));
    QStyleOptionViewItem branch_option;
    branch_option.initFrom(library);
    branch_option.rect = library->visualRect(root);
    const auto disclosure = library->style()->subElementRect(QStyle::SE_TreeViewDisclosureItem,
                                                             &branch_option, library);
    QVERIFY(!disclosure.isEmpty());
    QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier, disclosure.center());
    QTRY_VERIFY(library->isExpanded(root));
    QCoreApplication::processEvents();
    QVERIFY(library->isExpanded(root)); // Disclosure click toggles exactly once.

    const auto library_position = library->visualRect(root).center();
    QVERIFY(QMetaObject::invokeMethod(library, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, library_position)));
    QCOMPARE(library_menu->actions().at(0)->text(), QStringLiteral("Append to live queue"));
    QCOMPARE(library_menu->actions().at(1)->text(), QStringLiteral("Insert next in live queue"));
    QCOMPARE(library_menu->actions().at(2)->text(), QStringLiteral("Replace queue and play"));
    library_menu->close();
}

void BenchMainWindowTest::mpdGoToArtistAlbumNavigatesLibrary() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* queue = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* library = window.findChild<QTreeView*>(QStringLiteral("bench-mpd-library"));
    auto* library_model = window.findChild<ui::ServerLibraryTreeModel*>();
    auto* go_to_artist = window.findChild<QAction*>(QStringLiteral("action-mpd-go-to-artist"));
    auto* go_to_album = window.findChild<QAction*>(QStringLiteral("action-mpd-go-to-album"));
    QVERIFY(tabs != nullptr);
    QVERIFY(queue != nullptr);
    QVERIFY(controller != nullptr);
    QVERIFY(library != nullptr);
    QVERIFY(library_model != nullptr);
    QVERIFY(go_to_artist != nullptr);
    QVERIFY(go_to_album != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    tabs->setCurrentWidget(queue);

    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(controller->queueModel());
    QVERIFY(queue_model != nullptr);
    queue_model->replaceTracks({mpd::Track{
        .uri = "queue/target.flac",
        .metadata = mpd::Metadata{{
            {"Title", "Queued"},
            {"Artist", "Library Artist"},
            {"AlbumArtist", "Library Artist"},
            {"Album", "Library Album"},
        }},
        .musicbrainz = {},
        .queue_id = 7U,
        .queue_position = 0U,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    }});
    queue->setCurrentIndex(queue_model->index(0, 0));

    QSignalSpy root_requests{library_model, &ui::ServerLibraryTreeModel::rootRequested};
    QSignalSpy branch_requests{library_model, &ui::ServerLibraryTreeModel::branchRequested};

    // Go to album on an unloaded library: the tree loads its roots, then
    // the artist's branch, then lands on the album row.
    go_to_album->trigger();
    QTRY_COMPARE(root_requests.size(), 1);
    library_model->acceptRoot(
        root_requests.front().at(0).toULongLong(), root_requests.front().at(1).toString(),
        {QStringLiteral("Another Artist"), QStringLiteral("Library Artist")}, {});
    QTRY_COMPARE(branch_requests.size(), 1);
    library_model->acceptBranch(branch_requests.front().front().toULongLong(),
                                {mpd::Track{
                                    .uri = "library/artist/album/01.flac",
                                    .metadata = mpd::Metadata{{
                                        {"Artist", "Library Artist"},
                                        {"AlbumArtist", "Library Artist"},
                                        {"Album", "Library Album"},
                                        {"Track", "1"},
                                        {"Title", "Library Track"},
                                    }},
                                    .musicbrainz = {},
                                    .queue_id = std::nullopt,
                                    .queue_position = std::nullopt,
                                    .duration = std::chrono::milliseconds{180'000},
                                    .last_modified = std::nullopt,
                                    .audio_format = std::nullopt,
                                    .priority = std::nullopt,
                                    .unknown_structural_pairs = {},
                                }},
                                {});
    QTRY_VERIFY(library->currentIndex().isValid());
    QTRY_VERIFY(library->currentIndex()
                    .data(Qt::DisplayRole)
                    .toString()
                    .startsWith(QStringLiteral("Library Album")));
    QVERIFY(library->isExpanded(library->currentIndex().parent()));
    QCOMPARE(library->currentIndex()
                 .parent()
                 .data(ui::ServerLibraryTreeModel::QueryValueRole)
                 .toString(),
             QStringLiteral("Library Artist"));

    // Go to artist selects the loaded artist row directly, and an artist
    // the tree does not know reports instead of navigating.
    go_to_artist->trigger();
    QTRY_COMPARE(
        library->currentIndex().data(ui::ServerLibraryTreeModel::QueryValueRole).toString(),
        QStringLiteral("Library Artist"));
    queue_model->replaceTracks({mpd::Track{
        .uri = "queue/unknown.flac",
        .metadata = mpd::Metadata{{
            {"Title", "Unknown"},
            {"AlbumArtist", "Nobody Known"},
            {"Album", "Nowhere"},
        }},
        .musicbrainz = {},
        .queue_id = 8U,
        .queue_position = 0U,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    }});
    queue->setCurrentIndex(queue_model->index(0, 0));
    go_to_artist->trigger();
    QTRY_VERIFY(
        window.statusBar()->currentMessage().contains(QStringLiteral("not in the library tree")));
}

// ADR-0129: the sidebar Playlists list opens one closable tab per server
// playlist name, contents follow only authoritative re-reads, and edits are
// server commands — a disconnected controller must leave rows untouched.
void BenchMainWindowTest::mpdStoredPlaylistTabsFollowServerAuthority() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* playlists = window.findChild<QListWidget*>(QStringLiteral("bench-mpd-playlists"));
    auto* source_tabs = window.findChild<QTabBar*>(QStringLiteral("bench-mpd-source-tabs"));
    auto* source_pages =
        window.findChild<QStackedWidget*>(QStringLiteral("bench-mpd-source-pages"));
    auto* remove_selected =
        window.findChild<QAction*>(QStringLiteral("action-remove-selected-tracks"));
    auto* close_tab = window.findChild<QAction*>(QStringLiteral("action-close-tab"));
    QVERIFY(tabs != nullptr);
    QVERIFY(controller != nullptr);
    QVERIFY(playlists != nullptr);
    QVERIFY(source_tabs != nullptr);
    QVERIFY(source_pages != nullptr);
    QVERIFY(remove_selected != nullptr);
    QVERIFY(close_tab != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* queue = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    QVERIFY(queue != nullptr);
    tabs->setCurrentWidget(queue);    // MPD context shows the library panel.
    QVERIFY(!playlists->isVisible()); // The Library page is the default.

    emit controller->storedPlaylistListLoaded(
        {QStringLiteral("Quiet mix"), QStringLiteral("Road mix")});
    QCOMPARE(playlists->count(), 2);
    // The ADR-0130 sidebar tab bar switches to the full-height Playlists page.
    source_tabs->setCurrentIndex(1);
    QTRY_VERIFY(playlists->isVisible());
    QCOMPARE(source_pages->currentWidget(), playlists);

    // Activating a sidebar entry opens exactly one tab keyed by the name.
    emit playlists->itemActivated(playlists->item(1));
    QTRY_COMPARE(tabs->count(), 3);
    auto* view = window.findChild<QTableView*>(QStringLiteral("bench-mpd-playlist-view"));
    QVERIFY(view != nullptr);
    QCOMPARE(view->property("bench-mpd-playlist-name").toString(), QStringLiteral("Road mix"));
    QCOMPARE(tabs->currentWidget(), view);
    QCOMPARE(tabs->tabText(tabs->indexOf(view)), QStringLiteral("Road mix"));
    emit playlists->itemActivated(playlists->item(1));
    QCOMPARE(tabs->count(), 3); // Reopening refreshes instead of duplicating.

    // Contents arrive only through the authoritative playlist re-read.
    auto* playlist_model = qobject_cast<quick::MpdQueueModel*>(controller->browserPlaylistModel());
    QVERIFY(playlist_model != nullptr);
    playlist_model->replaceTracks({mpd::Track{
        .uri = "Artist/Release/01.flac",
        .metadata = mpd::Metadata{{{"Title", "Stored one"}, {"Artist", "Credited Artist"}}},
        .musicbrainz = {},
        .queue_id = std::nullopt,
        .queue_position = std::nullopt,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    }});
    emit controller->storedPlaylistLoaded(QStringLiteral("Road mix"));
    QTRY_COMPARE(view->model()->rowCount(), 1);

    // Removal is a server command: disconnected, the rows must stay visible.
    view->selectionModel()->select(view->model()->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    remove_selected->trigger();
    QCOMPARE(view->model()->rowCount(), 1);

    // The playlist tab is an MPD-authority surface and stays closable.
    QCOMPARE(window.property("trackknife-active-authority").toString(), QStringLiteral("mpd"));
    QVERIFY(close_tab->isEnabled());

    // Server-side rename retargets the open tab; deletion closes it.
    emit controller->storedPlaylistRenamed(QStringLiteral("Road mix"),
                                           QStringLiteral("Highway mix"));
    QTRY_COMPARE(tabs->tabText(tabs->indexOf(view)), QStringLiteral("Highway mix"));
    QCOMPARE(view->property("bench-mpd-playlist-name").toString(), QStringLiteral("Highway mix"));
    emit controller->storedPlaylistDeleted(QStringLiteral("Highway mix"));
    QTRY_COMPARE(tabs->count(), 2);

    // Disconnect clears the sidebar list.
    emit controller->storedPlaylistListLoaded({});
    QCOMPARE(playlists->count(), 0);
}

void BenchMainWindowTest::playbackBufferProfilesPersistAndExposeDiagnostics() {
    {
        QSettings settings;
        settings.setValue(QStringLiteral("playback/buffer-profile"), QStringLiteral("responsive"));
        settings.sync();
    }

    BenchMainWindow window;
    window.show();
    QCoreApplication::processEvents();

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
    QVERIFY(responsive->isChecked());
    QVERIFY(!balanced->isChecked());
    QCOMPARE(responsive->toolTip(), QStringLiteral("250 ms capacity; playback starts at 50 ms"));
    QCOMPARE(custom->text(), QStringLiteral("Custom…"));
    QTRY_COMPARE(window.property("trackknife-player-buffer-capacity-ms").toLongLong(), 250);
    QCOMPARE(window.property("trackknife-player-active-buffer-capacity-ms").toLongLong(), -1);
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

    QTimer::singleShot(0, [] {
        auto* dialog = QApplication::activeModalWidget();
        QVERIFY(dialog != nullptr);
        auto* capacity = dialog->findChild<QSpinBox*>(QStringLiteral("bench-buffer-capacity"));
        auto* threshold =
            dialog->findChild<QSpinBox*>(QStringLiteral("bench-buffer-start-threshold"));
        QVERIFY(capacity != nullptr);
        QVERIFY(threshold != nullptr);
        capacity->setValue(1'234);
        threshold->setValue(234);
        auto* custom_dialog = qobject_cast<QDialog*>(dialog);
        QVERIFY(custom_dialog != nullptr);
        custom_dialog->accept();
    });
    custom->trigger();
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
    QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_COMPARE(tabs->count(), 3);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget*>(properties));
    QVERIFY(!properties->isWindow());
    QVERIFY(tabs->tabText(tabs->currentIndex()).startsWith(QStringLiteral("Tags · 1 track")));
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
    QTRY_COMPARE(tabs->count(), 2);
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
    QSplitter* first_content = nullptr;
    QSplitter* first_metadata = nullptr;
    QTRY_VERIFY((first_content = first->findChild<QSplitter*>(
                     QStringLiteral("bench-metadata-content-splitter"))) != nullptr);
    QTRY_VERIFY((first_metadata = first->findChild<QSplitter*>(
                     QStringLiteral("bench-metadata-splitter"))) != nullptr);
    first->resize(930, 640);
    first_content->setSizes({610, 300});
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
    QTRY_COMPARE(saved_states.size(), 5);

    auto* second = create_properties();
    second->show();
    QSplitter* second_content = nullptr;
    QSplitter* second_metadata = nullptr;
    QTRY_VERIFY((second_content = second->findChild<QSplitter*>(
                     QStringLiteral("bench-metadata-content-splitter"))) != nullptr);
    QTRY_VERIFY((second_metadata = second->findChild<QSplitter*>(
                     QStringLiteral("bench-metadata-splitter"))) != nullptr);
    QTRY_COMPARE(second->height(), 640);
    QTRY_COMPARE(
        second_content->saveState(),
        saved_states.value(QStringLiteral("workspace/metadata-properties-content-splitter-v1")));
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
    auto* save_tags =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-preparation-save-tags"));
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
    auto* layout_name =
        properties->findChild<QLineEdit*>(QStringLiteral("bench-output-layout-name"));
    auto* layout_directory = properties->findChild<QLineEdit*>(
        QStringLiteral("bench-output-layout-directory-expression"));
    auto* layout_basename = properties->findChild<QLineEdit*>(
        QStringLiteral("bench-output-layout-basename-expression"));
    auto* layout_new =
        properties->findChild<QPushButton*>(QStringLiteral("bench-output-layout-new"));
    auto* layout_save =
        properties->findChild<QPushButton*>(QStringLiteral("bench-output-layout-save"));
    auto* destination_combo =
        properties->findChild<QComboBox*>(QStringLiteral("bench-destination-profile"));
    auto* destination_name =
        properties->findChild<QLineEdit*>(QStringLiteral("bench-destination-name"));
    auto* destination_root =
        properties->findChild<QLineEdit*>(QStringLiteral("bench-destination-root"));
    auto* destination_new =
        properties->findChild<QPushButton*>(QStringLiteral("bench-destination-new"));
    auto* destination_save =
        properties->findChild<QPushButton*>(QStringLiteral("bench-destination-save"));
    QVERIFY(layout_combo != nullptr);
    QVERIFY(layout_name != nullptr);
    QVERIFY(layout_directory != nullptr);
    QVERIFY(layout_basename != nullptr);
    QVERIFY(layout_new != nullptr);
    QVERIFY(layout_save != nullptr);
    QVERIFY(destination_combo != nullptr);
    QVERIFY(destination_name != nullptr);
    QVERIFY(destination_root != nullptr);
    QVERIFY(destination_new != nullptr);
    QVERIFY(destination_save != nullptr);
    QCOMPARE(layout_combo->count(), 1);
    QCOMPARE(layout_combo->currentText(), QStringLiteral("Albums"));
    QCOMPARE(layout_basename->text(), QStringLiteral("%tracknumber% - %title%"));
    QCOMPARE(destination_combo->count(), 1);
    QCOMPARE(destination_combo->currentText(), QStringLiteral("Library"));

    auto* layout_manage =
        properties->findChild<QPushButton*>(QStringLiteral("bench-output-layout-manage"));
    auto* layout_manager =
        properties->findChild<QDialog*>(QStringLiteral("bench-output-layout-manager"));
    QVERIFY(layout_manage != nullptr);
    QVERIFY(layout_manager != nullptr);
    QVERIFY(!layout_manager->isVisible());
    QTest::mouseClick(layout_manage, Qt::LeftButton);
    QTRY_VERIFY(layout_manager->isVisible());
    auto* layout_example =
        layout_manager->findChild<QLabel*>(QStringLiteral("bench-output-layout-example"));
    auto* layout_preview =
        layout_manager->findChild<QTreeWidget*>(QStringLiteral("bench-output-layout-preview"));
    auto* layout_example_timer =
        properties->findChild<QTimer*>(QStringLiteral("bench-output-layout-example-timer"));
    QVERIFY(layout_example != nullptr);
    QVERIFY(layout_preview != nullptr);
    QVERIFY(layout_example_timer != nullptr);
    // The preview table lists each track's resulting path, live.
    QTRY_VERIFY_WITH_TIMEOUT(layout_preview->topLevelItemCount() == 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        layout_preview->topLevelItem(0)->text(1).contains(QStringLiteral("Naming context")), 5'000);
    QVERIFY(layout_preview->topLevelItem(0)->text(1).contains(QStringLiteral(".flac")));
    QVERIFY(layout_preview->topLevelItem(0)->text(0).endsWith(QStringLiteral(".flac")));
    layout_basename->setText(QStringLiteral("$unknown(%title%)"));
    QVERIFY(layout_example_timer->isActive());
    QTRY_VERIFY_WITH_TIMEOUT(layout_example->text().startsWith(QStringLiteral("Preview error:")),
                             5'000);
    QCOMPARE(layout_preview->topLevelItemCount(), 0);
    layout_basename->setText(QStringLiteral("%title%"));
    QTRY_VERIFY_WITH_TIMEOUT(layout_preview->topLevelItemCount() == 1 &&
                                 layout_preview->topLevelItem(0)->text(1).contains(
                                     QStringLiteral("Naming context.flac")),
                             5'000);
    QVERIFY(layout_example->text().startsWith(QStringLiteral("Preview:")));
    QTest::mouseClick(layout_new, Qt::LeftButton);
    layout_name->setText(QStringLiteral("Artist folders"));
    layout_directory->setText(QStringLiteral("%artist%"));
    layout_basename->setText(QStringLiteral("%title%"));
    QTRY_VERIFY(layout_save->isEnabled());
    QTest::mouseClick(layout_save, Qt::LeftButton);
    QTRY_COMPARE(layouts.size(), 2U);
    QCOMPARE(layout_combo->count(), 2);
    QCOMPARE(layout_combo->currentText(), QStringLiteral("Artist folders"));

    auto* destination_manage =
        properties->findChild<QPushButton*>(QStringLiteral("bench-destination-manage"));
    auto* destination_manager =
        properties->findChild<QDialog*>(QStringLiteral("bench-destination-manager"));
    QVERIFY(destination_manage != nullptr);
    QVERIFY(destination_manager != nullptr);
    QTest::mouseClick(destination_manage, Qt::LeftButton);
    QTRY_VERIFY(destination_manager->isVisible());
    QTest::mouseClick(destination_new, Qt::LeftButton);
    destination_name->setText(QStringLiteral("Archive"));
    destination_root->setFocus();
    QTest::keyClicks(destination_root, media.filePath(QStringLiteral("archive")));
    QTRY_VERIFY(destination_save->isEnabled());
    QTest::mouseClick(destination_save, Qt::LeftButton);
    QTRY_COMPARE(destinations.size(), 2U);
    QCOMPARE(destination_combo->count(), 2);
    QCOMPARE(destination_combo->currentText(), QStringLiteral("Archive"));
    QCOMPARE(destinations.back().profile.root_raw_path,
             QFile::encodeName(media.filePath(QStringLiteral("archive"))).toStdString());

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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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
    // 17 step kinds under 4 unselectable group headers; kinds are found by
    // name because the row index no longer matches the action kind.
    QCOMPARE(kind->count(), 21);
    for (const auto& kind_name : {QStringLiteral("Capitalize first character"),
                                  QStringLiteral("Remove exact matching values"),
                                  QStringLiteral("Replace exact matching values"),
                                  QStringLiteral("Number by selected-file order"),
                                  QStringLiteral("Keep first characters of each value"),
                                  QStringLiteral("Remove field when condition matches"),
                                  QStringLiteral("Capture fields with tkcapture-1")}) {
        QVERIFY2(kind->findText(kind_name) >= 0, qPrintable(kind_name));
    }
    QVERIFY(!kind->model()->flags(kind->model()->index(0, 0)).testFlag(Qt::ItemIsSelectable));

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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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
    QTest::mouseClick(suggest, Qt::LeftButton);

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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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
        auto* local_files =
            dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-files"));
        auto* release_tracks =
            dialog->findChild<QTreeWidget*>(QStringLiteral("bench-musicbrainz-match-tracks"));
        auto* order =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-order"));
        auto* assign =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-assign"));
        auto* unmatch =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-unmatch"));
        QVERIFY(local_files && release_tracks && order && assign && unmatch);
        QTRY_COMPARE(local_files->topLevelItemCount(), 2);
        QVERIFY(!stage_matches->isEnabled());
        auto* sort =
            dialog->findChild<QPushButton*>(QStringLiteral("bench-musicbrainz-match-sort"));
        QVERIFY(sort);
        sort->click();
        QCOMPARE(local_files->topLevelItem(0)->text(0), QStringLiteral("2-first.flac"));
        order->click();
        QVERIFY(stage_matches->isEnabled());
        // Reverse the file-to-track mapping without reordering the source selection.
        local_files->setCurrentItem(local_files->topLevelItem(0));
        release_tracks->setCurrentItem(release_tracks->topLevelItem(1));
        QVERIFY(assign->isEnabled());
        assign->click();
        QVERIFY(local_files->topLevelItem(0)->text(2).contains(QStringLiteral("Two")));
        QVERIFY(local_files->topLevelItem(1)->text(2).contains(QStringLiteral("One")));
        release_tracks->setCurrentItem(release_tracks->topLevelItem(0));
        assign->click();
        unmatch->click();
        QCOMPARE(local_files->topLevelItem(0)->text(2), QStringLiteral("Unmatched"));
        assign->click();
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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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

void BenchMainWindowTest::settingsControlStartupContextAndMusicRoot() {
    // Startup in the MPD queue when the setting says so.
    {
        QSettings settings;
        settings.setValue(QLatin1String(SettingsDialog::startup_context_key),
                          QStringLiteral("mpd"));
        settings.sync();
    }
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_VERIFY(tabs->currentWidget() != nullptr &&
                    tabs->currentWidget()->objectName() == QStringLiteral("bench-mpd-queue"));

        // The settings dialog exposes both values and persists on Save.
        auto* settings_action = window.findChild<QAction*>(QStringLiteral("action-settings"));
        QVERIFY(settings_action != nullptr);
        settings_action->trigger();
        QDialog* dialog = nullptr;
        QTRY_VERIFY((dialog = window.findChild<QDialog*>(
                         QStringLiteral("bench-settings-dialog"))) != nullptr);
        auto* startup = dialog->findChild<QComboBox*>(QStringLiteral("bench-settings-startup"));
        auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("bench-settings-music-root"));
        auto* buttons =
            dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"));
        QVERIFY(startup != nullptr && root != nullptr && buttons != nullptr);
        QCOMPARE(startup->currentData().toString(), QStringLiteral("mpd"));
        startup->setCurrentIndex(startup->findData(QStringLiteral("local")));
        root->setText(QStringLiteral("/music/root"));
        QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        QTRY_VERIFY(window.findChild<QDialog*>(QStringLiteral("bench-settings-dialog")) == nullptr);
        const QSettings persisted;
        QCOMPARE(persisted.value(QLatin1String(SettingsDialog::startup_context_key)).toString(),
                 QStringLiteral("local"));
        QCOMPARE(persisted.value(QLatin1String(SettingsDialog::music_root_key)).toString(),
                 QStringLiteral("/music/root"));
    }

    // "Load as local files" resolves URIs below the music root into a new
    // local tab and reports the misses without failing.
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QDir{media.path()}.mkpath(QStringLiteral("Artist/Album"));
    const auto wav = media.filePath(QStringLiteral("Artist/Album/one.wav"));
    write_sine_wav_fixture(wav, 0.5);
    {
        QSettings settings;
        settings.setValue(QLatin1String(SettingsDialog::music_root_key), media.path());
        settings.sync();
    }
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_VERIFY(tabs->count() >= 2);
        const auto tabs_before = tabs->count();
        window.loadMpdUrisAsLocalFiles(
            {QStringLiteral("Artist/Album/one.wav"), QStringLiteral("Artist/Album/gone.wav")});
        QTRY_COMPARE(tabs->count(), tabs_before + 1);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        QTRY_COMPARE(view->model()->rowCount(), 1);
        QVERIFY(window.findChild<QStatusBar*>()->currentMessage().contains(
            QStringLiteral("1 track not found")));

        // The queue and library context actions exist for discoverability.
        QVERIFY(window.findChild<QAction*>(QStringLiteral("action-mpd-load-local")) != nullptr);

        // The library order toggle lives in the heading: hidden for local
        // folders, shown in MPD context, persisting the chosen mode.
        auto* order_az = window.findChild<QToolButton*>(QStringLiteral("bench-library-order-az"));
        auto* order_latest =
            window.findChild<QToolButton*>(QStringLiteral("bench-library-order-latest"));
        auto* mpd_queue = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
        QVERIFY(order_az != nullptr && order_latest != nullptr && mpd_queue != nullptr);
        QVERIFY(!order_az->isVisible() && !order_latest->isVisible());
        QVERIFY(order_az->isChecked());
        tabs->setCurrentWidget(mpd_queue);
        QTRY_VERIFY(order_az->isVisible() && order_latest->isVisible());
        QTest::mouseClick(order_latest, Qt::LeftButton);
        QVERIFY(order_latest->isChecked());
        {
            const QSettings persisted;
            QCOMPARE(persisted.value(QStringLiteral("mpd/library-order")).toString(),
                     QStringLiteral("latest"));
        }
        QTest::mouseClick(order_az, Qt::LeftButton);
        {
            const QSettings persisted;
            QCOMPARE(persisted.value(QStringLiteral("mpd/library-order")).toString(),
                     QStringLiteral("az"));
        }
    }

    // Leave a clean slate for the other window tests.
    QSettings settings;
    settings.remove(QLatin1String(SettingsDialog::startup_context_key));
    settings.remove(QLatin1String(SettingsDialog::music_root_key));
    settings.remove(QStringLiteral("mpd/library-order"));
    settings.sync();
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
    QVERIFY(preset != nullptr && root != nullptr && directories != nullptr && names != nullptr);
    QVERIFY(preview != nullptr && run != nullptr && status != nullptr && problems != nullptr);
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
    auto* preset_delete =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-convert-preset-delete"));
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
    QVERIFY(editor_buttons->button(QDialogButtonBox::Save)->isEnabled());
    QTest::mouseClick(editor_buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QTRY_COMPARE(preset_catalog.size(), 1U);
    QCOMPARE(preset_catalog.front().preset.codec_name, std::string{"libopus"});
    QCOMPARE(preset_catalog.front().preset.bit_rate, std::optional<std::int64_t>{96'000});
    // Separator + the saved preset, selected as the current choice.
    QTRY_COMPARE(preset->count(), builtin_count + 2);
    QCOMPARE(preset->currentText(), QStringLiteral("Phone Opus"));
    QTRY_VERIFY(preset_delete->isVisible());
    QTest::mouseClick(preset_delete, Qt::LeftButton);
    QTRY_VERIFY(preset_catalog.empty());
    QTRY_COMPARE(preset->count(), builtin_count);
    QTRY_VERIFY(!preset_delete->isVisible());

    preset->setCurrentIndex(preset->findData(QStringLiteral("opus-192")));
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

// ADR-0144: notifications fire only for track changes while playing in
// the background, never retro-notify, and the menu toggle persists.
void BenchMainWindowTest::desktopNotificationsNotifyBackgroundTrackChanges() {
    DesktopNotifier notifier;
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
    QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    auto* true_peak =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-replaygain-true-peak"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(scan != nullptr);
    QVERIFY(true_peak != nullptr);
    QVERIFY(!true_peak->isChecked());
    true_peak->setChecked(true);
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
    true_peak->setChecked(false);
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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* scan = properties->findChild<QPushButton*>(QStringLiteral("bench-replaygain-scan"));
    auto* sidecar_only =
        properties->findChild<QCheckBox*>(QStringLiteral("bench-replaygain-sidecar-only"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(scan != nullptr);
    QVERIFY(sidecar_only != nullptr);
    QVERIFY(!sidecar_only->isChecked());
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
    sidecar_only->setChecked(true);
    QTest::mouseClick(scan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN")).has_value(), 15'000);
    const auto conventional_column =
        grid_model->fieldColumn(QStringLiteral("REPLAYGAIN_TRACK_GAIN"));
    const auto conventional =
        grid_model->index(0, *conventional_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(conventional.size(), 1);
    QVERIFY(conventional.front().endsWith(QStringLiteral(" dB")));

    sidecar_only->setChecked(false);
    delete properties;
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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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
    QTRY_COMPARE(tabs->count(), 2);
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

// ADR-0153: a technical query against rows without retained technicals
// probes exactly those files once and reports the facts back.
void BenchMainWindowTest::searchDialogProbesMissingTechnicalsOnDemand() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto flac_path = media.filePath(QStringLiteral("tone.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), flac_path));
    const auto encoded = QFile::encodeName(flac_path);

    LocalTrackRow row;
    row.raw_path = std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    row.title = "Restored";
    row.probed = true;

    std::vector<std::pair<std::string, LocalTrackTechnicals>> reported;
    SearchDialog dialog{std::filesystem::path{},
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
    mode->setChecked(false);
}

// ADR-0156: the context-menu ReplayGain dialog scans the selection and
// writes immediately through the shared journaled pipeline — here two
// unwritable WAVs divert into loudness sidecars (ADR-0143).
void BenchMainWindowTest::contextReplayGainScansAndApplies() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first = media.filePath(QStringLiteral("one.wav"));
    const auto second = media.filePath(QStringLiteral("two.wav"));
    write_sine_wav_fixture(first, 0.6);
    write_sine_wav_fixture(second, 0.3);

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(
        {QFile::encodeName(first).toStdString(), QFile::encodeName(second).toStdString()});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    QTRY_COMPARE(view->model()->rowCount(), 2);
    view->selectAll();

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
    QVERIFY2(status->text().startsWith(QStringLiteral("Applied gains to 2")),
             qPrintable(status->text() + QStringLiteral(" · ") +
                        (problems ? problems->toPlainText() : QString{})));

    // Unwritable WAVs landed in sidecars with the measured track gains.
    for (const auto& path : {first, second}) {
        const auto encoded = QFile::encodeName(path);
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
    QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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
    auto* files = properties->findChild<QTableView*>(QStringLiteral("bench-metadata-files"));
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
    sections->setCurrentIndex(1);
    auto* items =
        properties->findChild<QTableView*>(QStringLiteral("bench-metadata-artwork-items"));
    auto* fetch =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-artwork-fetch-cover"));
    QVERIFY(items != nullptr);
    QVERIFY(fetch != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 1, 5'000);

    // One unambiguous release across the selection enables the fetch; the
    // whole download-and-add runs as one direct apply with no dialogs.
    QTRY_VERIFY(fetch->isEnabled());
    QTest::mouseClick(fetch, Qt::LeftButton);
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
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
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
    QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_COMPARE(tabs->count(), 2);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 1);
    QCOMPARE(model->rawPath(0), wave64_raw);
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
    QTRY_COMPARE(tabs->count(), 2);
    window.openLocalPaths(raw_paths);
    auto* source = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(source != nullptr);
    QTRY_COMPARE(source->model()->rowCount(), 3);

    duplicate->trigger();
    QCOMPARE(tabs->count(), 3);
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
    QVERIFY(track_menu->actions().contains(properties));
    QVERIFY(track_menu->actions().contains(convert));
    QVERIFY(remove->isEnabled());
    QCOMPARE(copy_menu->actions().size(), 1);
    QCOMPARE(move_menu->actions().size(), 1);
    copy_menu->actions().front()->trigger();
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
        QTRY_COMPARE(track_tabs->count(), 2);
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
    QTRY_COMPARE(tabs->count(), 2);
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

    auto* mpd = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    QVERIFY(mpd != nullptr);
    tabs->setCurrentWidget(mpd);
    QVERIFY(bar->isHidden());
    QVERIFY(find->isEnabled() && next->isEnabled() && previous->isEnabled());
    auto* mpd_model = qobject_cast<quick::MpdQueueModel*>(mpd->model());
    QVERIFY(mpd_model != nullptr);
    mpd::Track server_track;
    server_track.queue_id = 41U;
    server_track.uri = "server/remote.flac";
    server_track.metadata = mpd::Metadata{{{"Title", "Server song"}}};
    mpd_model->replaceTracks({server_track});
    mpd->setFocus();
    QTest::keyClick(mpd, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(query->hasFocus());
    query->setText(QStringLiteral("server song"));
    auto* status = bar->findChild<QLabel*>(QStringLiteral("bench-list-find-status"));
    QVERIFY(status != nullptr);
    QTRY_COMPARE(status->text(), QStringLiteral("Track 1 of 1"));
    QCOMPARE(mpd_model->queueIdAt(mpd->currentIndex().row()), std::optional<std::uint32_t>{41U});
    QCOMPARE(view->currentIndex().row(), 0);
    tabs->setCurrentWidget(view);
    QVERIFY(find->isEnabled());
    QVERIFY(bar->isHidden());
    QCOMPARE(query->text(), QStringLiteral("server song"));

    view->selectionModel()->select(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(properties_action != nullptr);
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();
    QTRY_VERIFY(qobject_cast<MetadataPropertiesDialog*>(tabs->currentWidget()) != nullptr);
    QVERIFY(!find->isEnabled() && !next->isEnabled() && !previous->isEnabled());
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
    model.appendRows({second}); // Structural operations outside this history start a new baseline.
    QVERIFY(!model.canUndo());
    QVERIFY(!model.canRedo());
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

void BenchMainWindowTest::localListUndoActionsRespectAuthorityAndTextEditing() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs);
    QTRY_COMPARE(tabs->count(), 2);
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
    auto* mpd = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    tabs->setCurrentWidget(mpd);
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
    QTRY_COMPARE(tabs->count(), 2);
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
    auto* mpd = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    tabs->setCurrentWidget(mpd);
    QVERIFY(!sort_menu->isEnabled());
    QVERIFY(!reverse->isEnabled());
    QVERIFY(!deduplicate->isEnabled());
    QVERIFY(bar->isHidden());
    // Even a directly invoked action cannot use a stale local destination.
    reverse->trigger();
    QCOMPARE(model->rowCount(), 3);
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
    QTRY_COMPARE(restored_tabs->count(), 2);
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
        QTRY_COMPARE(tabs->count(), 2);
        auto* local_view = qobject_cast<QTableView*>(tabs->currentWidget());
        auto* local_model = qobject_cast<LocalListModel*>(local_view->model());
        QVERIFY(local_model);
        auto* import_action = window.findChild<QAction*>(QStringLiteral("action-import-m3u8"));
        auto* export_action = window.findChild<QAction*>(QStringLiteral("action-export-m3u8"));
        auto* mpd = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
        tabs->setCurrentWidget(mpd);
        QVERIFY(import_action->isEnabled());
        QVERIFY(!export_action->isEnabled());
        const auto mpd_count = mpd->model()->rowCount();
        auto* bar = window.findChild<PlaylistTransferBar*>();
        QSignalSpy completed{bar, &PlaylistTransferBar::completed};
        window.importM3u8Path(QFile::encodeName(playlist).toStdString());
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(completed[0][0].toBool());
        QCOMPARE(tabs->count(), 3);
        QVERIFY(export_action->isEnabled());
        QCOMPARE(local_model->rowCount(), 0);
        QCOMPARE(mpd->model()->rowCount(), mpd_count);
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
    QTRY_COMPARE(tabs->count(), 3);
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
        QTRY_COMPARE(tabs->count(), 2);
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
        QTRY_COMPARE(tabs->count(), 2);
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
        QTRY_COMPARE(tabs->count(), 2);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(fallback.statusBar()->currentMessage().contains(QStringLiteral("preserved")));
        QVERIFY(fallback.close());
    }
    auto repository = persistence::ListRepository::open(database_path);
    QVERIFY(repository.has_value());
    const auto presets = repository->load_view_presets();
    QVERIFY(presets.has_value());
    QCOMPARE(presets->size(), 2U);
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
        QTRY_COMPARE(tabs->count(), 2);

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
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->currentIndex(), 2);
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
        QCOMPARE(tabs->count(), 3);
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->done(QMessageBox::Yes);
            }
        });
        close->trigger();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentWidget(), source);
        QVERIFY(pin->isChecked());
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
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
        QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_COMPARE(tabs->count(), 2);
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
    auto* files = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-files"));
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
    QTRY_COMPARE(tabs->count(), 2);
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

    auto* dialog = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    QVERIFY(!dialog->isModal());
    QVERIFY(!dialog->isWindow());
    QTRY_COMPARE(tabs->count(), 3);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget*>(dialog));
    QVERIFY(tabs->tabText(tabs->currentIndex()).startsWith(QStringLiteral("Tags · 2 tracks")));
    QCOMPARE(dialog->windowTitle(), QStringLiteral("Edit tags"));
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-loading")) != nullptr);

    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-files"))) !=
                nullptr);
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
    QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Qt::DirectConnection,
                                      Q_ARG(int, tabs->currentIndex())));
    QTRY_COMPARE(tabs->count(), 2);
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
    QCOMPARE(artwork->findChildren<QPushButton*>().size(), 8);
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
    QTRY_COMPARE_WITH_TIMEOUT(items->model()->rowCount(), 2, 5'000);
    QVERIFY(add->isEnabled());
    items->selectRow(1);
    QTRY_VERIFY(!replace->isEnabled());
    QVERIFY(!remove->isEnabled());
    QVERIFY(copy->isEnabled());
    items->clearSelection();
    items->selectRow(0);
    QTRY_VERIFY(replace->isEnabled());
    QTRY_VERIFY(remove->isEnabled());
    QVERIFY(!copy->isEnabled());
    QTest::mouseClick(remove, Qt::LeftButton);

    // Direct apply: the checked removal runs immediately; no review or
    // progress dialog appears and the inventory refreshes itself.
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
        QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_COMPARE(tabs->count(), 2);
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
        QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_COMPARE(tabs->count(), 2);
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
        QTRY_COMPARE(tabs->count(), 2);
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
    QTRY_COMPARE(tabs->count(), 2);
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
        QTRY_VERIFY(tabs->count() >= 2);
        tabs->setCurrentIndex(1);
        auto* repeat = window.findChild<QAction*>(QStringLiteral("action-local-repeat"));
        auto* random = window.findChild<QAction*>(QStringLiteral("action-local-random"));
        auto* single = window.findChild<QAction*>(QStringLiteral("action-local-single"));
        auto* consume = window.findChild<QAction*>(QStringLiteral("action-local-consume"));
        auto* automatic =
            window.findChild<QAction*>(QStringLiteral("action-local-replaygain-auto"));
        auto* rg = window.findChild<QToolButton*>(QStringLiteral("bench-local-replaygain"));
        QVERIFY(repeat && random && single && consume && automatic && rg);
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
        tabs->setCurrentIndex(0);
        QVERIFY(!rg->isVisible());
        QVERIFY(!repeat->isEnabled());
        QVERIFY(!automatic->isEnabled());
        // Local commands remain scoped even when invoked programmatically.
        random->trigger();
        QVERIFY(random->isChecked());
        tabs->setCurrentIndex(1);
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
    QTRY_VERIFY(window.property("trackknife-player-state").toInt() >= 3);
    if (window.property("trackknife-player-state").toInt() == 8) {
        QSKIP("live PipeWire playback unavailable");
    }
    // Progression must continue while the MPD authority is visible.
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(0);
    QTest::qWait(track_ms * 3 + 600);
    QCOMPARE(model->rowCount(), expected_count);
    if (ended) {
        QTRY_COMPARE(window.property("trackknife-player-state").toInt(), 7);
    } else {
        QVERIFY(window.property("trackknife-player-state").toInt() != 7);
        QVERIFY(window.property("trackknife-player-state").toInt() != 8);
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
