// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_grid_model.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/metadata/flac_mapping.hpp"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFutureWatcher>
#include <QPalette>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString display_utf8(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString state_label(const metadata::MetadataSelectionFieldState state) {
    auto label = display_utf8(metadata::metadata_selection_field_state_name(state));
    if (!label.isEmpty()) {
        label[0] = label[0].toUpper();
    }
    return label;
}

// Picard-style draft semantics on the changed content only: green text =
// field added, orange text = values changed, red struck-through text = field
// removed. Medium-strength hues read on both light and dark palettes; the
// cell background is never painted.
enum class DraftChangeKind : std::uint8_t { added, changed, removed };

[[nodiscard]] QBrush draft_change_brush(const DraftChangeKind kind) {
    switch (kind) {
    case DraftChangeKind::added:
        return QBrush{QColor{46, 160, 67}};
    case DraftChangeKind::changed:
        return QBrush{QColor{230, 126, 34}};
    case DraftChangeKind::removed:
        return QBrush{QColor{231, 76, 60}};
    }
    return {};
}

[[nodiscard]] QFont draft_change_font(const DraftChangeKind kind) {
    auto font = QApplication::font();
    font.setItalic(true);
    font.setStrikeOut(kind == DraftChangeKind::removed);
    return font;
}

[[nodiscard]] QStringList display_values(const std::vector<std::string>& values) {
    QStringList displayed;
    displayed.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) {
        displayed.push_back(display_utf8(value));
    }
    return displayed;
}

[[nodiscard]] QStringList visible_values(const std::vector<std::string>& values) {
    auto displayed = display_values(values);
    for (auto& value : displayed) {
        if (value.isEmpty()) {
            value = QStringLiteral("(empty value)");
        }
    }
    return displayed;
}

struct CellProjection {
    const metadata::StagedMetadataCell* baseline{nullptr};
    const metadata::StagedMetadataPatch* patch{nullptr};
    const std::vector<std::string>* values{nullptr};
    bool present{false};
};

[[nodiscard]] CellProjection project_cell(const metadata::StagedMetadataSelection& selection,
                                          const metadata::StagedMetadataPatchSet& patches,
                                          const std::size_t item_index,
                                          const std::size_t field_index) {
    const auto* baseline = selection.cell(item_index, field_index);
    const auto* patch = patches.patch(item_index, field_index);
    if (patch == nullptr) {
        return {.baseline = baseline,
                .patch = nullptr,
                .values = baseline == nullptr ? nullptr : &baseline->values,
                .present = baseline != nullptr};
    }
    if (patch->kind == metadata::StagedMetadataPatchKind::remove_field) {
        return {.baseline = baseline, .patch = patch, .values = nullptr, .present = false};
    }
    return {.baseline = baseline, .patch = patch, .values = &patch->values, .present = true};
}

[[nodiscard]] std::size_t patch_text_bytes(const metadata::StagedMetadataPatch& patch) {
    std::size_t bytes = 0U;
    for (const auto& value : patch.values) {
        bytes += value.size();
    }
    return bytes;
}

constexpr std::size_t maximum_inline_edit_bytes = 4U * 1'024U * 1'024U;
constexpr std::size_t maximum_history_transactions = 256U;
constexpr std::size_t maximum_history_text_bytes = 64U * 1'024U * 1'024U;
constexpr std::size_t maximum_field_transaction_cells = 100'000U;

} // namespace

MetadataGridModel::MetadataGridModel(metadata::StagedMetadataSelection selection,
                                     QStringList track_labels, QObject* parent)
    : QAbstractTableModel(parent),
      selection_(std::make_shared<const metadata::StagedMetadataSelection>(std::move(selection))),
      track_labels_(std::move(track_labels)) {}

int MetadataGridModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(std::min(selection_->item_count(),
                                     static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

int MetadataGridModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return 1 + static_cast<int>(
                   std::min(selection_->field_count(),
                            static_cast<std::size_t>(std::numeric_limits<int>::max() - 1)));
}

QVariant MetadataGridModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() || index.column() < 0 ||
        index.column() >= columnCount()) {
        return {};
    }
    const auto item_index = static_cast<std::size_t>(index.row());
    if (index.column() == 0) {
        if (role == Qt::DisplayRole) {
            return trackLabel(index.row());
        }
        if (role == Qt::ToolTipRole) {
            return QString::fromStdString(
                core::escape_raw_path(selection_->source(item_index).raw_path));
        }
        return {};
    }

    const auto field_index = static_cast<std::size_t>(index.column() - 1);
    const auto projected = project_cell(*selection_, patches_, item_index, field_index);
    const auto* cell = projected.baseline;
    if (role == metadata_field_state_role) {
        return static_cast<int>(selection_->field(field_index).state);
    }
    if (role == metadata_field_canonical_name_role) {
        return display_utf8(selection_->field(field_index).canonical_name);
    }
    if (role == metadata_cell_values_role) {
        return projected.values == nullptr ? QStringList{} : display_values(*projected.values);
    }
    if (role == metadata_cell_baseline_values_role) {
        return cell == nullptr ? QStringList{} : display_values(cell->values);
    }
    if (role == metadata_cell_provenance_role) {
        return projected.patch != nullptr || cell == nullptr ? -1
                                                             : static_cast<int>(cell->provenance);
    }
    if (role == metadata_cell_staged_role) {
        return projected.patch != nullptr;
    }
    if (role == metadata_cell_patch_kind_role) {
        return projected.patch == nullptr ? -1 : static_cast<int>(projected.patch->kind);
    }
    if (role == Qt::EditRole) {
        if (projected.values != nullptr && projected.values->size() == 1U) {
            return display_utf8(projected.values->front());
        }
        return QString{};
    }
    if (role == Qt::DisplayRole) {
        if (projected.patch != nullptr && !projected.present) {
            return QStringLiteral("(remove)");
        }
        if (!projected.present) {
            return QStringLiteral("—");
        }
        const auto values = visible_values(*projected.values);
        return values.empty() ? QStringLiteral("(empty field)")
                              : values.join(QStringLiteral("  ·  "));
    }
    if (role == metadata_cell_staged_source_role) {
        return projected.patch == nullptr ? QString{} : stagedSourceLabel(item_index, field_index);
    }
    if ((role == Qt::ForegroundRole || role == Qt::FontRole) && projected.patch != nullptr) {
        const auto kind =
            projected.patch->kind == metadata::StagedMetadataPatchKind::remove_field
                ? DraftChangeKind::removed
                : (cell == nullptr ? DraftChangeKind::added : DraftChangeKind::changed);
        if (role == Qt::ForegroundRole) {
            return draft_change_brush(kind);
        }
        auto font = draft_change_font(kind);
        // Scripted and provider edits speak in italics; hand edits stay
        // upright.
        font.setItalic(!stagedSourceLabel(item_index, field_index).isEmpty());
        return font;
    }
    if (role == Qt::ForegroundRole && !projected.present) {
        return QApplication::palette().brush(QPalette::PlaceholderText);
    }
    if (role == Qt::ToolTipRole) {
        if (projected.patch != nullptr) {
            const auto original = cell == nullptr
                                      ? QStringLiteral("missing")
                                      : visible_values(cell->values).join(QLatin1Char('\n'));
            const auto draft = projected.present
                                   ? visible_values(*projected.values).join(QLatin1Char('\n'))
                                   : QStringLiteral("remove field");
            const auto source = stagedSourceLabel(item_index, field_index);
            const auto provenance_line =
                source.isEmpty() ? QString{} : QStringLiteral("\nStaged by %1").arg(source);
            return QStringLiteral("Draft:\n%1\n\nOriginal:\n%2\n%3\nNot written to file")
                .arg(draft, original, provenance_line);
        }
        if (!projected.present) {
            return QStringLiteral("Missing on this track");
        }
        const auto values = visible_values(*projected.values);
        const auto provenance = display_utf8(metadata::field_provenance_name(cell->provenance));
        return QStringLiteral("%1\n\nSource: %2")
            .arg(values.empty() ? QStringLiteral("(empty field)") : values.join(QLatin1Char('\n')),
                 provenance);
    }
    return {};
}

