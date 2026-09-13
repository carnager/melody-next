// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/preparation_feedback_dialog.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/musicbrainz/web_service.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/artwork_export.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QDialog;
class QProgressBar;
class QPushButton;
class QStandardItemModel;
class QTableView;
class QTimer;

namespace trackknife::bench {

// Shared between the UI thread and the background Apply worker; the worker
// writes under the mutex and the inline progress readout copies under it.
struct ArtworkApplyProgressState {
    mutable std::mutex mutex;
    std::vector<operations::ArtworkApplySourceState> states;
    std::vector<std::optional<core::Error>> issues;
    std::size_t completed_sources{0U};
};

inline constexpr std::size_t metadata_artwork_source_limit = 64U;

struct MetadataArtworkScopeSource {
    std::string raw_path;
    std::optional<core::LocalSourceRevision> captured_revision;
    QString label;
    std::vector<std::size_t> occurrence_indexes;
    std::size_t occurrence_count{1U};
    bool captured_revision_consistent{true};

    friend bool operator==(const MetadataArtworkScopeSource&,
                           const MetadataArtworkScopeSource&) = default;
};

using ArtworkWritePlanApplier = std::function<core::Result<operations::ArtworkApplyResult>(
    const metadata::ArtworkWritePlan&, const operations::ArtworkApplyProgressCallback&,
    const core::CancellationToken&)>;
using ArtworkWritePlanApplierFactory = std::function<ArtworkWritePlanApplier()>;
using ArtworkApplyObserver = std::function<void(const operations::ArtworkApplyResult&)>;

// Bench-injected Cover Art Archive boundary (ADR-0091/0094): the listing
// for one release, paced cached byte downloads, and local storage of
// verified PNG/JPEG bytes ready for review. All-empty means cover fetching
// is unavailable.
struct ArtworkCoverArtService {
    std::function<void(const QString& release_id,
                       std::function<void(core::Result<musicbrainz::CoverArtListing>)>)>
        fetch_listing;
    std::function<void(const QString& url, std::function<void(core::Result<QByteArray>)>)>
        fetch_bytes;
    std::function<core::Result<QString>(const QString& identity, const QByteArray& bytes)>
        store_image;
};

// Lazy Properties presentation over ADR-0076's synchronous core inventory.
// Inventory, review, and Apply own no image bytes and perform no filesystem
// work on the UI thread.
class MetadataArtworkSection final : public QWidget {
    Q_OBJECT

  public:
    explicit MetadataArtworkSection(QWidget* parent = nullptr);
    ~MetadataArtworkSection() override;

    void setScope(std::vector<MetadataArtworkScopeSource> sources,
                  bool source_limit_exceeded = false);
    void setActive(bool active);
    void setMutationServices(ArtworkWritePlanApplierFactory applier_factory,
                             ArtworkApplyObserver observer);
    void setCoverArtService(ArtworkCoverArtService service);
    void setCoverArtRelease(std::optional<QString> release_id);
    void requestOperationCancellation();
    [[nodiscard]] bool hasPendingChanges() const { return !pending_intents_.empty(); }
    void discardPendingChanges();
    [[nodiscard]] std::vector<metadata::ArtworkWritePlanIntent> pendingIntents() const {
        return pending_intents_;
    }
    void setUnifiedApply(bool enabled);
    [[nodiscard]] bool isBusy() const {
        return plan_running_ || apply_running_ || export_running_ || cover_fetch_running_;
    }

  signals:
    void operationRunningChanged(bool running);
    void pendingChangesChanged(bool pending);

  private:
    struct BatchResult;
    struct ActionTarget;

