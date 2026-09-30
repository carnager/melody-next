// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/artwork_services.hpp"
#include "bench/file_work_tools.hpp"
#include "bench/preparation_feedback.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/musicbrainz/web_service.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/artwork_export.hpp"
#include "workspace/tagger_session.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QItemSelectionModel>
#include <QObject>
#include <QStandardItemModel>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// The tag editor's covers (ADR-0076): the pictures in and beside the
// selected files with their problems, what is staged to add, replace or
// remove, a cover picked from beside the files or the Cover Art Archive,
// exporting, and saving -- on its own or with the tags' Apply. Nothing here
// reads files on the UI thread or holds image bytes beyond thumbnails. Both
// windows' Artwork pages draw it; it is also the tag editor's artwork.
class ArtworkSession final : public QObject, public TaggerArtwork {
    Q_OBJECT

  public:
    // One image the picker offers: beside the files, or in the archive.
    struct PickerRow {
        QString kind;
        std::size_t index{0U};
        QImage thumbnail;
        QString from;
        QString type;
        QString details;
        QString tool_tip;
    };

    explicit ArtworkSession(QObject* parent = nullptr);
    ~ArtworkSession() override;

    // TaggerArtwork.
    [[nodiscard]] bool hasPendingChanges() const override { return !pending_intents_.empty(); }
    [[nodiscard]] std::vector<metadata::ArtworkWritePlanIntent> pendingIntents() const override {
        return pending_intents_;
    }
    [[nodiscard]] bool isBusy() const override {
        return plan_running_ || apply_running_ || export_running_ || cover_fetch_running_;
    }
    void setWorking(bool working) override;
    void discardPendingChanges() override;
    void requestOperationCancellation() override;
    void setScope(std::vector<MetadataArtworkScopeSource> sources,
                  bool source_limit_exceeded = false) override;
    void setCoverArtRelease(std::optional<QString> release_id) override;

    // ADR-0237: where it reads, resizes and hands over artwork -- this
    // process's until set.
    void setFileWorkTools(FileWorkTools tools) { tools_ = std::move(tools); }
    void setMutationServices(ArtworkWritePlanApplierFactory applier_factory,
                             ArtworkApplyObserver observer);
    void setCoverArtService(ArtworkCoverArtService service);
    // Saved with the tags' Apply rather than its own Save.
    void setUnifiedApply(bool enabled);
    // Read only while it is shown (or its cover is).
    void setActive(bool active);
    void refreshStoragePolicy();