QVariant MetadataGridModel::headerData(const int section, const Qt::Orientation orientation,
                                       const int role) const {
    if (orientation == Qt::Vertical) {
        return role == Qt::DisplayRole ? QVariant{section + 1} : QVariant{};
    }
    if (section < 0 || section >= columnCount()) {
        return {};
    }
    if (section == 0) {
        if (role == Qt::DisplayRole) {
            return QStringLiteral("Track");
        }
        return {};
    }
    const auto field_index = static_cast<std::size_t>(section - 1);
    const auto& field = selection_->field(field_index);
    const auto draft_count = patches_.field_patch_count(field_index);
    if (role == Qt::DisplayRole) {
        auto label = QStringLiteral("%1 · %2").arg(display_utf8(field.display_name),
                                                   state_label(field.state));
        if (draft_count > 0U) {
            label += QStringLiteral(" · Draft %1").arg(draft_count);
        }
        return label;
    }
    if (role == Qt::ToolTipRole) {
        auto tooltip = QStringLiteral("Baseline: %1 on %2 of %3 selected tracks")
                           .arg(state_label(field.state))
                           .arg(field.present_item_count)
                           .arg(selection_->item_count());
        if (draft_count > 0U) {
            tooltip += QStringLiteral("\n%1 staged cells; result state is calculated in preview")
                           .arg(draft_count);
        }
        return tooltip;
    }
    if (role == metadata_field_state_role) {
        return static_cast<int>(field.state);
    }
    if (role == metadata_field_canonical_name_role) {
        return display_utf8(field.canonical_name);
    }
    return {};
}

Qt::ItemFlags MetadataGridModel::flags(const QModelIndex& index) const {
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

std::optional<int> MetadataGridModel::fieldColumn(const QString& name) const {
    const auto encoded = name.toUtf8();
    const std::string_view bytes{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto native_first = !metadata::resolve_text_property_identity(bytes).conventional;
    auto field_index =
        native_first ? selection_->exact_native_field_index(bytes) : selection_->field_index(bytes);
    if (!field_index) {
        field_index = native_first ? selection_->field_index(bytes)
                                   : selection_->exact_native_field_index(bytes);
    }
    if (!field_index || *field_index >= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(*field_index) + 1;
}

core::Result<MetadataFieldInsertion> MetadataGridModel::ensureField(const QString& name) {
    constexpr auto maximum_field_name_bytes = std::size_t{1'024U};
    const auto trimmed = name.trimmed();
    const auto encoded = trimmed.toUtf8();
    const std::string_view bytes{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    if (bytes.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "metadata field name cannot be empty",
            .context = {},
        });
    }
    if (bytes.size() > maximum_field_name_bytes) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "metadata field names are limited to 1024 bytes",
            .context = {{.key = "limit", .value = std::to_string(maximum_field_name_bytes)}},
        });
    }
    if (const auto existing_column = fieldColumn(trimmed)) {
        return MetadataFieldInsertion{
            .field_index = static_cast<std::size_t>(*existing_column - 1),
            .inserted = false,
        };
    }

    const auto old_field_count = selection_->field_count();
    auto extended = std::make_shared<metadata::StagedMetadataSelection>(*selection_);
    const auto added = extended->ensure_missing_field(bytes, bytes);
    if (!added) {
        return std::unexpected(added.error());
    }
    if (extended->field_count() != old_field_count + 1U || *added != old_field_count) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "metadata field insertion did not append one field",
            .context = {},
        });
    }
    const auto column = static_cast<int>(*added) + 1;
    beginInsertColumns({}, column, column);
    selection_ = std::move(extended);
    endInsertColumns();
    return MetadataFieldInsertion{.field_index = *added, .inserted = true};
}

core::Result<std::size_t>
MetadataGridModel::advanceSourceRevision(const std::string& raw_path,
                                         const core::LocalSourceRevision& previous_revision,
                                         const core::LocalSourceRevision& published_revision) {
    auto revised = std::make_shared<metadata::StagedMetadataSelection>(*selection_);
    auto result = revised->advance_source_revision(raw_path, previous_revision, published_revision);
    if (result && *result > 0U) {
        selection_ = std::move(revised);
    }
    return result;
}

QString MetadataGridModel::trackLabel(const int row) const {
    if (row < 0 || row >= rowCount()) {
        return {};
    }
    if (row < track_labels_.size() && !track_labels_[row].isEmpty()) {
        return track_labels_[row];
    }
    return QString::fromStdString(
        core::escape_raw_path(selection_->source(static_cast<std::size_t>(row)).raw_path));
}

bool MetadataGridModel::undo() {
    if (history_cursor_ == 0U) {
        return false;
    }
    auto& transaction = history_[--history_cursor_];
    for (auto mutation = transaction.mutations.rbegin(); mutation != transaction.mutations.rend();
         ++mutation) {
        restorePatch(*mutation, false);
    }
    notifyMutations(transaction.mutations);
    return true;
}

bool MetadataGridModel::redo() {
    if (history_cursor_ >= history_.size()) {
        return false;
    }
    auto& transaction = history_[history_cursor_++];
    for (const auto& mutation : transaction.mutations) {
        restorePatch(mutation, true);
    }
    notifyMutations(transaction.mutations);
    return true;
}

