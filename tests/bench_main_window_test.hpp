// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The window test (bench_main_window_test*.cpp), its cases in several files
// so they compile side by side: what they share is here.

#include "bench/animated_panel_dock.hpp"
#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/catalogue_source.hpp"
#include "bench/convert_dialog.hpp"
#include "bench/cover_thumbnail.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/dynamic_playlist_service.hpp"
#include "bench/engine_folder_dialog.hpp"
#include "bench/identify_albums_dialog.hpp"
#include "bench/lastfm_service.hpp"
#include "bench/list_view_dialog.hpp"
#include "bench/lists_panel.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/local_list_model.hpp"
#include "bench/lyrics_panel.hpp"
#include "bench/metadata_artwork_section.hpp"
#include "bench/metadata_grid_model.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "bench/musicbrainz_track_match_widget.hpp"
#include "bench/playback_tab_widget.hpp"
#include "bench/playlist_transfer_bar.hpp"
#include "bench/quick_pick_popup.hpp"
#include "bench/remote_engines.hpp"
#include "bench/replaygain_dialog.hpp"
#include "bench/search_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/track_list_find_bar.hpp"
#include "bench/trackknife_style.hpp"
#include "bench/up_next_delegate.hpp"
#include "bench/widget_color_scheme.hpp"
#include "drag_position.hpp"
#include "test_engine.hpp"
#include "trackknife/discovery/mdns.hpp"
#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "uicommon/local_files_mime_data.hpp"
#include "workspace/album_batch_session.hpp"
#include "workspace/color_scheme.hpp"
#include "workspace/identify_session.hpp"
#include "workspace/interface_scale.hpp"
#include "workspace/keep_awake.hpp"
#include "workspace/lastfm_settings_session.hpp"
#include "workspace/list_view_definitions.hpp"
#include "workspace/settings_session.hpp"
#include "workspace/tab_store.hpp"

#include <signal.h>

#include "qtest_shard.hpp"
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
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLocale>
#include <QMimeData>
#include <QProcess>
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
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDataStream>
#include <QDeadlineTimer>
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
#include <QRadioButton>
#include <QScopeGuard>
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
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A case that needs audio to play in real time -- a track to finish, a
// listen to count. A shared CI runner's null sink stalls now and then, so CI
// skips them (MELODY_TEST_SKIP_REALTIME); where there is a sound server
// they run.
#define MELODY_REQUIRE_REALTIME_AUDIO()                                                        \
    if (qEnvironmentVariableIsSet("MELODY_TEST_SKIP_REALTIME")) {                              \
        QSKIP("needs audio to play in real time");                                                 \
    }