    // What is shown.
    [[nodiscard]] QStandardItemModel* items() const { return items_model_; }
    [[nodiscard]] QItemSelectionModel* itemSelection() const { return item_selection_; }
    [[nodiscard]] QStandardItemModel* pending() const { return pending_model_; }
    [[nodiscard]] QItemSelectionModel* pendingSelection() const { return pending_selection_; }
    [[nodiscard]] QStandardItemModel* issues() const { return issues_model_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString emptyText() const { return empty_text_; }
    [[nodiscard]] bool emptyVisible() const { return empty_visible_; }
    [[nodiscard]] bool issuesVisible() const { return issues_visible_; }
    [[nodiscard]] bool pendingVisible() const { return hasPendingChanges(); }
    [[nodiscard]] QString draftHelp() const { return draft_help_; }
    [[nodiscard]] bool saveVisible() const { return !unified_apply_; }
    [[nodiscard]] bool working() const { return working_; }
    [[nodiscard]] bool progressVisible() const { return progress_visible_; }
    [[nodiscard]] int progressValue() const { return progress_value_; }
    // 0: under way, without a count.
    [[nodiscard]] int progressMaximum() const { return progress_maximum_; }
    [[nodiscard]] bool canStop() const { return progress_visible_ && !stop_requested_; }
    [[nodiscard]] bool canFetch() const;
    [[nodiscard]] bool canAdd() const;
    [[nodiscard]] bool canCopy() const;
    [[nodiscard]] bool canExport() const;
    [[nodiscard]] bool canReplace() const;
    [[nodiscard]] bool canRemove() const { return canReplace(); }
    [[nodiscard]] bool canSave() const;
    [[nodiscard]] bool canDiscard() const;
    [[nodiscard]] bool canUndoSelected() const;
    // The files' front cover, as the compact cover shows it.
    [[nodiscard]] QImage frontImage() const { return shown_front_; }
    [[nodiscard]] bool frontMixed() const { return shown_front_mixed_; }
    [[nodiscard]] bool frontEditable() const { return add_available_ && mutationIdle(); }
    [[nodiscard]] bool canRemoveFront() const;

    // What the user does.
    // A front cover for every file: dropped or chosen, or pasted.
    void stageFrontCover(const QString& path);
    void pasteFrontCover(const QImage& image);
    void removeFrontCover();
    [[nodiscard]] static QStringList addRoles();
    void add(const QString& path, int role_index);
    void replace(const QString& path);
    void removeSelected();
    void copySelected();
    void exportSelected(const QString& directory);
    void save();
    void undoSelectedPending();
    void requestStop();
    // The folder images a Save writes were seen (accepted) or not.
    void folderImagesReviewed(bool accepted);

    // The cover picker.
    [[nodiscard]] bool openPicker();
    [[nodiscard]] bool pickerOpen() const { return picker_open_; }
    [[nodiscard]] const std::vector<PickerRow>& pickerRows() const { return picker_rows_; }
    [[nodiscard]] QString pickerStatus() const { return picker_status_; }
    // The row to start on, once there are rows; -1 for none.
    [[nodiscard]] int pickerSuggested() const { return picker_suggested_; }
    void usePicker(int row);
    void closePicker();

  signals:
    void changed();
    void operationRunningChanged(bool running);
    void frontCoverChanged(const QImage& image, bool mixed);
    void pendingChangesChanged(bool pending);
    // The picker's rows or status changed; it closed.
    void pickerChanged();
    void pickerClosed();
    void feedbackRequested(const QString& title, const QString& summary,
                           std::vector<PreparationFeedbackRow> rows);
    void folderImagesReviewRequested(std::vector<metadata::FolderImageWritePlan> images);

  private:
    struct BatchResult;
    struct ActionTarget {
        MetadataArtworkScopeSource scope;
        metadata::ArtworkInventoryItem item;
    };
    struct LocalCover {
        std::string raw_path;
        QImage thumbnail;
        QString details;
    };

    [[nodiscard]] bool operationIdle() const;
    [[nodiscard]] bool mutationIdle() const;
    [[nodiscard]] std::vector<int> selectedItemRows() const;
    void scheduleInventory();
    void startInventory();
    void finishInventory();
    void clearPresentation();
    void present(const BatchResult& result);
    [[nodiscard]] bool coverServiceReady() const;
    [[nodiscard]] bool localCoversOffered() const;
    [[nodiscard]] bool archiveReady() const;
    void useArchiveImage(const musicbrainz::CoverArtImage& image);
    void reviewFetchedCover(const std::string& replacement_raw_path);
    void dispatchReview(std::vector<metadata::ArtworkWritePlanIntent> intents);
    void startReview(metadata::ArtworkWritePlanIntentKind kind,
                     std::optional<std::string> replacement_raw_path,
                     metadata::ArtworkRole added_role = metadata::ArtworkRole::front,
                     std::string added_description = {},
                     std::optional<metadata::ArtworkInventoryItem> embedded_donor = std::nullopt);
    void finishReview();
    void updatePendingPresentation();
    void startPendingPreviews();
    void finishPendingPreviews();
    void startApply(std::shared_ptr<const metadata::ArtworkWritePlan> plan);
    void updateApplyProgress();
    void finishApply();
    void updateExportProgress();
    void finishExport();
    void setProgressVisible(bool visible, int total = 0);
    void showFront(const QImage& image, bool mixed);
    void setStatus(const QString& text);

    QFutureWatcher<std::shared_ptr<BatchResult>> watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<metadata::ArtworkWritePlan>>> plan_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::ArtworkApplyResult>>> apply_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::ArtworkExportResult>>> export_watcher_;
    QFutureWatcher<std::vector<QImage>> preview_watcher_;
    QFutureWatcher<core::Result<QString>> paste_watcher_;
    QImage front_image_;
    bool front_mixed_{false};
    QImage shown_front_;
    bool shown_front_mixed_{false};
    core::CancellationSource preview_cancellation_;
    std::size_t preview_generation_{0U};
    std::size_t preview_job_generation_{0U};
    bool preview_running_{false};
    QTimer debounce_;
    QTimer apply_progress_timer_;
    QTimer export_progress_timer_;
    QString status_;
    QString draft_help_;
    QString empty_text_;
    bool empty_visible_{true};
    bool issues_visible_{false};
    bool unified_apply_{false};
    bool working_{false};
    bool progress_visible_{false};
    int progress_value_{0};
    int progress_maximum_{0};
    QStandardItemModel* items_model_{nullptr};
    QItemSelectionModel* item_selection_{nullptr};
    QStandardItemModel* pending_model_{nullptr};
    QItemSelectionModel* pending_selection_{nullptr};
    QStandardItemModel* issues_model_{nullptr};
    std::vector<metadata::ArtworkWritePlanIntent> pending_intents_;
    std::vector<metadata::ArtworkWritePlanIntent> pending_rows_;
    std::vector<MetadataArtworkScopeSource> scope_;
    std::vector<std::optional<ActionTarget>> action_targets_;
    std::vector<ActionTarget> copy_targets_;
    ArtworkWritePlanApplierFactory applier_factory_;
    FileWorkTools tools_;
    ArtworkApplyObserver apply_observer_;
    ArtworkCoverArtService cover_service_;
    std::optional<QString> cover_release_id_;
    core::CancellationSource cancellation_;
    core::CancellationSource mutation_cancellation_;
    std::shared_ptr<ArtworkApplyProgressState> apply_progress_state_;
    std::shared_ptr<std::atomic_size_t> export_completed_items_;
    std::shared_ptr<const metadata::ArtworkWritePlan> pending_plan_;
    // Front images found beside the files -- cover.jpg and the like -- as
    // choices for the picker, not as the files' own cover.
    std::vector<LocalCover> local_covers_;
    bool picker_open_{false};
    std::size_t picker_generation_{0U};
    std::vector<PickerRow> picker_rows_;
    std::shared_ptr<std::vector<musicbrainz::CoverArtImage>> picker_images_;
    std::vector<LocalCover> picker_locals_;
    QString picker_status_;
    int picker_suggested_{-1};
    std::size_t generation_{0U};
    std::size_t job_generation_{0U};
    std::size_t displayed_generation_{0U};
    bool source_limit_exceeded_{false};
    bool active_{false};
    bool job_running_{false};
    bool plan_running_{false};
    bool apply_running_{false};
    bool export_running_{false};
    bool cover_fetch_running_{false};
    bool stop_requested_{false};
    bool add_available_{false};
};

} // namespace trackknife::bench