bool MetadataGridModel::discardAll() {
    if (patches_.empty()) {
        return false;
    }
    patches_.clear();
    history_.clear();
    history_cursor_ = 0U;
    history_text_bytes_ = 0U;
    if (rowCount() > 0 && columnCount() > 1) {
        emit dataChanged(index(0, 1), index(rowCount() - 1, columnCount() - 1));
        emit headerDataChanged(Qt::Horizontal, 1, columnCount() - 1);
    }
    emit draftStateChanged(0, false, false);
    return true;
}

bool MetadataGridModel::replaceFieldValues(const std::span<const std::size_t> item_indexes,
                                           const std::size_t field_index,
                                           std::vector<std::string> values) {
    return applyFieldRequests(item_indexes,
                              {FieldDraftRequest{
                                  .field_index = field_index,
                                  .kind = metadata::StagedMetadataPatchKind::replace_values,
                                  .values = std::move(values),
                              }});
}

bool MetadataGridModel::removeFields(const std::span<const std::size_t> item_indexes,
                                     const std::vector<std::size_t>& field_indexes) {
    std::vector<FieldDraftRequest> requests;
    requests.reserve(field_indexes.size());
    for (const auto field_index : field_indexes) {
        requests.push_back(FieldDraftRequest{
            .field_index = field_index,
            .kind = metadata::StagedMetadataPatchKind::remove_field,
            .values = {},
        });
    }
    return applyFieldRequests(item_indexes, std::move(requests));
}

bool MetadataGridModel::revertFields(const std::span<const std::size_t> item_indexes,
                                     const std::vector<std::size_t>& field_indexes) {
    std::vector<FieldDraftRequest> requests;
    requests.reserve(field_indexes.size());
    for (const auto field_index : field_indexes) {
        requests.push_back(FieldDraftRequest{
            .field_index = field_index,
            .kind = std::nullopt,
            .values = {},
        });
    }
    return applyFieldRequests(item_indexes, std::move(requests));
}

bool MetadataGridModel::stageTransformation(const metadata::MetadataTransformationPreview& preview,
                                            const QStringList& step_sources) {
    if (preview.cells.empty() || preview.cells.size() > maximum_field_transaction_cells) {
        emit editRejected(QStringLiteral("A transformation must change between 1 and %1 cells")
                              .arg(maximum_field_transaction_cells));
        return false;
    }

    using FieldAddress = std::pair<metadata::MetadataFieldMatchMode, std::string>;
    const auto address_for = [](const metadata::MetadataTransformationCellPreview& cell) {
        return FieldAddress{
            cell.match_mode,
            cell.match_mode == metadata::MetadataFieldMatchMode::exact_native
                ? metadata::canonicalize_native_field_name(cell.display_field)
                : cell.canonical_field,
        };
    };
    std::set<std::pair<std::size_t, FieldAddress>> addressed_fields;
    for (const auto& cell : preview.cells) {
        if (cell.item_index >= selection_->item_count() || cell.canonical_field.empty() ||
            metadata::canonicalize_field_name(cell.display_field) != cell.canonical_field ||
            (cell.match_mode != metadata::MetadataFieldMatchMode::logical &&
             cell.match_mode != metadata::MetadataFieldMatchMode::exact_native) ||
            !addressed_fields.emplace(cell.item_index, address_for(cell)).second) {
            emit editRejected(QStringLiteral("Transformation preview contains an invalid cell"));
            return false;
        }
    }

    auto extended_selection = std::make_shared<metadata::StagedMetadataSelection>(*selection_);
    std::map<FieldAddress, std::size_t> field_indexes;
    for (const auto& cell : preview.cells) {
        const auto address = address_for(cell);
        if (field_indexes.contains(address)) {
            continue;
        }
        auto field_index = cell.match_mode == metadata::MetadataFieldMatchMode::exact_native
                               ? extended_selection->exact_native_field_index(cell.display_field)
                               : extended_selection->field_index(cell.canonical_field);
        if (!field_index) {
            auto inserted = cell.match_mode == metadata::MetadataFieldMatchMode::exact_native
                                ? extended_selection->ensure_exact_native_field(
                                      cell.display_field, "Exact native: " + cell.display_field)
                                : extended_selection->ensure_missing_field(cell.canonical_field,
                                                                           cell.display_field);
            if (!inserted) {
                emit editRejected(display_utf8(inserted.error().message));
                return false;
            }
            field_index = *inserted;
        }
        field_indexes.emplace(address, *field_index);
    }

    for (const auto& cell : preview.cells) {
        const auto found = field_indexes.find(address_for(cell));
        if (found == field_indexes.end()) {
            emit editRejected(QStringLiteral("Transformation preview contains an invalid cell"));
            return false;
        }
        const auto current =
            project_cell(*extended_selection, patches_, cell.item_index, found->second);
        const auto current_values = current.present
                                        ? std::optional<std::vector<std::string>>{*current.values}
                                        : std::nullopt;
        if (current_values != cell.before) {
            emit editRejected(
                QStringLiteral("The metadata draft changed after transformation preview"));
            return false;
        }
    }

    auto checked_patches = patches_;
    for (const auto& cell : preview.cells) {
        const auto found = field_indexes.find(address_for(cell));
        if (found == field_indexes.end()) {
            emit editRejected(QStringLiteral("Transformation preview contains an invalid cell"));
            return false;
        }
        const auto field_index = found->second;
        const auto checked =
            cell.after
                ? checked_patches.replace_values(*extended_selection, cell.item_index, field_index,
                                                 *cell.after)
                : checked_patches.remove_field(*extended_selection, cell.item_index, field_index);
        if (!checked) {
            emit editRejected(display_utf8(checked.error().message));
            return false;
        }
    }
    const auto old_field_count = selection_->field_count();
    const auto new_field_count = extended_selection->field_count();
    if (new_field_count > old_field_count) {
        const auto first = static_cast<int>(old_field_count) + 1;
        const auto last = static_cast<int>(new_field_count);
        beginInsertColumns({}, first, last);
        selection_ = std::move(extended_selection);
        endInsertColumns();
    }

    std::vector<DraftRequest> requests;
    requests.reserve(preview.cells.size());
    for (const auto& cell : preview.cells) {
        const auto found = field_indexes.find(address_for(cell));
        if (found == field_indexes.end()) {
            emit editRejected(QStringLiteral("Transformation preview contains an invalid cell"));
            return false;
        }
        DraftRequest request{
            .item_index = cell.item_index,
            .field_index = found->second,
            .desired =
                metadata::StagedMetadataPatch{
                    .item_index = cell.item_index,
                    .field_index = found->second,
                    .kind = cell.after ? metadata::StagedMetadataPatchKind::replace_values
                                       : metadata::StagedMetadataPatchKind::remove_field,
                    .values = cell.after.value_or(std::vector<std::string>{}),
                },
        };
        requests.push_back(std::move(request));
    }
    if (!applyRequests(std::move(requests))) {
        return false;
    }
    if (!step_sources.isEmpty()) {
        for (const auto& cell : preview.cells) {
            const auto found = field_indexes.find(address_for(cell));
            if (found == field_indexes.end()) {
                continue;
            }
            const auto* patch = patches_.patch(cell.item_index, found->second);
            if (patch == nullptr) {
                continue;
            }
            const auto step = static_cast<qsizetype>(cell.last_action_index);
            const auto& source =
                step < step_sources.size() ? step_sources.at(step) : step_sources.constLast();
            staged_sources_[{cell.item_index, found->second}] = {*patch, source};
        }
    }
    return true;
}

