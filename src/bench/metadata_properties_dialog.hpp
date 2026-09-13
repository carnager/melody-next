// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/metadata_artwork_section.hpp"
#include "bench/musicbrainz_identify_dialog.hpp"
#include "bench/preparation_feedback_dialog.hpp"
#include "bench/replaygain_scan.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/transformation.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/preparation_plan.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QByteArray>
#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QStringList>

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
#include <utility>
#include <vector>

class QModelIndex;
class QCloseEvent;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QEvent;
class QLabel;
class QInputDialog;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTabWidget;
class QTemporaryDir;
class QTableView;
class QTimer;
class QTreeWidget;
class QVBoxLayout;

namespace trackknife::bench {

class MetadataGridModel;
class MetadataAggregateModel;
class MetadataFieldReviewBar;

struct MetadataPropertiesSource {
    metadata::StagedMetadataSource source;
    QString track_label;
    MetadataPropertiesAudioSource audio{};
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

struct OutputProfileStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                           std::vector<persistence::SavedDestinationProfile>, QString)>;
    using Completion = std::function<void(QString)>;

    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedOutputLayoutProfile, Completion)> save_layout;
    std::function<void(core::StableId, Completion)> remove_layout;
    std::function<void(persistence::SavedDestinationProfile, Completion)> save_destination;
    std::function<void(core::StableId, Completion)> remove_destination;
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

class MetadataPropertiesDialog final : public QDialog {
    Q_OBJECT

  public:
    MetadataPropertiesDialog(std::size_t requested_item_count,
                             MetadataPropertiesSourceReader source_reader,
                             std::span<const std::string_view> preferred_fields,
                             MetadataWritePlanApplierFactory plan_applier_factory,
                             MetadataApplyObserver apply_observer,
                             MetadataTransformationStore transformation_store = {},
                             OutputProfileStore output_profile_store = {},
                             FilePublicationPlanApplierFactory file_plan_applier_factory = {},
                             FilePublicationApplyObserver file_apply_observer = {},
                             QWidget* parent = nullptr, MetadataDialogLayoutStore layout_store = {},
                             MusicBrainzLookupService musicbrainz = {});
    ~MetadataPropertiesDialog() override;

    void setArtworkMutationServices(ArtworkWritePlanApplierFactory applier_factory,
                                    ArtworkApplyObserver observer);

  private:
    using SelectionResult = core::Result<metadata::StagedMetadataSelection>;
    using WritePlanResult = core::Result<operations::PreparationPlan>;
    struct OutputLayoutPreviewRow {
        std::string source_raw_path;
        std::string target_relative_path;
    };
    struct OutputLayoutPreview {
        std::vector<OutputLayoutPreviewRow> rows;
        std::size_t item_count{0U};
        bool truncated{false};
    };
    using OutputLayoutExampleResult = core::Result<OutputLayoutPreview>;

    void captureSources();
    void startSelection();
    void finishSelection();
    void buildGrid(metadata::StagedMetadataSelection selection);
    void scheduleSelectionProjection();
    void updateSelectionProjection();
    void updateArtworkScope(std::span<const std::size_t> selected_items);
    void artworkApplied(const operations::ArtworkApplyResult& result);
    [[nodiscard]] core::Result<QString> storeCoverArtImage(const QString& release_id,
                                                           const QByteArray& bytes);
    void updateDraftState(int patch_count, bool can_undo, bool can_redo);
    void updateFieldButtons();
    void updateEditValuesButton();
    void updateTransformationButton();
    void loadTransformationCatalog(std::optional<core::StableId> selected = std::nullopt);
    void
    rebuildTransformationCatalogControls(std::optional<core::StableId> selected = std::nullopt);
    void toggleAutomaticTransformation(core::StableId id, bool enabled);
    void loadOutputProfiles();
    void
    rebuildOutputProfileControls(std::optional<core::StableId> selected_layout = std::nullopt,
                                 std::optional<core::StableId> selected_destination = std::nullopt);
    void selectOutputLayout(int index);
    void selectDestination(int index);
    void newOutputLayout();
    void newDestination();
    void saveOutputLayout();
    void saveDestination();
    void removeOutputLayout();
    void removeDestination();
    void updateOutputProfileButtons();
    void scheduleOutputLayoutExample();
    void startOutputLayoutExample();
    void finishOutputLayoutExample();
    void updateWritePlanButton();
    void invalidateWritePlan();
    void startProposals();
    void finishProposals();
    void stageAutomaticTransformations();
    void finishAutomaticStage();
    struct AutomaticChainPlan {
        metadata::MetadataTransformationChain chain;
        QStringList step_sources;
    };
    [[nodiscard]] std::optional<AutomaticChainPlan> combinedAutomaticChain() const;
    void startIdentify();
    void startReplayGainScan(std::vector<std::size_t> forced_items = {});
    [[nodiscard]] QString replayGainStatusLinks() const;
    void exportReplayGainResults();
    void showLoudnessProvenance();
    [[nodiscard]] bool
    stageTransformationPreservingSelection(const metadata::MetadataTransformationPreview& preview,
                                           const QStringList& step_sources = {});
    void showStickyStatus(const QString& text);
    void finishReplayGainScan();
    void openIdentifyDialog(std::vector<musicbrainz::LocalTrackDescriptor> descriptors,
                            std::vector<QString> local_paths, std::vector<std::size_t> items,
                            QString initial_artist, QString initial_release);
    void applyMusicBrainzProposals(metadata::MetadataProposalSet proposals);
    void startWritePlan();
    void finishWritePlan();
    void showPreparationFeedback(const QString& window_title, const QString& summary,
                                 std::vector<PreparationFeedbackRow> rows);
    void requestApplyStop();
    void setApplyProgressVisible(bool visible);
    void startApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void startMetadataApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void startFileApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void updateApplyProgress();
    void finishMetadataApply();
    void finishFileApply();
    [[nodiscard]] QStringList metadataFieldNameSuggestions(const QString& query) const;
    [[nodiscard]] std::vector<std::size_t> selectedItemIndexes() const;
    void promptAddField();
    void removeSelectedFields();
    void editCurrentValues();
    void promptTransformation(std::optional<core::StableId> initially_selected = std::nullopt,
                              bool preview_initially_selected = false);
    void restoreLayoutState();
    void persistLayoutState();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

