// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/artwork_services.hpp"
#include "bench/file_work_tools.hpp"
#include "bench/musicbrainz_lookup.hpp"
#include "bench/output_profile_store.hpp"
#include "bench/preparation_feedback.hpp"
#include "bench/replaygain_scan.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/transformation.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/musicbrainz/matching.hpp"
#include "trackknife/musicbrainz/web_service.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/preparation_plan.hpp"
#include "trackknife/operations/undo.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QByteArray>
#include <QFutureWatcher>
#include <QItemSelectionModel>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class QTemporaryDir;

namespace trackknife::bench {

class MetadataGridModel;
class MetadataAggregateModel;

struct MetadataPropertiesSource {
    metadata::StagedMetadataSource source;
    QString track_label;
    MetadataPropertiesAudioSource audio{};
    // The length the list knows, for matching to a release (ADR-0261).
    std::optional<std::int64_t> duration_ms{};
};

using MetadataPropertiesSourceReader =
    std::function<std::optional<MetadataPropertiesSource>(std::size_t)>;
using MetadataWritePlanApplier = std::function<core::Result<operations::MetadataApplyResult>(
    const metadata::MetadataWritePlan&, const operations::MetadataApplyProgressCallback&,
    const core::CancellationToken&)>;
using MetadataWritePlanApplierFactory = std::function<MetadataWritePlanApplier()>;
using MetadataApplyObserver = std::function<void(const operations::MetadataApplyResult&)>;
using FilePublicationPlanApplier =
    std::function<core::Result<operations::FilePublicationApplyResult>(
        const operations::PreparationPlan&, const operations::FilePublicationApplyProgressCallback&,
        const core::CancellationToken&)>;
using FilePublicationPlanApplierFactory = std::function<FilePublicationPlanApplier()>;
using FilePublicationApplyObserver =
    std::function<void(const operations::FilePublicationApplyResult&)>;

struct MetadataTransformationStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedMetadataTransformationChain>, QString)>;
    using Completion = std::function<void(QString)>;

    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedMetadataTransformationChain, Completion)> save;
    std::function<void(core::StableId, Completion)> remove;
};

struct MetadataDialogLayoutStore {
    using LoadCompletion = std::function<void(QByteArray, QString)>;
    using Completion = std::function<void(QString)>;

    std::function<void(QString, LoadCompletion)> load;
    std::function<void(QString, QByteArray, Completion)> save;
};

// Shared between the UI thread and the background Apply worker; the worker
// writes under the mutex and the footer progress readout copies under it.
struct MetadataApplyProgressState {
    mutable std::mutex mutex;
    std::vector<operations::MetadataApplySourceState> states;
    std::vector<std::optional<core::Error>> issues;
    std::size_t completed_sources{0U};
};

struct FilePublicationApplyProgressState {
    mutable std::mutex mutex;
    std::vector<operations::FilePublicationApplySourceState> states;
    std::vector<std::optional<core::Error>> issues;
    std::size_t completed_sources{0U};
};

// What the tag editor is given: where it applies tags and files, where its
// scripts, profiles and layouts are kept, and how it looks things up.
struct TaggerServices {
    MetadataWritePlanApplierFactory plan_applier_factory;
    MetadataApplyObserver apply_observer;
    MetadataTransformationStore transformation_store;
    OutputProfileStore output_profile_store;
    FilePublicationPlanApplierFactory file_plan_applier_factory;
    FilePublicationApplyObserver file_apply_observer;
    MetadataDialogLayoutStore layout_store;
    MusicBrainzLookupService musicbrainz;
    FileWorkTools tools;
    // The library folders of the engine holding the files, as raw paths.
    std::function<std::vector<std::string>()> library_roots;
    // ADR-0263: writes undone by the engine holding the files, newest first
    // as given. Run on a worker.
    std::function<core::Result<std::vector<operations::UndoOutcome>>(
        std::vector<operations::UndoRequest>, const core::CancellationToken&)>
        undo;
};