QString MetadataGridModel::stagedSourceLabel(const std::size_t item_index,
                                             const std::size_t field_index) const {
    const auto found = staged_sources_.find({item_index, field_index});
    if (found == staged_sources_.end()) {
        return {};
    }
    const auto* patch = patches_.patch(item_index, field_index);
    if (patch == nullptr || *patch != found->second.first) {
        return {};
    }
    return found->second.second;
}

bool MetadataGridModel::applyFieldRequests(const std::span<const std::size_t> item_indexes,
                                           std::vector<FieldDraftRequest> requests) {
    std::set<std::size_t> unique_fields;
    for (const auto& request : requests) {
        if (request.field_index >= selection_->field_count()) {
            emit editRejected(QStringLiteral("A selected metadata field is out of range"));
            return false;
        }
        unique_fields.insert(request.field_index);
    }
    const auto item_count = item_indexes.size();
    if (!unique_fields.empty() &&
        item_count > maximum_field_transaction_cells / unique_fields.size()) {
        emit editRejected(QStringLiteral("A bulk field edit is limited to %1 addressed cells")
                              .arg(maximum_field_transaction_cells));
        return false;
    }

    std::vector<DraftRequest> cell_requests;
    cell_requests.reserve(item_count * unique_fields.size());
    for (const auto& field_request : requests) {
        for (const auto item_index : item_indexes) {
            DraftRequest request{
                .item_index = item_index,
                .field_index = field_request.field_index,
                .desired = std::nullopt,
            };
            if (field_request.kind) {
                request.desired = metadata::StagedMetadataPatch{
                    .item_index = item_index,
                    .field_index = field_request.field_index,
                    .kind = *field_request.kind,
                    .values = field_request.values,
                };
            }
            cell_requests.push_back(std::move(request));
        }
    }
    return applyRequests(std::move(cell_requests));
}

bool MetadataGridModel::applyRequests(std::vector<DraftRequest> requests) {
    if (requests.size() > maximum_field_transaction_cells) {
        emit editRejected(QStringLiteral("A bulk field edit is limited to %1 addressed cells")
                              .arg(maximum_field_transaction_cells));
        return false;
    }
    DraftTransaction transaction;
    transaction.mutations.reserve(requests.size());
    const auto rollback = [this, &transaction] {
        for (auto mutation = transaction.mutations.rbegin();
             mutation != transaction.mutations.rend(); ++mutation) {
            restorePatch(*mutation, false);
        }
    };
    for (const auto& request : requests) {
        if (request.item_index >= selection_->item_count() ||
            request.field_index >= selection_->field_count()) {
            rollback();
            emit editRejected(QStringLiteral("A selected metadata cell is out of range"));
            return false;
        }
        const auto* before = patches_.patch(request.item_index, request.field_index);
        const auto before_copy = before == nullptr ? std::nullopt : std::optional{*before};
        const auto applied = applyPatch(request);
        if (!applied) {
            rollback();
            emit editRejected(display_utf8(applied.error().message));
            return false;
        }
        if (!*applied) {
            continue;
        }
        const auto* after = patches_.patch(request.item_index, request.field_index);
        const auto after_copy = after == nullptr ? std::nullopt : std::optional{*after};
        DraftMutation mutation{
            .item_index = request.item_index,
            .field_index = request.field_index,
            .before = before_copy,
            .after = after_copy,
        };
        transaction.text_bytes += before_copy ? patch_text_bytes(*before_copy) : 0U;
        transaction.text_bytes += after_copy ? patch_text_bytes(*after_copy) : 0U;
        transaction.mutations.push_back(std::move(mutation));
        if (transaction.text_bytes > maximum_history_text_bytes) {
            rollback();
            emit editRejected(QStringLiteral("One undoable metadata edit is limited to 64 MiB"));
            return false;
        }
    }
    if (transaction.mutations.empty()) {
        return false;
    }
    pushHistory(std::move(transaction));
    notifyMutations(history_[history_cursor_ - 1U].mutations);
    return true;
}

core::Result<bool> MetadataGridModel::applyPatch(const DraftRequest& request) {
    if (!request.desired) {
        return patches_.revert(*selection_, request.item_index, request.field_index);
    }
    if (request.desired->kind == metadata::StagedMetadataPatchKind::remove_field) {
        return patches_.remove_field(*selection_, request.item_index, request.field_index);
    }
    return patches_.replace_values(*selection_, request.item_index, request.field_index,
                                   request.desired->values);
}

void MetadataGridModel::restorePatch(const DraftMutation& mutation, const bool use_after) {
    const auto& desired = use_after ? mutation.after : mutation.before;
    core::Result<bool> restored = false;
    if (!desired) {
        restored = patches_.revert(*selection_, mutation.item_index, mutation.field_index);
    } else if (desired->kind == metadata::StagedMetadataPatchKind::remove_field) {
        restored = patches_.remove_field(*selection_, mutation.item_index, mutation.field_index);
    } else {
        restored = patches_.replace_values(*selection_, mutation.item_index, mutation.field_index,
                                           desired->values);
    }
    Q_ASSERT(restored.has_value());
}

void MetadataGridModel::notifyMutations(const std::vector<DraftMutation>& mutations) {
    std::map<std::size_t, std::pair<std::size_t, std::size_t>> changed_fields;
    for (const auto& mutation : mutations) {
        const auto [found, inserted] = changed_fields.emplace(
            mutation.field_index, std::pair{mutation.item_index, mutation.item_index});
        if (!inserted) {
            found->second.first = std::min(found->second.first, mutation.item_index);
            found->second.second = std::max(found->second.second, mutation.item_index);
        }
    }
    for (const auto& [field_index, rows] : changed_fields) {
        const auto column = static_cast<int>(field_index) + 1;
        emit dataChanged(index(static_cast<int>(rows.first), column),
                         index(static_cast<int>(rows.second), column));
        emit headerDataChanged(Qt::Horizontal, column, column);
    }
    emit draftStateChanged(
        static_cast<int>(std::min(patches_.patch_count(),
                                  static_cast<std::size_t>(std::numeric_limits<int>::max()))),
        history_cursor_ > 0U, history_cursor_ < history_.size());
}