    QFutureWatcher<std::shared_ptr<SelectionResult>> selection_watcher_;
    QFutureWatcher<std::shared_ptr<WritePlanResult>> write_plan_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::MetadataApplyResult>>>
        metadata_apply_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::FilePublicationApplyResult>>>
        file_apply_watcher_;
    QFutureWatcher<std::shared_ptr<OutputLayoutExampleResult>> output_example_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<metadata::MetadataTransformationPreview>>>
        proposal_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<metadata::MetadataTransformationPreview>>>
        automatic_watcher_;
    QFutureWatcher<std::shared_ptr<ReplayGainScanOutcome>> replaygain_watcher_;
    MetadataPropertiesSourceReader source_reader_;
    MetadataWritePlanApplierFactory plan_applier_factory_;
    MetadataApplyObserver apply_observer_;
    ArtworkWritePlanApplierFactory artwork_plan_applier_factory_;
    ArtworkApplyObserver artwork_apply_observer_;
    MetadataTransformationStore transformation_store_;
    OutputProfileStore output_profile_store_;
    FilePublicationPlanApplierFactory file_plan_applier_factory_;
    FilePublicationApplyObserver file_apply_observer_;
    MetadataDialogLayoutStore layout_store_;
    MusicBrainzLookupService musicbrainz_;
    std::unique_ptr<QTemporaryDir> cover_art_directory_;
    std::vector<persistence::SavedMetadataTransformationChain> transformation_catalog_;
    std::vector<persistence::SavedOutputLayoutProfile> output_layout_catalog_;
    std::vector<persistence::SavedDestinationProfile> destination_catalog_;
    std::vector<metadata::StagedMetadataSource> sources_;
    // Filled only during bounded capture; scan workers share a const view.
    std::shared_ptr<std::vector<MetadataPropertiesAudioSource>> audio_sources_{
        std::make_shared<std::vector<MetadataPropertiesAudioSource>>()};
    std::vector<std::string> preferred_fields_;
    std::vector<std::string> recent_field_names_;
    QStringList track_labels_;
    std::size_t requested_item_count_{0U};
    std::size_t capture_index_{0U};
    QVBoxLayout* root_layout_{nullptr};
    QLabel* summary_{nullptr};
    QLabel* read_only_{nullptr};
    QLabel* loading_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* undo_button_{nullptr};
    QPushButton* redo_button_{nullptr};
    QPushButton* discard_button_{nullptr};
    QPushButton* add_field_button_{nullptr};
    QPushButton* remove_field_button_{nullptr};
    QPushButton* edit_values_button_{nullptr};
    QPushButton* suggest_button_{nullptr};
    QPushButton* identify_button_{nullptr};
    QPushButton* transform_button_{nullptr};
    QWidget* transformation_panel_{nullptr};
    QWidget* grid_tools_{nullptr};
    QListWidget* transformation_list_{nullptr};
    QLabel* transformation_status_{nullptr};
    QCheckBox* save_tags_check_{nullptr};
    QCheckBox* rename_files_check_{nullptr};
    QCheckBox* move_files_check_{nullptr};
    QPushButton* replaygain_scan_button_{nullptr};
    QComboBox* replaygain_grouping_{nullptr};
    QLineEdit* replaygain_expression_{nullptr};
    QCheckBox* replaygain_sidecar_only_{nullptr};
    QCheckBox* replaygain_true_peak_{nullptr};
    QPushButton* replaygain_provenance_button_{nullptr};
    std::vector<std::size_t> replaygain_retry_items_;
    QStringList replaygain_export_rows_;
    QComboBox* output_layout_combo_{nullptr};
    QLineEdit* output_layout_name_{nullptr};
    QLineEdit* output_directory_expression_{nullptr};
    QLineEdit* output_basename_expression_{nullptr};
    QPushButton* output_layout_new_button_{nullptr};
    QPushButton* output_layout_save_button_{nullptr};
    QPushButton* output_layout_remove_button_{nullptr};
    QComboBox* destination_combo_{nullptr};
    QLineEdit* destination_name_{nullptr};
    QLineEdit* destination_root_{nullptr};
    QPushButton* destination_browse_button_{nullptr};
    QPushButton* destination_new_button_{nullptr};
    QPushButton* destination_save_button_{nullptr};
    QPushButton* destination_remove_button_{nullptr};
    QLabel* output_profile_status_{nullptr};
    QLabel* output_layout_example_{nullptr};
    QTreeWidget* output_layout_preview_{nullptr};
    QPushButton* apply_plan_button_{nullptr};
    QProgressBar* apply_progress_bar_{nullptr};
    QPushButton* apply_stop_button_{nullptr};
    MetadataGridModel* grid_model_{nullptr};
    MetadataAggregateModel* aggregate_model_{nullptr};
    // ADR-0152: read-only technical summary under the file list, fed by a
    // bounded background prober with a dialog-lifetime path cache.
    struct TechnicalInfo {
        std::string codec;
        int sample_rate{0};
        int bits{0};
        int channels{0};
        std::int64_t bit_rate{0};
        std::int64_t duration_ms{-1};
    };
    void updateTechnicalSummary();
    void pumpTechnicalQueue();