// The tagger's artwork, as the session needs it: its pending covers, whether
// it is at work, and the files it applies to.
class TaggerArtwork {
  public:
    TaggerArtwork() = default;
    TaggerArtwork(const TaggerArtwork&) = delete;
    TaggerArtwork& operator=(const TaggerArtwork&) = delete;
    virtual ~TaggerArtwork() = default;
    [[nodiscard]] virtual bool hasPendingChanges() const = 0;
    [[nodiscard]] virtual std::vector<metadata::ArtworkWritePlanIntent> pendingIntents() const = 0;
    [[nodiscard]] virtual bool isBusy() const = 0;
    virtual void setWorking(bool working) = 0;
    virtual void discardPendingChanges() = 0;
    virtual void requestOperationCancellation() = 0;
    virtual void setScope(std::vector<MetadataArtworkScopeSource> sources,
                          bool source_limit_exceeded) = 0;
    virtual void setCoverArtRelease(std::optional<QString> release_id) = 0;
};

// One tag-editing session (the "Edit tags" window): the selected files read
// and shown as a grid of fields, edits staged as a draft with undo, scripts,
// suggestions, MusicBrainz and ReplayGain staged into it, and Apply -- tags,
// renaming and moving -- planned, checked and carried out. Everything the
// window decides is here; the window's tag editor draws it and passes it what
// the user does (ADR-0220).
class TaggerSession final : public QObject {
    Q_OBJECT

  public:
    // The Settings pages the editor asks for.
    enum class SettingsPage { naming, covers, replaygain };
    // What closing the editor needs first.
    enum class CloseAnswer { close, confirm_artwork, confirm_drafts, wait };

    struct Choice {
        QString id;
        QString name;
    };
    struct Script {
        QString id;
        QString name;
        bool automatic{false};
    };
    struct FieldLayout {
        QString id;
        QString name;
        QStringList fields;
    };
    struct Identify {
        std::vector<musicbrainz::LocalTrackDescriptor> descriptors;
        std::vector<QString> local_paths;
        std::vector<std::size_t> items;
        QString artist;
        QString release;
    };
    // The exact ordered values of a field on the selected files, to edit.
    struct ExactValues {
        int row{-1};
        QString heading;
        QString context;
        QStringList values;
    };
    struct Provenance {
        QStringList headers;
        std::vector<QStringList> rows;
    };

    TaggerSession(std::size_t requested_item_count, MetadataPropertiesSourceReader source_reader,
                  std::span<const std::string_view> preferred_fields, TaggerServices services,
                  QObject* parent = nullptr);
    ~TaggerSession() override;

    // Begins: the files are read, a few at a time.
    void start();
    void setArtwork(TaggerArtwork* artwork);
    void setArtworkServices(ArtworkWritePlanApplierFactory applier_factory,
                            ArtworkApplyObserver observer);
    [[nodiscard]] const ArtworkWritePlanApplierFactory& artworkApplierFactory() const {
        return artwork_plan_applier_factory_;
    }
    // Covers from the Cover Art Archive: none without MusicBrainz lookups.
    [[nodiscard]] std::optional<ArtworkCoverArtService> coverArtService();
    // The artwork applied through the artwork section's own Apply.
    [[nodiscard]] ArtworkApplyObserver artworkAppliedObserver();
    // An artwork Apply finished: the files' new revisions taken in.
    void artworkApplied(const operations::ArtworkApplyResult& result);
    // The artwork's pending covers changed; its own work started or ended.
    void artworkStateChanged();
    void setArtworkOperationRunning(bool running);
    [[nodiscard]] const TaggerServices& services() const { return services_; }