void MetadataGridModel::pushHistory(DraftTransaction transaction) {
    while (history_.size() > history_cursor_) {
        history_text_bytes_ -= history_.back().text_bytes;
        history_.pop_back();
    }
    history_text_bytes_ += transaction.text_bytes;
    history_.push_back(std::move(transaction));
    history_cursor_ = history_.size();
    while (history_.size() > 1U && (history_.size() > maximum_history_transactions ||
                                    history_text_bytes_ > maximum_history_text_bytes)) {
        history_text_bytes_ -= history_.front().text_bytes;
        history_.erase(history_.begin());
        --history_cursor_;
    }
}

MetadataAggregateModel::MetadataAggregateModel(MetadataGridModel* grid_model, QObject* parent)
    : QAbstractTableModel(parent), grid_model_(grid_model) {
    Q_ASSERT(grid_model_ != nullptr);
    const auto& selection = grid_model_->selection();
    item_indexes_.reserve(selection.item_count());
    subset_fields_.reserve(selection.field_count());
    draft_fields_.resize(selection.field_count());
    staged_counts_.assign(selection.field_count(), 0U);
    for (std::size_t item_index = 0U; item_index < selection.item_count(); ++item_index) {
        item_indexes_.push_back(item_index);
    }
    for (std::size_t field_index = 0U; field_index < selection.field_count(); ++field_index) {
        const auto& field = selection.field(field_index);
        subset_fields_.push_back(metadata::StagedMetadataSubsetField{
            .state = field.state,
            .present_item_count = field.present_item_count,
            .representative_item_index = field.representative_item_index,
        });
    }
    draft_debounce_ = new QTimer(this);
    draft_debounce_->setSingleShot(true);
    draft_debounce_->setInterval(40);
    connect(draft_debounce_, &QTimer::timeout, this, &MetadataAggregateModel::startDraftProjection);
    connect(grid_model_, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& top_left, const QModelIndex& bottom_right,
                   const QList<int>&) { gridDataChanged(top_left, bottom_right); });
    connect(grid_model_, &QAbstractItemModel::modelReset, this, [this] {
        beginResetModel();
        uniform_drafts_.clear();
        endResetModel();
    });
    connect(grid_model_, &QAbstractItemModel::columnsAboutToBeInserted, this,
            [this](const QModelIndex& insertion_parent, const int first, const int last) {
                if (insertion_parent.isValid() || first <= 0 || last < first) {
                    return;
                }
                Q_ASSERT(!field_insert_pending_);
                field_insert_pending_ = true;
                beginInsertRows({}, first - 1, last - 1);
            });
    connect(grid_model_, &QAbstractItemModel::columnsInserted, this,
            [this](const QModelIndex& insertion_parent, const int first, const int last) {
                if (insertion_parent.isValid() || first <= 0 || last < first ||
                    !field_insert_pending_) {
                    return;
                }
                for (auto column = first; column <= last; ++column) {
                    static_cast<void>(column);
                    subset_fields_.push_back(metadata::StagedMetadataSubsetField{});
                    draft_fields_.push_back(metadata::StagedMetadataFieldProjection{});
                    staged_counts_.push_back(0U);
                }
                field_insert_pending_ = false;
                endInsertRows();
                invalidateDraftProjection();
            });
}

int MetadataAggregateModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || grid_model_ == nullptr) {
        return 0;
    }
    return static_cast<int>(std::min(grid_model_->selection().field_count(),
                                     static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

int MetadataAggregateModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 3;
}

QVariant MetadataAggregateModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || grid_model_ == nullptr || index.row() < 0 ||
        index.row() >= rowCount() || index.column() < 0 || index.column() >= columnCount()) {
        return {};
    }
    const auto field_index = static_cast<std::size_t>(index.row());
    const auto& field = grid_model_->selection().field(field_index);
    if (index.column() == 0) {
        const auto staged =
            summary_ready_ && draft_counts_ready_ && staged_counts_[field_index] > 0U;
        if (role == Qt::FontRole && staged) {
            auto font = QApplication::font();
            font.setBold(true);
            return font;
        }
        if (role == Qt::AccessibleDescriptionRole) {
            return staged ? QStringLiteral("Field has staged edits in the selected files")
                          : QString{};
        }

        if (role == Qt::DisplayRole) {
            return display_utf8(field.display_name);
        }
        if (role == Qt::ToolTipRole) {
            return QStringLiteral("Canonical field: %1").arg(display_utf8(field.canonical_name));
        }
        if (role == metadata_field_state_role) {
            return static_cast<int>(summary_ready_ ? subset_fields_[field_index].state
                                                   : field.state);
        }
        if (role == metadata_field_canonical_name_role) {
            return display_utf8(field.canonical_name);
        }
        return {};
    }
    if (index.column() == 1) {
        return originalData(field_index, role);
    }
    return draftData(field_index, role);
}

QVariant MetadataAggregateModel::headerData(const int section, const Qt::Orientation orientation,
                                            const int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Vertical) {
        return section + 1;
    }
    switch (section) {
    case 0:
        return QStringLiteral("Field");
    case 1:
        return QStringLiteral("Original");
    case 2:
        return QStringLiteral("Draft");
    default:
        return {};
    }
}

Qt::ItemFlags MetadataAggregateModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == 2 && summary_ready_ && !item_indexes_.empty()) {
        flags |= Qt::ItemIsEditable;
    }
    return flags;
}

bool MetadataAggregateModel::setData(const QModelIndex& index, const QVariant& value,
                                     const int role) {
    if (!index.isValid() || grid_model_ == nullptr || index.row() < 0 ||
        index.row() >= rowCount() || index.column() != 2 || role != Qt::EditRole ||
        !summary_ready_ || item_indexes_.empty()) {
        return false;
    }
    const auto encoded = value.toString().toUtf8();
    if (static_cast<std::size_t>(encoded.size()) > maximum_inline_edit_bytes) {
        emit editRejected(QStringLiteral("A directly edited value is limited to 4 MiB"));
        return false;
    }
    const auto field_index = static_cast<std::size_t>(index.row());
    applying_uniform_edit_ = true;
    const auto changed =
        encoded.isEmpty()
            ? grid_model_->removeFields(item_indexes_, {field_index})
            : grid_model_->replaceFieldValues(
                  item_indexes_, field_index,
                  {std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
    applying_uniform_edit_ = false;
    if (!changed) {
        return false;
    }
    refreshDraftCount(field_index);
    if (staged_counts_[field_index] == 0U) {
        uniform_drafts_.erase(field_index);
    } else if (encoded.isEmpty()) {
        uniform_drafts_[field_index] = UniformDraft{
            .kind = metadata::StagedMetadataPatchKind::remove_field,
            .values = {},
        };
    } else {
        uniform_drafts_[field_index] = UniformDraft{
            .kind = metadata::StagedMetadataPatchKind::replace_values,
            .values = {std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}},
        };
    }
    notifyField(field_index);
    return true;
}

std::optional<int> MetadataAggregateModel::fieldRow(const QString& name) const {
    if (grid_model_ == nullptr) {
        return std::nullopt;
    }
    const auto encoded = name.toUtf8();
    const std::string_view bytes{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto native_first = !metadata::resolve_text_property_identity(bytes).conventional;
    auto field_index = native_first ? grid_model_->selection().exact_native_field_index(bytes)
                                    : grid_model_->selection().field_index(bytes);
    if (!field_index) {
        field_index = native_first ? grid_model_->selection().field_index(bytes)
                                   : grid_model_->selection().exact_native_field_index(bytes);
    }
    if (!field_index || *field_index >= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(*field_index);
}

core::Result<int> MetadataAggregateModel::ensureField(const QString& name) {
    if (grid_model_ == nullptr || !summary_ready_) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "metadata fields cannot be added while the selection is updating",
            .context = {},
        });
    }
    const auto insertion = grid_model_->ensureField(name);
    if (!insertion) {
        return std::unexpected(insertion.error());
    }
    return static_cast<int>(insertion->field_index);
}