    QTableView* fields_{nullptr};
    MetadataFieldReviewBar* field_review_bar_{nullptr};
    QTableView* file_list_{nullptr};
    QLabel* technical_status_{nullptr};
    std::map<std::string, std::optional<TechnicalInfo>> technical_cache_;
    std::deque<std::string> technical_queue_;
    std::set<std::string> technical_pending_;
    bool technical_probing_{false};
    bool technical_truncated_{false};
    QFutureWatcher<std::pair<std::string, std::optional<TechnicalInfo>>> technical_watcher_;
    core::CancellationSource technical_cancellation_;
    QTabWidget* metadata_sections_{nullptr};
    MetadataArtworkSection* artwork_section_{nullptr};
    QSplitter* content_splitter_{nullptr};
    QSplitter* metadata_splitter_{nullptr};
    QTimer* selection_debounce_{nullptr};
    QTimer* apply_progress_timer_{nullptr};
    QTimer* output_example_debounce_{nullptr};
    QPointer<QDialog> exact_values_dialog_;
    QPointer<QInputDialog> field_name_dialog_;
    QPointer<QDialog> transformation_dialog_;
    QPointer<QDialog> identify_dialog_;
    std::shared_ptr<const operations::PreparationPlan> active_metadata_plan_;
    bool metadata_had_commits_{false};
    QPointer<QDialog> feedback_dialog_;
    std::shared_ptr<MetadataApplyProgressState> apply_progress_state_;
    std::shared_ptr<FilePublicationApplyProgressState> file_apply_progress_state_;
    QString selection_summary_;
    QString revision_summary_;
    QByteArray pending_content_splitter_state_;
    QByteArray pending_metadata_splitter_state_;
    std::size_t loaded_item_count_{0U};
    std::size_t loaded_source_count_{0U};
    std::size_t loaded_field_count_{0U};
    std::size_t selected_item_count_{0U};
    std::size_t write_plan_generation_{0U};
    std::size_t write_plan_job_generation_{0U};
    std::size_t output_example_generation_{0U};
    std::size_t output_example_job_generation_{0U};
    core::CancellationSource write_plan_cancellation_;
    core::CancellationSource replaygain_cancellation_;
    core::CancellationSource apply_cancellation_;
    core::CancellationSource output_example_cancellation_;
    int draft_count_{0};
    bool transformation_catalog_loading_{false};
    bool output_profiles_loading_{false};
    bool output_profile_mutation_running_{false};
    std::optional<core::StableId> editing_output_layout_id_;
    std::optional<core::StableId> editing_destination_id_;
    std::string destination_root_raw_path_;
    bool write_plan_running_{false};
    bool proposal_running_{false};
    bool automatic_stage_running_{false};
    bool replaygain_running_{false};
    QStringList automatic_step_sources_;
    QString sticky_status_;
    bool apply_running_{false};
    bool artwork_operation_running_{false};
    bool applying_file_paths_{false};
    bool apply_stop_requested_{false};
    bool apply_committed_{false};
    bool layout_state_saved_{false};
    bool output_example_running_{false};
    bool output_example_pending_{false};
};

} // namespace trackknife::bench
