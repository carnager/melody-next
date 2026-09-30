// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/artwork_services.hpp"
#include "bench/file_work_tools.hpp"
#include "workspace/artwork_session.hpp"

#include <QImage>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QDialog;
class QProgressBar;
class QPushButton;
class QTableView;
class QTreeWidget;

namespace trackknife::bench {

// The tag editor's Artwork page in the widgets window: a view over an
// ArtworkSession, which decides what it shows and does (ADR-0076, ADR-0220).
class MetadataArtworkSection final : public QWidget {
    Q_OBJECT

  public:
    explicit MetadataArtworkSection(QWidget* parent = nullptr);
    ~MetadataArtworkSection() override;

    [[nodiscard]] ArtworkSession& session() { return *session_; }
    QWidget* createCompactCover(QWidget* parent);
    void stageFrontCover(const QString& path) { session_->stageFrontCover(path); }
    void pasteFrontCover(const QImage& image) { session_->pasteFrontCover(image); }
    void removeFrontCover() { session_->removeFrontCover(); }
    void refreshStoragePolicy() { session_->refreshStoragePolicy(); }
    void setScope(std::vector<MetadataArtworkScopeSource> sources,
                  bool source_limit_exceeded = false) {
        session_->setScope(std::move(sources), source_limit_exceeded);
    }
    void setActive(bool active) { session_->setActive(active); }
    // ADR-0237: where the section reads, resizes and hands over artwork --
    // this process's until set.
    void setFileWorkTools(FileWorkTools tools) { session_->setFileWorkTools(std::move(tools)); }
    void setMutationServices(ArtworkWritePlanApplierFactory applier_factory,
                             ArtworkApplyObserver observer) {
        session_->setMutationServices(std::move(applier_factory), std::move(observer));
    }
    void setCoverArtService(ArtworkCoverArtService service) {
        session_->setCoverArtService(std::move(service));
    }
    void setCoverArtRelease(std::optional<QString> release_id) {
        session_->setCoverArtRelease(std::move(release_id));
    }
    void requestOperationCancellation() { session_->requestOperationCancellation(); }
    [[nodiscard]] bool hasPendingChanges() const { return session_->hasPendingChanges(); }
    void discardPendingChanges() { session_->discardPendingChanges(); }
    [[nodiscard]] std::vector<metadata::ArtworkWritePlanIntent> pendingIntents() const {
        return session_->pendingIntents();
    }
    void setUnifiedApply(bool enabled) { session_->setUnifiedApply(enabled); }
    [[nodiscard]] bool isBusy() const { return session_->isBusy(); }

  signals:
    void operationRunningChanged(bool running);
    void frontCoverChanged(const QImage& image, bool mixed);
    void frontActionsChanged(bool editable, bool fetchable);
    void openArtworkRequested();
    void coverSettingsRequested();
    void pendingChangesChanged(bool pending);

  private:
    void sync();
    void openCoverPicker();
    void syncPicker();
    void promptAddition();
    void promptReplacement();
    void promptExport();
    void showFeedback(const QString& window_title, const QString& summary,
                      std::vector<PreparationFeedbackRow> rows);

    ArtworkSession* session_{nullptr};
    QLabel* status_{nullptr};
    QLabel* draft_help_{nullptr};
    QLabel* empty_state_{nullptr};
    QWidget* issues_pane_{nullptr};
    QProgressBar* progress_bar_{nullptr};
    QPushButton* stop_button_{nullptr};
    QTableView* items_{nullptr};
    QTableView* issues_{nullptr};
    QPushButton* fetch_cover_button_{nullptr};
    QPushButton* add_button_{nullptr};
    QPushButton* copy_button_{nullptr};
    QPushButton* export_button_{nullptr};
    QPushButton* replace_button_{nullptr};
    QPushButton* remove_button_{nullptr};
    QPushButton* save_button_{nullptr};
    QPushButton* discard_button_{nullptr};
    QPushButton* undo_pending_button_{nullptr};
    QTableView* pending_view_{nullptr};
    QPointer<QDialog> feedback_dialog_;
    QPointer<QDialog> picker_dialog_;
    QPointer<QTreeWidget> picker_list_;
    std::optional<std::pair<bool, bool>> front_actions_;
};

} // namespace trackknife::bench