bool MetadataAggregateModel::removeIndexes(const QModelIndexList& indexes) {
    if (grid_model_ == nullptr || !summary_ready_ || item_indexes_.empty()) {
        return false;
    }
    const auto fields = selectedFields(indexes);
    applying_uniform_edit_ = true;
    const auto changed = grid_model_->removeFields(item_indexes_, fields);
    applying_uniform_edit_ = false;
    if (!changed) {
        return false;
    }
    for (const auto field_index : fields) {
        refreshDraftCount(field_index);
        if (staged_counts_[field_index] == 0U) {
            uniform_drafts_.erase(field_index);
        } else {
            uniform_drafts_[field_index] = UniformDraft{
                .kind = metadata::StagedMetadataPatchKind::remove_field,
                .values = {},
            };
        }
        notifyField(field_index);
    }
    return true;
}

bool MetadataAggregateModel::revertIndexes(const QModelIndexList& indexes) {
    if (grid_model_ == nullptr || !summary_ready_ || item_indexes_.empty()) {
        return false;
    }
    const auto fields = selectedFields(indexes);
    applying_uniform_edit_ = true;
    const auto changed = grid_model_->revertFields(item_indexes_, fields);
    applying_uniform_edit_ = false;
    if (!changed) {
        return false;
    }
    for (const auto field_index : fields) {
        refreshDraftCount(field_index);
        uniform_drafts_.erase(field_index);
        notifyField(field_index);
    }
    return true;
}

bool MetadataAggregateModel::replaceRowValues(const int row, std::vector<std::string> values) {
    if (grid_model_ == nullptr || row < 0 || row >= rowCount() || !summary_ready_ ||
        item_indexes_.empty()) {
        emit editRejected(QStringLiteral("A selected metadata field is out of range"));
        return false;
    }
    const auto field_index = static_cast<std::size_t>(row);
    applying_uniform_edit_ = true;
    const auto changed = grid_model_->replaceFieldValues(item_indexes_, field_index, values);
    applying_uniform_edit_ = false;
    if (!changed) {
        return false;
    }
    refreshDraftCount(field_index);
    uniform_drafts_[field_index] = UniformDraft{
        .kind = metadata::StagedMetadataPatchKind::replace_values,
        .values = std::move(values),
    };
    notifyField(field_index);
    return true;
}

void MetadataAggregateModel::setSelectedItems(std::vector<std::size_t> item_indexes) {
    std::sort(item_indexes.begin(), item_indexes.end());
    item_indexes.erase(std::unique(item_indexes.begin(), item_indexes.end()), item_indexes.end());
    if (item_indexes == item_indexes_ && summary_ready_) {
        return;
    }
    item_indexes_ = std::move(item_indexes);
    uniform_drafts_.clear();
    if (draft_debounce_ != nullptr) {
        draft_debounce_->stop();
    }
    ++draft_generation_;
    draft_ready_ = false;
    draft_counts_ready_ = false;
    draft_matches_original_ = false;
    const auto generation = ++selection_generation_;
    summary_ready_ = false;
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 2));
    }
    emit selectionProjectionChanged(false, static_cast<int>(item_indexes_.size()));
    emit draftProjectionChanged(false);

    const auto selection = grid_model_->sharedSelection();
    auto complete_selection = item_indexes_.size() == selection->item_count();
    for (std::size_t index = 0U; complete_selection && index < item_indexes_.size(); ++index) {
        complete_selection = item_indexes_[index] == index;
    }
    if (complete_selection) {
        std::vector<metadata::StagedMetadataSubsetField> summaries;
        summaries.reserve(selection->field_count());
        for (std::size_t field_index = 0U; field_index < selection->field_count(); ++field_index) {
            const auto& field = selection->field(field_index);
            summaries.push_back(metadata::StagedMetadataSubsetField{
                .state = field.state,
                .present_item_count = field.present_item_count,
                .representative_item_index = field.representative_item_index,
            });
        }
        applySelectionSummary(generation, std::move(summaries));
        return;
    }
    if (item_indexes_.size() <= 1U) {
        applySelectionSummary(generation, selection->summarize_items(item_indexes_));
        return;
    }
    using SummaryResult = core::Result<std::vector<metadata::StagedMetadataSubsetField>>;
    auto* watcher = new QFutureWatcher<SummaryResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        auto result = watcher->result();
        watcher->deleteLater();
        applySelectionSummary(generation, std::move(result));
    });
    watcher->setFuture(QtConcurrent::run(
        [selection, items = item_indexes_]() { return selection->summarize_items(items); }));
}

QVariant MetadataAggregateModel::originalData(const std::size_t field_index, const int role) const {
    const auto& selection = grid_model_->selection();
    const auto& field = selection.field(field_index);
    if (!summary_ready_) {
        if (role == Qt::DisplayRole) {
            return QStringLiteral("(preparing selection…)");
        }
        if (role == Qt::ForegroundRole) {
            return QApplication::palette().brush(QPalette::PlaceholderText);
        }
        return {};
    }
    const auto& subset = subset_fields_[field_index];
    const metadata::StagedMetadataCell* representative = nullptr;
    if (subset.representative_item_index) {
        representative = selection.cell(*subset.representative_item_index, field_index);
    }
    if (role == metadata_field_state_role) {
        return static_cast<int>(subset.state);
    }
    if (role == metadata_field_canonical_name_role) {
        return display_utf8(field.canonical_name);
    }
    if (role == metadata_cell_values_role || role == metadata_cell_baseline_values_role) {
        return subset.state == metadata::MetadataSelectionFieldState::common &&
                       representative != nullptr
                   ? QVariant{display_values(representative->values)}
                   : QVariant{QStringList{}};
    }
    if (role == Qt::DisplayRole) {
        switch (subset.state) {
        case metadata::MetadataSelectionFieldState::common: {
            const auto values =
                representative == nullptr ? QStringList{} : visible_values(representative->values);
            return values.empty() ? QStringLiteral("(empty field)")
                                  : values.join(QStringLiteral("  ·  "));
        }
        case metadata::MetadataSelectionFieldState::mixed:
            return QStringLiteral("(various across %1 files)").arg(item_indexes_.size());
        case metadata::MetadataSelectionFieldState::partial:
            return QStringLiteral("(various · present on %1 of %2 files)")
                .arg(subset.present_item_count)
                .arg(item_indexes_.size());
        case metadata::MetadataSelectionFieldState::missing:
            return QStringLiteral("—");
        }
    }
    if (role == Qt::ToolTipRole) {
        return QStringLiteral("%1 on %2 of %3 selected files")
            .arg(state_label(subset.state))
            .arg(subset.present_item_count)
            .arg(item_indexes_.size());
    }
    if (role == Qt::ForegroundRole &&
        subset.state != metadata::MetadataSelectionFieldState::common) {
        return QApplication::palette().brush(QPalette::PlaceholderText);
    }
    return {};
}