    // What is shown.
    [[nodiscard]] bool ready() const { return grid_model_ != nullptr; }
    [[nodiscard]] QString loadingText() const { return loading_text_; }
    // Stops reading the selection's tags, before the grid is shown.
    void stopReading();
    // Which files could not be read, and why; empty when all were.
    [[nodiscard]] QString unreadableDetails() const { return unreadable_details_; }
    [[nodiscard]] QString summary() const { return summary_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool statusRich() const { return status_rich_; }
    [[nodiscard]] QString technical() const { return technical_; }
    [[nodiscard]] QString applySummary() const { return apply_summary_; }
    [[nodiscard]] MetadataGridModel* gridModel() const { return grid_model_; }
    [[nodiscard]] MetadataAggregateModel* aggregateModel() const { return aggregate_model_; }
    [[nodiscard]] QItemSelectionModel* fileSelection() const { return file_selection_; }
    [[nodiscard]] std::vector<std::size_t> selectedItems() const;
    // ADR-0221: the folder every file is in, shown once above the list, the
    // files named below it relative to it; empty when they share none.
    [[nodiscard]] QString commonFolder() const;
    [[nodiscard]] std::size_t selectedItemCount() const { return selected_item_count_; }

    // Enablement, as the widgets window's buttons had it.
    [[nodiscard]] int draftCount() const { return draft_count_; }
    [[nodiscard]] bool canUndo() const { return can_undo_; }
    [[nodiscard]] bool canRedo() const { return can_redo_; }
    [[nodiscard]] bool selectionReady() const;
    [[nodiscard]] bool canAddField() const;
    [[nodiscard]] bool canRemoveFields() const;
    [[nodiscard]] bool canEditValues() const;
    [[nodiscard]] bool canTransform() const;
    [[nodiscard]] bool canSuggest() const { return canTransform() && !proposal_running_; }
    [[nodiscard]] bool canIdentify() const;
    [[nodiscard]] bool canScanReplayGain() const;
    [[nodiscard]] bool canShowProvenance() const { return grid_model_ != nullptr; }
    [[nodiscard]] bool canApply() const;
    [[nodiscard]] bool applyRunning() const { return apply_running_; }
    [[nodiscard]] bool progressVisible() const { return apply_running_; }
    [[nodiscard]] int progressValue() const { return progress_value_; }
    [[nodiscard]] int progressMaximum() const { return progress_maximum_; }
    [[nodiscard]] bool canStopApply() const { return apply_running_ && !apply_stop_requested_; }
    [[nodiscard]] bool fileListEnabled() const;

    // The view says what its fields table has: a row selected, a cell
    // current. And which of its dialogs are open, which pause others.
    void setFieldSelection(bool has_selected_fields, bool has_current_field);
    void setFieldNameDialogOpen(bool open);
    void setExactValuesDialogOpen(bool open);
    void setTransformationDialogOpen(bool open);
    void setIdentifyDialogOpen(bool open);

    // The draft.
    void undo();
    void redo();
    void discardAll();
    // A link in the status line: undo the automatic scripts, stop or retry
    // the ReplayGain scan, export its results.
    void statusLinkActivated(const QString& link);
    // A field added to the selected files: the row it is in, or -1.
    int addField(const QString& name);
    [[nodiscard]] QStringList fieldNameSuggestions(const QString& query) const;
    [[nodiscard]] std::optional<ExactValues> exactValues(int row) const;
    void replaceValues(int row, std::vector<std::string> values);
    // A transformation previewed elsewhere (the script editor), staged; the
    // selection kept.
    bool stageTransformation(const metadata::MetadataTransformationPreview& preview,
                             const QStringList& step_sources = {});

    // Field sets.
    [[nodiscard]] const std::vector<FieldLayout>& fieldLayouts() const { return field_layouts_; }
    [[nodiscard]] QString activeFieldLayout() const { return active_field_layout_id_; }
    [[nodiscard]] QStringList activeFieldLayoutFields() const;
    void selectFieldLayout(const QString& id);
    // Saved under `name`; the new set's index in fieldLayouts(), or -1.
    int saveFieldLayout(const QString& name, QStringList fields);
    void removeFieldLayout();

    // What Apply does (ADR-0238).
    [[nodiscard]] bool saveTags() const { return save_tags_; }
    [[nodiscard]] bool renameFiles() const { return rename_files_; }
    [[nodiscard]] bool moveFiles() const { return move_files_; }
    [[nodiscard]] bool renameAvailable() const;
    [[nodiscard]] bool moveAvailable() const;
    [[nodiscard]] QString renameTooltip() const;
    [[nodiscard]] QString moveTooltip() const;
    [[nodiscard]] bool layoutsAvailable() const;
    [[nodiscard]] bool destinationsAvailable() const;
    void setSaveTags(bool on);
    // As chosen by the user: remembered, and set again when possible.
    void chooseRename(bool on);
    void chooseMove(bool on);
    // As set by the window itself.
    void setRenameFiles(bool on);
    void setMoveFiles(bool on);
    [[nodiscard]] std::vector<Choice> layouts() const;
    // Where Move goes: the engine's saved destinations, then the library
    // folder each file is in, then the folder chosen last (ADR-0262).
    [[nodiscard]] std::vector<Choice> destinations() const;
    [[nodiscard]] int layoutIndex() const;
    [[nodiscard]] int destinationIndex() const;
    void selectLayout(int index);
    void selectDestination(int index);
    // A folder of the engine's, moved into without being saved.
    void chooseMoveFolder(std::string raw_path);
    // Where a folder is chosen from: the engine's lister, or (empty) this
    // computer's file dialog; and where to start.
    [[nodiscard]] EngineFolderLister moveFolderLister() const;
    [[nodiscard]] std::string moveFolderStart() const;
    // The destination Move goes to; none for the library folder each is in.
    [[nodiscard]] std::optional<operations::DestinationProfile> moveDestination() const;
    [[nodiscard]] bool movesIntoLibraryFolders() const {
        return move_target_ == MoveTarget::library;
    }
    [[nodiscard]] QString outputProfileStatus() const { return output_profile_status_; }
    [[nodiscard]] QString destinationsOn() const {
        return services_.output_profile_store.destinations_on;
    }
    [[nodiscard]] bool outputProfilesAvailable() const { return !output_profiles_loading_; }
    void reloadOutputProfiles();
    // The choices remembered for the next editor.
    void rememberActionChoices() const;

    // ReplayGain.
    [[nodiscard]] int replayGainGrouping() const { return replaygain_grouping_; }
    [[nodiscard]] QString replayGainExpression() const { return replaygain_expression_; }
    void setReplayGainGrouping(int index);
    void setReplayGainExpression(const QString& expression);
    void startReplayGainScan(std::vector<std::size_t> forced_items = {});
    // ReplayGain as an Apply action: measured first and staged into the
    // draft, then written with everything else in one write -- tagging and
    // moving never change the audio. Albums (or, track gain only, tracks)
    // that have the gain already are left alone when asked.
    [[nodiscard]] bool replayGainOnApply() const { return replaygain_on_apply_; }
    [[nodiscard]] bool skipExistingGain() const { return skip_existing_gain_; }
    void chooseReplayGain(bool on);
    void chooseSkipExistingGain(bool on);
    // Of `items`, those to measure: all, or those whose group lacks the gain.
    [[nodiscard]] std::vector<std::size_t> itemsNeedingGain(std::vector<std::size_t> items) const;
    // Measured and staged, then gainsMeasured(); false when it cannot start.
    bool measureBeforeWrite(std::vector<std::size_t> items);
    void exportReplayGainResults(const QString& path);
    [[nodiscard]] Provenance loudnessProvenance() const;
    [[nodiscard]] bool hasReplayGainExport() const { return !replaygain_export_rows_.isEmpty(); }

    // Scripts.
    [[nodiscard]] std::vector<Script> scripts() const;
    [[nodiscard]] bool scriptsLoading() const { return transformation_catalog_loading_; }
    [[nodiscard]] QString scriptStatus() const;
    void toggleAutomaticScript(const QString& id, bool enabled);
    void reloadScripts(std::optional<core::StableId> selected = std::nullopt);

    // Suggestions and MusicBrainz.
    void startProposals();
    // What the Identify window starts from; none when it cannot open.
    [[nodiscard]] std::optional<Identify> identifyRequest() const;
    void applyMusicBrainzProposals(metadata::MetadataProposalSet proposals);

    // ADR-0261: what Identify albums… asks of the files open here.
    [[nodiscard]] std::size_t itemCount() const;
    // An item's file as it was read (or, until read, cached).
    [[nodiscard]] const metadata::StagedMetadataSource* itemSource(std::size_t item) const;
    // Its length: as the list knew it, else as probed here.
    [[nodiscard]] std::optional<std::int64_t> durationOf(std::size_t item) const;
    // Whether a proposal set can be staged now: the files are read and
    // nothing else is being staged. proposalsSettled says when one was.
    [[nodiscard]] bool canStageProposals() const;

    // ADR-0261: Identify albums… writes some of the files itself. While it
    // does, nothing here is edited or applied; then the files it wrote are
    // read again -- at their new paths, when moved -- and the draft of the
    // rest kept, the files' own drafts having been written.
    struct Rewritten {
        std::size_t item{0U};
        std::string raw_path;
    };
    [[nodiscard]] bool canWriteElsewhere() const;
    [[nodiscard]] bool writingElsewhere() const { return writing_elsewhere_; }
    // What such a write starts from.
    [[nodiscard]] std::shared_ptr<const metadata::StagedMetadataSelection> sharedSelection() const;
    [[nodiscard]] const metadata::StagedMetadataPatchSet* draft() const;
    [[nodiscard]] std::optional<operations::OutputLayoutProfile> layoutProfile(int index) const;
    bool beginWriteElsewhere();
    // `stale`: files found changed since they were read, read again too,
    // their drafts kept, so the next write can take them.
    void finishWriteElsewhere(std::vector<Rewritten> written, std::vector<std::size_t> stale = {});

    // Apply.
    void startWritePlan();
    // The folder images a plan writes were seen (accepted) or not.
    void folderImagesReviewed(bool accepted);
    void requestApplyStop();
    // The files a failed Apply did not finish, tried again.
    [[nodiscard]] bool canRetry() const { return retry_plan_ != nullptr; }
    void retryUnfinished();
    [[nodiscard]] bool applyCommitted() const { return apply_committed_; }

    // Closing: what it needs first. Waiting, it has asked the work in
    // flight to stop, and said so.
    [[nodiscard]] CloseAnswer requestClose();
    // It closes: the check in flight dropped, the draft thrown away when
    // `discard`.
    void closing(bool discard);
    // A feedback window closed, not to retry: the editor closes when Apply
    // already changed files.
    void feedbackFinished();

    // The window's own geometry and splitter, as stored; stored once.
    void loadLayoutState(std::function<void(QByteArray)> geometry,
                         std::function<void(QByteArray)> splitter);
    void storeLayoutState(const QByteArray& geometry, const QByteArray& splitter);
    // The same, in a form any window can restore: "width", "height",
    // "maximized" and the file list's "listWidth".
    void loadWindowState(std::function<void(QVariantMap)> loaded);
    void storeWindowState(const QVariantMap& state);

  signals:
    // Anything shown changed.
    void changed();
    // The grid and its models exist, to be drawn; then its files are
    // selected and its first fields shown.
    void gridReady();
    void gridFilled();
    void scriptsChanged(const QString& selected);
    void outputProfilesChanged();
    void fieldLayoutsChanged();
    // A window listing files and what is wrong with them.
    void feedbackRequested(const QString& title, const QString& summary,
                           std::vector<PreparationFeedbackRow> rows, bool retry_offered);
    // Folder images this Apply writes, to be seen first (folderImagesReviewed).
    void folderImagesReviewRequested(std::vector<metadata::FolderImageWritePlan> images);
    void closeRequested();
    void openSettingsRequested(SettingsPage page);
    void openDestinationsRequested();
    // Worth a line in the main window's status bar once the editor closes.
    void statusMessage(const QString& message);
    // ADR-0261: a proposal set has been staged -- or not, saying why in the
    // status.
    void proposalsSettled(bool staged);
    // A write made elsewhere is over, and the files it wrote read again.
    void writtenElsewhere();
    // measureBeforeWrite() is over: the gains are in the draft (or there
    // was nothing to measure), or not, saying why in the status.
    void gainsMeasured(bool ok);

  private:
    using SelectionResult = core::Result<metadata::StagedMetadataSelection>;
    // Where the worker reading a selection's tags says how far it has got,
    // and which files it could not read.
    struct Reading {
        std::atomic<std::size_t> read{0};
        std::atomic<std::size_t> total{0};
        std::vector<std::pair<std::string, core::Error>> unreadable;
    };
    using WritePlanResult = core::Result<operations::PreparationPlan>;
    struct AutomaticChainPlan {
        metadata::MetadataTransformationChain chain;
        QStringList step_sources;
    };
    struct TechnicalInfo {
        std::string codec;
        int sample_rate{0};
        int bits{0};
        int channels{0};
        std::int64_t bit_rate{0};
        std::int64_t duration_ms{-1};
    };

    void persistFieldLayouts();
    void captureSources();
    void startSelection();
    // ADR-0257: the cached tags, shown while the files are read.
    void showProvisional(metadata::StagedMetadataSelection selection);
    // The selection's counts, for the summary line.
    void describeSelection(const metadata::StagedMetadataSelection& selection);
    // Every row chosen, as when the grid first opens.
    void selectAllRows();
    // Which files could not be read, said in the status line.
    void reportUnreadable();
    void finishSelection();
    void buildGrid(metadata::StagedMetadataSelection selection);
    void updateSelectionProjection();
    void updateArtworkScope(std::span<const std::size_t> selected_items);
    [[nodiscard]] core::Result<QString> storeCoverArtImage(const QString& release_id,
                                                           const QByteArray& bytes);
    void updateDraftState(int patch_count, bool can_undo, bool can_redo);
    void loadTransformationCatalog(std::optional<core::StableId> selected = std::nullopt);
    void loadOutputProfiles();
    void reconcileOutputProfileChoices();
    [[nodiscard]] bool layoutReady() const;
    [[nodiscard]] bool destinationReady() const;
    void updateApplySummary();
    void invalidateWritePlan();
    void finishProposals();
    void stageAutomaticTransformations();
    void finishAutomaticStage();
    [[nodiscard]] std::optional<AutomaticChainPlan> combinedAutomaticChain();
    [[nodiscard]] QString replayGainStatusLinks() const;
    void finishReplayGainScan();
    [[nodiscard]] std::optional<loudness::LoudnessGrouping> replayGainGroupingChoice() const;
    void finishWritePlan();
    void startApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void startMetadataApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void startFileApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void updateApplyProgress();
    void finishMetadataApply();
    void finishFileApply();
    void showFeedback(const QString& title, const QString& summary,
                      std::vector<PreparationFeedbackRow> rows, bool retry_offered = false);
    void updateTechnicalSummary();
    void pumpTechnicalQueue();
    void setStatus(const QString& text, bool rich = false);
    void showStickyStatus(const QString& text);
    [[nodiscard]] std::vector<std::size_t> itemsOrAll() const;

    MetadataPropertiesSourceReader source_reader_;
    TaggerServices services_;
    TaggerArtwork* artwork_{nullptr};
    ArtworkWritePlanApplierFactory artwork_plan_applier_factory_;
    ArtworkApplyObserver artwork_apply_observer_;
    std::unique_ptr<QTemporaryDir> cover_art_directory_;

    QFutureWatcher<std::shared_ptr<SelectionResult>> selection_watcher_;
    QFutureWatcher<std::shared_ptr<WritePlanResult>> write_plan_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::MetadataApplyResult>>>
        metadata_apply_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::FilePublicationApplyResult>>>
        file_apply_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<metadata::MetadataTransformationPreview>>>
        proposal_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<metadata::MetadataTransformationPreview>>>
        automatic_watcher_;
    QFutureWatcher<std::shared_ptr<ReplayGainScanOutcome>> replaygain_watcher_;
    QFutureWatcher<std::pair<std::string, std::optional<TechnicalInfo>>> technical_watcher_;

    std::vector<persistence::SavedMetadataTransformationChain> transformation_catalog_;
    std::vector<persistence::SavedOutputLayoutProfile> output_layout_catalog_;
    std::vector<persistence::SavedDestinationProfile> destination_catalog_;
    std::vector<metadata::StagedMetadataSource> sources_;
    std::shared_ptr<std::vector<MetadataPropertiesAudioSource>> audio_sources_{
        std::make_shared<std::vector<MetadataPropertiesAudioSource>>()};
    // Each source's length as its list knew it, by item.
    std::vector<std::optional<std::int64_t>> durations_;
    std::vector<std::string> preferred_fields_;
    std::vector<std::string> recent_field_names_;
    std::vector<FieldLayout> field_layouts_;
    QString active_field_layout_id_;
    QStringList track_labels_;
    std::size_t requested_item_count_{0U};
    std::size_t capture_index_{0U};

    MetadataGridModel* grid_model_{nullptr};
    MetadataAggregateModel* aggregate_model_{nullptr};
    QItemSelectionModel* file_selection_{nullptr};
    QTimer selection_debounce_;
    QTimer apply_progress_timer_;

    QString loading_text_;
    QString summary_;
    QString status_;
    bool status_rich_{false};
    QString sticky_status_;
    QString technical_;
    QString apply_summary_;
    QString output_profile_status_;
    QString selection_summary_;
    QString revision_summary_;

    bool save_tags_{true};
    bool rename_files_{false};
    bool move_files_{false};
    // Rename and Move as last chosen: set again once a layout (and a
    // destination) make them possible.
    bool wants_rename_{false};
    bool wants_move_{false};
    int replaygain_grouping_{0};
    QString replaygain_expression_;
    std::vector<std::size_t> replaygain_retry_items_;
    QStringList replaygain_export_rows_;
    QStringList automatic_step_sources_;

    bool has_selected_fields_{false};
    bool has_current_field_{false};
    bool field_name_dialog_open_{false};
    bool exact_values_dialog_open_{false};
    bool transformation_dialog_open_{false};
    bool identify_dialog_open_{false};

    std::map<std::string, std::optional<TechnicalInfo>> technical_cache_;
    std::deque<std::string> technical_queue_;
    std::set<std::string> technical_pending_;
    bool technical_probing_{false};
    bool technical_truncated_{false};
    core::CancellationSource technical_cancellation_;

    std::shared_ptr<const operations::PreparationPlan> active_metadata_plan_;
    std::shared_ptr<const operations::PreparationPlan> pending_plan_;
    std::shared_ptr<const operations::PreparationPlan> retry_plan_;
    bool metadata_had_commits_{false};
    std::shared_ptr<MetadataApplyProgressState> apply_progress_state_;
    std::shared_ptr<FilePublicationApplyProgressState> file_apply_progress_state_;
    std::size_t loaded_item_count_{0U};
    std::size_t loaded_source_count_{0U};
    std::size_t loaded_field_count_{0U};
    std::size_t selected_item_count_{0U};
    std::size_t write_plan_generation_{0U};
    std::size_t write_plan_job_generation_{0U};
    core::CancellationSource write_plan_cancellation_;
    core::CancellationSource replaygain_cancellation_;
    core::CancellationSource apply_cancellation_;
    std::shared_ptr<std::atomic_size_t> replaygain_completed_;
    std::size_t replaygain_total_{0U};
    QTimer replaygain_progress_timer_;
    std::shared_ptr<Reading> reading_;
    // ADR-0257: the grid shows cached tags while the files are read; scripts,
    // suggestions and Save wait for the files' own.
    bool provisional_{false};
    bool writing_elsewhere_{false};
    // Proposals waiting for the automatic scripts to finish staging, and
    // the set being staged, previewed again when the draft moved under it.
    std::optional<metadata::MetadataProposalSet> waiting_proposals_;
    std::optional<metadata::MetadataProposalSet> staging_proposals_;
    int proposal_attempts_{0};
    bool replaygain_on_apply_{false};
    bool skip_existing_gain_{true};
    // A scan for a write is under way; Apply waits for it; it has been made.
    bool measuring_for_write_{false};
    bool apply_after_gains_{false};
    bool gains_measured_{false};
    // Read again after a write elsewhere: their drafts were written.
    std::vector<std::size_t> rewritten_items_;
    // The files that could not be read, one a line, for a tooltip.
    QString unreadable_details_;
    QTimer reading_progress_timer_;
    int draft_count_{0};
    bool can_undo_{false};
    bool can_redo_{false};
    int progress_value_{0};
    int progress_maximum_{0};
    bool transformation_catalog_loading_{false};
    bool output_profiles_loading_{false};
    std::optional<core::StableId> editing_output_layout_id_;
    std::optional<core::StableId> editing_destination_id_;
    enum class MoveTarget : std::uint8_t { saved, library, folder };
    MoveTarget move_target_{MoveTarget::saved};
    std::optional<std::string> move_folder_;
    bool write_plan_running_{false};
    bool apply_running_{false};
    bool applying_file_paths_{false};
    bool apply_stop_requested_{false};
    bool apply_committed_{false};
    bool proposal_running_{false};
    bool automatic_stage_running_{false};
    bool replaygain_running_{false};
    bool artwork_operation_running_{false};
    bool layout_state_saved_{false};
};

// The folder images an Apply writes, as they are reviewed first: where each
// goes, what it does there, and the image written.
inline constexpr auto folder_image_review_note =
    "Save publishes these folder images and the reviewed media edits. Each file has its own "
    "recovery journal; a later failure can leave earlier files saved. Existing folder images "
    "retain a recovery backup.";
[[nodiscard]] std::vector<QStringList>
folderImageReviewRows(const std::vector<metadata::FolderImageWritePlan>& images);

} // namespace trackknife::bench
