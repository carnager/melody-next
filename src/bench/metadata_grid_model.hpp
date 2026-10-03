// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/transformation.hpp"

#include <QAbstractTableModel>
#include <QPointer>
#include <QStringList>

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class QTimer;

namespace trackknife::bench {

inline constexpr int metadata_field_state_role = Qt::UserRole + 200;
inline constexpr int metadata_field_canonical_name_role = Qt::UserRole + 201;
inline constexpr int metadata_cell_values_role = Qt::UserRole + 202;
inline constexpr int metadata_cell_provenance_role = Qt::UserRole + 203;
inline constexpr int metadata_cell_staged_role = Qt::UserRole + 204;
inline constexpr int metadata_cell_baseline_values_role = Qt::UserRole + 205;
inline constexpr int metadata_cell_patch_kind_role = Qt::UserRole + 206;
inline constexpr int metadata_cell_staged_source_role = Qt::UserRole + 207;

struct MetadataFieldInsertion {
    std::size_t field_index{0U};
    bool inserted{false};
};

// Sparse virtual projection over a staged selection. Rows are selected track
// occurrences; metadata fields are columns after the fixed Track column.
class MetadataGridModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    MetadataGridModel(metadata::StagedMetadataSelection selection, QStringList track_labels,
                      QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

    [[nodiscard]] const metadata::StagedMetadataSelection& selection() const noexcept {
        return *selection_;
    }
    [[nodiscard]] std::shared_ptr<const metadata::StagedMetadataSelection>
    sharedSelection() const noexcept {
        return selection_;
    }
    [[nodiscard]] std::optional<int> fieldColumn(const QString& name) const;
    [[nodiscard]] core::Result<MetadataFieldInsertion> ensureField(const QString& name);
    [[nodiscard]] QString trackLabel(int row) const;
    [[nodiscard]] core::Result<std::size_t>
    advanceSourceRevision(const std::string& raw_path,
                          const core::LocalSourceRevision& previous_revision,
                          const core::LocalSourceRevision& published_revision);
    [[nodiscard]] const metadata::StagedMetadataPatchSet& patches() const noexcept {
        return patches_;
    }
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    [[nodiscard]] bool discardAll();
    [[nodiscard]] bool replaceFieldValues(std::span<const std::size_t> item_indexes,
                                          std::size_t field_index, std::vector<std::string> values);
    [[nodiscard]] bool removeFields(std::span<const std::size_t> item_indexes,
                                    const std::vector<std::size_t>& field_indexes);
    [[nodiscard]] bool revertFields(std::span<const std::size_t> item_indexes,
                                    const std::vector<std::size_t>& field_indexes);
    [[nodiscard]] bool stageTransformation(const metadata::MetadataTransformationPreview& preview,
                                           const QStringList& step_sources = {});
    // ADR-0257: the selection built from the files' own tags, taking the place
    // of the provisional one built from cached tags. Drafts carry over by row
    // and field name; the undo history before it is dropped. Answers how many
    // drafts carried and how many were no change against the real tags.
    [[nodiscard]] core::Result<std::pair<std::size_t, std::size_t>>
    adoptReadSelection(metadata::StagedMetadataSelection read);

  signals:
    void draftStateChanged(int patch_count, bool can_undo, bool can_redo);
    void editRejected(const QString& message);

  private:
    struct DraftMutation {
        std::size_t item_index{0U};
        std::size_t field_index{0U};
        std::optional<metadata::StagedMetadataPatch> before;
        std::optional<metadata::StagedMetadataPatch> after;
    };

    struct DraftTransaction {
        std::vector<DraftMutation> mutations;
        std::size_t text_bytes{0U};
    };

    struct DraftRequest {
        std::size_t item_index{0U};
        std::size_t field_index{0U};
        std::optional<metadata::StagedMetadataPatch> desired;
    };

    struct FieldDraftRequest {
        std::size_t field_index{0U};
        std::optional<metadata::StagedMetadataPatchKind> kind;
        std::vector<std::string> values;
    };

    [[nodiscard]] bool applyFieldRequests(std::span<const std::size_t> item_indexes,
                                          std::vector<FieldDraftRequest> requests);
    [[nodiscard]] bool applyRequests(std::vector<DraftRequest> requests);
    [[nodiscard]] core::Result<bool> applyPatch(const DraftRequest& request);
    void restorePatch(const DraftMutation& mutation, bool use_after);
    void notifyMutations(const std::vector<DraftMutation>& mutations);
    void pushHistory(DraftTransaction transaction);

    [[nodiscard]] QString stagedSourceLabel(std::size_t item_index, std::size_t field_index) const;

    std::shared_ptr<const metadata::StagedMetadataSelection> selection_;
    metadata::StagedMetadataPatchSet patches_;
    // The script or provider that staged each cell, valid only while the
    // current patch still equals the one that staging stored — a hand edit
    // or undo silently retires the label, redo revives it.
    std::map<std::pair<std::size_t, std::size_t>, std::pair<metadata::StagedMetadataPatch, QString>>
        staged_sources_;
    QStringList track_labels_;
    std::vector<DraftTransaction> history_;
    std::size_t history_cursor_{0U};
    std::size_t history_text_bytes_{0U};
};

// Selection-scoped edit projection over MetadataGridModel's shared draft.
// Fields are rows; Original and Draft are columns. The selected item indexes
// determine whether each edit is individual or bulk.
class MetadataAggregateModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    explicit MetadataAggregateModel(MetadataGridModel* grid_model, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    [[nodiscard]] std::optional<int> fieldRow(const QString& name) const;
    [[nodiscard]] core::Result<int> ensureField(const QString& name);
    [[nodiscard]] bool removeIndexes(const QModelIndexList& indexes);
    [[nodiscard]] bool revertIndexes(const QModelIndexList& indexes);
    [[nodiscard]] bool replaceRowValues(int row, std::vector<std::string> values);
    void setSelectedItems(std::vector<std::size_t> item_indexes);
    [[nodiscard]] std::size_t selectedItemCount() const noexcept { return item_indexes_.size(); }
    [[nodiscard]] bool summaryReady() const noexcept { return summary_ready_; }
    [[nodiscard]] bool draftPreviewReady() const noexcept { return draft_ready_; }

  signals:
    void editRejected(const QString& message);
    void selectionProjectionChanged(bool ready, int item_count);
    void draftProjectionChanged(bool ready);

  private:
    struct UniformDraft {
        metadata::StagedMetadataPatchKind kind{metadata::StagedMetadataPatchKind::replace_values};
        std::vector<std::string> values;
    };

    [[nodiscard]] QVariant originalData(std::size_t field_index, int role) const;
    [[nodiscard]] QVariant draftData(std::size_t field_index, int role) const;
    [[nodiscard]] std::vector<std::size_t> selectedFields(const QModelIndexList& indexes) const;
    void
    applySelectionSummary(std::size_t generation,
                          core::Result<std::vector<metadata::StagedMetadataSubsetField>> result);
    void refreshDraftCounts();
    void refreshDraftCount(std::size_t field_index);
    void invalidateDraftProjection();
    void startDraftProjection();
    void
    applyDraftProjection(std::size_t generation,
                         core::Result<std::vector<metadata::StagedMetadataFieldProjection>> result);
    void gridDataChanged(const QModelIndex& top_left, const QModelIndex& bottom_right);
    void notifyField(std::size_t field_index);

    QPointer<MetadataGridModel> grid_model_;
    std::map<std::size_t, UniformDraft> uniform_drafts_;
    std::vector<std::size_t> item_indexes_;
    std::vector<metadata::StagedMetadataSubsetField> subset_fields_;
    std::vector<metadata::StagedMetadataFieldProjection> draft_fields_;
    std::vector<std::size_t> staged_counts_;
    std::size_t selection_generation_{0U};
    std::size_t draft_generation_{0U};
    core::CancellationSource draft_cancellation_;
    QTimer* draft_debounce_{nullptr};
    bool field_insert_pending_{false};
    bool summary_ready_{true};
    bool draft_ready_{true};
    bool draft_counts_ready_{true};
    bool draft_matches_original_{true};
    bool draft_job_running_{false};
    bool applying_uniform_edit_{false};
};

} // namespace trackknife::bench