QVariant MetadataAggregateModel::draftData(const std::size_t field_index, const int role) const {
    const auto& field = grid_model_->selection().field(field_index);
    if (role == metadata_field_canonical_name_role) {
        return display_utf8(field.canonical_name);
    }
    if (!summary_ready_) {
        if (role == Qt::DisplayRole) {
            return QStringLiteral("(preparing selection…)");
        }
        if (role == Qt::ForegroundRole) {
            return QApplication::palette().brush(QPalette::PlaceholderText);
        }
        return {};
    }

    const auto staged_count = staged_counts_[field_index];
    const auto uniform = uniform_drafts_.find(field_index);
    if (role == metadata_cell_staged_role) {
        return staged_count > 0U;
    }
    if (role == metadata_cell_patch_kind_role) {
        return uniform == uniform_drafts_.end() ? -1 : static_cast<int>(uniform->second.kind);
    }
    if (role == metadata_cell_baseline_values_role) {
        return originalData(field_index, metadata_cell_values_role);
    }
    if ((role == Qt::ForegroundRole || role == Qt::FontRole) && staged_count > 0U) {
        const auto kind =
            uniform != uniform_drafts_.end() &&
                    uniform->second.kind == metadata::StagedMetadataPatchKind::remove_field
                ? DraftChangeKind::removed
                : (subset_fields_[field_index].present_item_count == 0U ? DraftChangeKind::added
                                                                        : DraftChangeKind::changed);
        return role == Qt::ForegroundRole ? QVariant{draft_change_brush(kind)}
                                          : QVariant{draft_change_font(kind)};
    }

    if (!draft_ready_ && (!draft_counts_ready_ || staged_count > 0U)) {
        if (uniform != uniform_drafts_.end()) {
            const auto removing =
                uniform->second.kind == metadata::StagedMetadataPatchKind::remove_field;
            if (role == metadata_field_state_role) {
                return static_cast<int>(removing ? metadata::MetadataSelectionFieldState::missing
                                                 : metadata::MetadataSelectionFieldState::common);
            }
            if (role == metadata_cell_values_role) {
                return removing ? QVariant{QStringList{}}
                                : QVariant{display_values(uniform->second.values)};
            }
            if (role == Qt::EditRole) {
                const auto values =
                    removing ? QStringList{} : display_values(uniform->second.values);
                return values.size() == 1 ? QVariant{values.front()} : QVariant{QString{}};
            }
            if (role == Qt::DisplayRole) {
                if (removing) {
                    return QStringLiteral("(remove)");
                }
                const auto values = visible_values(uniform->second.values);
                return values.empty() ? QStringLiteral("(empty field)")
                                      : values.join(QStringLiteral("  ·  "));
            }
        } else {
            if (role == metadata_field_state_role) {
                return static_cast<int>(subset_fields_[field_index].state);
            }
            if (role == metadata_cell_values_role || role == Qt::EditRole) {
                return role == metadata_cell_values_role ? QVariant{QStringList{}}
                                                         : QVariant{QString{}};
            }
            if (role == Qt::DisplayRole) {
                return QStringLiteral("(preparing draft preview…)");
            }
        }
        if (role == Qt::ToolTipRole) {
            return QStringLiteral("Computing the complete result for %1 selected files")
                .arg(item_indexes_.size());
        }
        if (role == Qt::ForegroundRole) {
            return QApplication::palette().brush(QPalette::PlaceholderText);
        }
        return {};
    }

    if (draft_matches_original_ || (draft_counts_ready_ && staged_count == 0U && !draft_ready_)) {
        if (role == Qt::ToolTipRole) {
            return QStringLiteral("No staged change; Draft currently matches Original");
        }
        return originalData(field_index, role);
    }

    Q_ASSERT(draft_ready_);
    Q_ASSERT(field_index < draft_fields_.size());
    const auto& projection = draft_fields_[field_index];
    if (role == metadata_field_state_role) {
        return static_cast<int>(projection.state);
    }
    if (role == metadata_cell_values_role) {
        return projection.state == metadata::MetadataSelectionFieldState::common
                   ? QVariant{display_values(projection.common_values)}
                   : QVariant{QStringList{}};
    }
    if (role == Qt::EditRole) {
        const auto values = projection.state == metadata::MetadataSelectionFieldState::common
                                ? display_values(projection.common_values)
                                : QStringList{};
        return values.size() == 1 ? QVariant{values.front()} : QVariant{QString{}};
    }
    if (role == Qt::DisplayRole) {
        switch (projection.state) {
        case metadata::MetadataSelectionFieldState::common: {
            const auto values = visible_values(projection.common_values);
            return values.empty() ? QStringLiteral("(empty field)")
                                  : values.join(QStringLiteral("  ·  "));
        }
        case metadata::MetadataSelectionFieldState::mixed:
            return QStringLiteral("(various across %1 files)").arg(item_indexes_.size());
        case metadata::MetadataSelectionFieldState::partial:
            return QStringLiteral("(various · present on %1 of %2 files)")
                .arg(projection.present_item_count)
                .arg(item_indexes_.size());
        case metadata::MetadataSelectionFieldState::missing:
            return staged_count > 0U ? QStringLiteral("(remove)") : QStringLiteral("—");
        }
    }
    if (role == Qt::ToolTipRole) {
        return QStringLiteral("Complete draft: %1 on %2 of %3 selected files · %4 staged %5 · "
                              "not written to file")
            .arg(state_label(projection.state))
            .arg(projection.present_item_count)
            .arg(item_indexes_.size())
            .arg(staged_count)
            .arg(staged_count == 1U ? QStringLiteral("cell") : QStringLiteral("cells"));
    }
    if (role == Qt::ForegroundRole &&
        projection.state != metadata::MetadataSelectionFieldState::common) {
        return QApplication::palette().brush(QPalette::PlaceholderText);
    }
    return {};
}