namespace trackknife::bench {

namespace window_test {
// ADR-0221: playback anchors are entry identities now, so the white-box
// assertions below resolve them to a row instead of reading .row() off a
// persistent index. Same question, asked explicitly. Templated because
// BenchMainWindow::ListTab is private to the window and its friend test class.
template <typename Tab> int row_of(const Tab* tab, const core::StableId& entry) {
    return tab == nullptr ? -1 : tab->model->rowOfEntry(entry, -1);
}
} // namespace window_test

namespace window_test {

constexpr std::uint32_t wave_sample_rate = 44'100U;
// The live progression test reaches the user's default PipeWire sink. Keep its
// non-silent probe roughly 32 dB below the previous level so it is detectable
// by the decoder without being an intrusive test sound.
constexpr std::int32_t test_wave_peak = 256;

inline void append_u16(std::vector<unsigned char>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

inline void append_u32(std::vector<unsigned char>& bytes, const std::uint32_t value) {
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        bytes.push_back(static_cast<unsigned char>((value >> (byte * 8U)) & 0xFFU));
    }
}

// Writes a real 16-bit mono PCM WAV with a non-silent deterministic pattern so
// playback has actual audio to drain.
inline void write_wave(const QString& path, const std::uint32_t frames) {
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

inline bool materialize_audio_fixture(const QString& encoded_name, const QString& output_path) {
    QFile source{QStringLiteral(MELODY_AUDIO_FIXTURE_DIR) + QLatin1Char('/') + encoded_name};
    if (!source.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto decoded = QByteArray::fromBase64(source.readAll());
    QFile output{output_path};
    return !decoded.isEmpty() && output.open(QIODevice::WriteOnly) &&
           output.write(decoded) == decoded.size();
}

} // namespace window_test

using namespace window_test;

class BenchMainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void discsOfAnAlbumAreNamed();
    void init();
    void cleanup();
    void transportIsOneRowWithCoverAndPills();
    void theDesktopNamesWhatPlaysAfterARestart();
    void thePlayingRowFollowsItsList();
    void headerShowsThePlayingAlbumsCover();
    void libraryDragsIntoUpNextWithCovers();
    void quickAlbumFindsByWordsAndPutsItAway();
    void quickAlbumShiftEnterReplacesAndPlays();
    void quickTrackFindsATrackByItsTitle();
    void settingsListEnginesFoundOnTheNetwork();
    void recentlyAddedComesFirstWhereAskedFor();
    void ffmpegEncoderIsTheTagTagLibCallsEncoding();
    void activePlaybackTabRemainsMarkedWhileBrowsing();
    void doubleClickOnEmptyTabBarMakesAList();
    void activeTabAccentSurvivesThemeTextColor();
    void listTabsMeasureTheSameUnderEveryStyle();
    void followPlaybackAndJumpRespectBrowsing();
    void commandPaletteFindsAndRunsRegisteredActions();
    void commandPaletteTracksAvailabilityAndLifetime();
    void localListeningColumnsLoadRefreshAndRespectAuthority();
    void shortcutSettingsValidateSaveAndCancel();
    void everyCommandTakesAKeyAndCtrlLSearchesTheLibrary();
    void quittingStopsTheEngineForGood();
    void sharingSettingsAreHandedToTheEngineOnce();
    void theWindowsListsAreOnItsEngine();
    void listsTravelAsEdits();
    void anotherClientsListChangesReachTheWindow();
    void aListChangedWhileClosedIsTakenUpOnOpening();
    void aListFromElsewhereOpensAsATab();
    void aListFromTheEngineCarriesItsLibraryTags();
    void aMoveIsFollowedInListsNotOpenHere();
    void libraryAndFoldersAddToAChosenList();
    void tabsAreGroupedByEngine();
    void theRemotesListsTakeItsIdOnceItSaysIt();
    void remoteEnginesStartFromTheOneRemoteOfBefore();
    void twoEnginesElsewhereSideBySide();
    void settingsListTheEnginesElsewhere();
    void anEnginesOwnPasswordIsSaidAndCanGo();
    void enginesAddedOrRemovedApplyAtOnce();
    void aPasswordGivenLaterReachesTheEngine();
    void anotherEngineAtTheSameAddressLeavesTheOldOnesLists();
    void theListsPanelShowsEveryListAndTakesDrops();
    void aScratchTabNeverReachesItsEngine();
    void listsLeftOnTheEngineAreOfferedOnce();
    void theTabStoreKeepsWhatItCannotRead();
    void aPlayingTabsEditsReachTheQueueAsEdits();
    void thisComputersTabsAreMarkedWhenTheLibraryIsElsewhere();
    void aboutShowsTheMascotAndTheRelease();
    void aPluginActionSharesWhatIsChosen();
    void manyPluginActionsGoInASubmenu();
    void aListDeletedElsewhereWhileBeingWrittenStaysDeleted();
    void aRemovalLeftPendingByAnOlderReleaseIsCarriedOut();
    void playbackBufferProfilesPersistAndExposeDiagnostics();
    void statusBarSummarizesTrackSelection();
    void committedMetadataRefreshesDuplicatesAndPreservesCueOverlay();
    void metadataReadyPlanAppliesAndRefreshesHistory();
    void metadataRenameMovesTheFileAndItsRow();
    void applyReadsAgainAFileChangedElsewhere();
    void listRowsFollowChangedFiles();
    void fileWorkWithoutAnEngineSaysWhy();
    void metadataApplyCancellationPreservesDraftForFreshPreview();
    void metadataDialogLayoutsPersistAsynchronously();
    void taggerWindowStateRoundTrips();
    void metadataFieldLayoutsLoadFilterAndPersist();
    void metadataGridDisplaysUnicodePaths();
    void metadataGridReusesExactNativeFieldWithoutInvalidIndexes();
    void metadataTransformationChainPreviewsAndStagesOneUndo();
    void metadataCapturePatternSavesReloadsAndStagesAllFields();
    void preparationSidePanelEditsReusableOutputProfiles();
    void pathOnlyPreparationUsesActualTagsAndAppliesReviewedPlan();
    void moveDestinationsArePerEngine();
    void actionChoicesAreRemembered();
    void actionsLinksOpenTheirOwnSettings();
    void namingLayoutsReachEveryEngine();
    void combinedTagAndRenameReviewReachesPreparationApply_data();
    void combinedTagAndRenameReviewReachesPreparationApply();
    void metadataSuggestionsStageSelectionConsistency();
    void musicBrainzMatchingHandlesUnequalCounts_data();
    void musicBrainzMatchingHandlesUnequalCounts();
    void musicBrainzIdentifyStagesChosenVersion_data();
    void musicBrainzIdentifyStagesChosenVersion();
    void identifyAlbumsGroupsLooksUpAndStages();
    void identifyAlbumsReviewsWhatNeedsYou();
    void identifyAlbumsTakesAFileGroupedApart();
    void identifyAlbumsWritesWhatIsChosen();
    void identifyAlbumsOpensFromTools();
    void paintedStatusIconsFollowThePalette();
    void identifyAlbumsStagesAlongsideAutomaticScripts();
    void cachedRowsAreReadEvenWithARevision();
    void settingsGroupWhatTheyAreAbout();
    void settingsChooseWhereUndoCopiesAreKept();
    void listRowsWhoseFilesAreGoneAreGreyed();
    void upNextFromAnotherClientShows();
    void applyMeasuresReplayGainAndWritesOnce();
    void applyMovesIntoAChosenFolder();
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
    void theTaggerOpensOnCachedTagsAndReadsBehind();
    void coversAreResizedBeforeTheyTravel();
    void contextReplayGainScansAndApplies_data();
    void contextReplayGainScansAndApplies();
    void loudnessSidecarProjectsOntoProbedRows();
    void desktopNotificationsNotifyBackgroundTrackChanges();
    void upNextPreservesNormalPlayback_data();
    void upNextPreservesNormalPlayback();
    void aRemoteEnginePlaysItsOwnTabs();
    void theWindowFollowsAnEngineStartedElsewhere();
    void aNotificationWaitsForItsCover();
    void theRemoteTabTakesTheNameItsEngineAnnounces();
    void anUnreachableRemoteDoesNotHoldTheWindow();
    void aListReplacedOnTheFollowedEngineMarksWhatPlays();
    void aRemoteTabGetsTagsAndCoversFromItsEngine();
    void aRemoteTabRatesOnItsEngine();
    void thisComputersFilesMoveIntoTheLibrary();
    void aRatingSetElsewhereShowsInTheTabs();
    void dynamicPlaylistsReadTheLibraryChosen();
    void replacingARemoteTabFromItsLibraryPlays();
    void locateFindsARemoteTracksAlbumInTheRemoteLibrary();
    void remoteUpNextKeepsItsIdentityAcrossARestart();
    void sourcePanelOpensOnALibrary();
    void emptyListsSayHowToFillThem();
    void aKeptSearchOpensAtOnce();
    void narrowWindowKeepsListAndUpNextCompact();
    void aRestoredRemoteTabGetsItsCovers();
    void lastFmIsHandedToTheEngine();
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
    void convertDialogFetchesFilesItCannotReach();
    void propertiesFileListLivesInTheTaggerWindow();
    void selectionActionsFollowTheActiveTab();
    void closingATabReturnsToThePreviousOne();
    void localArtworkSurvivesInvalidation();
    void folderBookmarksRevealTreePaths();
    void folderBookmarksMigrateFromLibraryRoots();
    void artworkFetchesCoverArtFromArchiveAndAddsFront_data();
    void artworkFetchesCoverArtFromArchiveAndAddsFront();
    void metadataRetrySkipsSavedFiles_data();
    void metadataRetrySkipsSavedFiles();
    void coverPolicyRoundTrip();
    void upnpSettingRoundTrip();
    void aSaveWritesOnlyListsThatChanged();
    void playbackSettingsApplyLiveAndCancel();
    void librarySettingsManageFoldersWithoutScanning();
    void ratingsInTagsIsAnEngineOption();
    void metadataServiceSettingsAndCompactPages();
    void coverThumbnailAppliesPolicy_data();
    void coverThumbnailAppliesPolicy();
    void metadataApplyCombinesTagsAndArtwork_data();
    void metadataApplyCombinesTagsAndArtwork();
    void artworkArchivePickerAddsChosenImageWithItsRole();
    void aCoverBesideTheFilesIsOfferedNotShown();
    void automaticScriptsStageOnOpen();
    void metadataStartupPresentsReconciliation();
    void filePublicationStartupPresentsReconciliation();
    void combinedPublicationStartupRecoversMetadataAndPath();
    void folderDiscoveryAdmitsWave64();
    void contextMenusTargetSelectionsListsAndFolders();
    void contextTransfersCreateTabs();
    void tabBarDropsTransferLocalRows();
    void filesDroppedOnTheTabBarMakeATab();
    void panelLayoutPersistsAndPreservesFutureState();
    void trackViewLayoutMatchesGroupedQueueAndPersists();
    void listViewsShowExpressionsGroupAndSort();
    void listViewEditorChecksAndSavesACopy();
    void builtinColumnsShowTheirExpressions();
    void listViewsReadListeningHistory();
    void groupsFoldToTheirHeaders();
    void rowsHaveMoreLines();
    void listLayoutsFromBeforeViewsMigrate();
    void localReorderPreservesVisibleRowGeometry();
    void localListUndoRestoresOccurrencesAndFreshMetadata();
    void localListHistoryBranchesAndBounds();
    void crossTabMoveUndoIsOneTransaction();
    void localListUndoActionsRespectAuthorityAndTextEditing();
    void deleteInUpNextTakesItsTrackNotTheLists();
    void settingsLeftOffReadAsOff();
    void colorSchemesAreChosenAndApplied();
    void interfaceSizeIsChosenForTheNextStart();
    void headerPillsFollowAPaletteChangedLater();
    void listTabCloseButtonsClose();
    void mprisOrderAndCoverFollowTheWorkspace();
    void longJobsKeepTheComputerAwake();
    void lyricsPanelFollowsThePosition();
    void windowListTabsCloseFromTheirButtons();
    void localListOrderingActionsRespectAuthorityAndPersist();
    void portablePlaylistImportsPreserveAuthorityAndPersist();
    void trackListFindActionsFollowActiveTab();
    void noncontiguousLocalReorderPreservesOccurrences();
    void persistsPinnedDuplicatedAndDirtyTabs();
    void theWindowKeepsItsTabsNotTheLists();
    void richMetadataValuesAndIdentitiesSurviveListRestart();
    void metadataPropertiesFileSelectionDrivesIndividualAndBulkEdits();
    void metadataFieldReviewPreservesDraftAndSelectionScope();
    void metadataPropertiesArtworkSectionShowsProvenanceAndCapabilities();
    void metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes_data();
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
    // A home of this process's own: test-mode data lives under ~/.qttest,
    // which every run shares otherwise -- and each run's first act is to
    // clear it, so two runs at once (two checkouts, two sessions) broke each
    // other's engines.
    QTemporaryDir home_directory_;
    QTemporaryDir settings_directory_;
    testing::TestEngine engine_;
};

namespace window_test {

// A key click whose press is taken as a shortcut, or takes its widget away,
// can leave its modifier held: QGuiApplication then reports Ctrl or Shift
// down for every case after. Releases whatever is held on `on`, a widget
// that is still there.
inline void releaseHeldModifiers(QWidget* on) {
    constexpr std::array<std::pair<Qt::KeyboardModifier, Qt::Key>, 4> modifiers{{
        {Qt::ShiftModifier, Qt::Key_Shift},
        {Qt::ControlModifier, Qt::Key_Control},
        {Qt::AltModifier, Qt::Key_Alt},
        {Qt::MetaModifier, Qt::Key_Meta},
    }};
    auto held = QGuiApplication::keyboardModifiers();
    for (const auto& [modifier, key] : modifiers) {
        if (held.testFlag(modifier)) {
            held.setFlag(modifier, false);
            QTest::keyRelease(on, key, held);
        }
    }
}

} // namespace window_test

namespace window_test {

// Fetch cover opens a picker; choose the row from this place with this type.
[[nodiscard]] inline bool chooseInCoverPicker(QWidget* owner, const QString& from,
                                              const QString& type) {
    QTreeWidget* list = nullptr;
    QPushButton* use = nullptr;
    QTreeWidgetItem* wanted = nullptr;
    const QDeadlineTimer deadline{5'000};
    while (wanted == nullptr && !deadline.hasExpired()) {
        if (auto* picker =
                owner->findChild<QDialog*>(QStringLiteral("bench-metadata-artwork-picker"))) {
            list = picker->findChild<QTreeWidget*>(
                QStringLiteral("bench-metadata-artwork-picker-list"));
            use = picker->findChild<QPushButton*>(
                QStringLiteral("bench-metadata-artwork-picker-use"));
            for (int row = 0; list != nullptr && row < list->topLevelItemCount(); ++row) {
                auto* item = list->topLevelItem(row);
                if (item->text(1) == from && item->text(2) == type) {
                    wanted = item;
                    break;
                }
            }
        }
        if (wanted == nullptr) {
            QTest::qWait(20);
        }
    }
    if (wanted == nullptr || use == nullptr) {
        return false;
    }
    list->setCurrentItem(wanted);
    QTest::mouseClick(use, Qt::LeftButton);
    return true;
}

} // namespace window_test

namespace window_test {

inline void write_sine_wav_fixture(const QString& path, const double amplitude,
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

} // namespace window_test

namespace window_test {

// A list the way another client -- the phone, a second window -- writes it.
inline protocol::Json listItems(const std::vector<std::string>& paths) {
    auto items = protocol::Json::array();
    for (const auto& path : paths) {
        items.push_back(protocol::Json{{"path", protocol::encode_raw_path(path)},
                                       {"title", path.substr(path.rfind('/') + 1)}});
    }
    return items;
}

inline std::vector<std::string> rowPaths(const LocalListModel& model) {
    std::vector<std::string> paths;
    for (const auto& row : model.rows()) {
        paths.push_back(row.raw_path);
    }
    return paths;
}

} // namespace window_test

} // namespace trackknife::bench