    void scheduleInventory();
    void startInventory();
    void finishInventory();
    void clearPresentation();
    void present(const BatchResult& result);
    void updateActionButtons();
    [[nodiscard]] bool coverServiceReady() const;
    void startCoverArtFetch();
    void startArchivePicker();
    void presentArchivePicker(musicbrainz::CoverArtListing listing);
    void useArchiveImage(const musicbrainz::CoverArtImage& image);
    void reviewFetchedCover(const std::string& replacement_raw_path);
    void dispatchReview(std::vector<metadata::ArtworkWritePlanIntent> intents,
                        qsizetype change_count);
    void promptAddition();
    void promptReplacement();
    void reviewRemoval();
    void reviewCopy();
    void promptExport();
    void finishExport();
    void updateExportProgress();
    void showFeedback(const QString& window_title, const QString& summary,
                      std::vector<PreparationFeedbackRow> rows);
    void requestStop();
    void setProgressVisible(bool visible, int total = 0);
    void startReview(metadata::ArtworkWritePlanIntentKind kind,
                     std::optional<std::string> replacement_raw_path,
                     metadata::ArtworkRole added_role = metadata::ArtworkRole::front,
                     std::string added_description = {},
                     std::optional<metadata::ArtworkInventoryItem> embedded_donor = std::nullopt);
    void finishReview();
    void savePendingChanges();
    void updatePendingPresentation();
    void undoSelectedChanges();
    void startPendingPreviews();
    void finishPendingPreviews();
    void startApply(std::shared_ptr<const metadata::ArtworkWritePlan> plan);
    void updateApplyProgress();
    void finishApply();

    QFutureWatcher<std::shared_ptr<BatchResult>> watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<metadata::ArtworkWritePlan>>> plan_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::ArtworkApplyResult>>> apply_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::ArtworkExportResult>>> export_watcher_;
    QFutureWatcher<std::vector<QImage>> preview_watcher_;
    core::CancellationSource preview_cancellation_;
    std::size_t preview_generation_{0U};
    std::size_t preview_job_generation_{0U};
    bool preview_running_{false};
    QTimer* debounce_{nullptr};
    QTimer* apply_progress_timer_{nullptr};
    QTimer* export_progress_timer_{nullptr};
    QLabel* status_{nullptr};
    QLabel* draft_help_{nullptr};
    bool unified_apply_{false};
    QLabel* empty_state_{nullptr};
    QWidget* issues_pane_{nullptr};
    QProgressBar* progress_bar_{nullptr};
    QPushButton* stop_button_{nullptr};
    QTableView* items_{nullptr};
    QTableView* issues_{nullptr};
    QPushButton* fetch_cover_button_{nullptr};
    QPushButton* archive_button_{nullptr};
    QPushButton* add_button_{nullptr};
    QPushButton* copy_button_{nullptr};
    QPushButton* export_button_{nullptr};
    QPushButton* replace_button_{nullptr};
    QPushButton* remove_button_{nullptr};
    QPushButton* save_button_{nullptr};
    QPushButton* discard_button_{nullptr};
    QPushButton* undo_pending_button_{nullptr};
    QTableView* pending_view_{nullptr};
    QStandardItemModel* pending_model_{nullptr};
    std::vector<metadata::ArtworkWritePlanIntent> pending_intents_;
    std::vector<metadata::ArtworkWritePlanIntent> pending_rows_;
    QStandardItemModel* items_model_{nullptr};
    QStandardItemModel* issues_model_{nullptr};
    std::vector<MetadataArtworkScopeSource> scope_;
    std::vector<std::optional<ActionTarget>> action_targets_;
    std::vector<ActionTarget> copy_targets_;
    ArtworkWritePlanApplierFactory applier_factory_;
    ArtworkApplyObserver apply_observer_;
    ArtworkCoverArtService cover_service_;
    std::optional<QString> cover_release_id_;
    core::CancellationSource cancellation_;
    core::CancellationSource mutation_cancellation_;
    std::shared_ptr<ArtworkApplyProgressState> apply_progress_state_;
    std::shared_ptr<std::atomic_size_t> export_completed_items_;
    QPointer<QDialog> feedback_dialog_;
    QPointer<QDialog> archive_dialog_;
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