void MetadataAggregateModel::applySelectionSummary(
    const std::size_t generation,
    core::Result<std::vector<metadata::StagedMetadataSubsetField>> result) {
    if (generation != selection_generation_) {
        return;
    }
    if (!result) {
        emit editRejected(display_utf8(result.error().message));
        return;
    }
    subset_fields_ = std::move(*result);
    summary_ready_ = true;
    refreshDraftCounts();
    invalidateDraftProjection();
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 2));
    }
    emit selectionProjectionChanged(true, static_cast<int>(item_indexes_.size()));
}

void MetadataAggregateModel::refreshDraftCounts() {
    staged_counts_.assign(grid_model_->selection().field_count(), 0U);
    uniform_drafts_.clear();
    draft_counts_ready_ = grid_model_->patches().empty();
}

void MetadataAggregateModel::refreshDraftCount(const std::size_t field_index) {
    if (field_index >= staged_counts_.size()) {
        return;
    }
    auto count = std::size_t{0U};
    std::optional<UniformDraft> uniform;
    auto uniform_patch = true;
    for (const auto item_index : item_indexes_) {
        const auto* patch = grid_model_->patches().patch(item_index, field_index);
        if (patch == nullptr) {
            uniform_patch = false;
            continue;
        }
        ++count;
        const auto candidate = UniformDraft{.kind = patch->kind, .values = patch->values};
        if (!uniform) {
            uniform = candidate;
        } else if (uniform->kind != candidate.kind || uniform->values != candidate.values) {
            uniform_patch = false;
        }
    }
    staged_counts_[field_index] = count;
    if (uniform_patch && uniform && count == item_indexes_.size()) {
        uniform_drafts_[field_index] = std::move(*uniform);
    } else {
        uniform_drafts_.erase(field_index);
    }
}

void MetadataAggregateModel::invalidateDraftProjection() {
    ++draft_generation_;
    draft_cancellation_.request_cancellation();
    draft_cancellation_ = core::CancellationSource{};
    if (draft_debounce_ != nullptr) {
        draft_debounce_->stop();
    }
    draft_ready_ = false;
    draft_matches_original_ = false;
    emit draftProjectionChanged(false);
    if (!summary_ready_) {
        return;
    }
    const auto has_any_patches = !grid_model_->patches().empty();
    if (!has_any_patches) {
        draft_ready_ = true;
        draft_counts_ready_ = true;
        draft_matches_original_ = true;
        if (rowCount() > 0) {
            emit dataChanged(index(0, 0), index(rowCount() - 1, 2));
        }
        emit draftProjectionChanged(true);
        return;
    }
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 2));
    }
    draft_debounce_->start();
}

void MetadataAggregateModel::startDraftProjection() {
    if (!summary_ready_ || grid_model_ == nullptr || draft_job_running_) {
        return;
    }
    using ProjectionResult = core::Result<std::vector<metadata::StagedMetadataFieldProjection>>;
    const auto generation = draft_generation_;
    const auto selection = grid_model_->sharedSelection();
    auto draft = grid_model_->patches();
    auto items = item_indexes_;
    const auto cancellation = draft_cancellation_.token();
    draft_job_running_ = true;
    auto* watcher = new QFutureWatcher<ProjectionResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        auto result = watcher->result();
        watcher->deleteLater();
        draft_job_running_ = false;
        const auto obsolete = generation != draft_generation_;
        applyDraftProjection(generation, std::move(result));
        if (obsolete && !draft_ready_ && summary_ready_ && !grid_model_->patches().empty() &&
            !draft_debounce_->isActive()) {
            draft_debounce_->start();
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [selection, draft = std::move(draft), items = std::move(items), cancellation] {
            return draft.project_items(*selection, items, cancellation);
        }));
}

void MetadataAggregateModel::applyDraftProjection(
    const std::size_t generation,
    core::Result<std::vector<metadata::StagedMetadataFieldProjection>> result) {
    if (generation != draft_generation_ || !summary_ready_) {
        return;
    }
    if (!result) {
        emit editRejected(display_utf8(result.error().message));
        return;
    }
    if (result->size() != grid_model_->selection().field_count()) {
        emit editRejected(
            QStringLiteral("The metadata draft preview returned an incomplete field set"));
        return;
    }
    draft_fields_ = std::move(*result);
    staged_counts_.resize(draft_fields_.size());
    for (std::size_t field_index = 0U; field_index < draft_fields_.size(); ++field_index) {
        staged_counts_[field_index] = draft_fields_[field_index].staged_item_count;
    }
    draft_ready_ = true;
    draft_counts_ready_ = true;
    draft_matches_original_ =
        std::ranges::none_of(staged_counts_, [](const std::size_t count) { return count > 0U; });
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 2));
    }
    emit draftProjectionChanged(true);
}

std::vector<std::size_t>
MetadataAggregateModel::selectedFields(const QModelIndexList& indexes) const {
    std::set<std::size_t> fields;
    for (const auto& index : indexes) {
        if (index.isValid() && index.row() >= 0 && index.row() < rowCount()) {
            fields.insert(static_cast<std::size_t>(index.row()));
        }
    }
    return {fields.begin(), fields.end()};
}

void MetadataAggregateModel::gridDataChanged(const QModelIndex& top_left,
                                             const QModelIndex& bottom_right) {
    if (top_left.column() <= 0 && bottom_right.column() <= 0) {
        return;
    }
    const auto first_column = std::max(1, top_left.column());
    const auto last_column = std::min(grid_model_->columnCount() - 1, bottom_right.column());
    if (first_column > last_column) {
        return;
    }
    const auto first_field = static_cast<std::size_t>(first_column - 1);
    const auto last_field = static_cast<std::size_t>(last_column - 1);
    if (grid_model_->patches().empty()) {
        std::fill(staged_counts_.begin(), staged_counts_.end(), 0U);
        uniform_drafts_.clear();
        invalidateDraftProjection();
        emit dataChanged(index(static_cast<int>(first_field), 0),
                         index(static_cast<int>(last_field), 2));
        return;
    }
    if (!applying_uniform_edit_) {
        for (auto field_index = first_field; field_index <= last_field; ++field_index) {
            uniform_drafts_.erase(field_index);
        }
    }
    for (auto field_index = first_field; field_index <= last_field; ++field_index) {
        refreshDraftCount(field_index);
    }
    invalidateDraftProjection();
    emit dataChanged(index(static_cast<int>(first_field), 0),
                     index(static_cast<int>(last_field), 2));
}

void MetadataAggregateModel::notifyField(const std::size_t field_index) {
    const auto row = static_cast<int>(field_index);
    emit dataChanged(index(row, 0), index(row, 2));
}

} // namespace trackknife::bench
