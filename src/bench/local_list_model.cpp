// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_list_model.hpp"

#include "trackknife/core/local_sources.hpp"
#include "uicommon/rating_stars.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QBrush>
#include <QImage>
#include <QSet>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <string_view>
#include <utility>

namespace trackknife::bench {

namespace {

[[nodiscard]] std::string file_name_of(const std::string& raw_path) {
    const auto slash = raw_path.find_last_of('/');
    if (slash == std::string::npos || slash + 1U >= raw_path.size()) {
        return raw_path;
    }
    return raw_path.substr(slash + 1U);
}

[[nodiscard]] QString escaped(const std::string& raw) {
    return QString::fromStdString(core::escape_raw_path(raw));
}

[[nodiscard]] QString display_utf8(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] QString format_duration(const std::int64_t milliseconds) {
    const auto total_seconds = std::max<std::int64_t>(milliseconds, 0) / 1'000;
    return QStringLiteral("%1:%2")
        .arg(total_seconds / 60)
        .arg(total_seconds % 60, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] bool same_album(const LocalTrackRow& left, const LocalTrackRow& right) {
    const auto& left_artist = left.album_artist.empty() ? left.artist : left.album_artist;
    const auto& right_artist = right.album_artist.empty() ? right.artist : right.album_artist;
    return left_artist == right_artist && left.album == right.album && left.date == right.date;
}

[[nodiscard]] std::string
metadata_value(const metadata::MetadataDocument& document,
               const std::initializer_list<std::string_view> candidate_names) {
    for (const auto name : candidate_names) {
        if (auto value = document.first_effective_value(name)) {
            return std::move(*value);
        }
    }
    return {};
}

void project_display_metadata(LocalTrackRow& row) {
    row.title = metadata_value(row.metadata, {"title"});
    row.artist = metadata_value(row.metadata, {"artist"});
    row.album = metadata_value(row.metadata, {"album"});
    row.album_artist = metadata_value(row.metadata, {"albumartist"});
    row.date = metadata_value(row.metadata, {"date", "year"});
    row.track_number = metadata_value(row.metadata, {"tracknumber", "track"});
}

[[nodiscard]] bool replaceable_source_field(const metadata::MetadataField& field) {
    return field.provenance == metadata::FieldProvenance::cached_snapshot ||
           field.provenance == metadata::FieldProvenance::embedded ||
           field.provenance == metadata::FieldProvenance::stream;
}

[[nodiscard]] std::vector<int> normalized_rows(std::vector<int> rows, const int row_count) {
    std::ranges::sort(rows);
    rows.erase(std::ranges::unique(rows).begin(), rows.end());
    std::erase_if(rows, [row_count](const int row) { return row < 0 || row >= row_count; });
    return rows;
}

} // namespace

LocalListModel::LocalListModel(QObject* parent) : QAbstractTableModel(parent) {}

void LocalListModel::replaceRows(std::vector<LocalTrackRow> rows, const bool remember,
                                 QString label) {
    Edit edit;
    if (remember) {
        edit.kind = Edit::Kind::replacement;
        edit.label = label.isEmpty() ? tr("Replace list contents") : std::move(label);
    } else {
        clearHistory();
    }
    beginResetModel();
    if (remember) {
        edit.detached = std::move(rows_);
    }
    rows_ = std::move(rows);
    endResetModel();
    refreshCurrentRow();
    if (remember) {
        rememberEdit(std::move(edit));
    }
}

void LocalListModel::appendPaths(std::vector<std::string> raw_paths, const int insertion_row) {
    std::vector<LocalTrackRow> rows;
    rows.reserve(raw_paths.size());
    for (auto& raw : raw_paths) {
        LocalTrackRow row;
        row.raw_path = std::move(raw);
        rows.push_back(std::move(row));
    }
    appendRows(std::move(rows), insertion_row);
}

void LocalListModel::applyTechnicals(const std::string& raw_path,
                                     const LocalTrackTechnicals& technicals) {
    for (auto& row : rows_) {
        if (row.raw_path == raw_path) {
            row.technicals = technicals;
        }
    }
}

void LocalListModel::applyRatings(const QHash<QString, unsigned>& ratings) {
    bool changed = false;
    for (auto& row : rows_) {
        const auto track_hash = QString::fromStdString(row.rating_hash);
        if (!track_hash.isEmpty() && ratings.contains(track_hash)) {
            const auto rating = ratings.value(track_hash);
            if (row.rating != rating) {
                row.rating = rating;
                changed = true;
            }
        }
        const auto album_hash = QString::fromStdString(row.album_rating_hash);
        if (!album_hash.isEmpty() && ratings.contains(album_hash)) {
            const auto rating = ratings.value(album_hash);
            if (row.album_rating != rating) {
                row.album_rating = rating;
                changed = true;
            }
        }
    }
    if (changed && !rows_.empty()) {
        // Album ratings paint over the artwork gutter, so both ends of the
        // shared column contract refresh.
        emit dataChanged(index(0, local_artwork_column),
                         index(static_cast<int>(rows_.size()) - 1, local_artwork_column),
                         {ui::track_album_rating_role});
        emit dataChanged(index(0, local_rating_column),
                         index(static_cast<int>(rows_.size()) - 1, local_rating_column),
                         {Qt::DisplayRole, ui::track_rating_role});
    }
}

QStringList LocalListModel::ratingHashes() const {
    QStringList hashes;
    QSet<QString> unique;
    const auto append = [&](const std::string& value) {
        if (value.empty()) {
            return;
        }
        const auto hash = QString::fromStdString(value);
        if (!unique.contains(hash)) {
            unique.insert(hash);
            hashes.push_back(hash);
        }
    };
    for (const auto& row : rows_) {
        append(row.rating_hash);
        append(row.album_rating_hash);
    }
    return hashes;
}

void LocalListModel::appendRows(std::vector<LocalTrackRow> rows, const int insertion_row,
                                const bool remember) {
    if (rows.empty()) {
        return;
    }
    const auto row_count = static_cast<int>(rows_.size());
    const auto target = insertion_row < 0 || insertion_row > row_count ? row_count : insertion_row;
    const auto count = static_cast<int>(rows.size());
    if (!remember) {
        clearHistory();
    }
    beginInsertRows({}, target, target + static_cast<int>(rows.size()) - 1);
    rows_.insert(rows_.begin() + target, std::make_move_iterator(rows.begin()),
                 std::make_move_iterator(rows.end()));
    endInsertRows();
    refreshCurrentRow();
    if (remember) {
        Edit edit;
        edit.kind = Edit::Kind::addition;
        edit.label = tr("Add tracks");
        edit.positions.resize(static_cast<std::size_t>(count));
        std::iota(edit.positions.begin(), edit.positions.end(), target);
        rememberEdit(std::move(edit));
    }
}

void LocalListModel::removeRowIndexes(std::vector<int> rows, const bool remember, QString label) {
    rows = normalized_rows(std::move(rows), static_cast<int>(rows_.size()));
    if (rows.empty())
        return;
    if (!remember)
        clearHistory();
    Edit edit;
    edit.kind = Edit::Kind::removal;
    edit.label = std::move(label);
    edit.positions = rows;
    removePositions(rows, remember ? &edit.detached : nullptr);
    refreshCurrentRow();
    if (remember)
        rememberEdit(std::move(edit));
}

void LocalListModel::reorderRows(std::vector<int> rows, const int insertion_row) {
    rows = normalized_rows(std::move(rows), static_cast<int>(rows_.size()));
    if (rows.empty()) {
        return;
    }
    std::vector<int> order;
    order.reserve(rows_.size());
    auto target_position =
        insertion_row < 0 ? rowCount() : std::clamp(insertion_row, 0, rowCount());
    for (int row = 0; row < rowCount(); ++row) {
        if (!std::binary_search(rows.begin(), rows.end(), row))
            order.push_back(row);
    }
    const auto preceding =
        std::lower_bound(rows.begin(), rows.end(), target_position) - rows.begin();
    target_position -= static_cast<int>(preceding);
    order.insert(order.begin() + target_position, rows.begin(), rows.end());
    bool changed = false;
    Edit edit;
    edit.order.resize(order.size());
    for (std::size_t position = 0; position < order.size(); ++position) {
        edit.order[static_cast<std::size_t>(order[position])] = static_cast<int>(position);
        changed = changed || order[position] != static_cast<int>(position);
    }
    if (!changed)
        return;
    const auto contiguous =
        std::adjacent_find(rows.begin(), rows.end(), [](const int left, const int right) {
            return right != left + 1;
        }) == rows.end();
    if (contiguous) {
        const auto first = rows.front();
        const auto last = rows.back();
        const auto count = last - first + 1;
        auto target = insertion_row < 0
                          ? static_cast<int>(rows_.size())
                          : std::clamp(insertion_row, 0, static_cast<int>(rows_.size()));
        if (target >= first && target <= last + 1) {
            return;
        }
        if (!beginMoveRows({}, first, last, {}, target)) {
            return;
        }
        std::vector<LocalTrackRow> moved;
        moved.reserve(static_cast<std::size_t>(count));
        auto first_iterator = rows_.begin() + first;
        auto last_iterator = first_iterator + count;
        std::move(first_iterator, last_iterator, std::back_inserter(moved));
        rows_.erase(first_iterator, last_iterator);
        if (target > last) {
            target -= count;
        }
        rows_.insert(rows_.begin() + target, std::make_move_iterator(moved.begin()),
                     std::make_move_iterator(moved.end()));
        if (current_row_ >= 0)
            current_row_ = edit.order[static_cast<std::size_t>(current_row_)];
        endMoveRows();
        refreshCurrentRow();
        rememberEdit(std::move(edit));
        return;
    }
    applyOrder(order);
    rememberEdit(std::move(edit));
}

bool LocalListModel::applyPermutation(const std::vector<int>& order, QString label) {
    if (order.size() != rows_.size())
        return false;
    Edit edit;
    edit.label = std::move(label);
    edit.order.assign(order.size(), -1);
    bool changed = false;
    for (std::size_t row = 0; row < order.size(); ++row) {
        const auto source = order[row];
        if (source < 0 || source >= rowCount() ||
            edit.order[static_cast<std::size_t>(source)] != -1)
            return false;
        edit.order[static_cast<std::size_t>(source)] = static_cast<int>(row);
        changed = changed || source != static_cast<int>(row);
    }
    if (!changed)
        return false;
    applyOrder(order);
    rememberEdit(std::move(edit));
    return true;
}

void LocalListModel::applyOrder(const std::vector<int>& order) {
    emit layoutAboutToBeChanged();
    const auto previous = persistentIndexList();
    std::vector<int> destinations(order.size());
    std::vector<LocalTrackRow> reordered;
    reordered.reserve(rows_.size());
    for (std::size_t row = 0; row < order.size(); ++row) {
        destinations[static_cast<std::size_t>(order[row])] = static_cast<int>(row);
        reordered.push_back(std::move(rows_[static_cast<std::size_t>(order[row])]));
    }
    rows_ = std::move(reordered);
    QModelIndexList next;
    next.reserve(previous.size());
    for (const auto& old : previous)
        next.push_back(index(destinations[static_cast<std::size_t>(old.row())], old.column()));
    changePersistentIndexList(previous, next);
    if (current_row_ >= 0)
        current_row_ = destinations[static_cast<std::size_t>(current_row_)];
    emit layoutChanged();
    refreshCurrentRow();
}

QString LocalListModel::undoLabel() const {
    if (canUndo() && !history_[history_cursor_ - 1].label.isEmpty())
        return history_[history_cursor_ - 1].label;
    return canUndo()
               ? (history_[history_cursor_ - 1].kind == Edit::Kind::removal ? tr("Remove tracks")
                                                                            : tr("Reorder tracks"))
               : QString{};
}
QString LocalListModel::redoLabel() const {
    if (canRedo() && !history_[history_cursor_].label.isEmpty())
        return history_[history_cursor_].label;
    return canRedo()
               ? (history_[history_cursor_].kind == Edit::Kind::removal ? tr("Remove tracks")
                                                                        : tr("Reorder tracks"))
               : QString{};
}
void LocalListModel::clearHistory() {
    if (history_.empty())
        return;
    history_.clear();
    history_cursor_ = 0;
    emit historyChanged();
}
void LocalListModel::rememberEdit(Edit edit) {
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_cursor_), history_.end());
    history_.push_back(std::move(edit));
    history_cursor_ = history_.size();
    trimHistory();
    emit historyChanged();
}
void LocalListModel::trimHistory() {
    const auto bytes = [](const Edit& edit) {
        std::size_t size = sizeof(Edit) +
                           static_cast<std::size_t>(edit.label.size()) * sizeof(QChar) +
                           (edit.positions.capacity() + edit.order.capacity()) * sizeof(int) +
                           edit.detached.capacity() * sizeof(LocalTrackRow);
        for (const auto& row : edit.detached) {
            for (const auto* value : {&row.raw_path, &row.title, &row.artist, &row.album,
                                      &row.album_artist, &row.date, &row.track_number})
                size += value->capacity();
            if (row.logical_reference)
                size += row.logical_reference->capacity();
            size += row.metadata.fields.capacity() * sizeof(metadata::MetadataField);
            for (const auto& field : row.metadata.fields) {
                size += field.canonical_name.capacity() + field.native_name.capacity();
                if (field.qualifier.language)
                    size += field.qualifier.language->capacity();
                if (field.qualifier.description)
                    size += field.qualifier.description->capacity();
                size += field.values.capacity() * sizeof(std::string);
                for (const auto& value : field.values)
                    size += value.capacity();
            }
            size += row.metadata.unsupported_native_objects.capacity() *
                    sizeof(metadata::NativeObjectIdentity);
            for (const auto& object : row.metadata.unsupported_native_objects)
                size += object.identity.capacity();
        }
        return size;
    };
    std::size_t total = 0;
    for (const auto& edit : history_)
        total += bytes(edit);
    bool trimmed = false;
    while (history_.size() > 100 || total > 64U * 1024U * 1024U) {
        // A redo-only chain depends on its first entry; discard it as a unit.
        if (history_cursor_ == 0) {
            clearHistory();
            trimmed = true;
            break;
        }
        total -= bytes(history_.front());
        history_.erase(history_.begin());
        --history_cursor_;
        trimmed = true;
    }
    if (trimmed) {
        emit historyChanged();
        emit historyDiscarded(tr("Older list edits were discarded to keep undo history bounded."));
    }
}
void LocalListModel::removePositions(const std::vector<int>& positions,
                                     std::vector<LocalTrackRow>* detached) {
    if (detached != nullptr)
        detached->resize(positions.size());
    for (std::size_t end = positions.size(); end > 0;) {
        auto begin = end - 1;
        while (begin > 0 && positions[begin - 1] + 1 == positions[begin])
            --begin;
        const auto first = positions[begin];
        const auto last = positions[end - 1];
        beginRemoveRows({}, first, last);
        if (detached != nullptr) {
            std::move(rows_.begin() + first, rows_.begin() + last + 1,
                      detached->begin() + static_cast<std::ptrdiff_t>(begin));
        }
        rows_.erase(rows_.begin() + first, rows_.begin() + last + 1);
        if (current_row_ > last)
            current_row_ -= last - first + 1;
        else if (current_row_ >= first) {
            current_row_ = -1;
            current_source_ = {};
        }
        endRemoveRows();
        end = begin;
    }
}

void LocalListModel::replayEdit(Edit& edit, const bool undoing) {
    if (edit.kind == Edit::Kind::reorder) {
        std::vector<int> inverse(edit.order.size());
        for (std::size_t row = 0; row < edit.order.size(); ++row)
            inverse[static_cast<std::size_t>(edit.order[row])] = static_cast<int>(row);
        applyOrder(edit.order);
        edit.order = std::move(inverse);
    } else if (edit.kind == Edit::Kind::replacement) {
        beginResetModel();
        rows_.swap(edit.detached);
        endResetModel();
        refreshCurrentRow();
    } else if ((edit.kind == Edit::Kind::removal && undoing) ||
               (edit.kind == Edit::Kind::addition && !undoing)) {
        for (std::size_t begin = 0; begin < edit.positions.size();) {
            auto end = begin + 1;
            while (end < edit.positions.size() &&
                   edit.positions[end - 1] + 1 == edit.positions[end])
                ++end;
            const auto first = edit.positions[begin];
            const auto last = edit.positions[end - 1];
            beginInsertRows({}, first, last);
            rows_.insert(
                rows_.begin() + first,
                std::make_move_iterator(edit.detached.begin() + static_cast<std::ptrdiff_t>(begin)),
                std::make_move_iterator(edit.detached.begin() + static_cast<std::ptrdiff_t>(end)));
            if (current_row_ >= first)
                current_row_ += last - first + 1;
            endInsertRows();
            begin = end;
        }
        edit.detached.clear();
        refreshCurrentRow();
    } else {
        removePositions(edit.positions, &edit.detached);
        refreshCurrentRow();
    }
}

bool LocalListModel::undo() {
    if (!canUndo())
        return false;
    replayEdit(history_[--history_cursor_], true);
    if (history_[history_cursor_].kind == Edit::Kind::removal) {
        const auto& positions = history_[history_cursor_].positions;
        emit historyRowsRestored(QList<int>(positions.begin(), positions.end()));
    }
    emit historyChanged();
    return true;
}
bool LocalListModel::redo() {
    if (!canRedo())
        return false;
    replayEdit(history_[history_cursor_++], false);
    trimHistory();
    emit historyChanged();
    return true;
}
std::vector<LocalTrackRow*> LocalListModel::retainedRows() {
    std::vector<LocalTrackRow*> retained;
    for (auto& edit : history_)
        for (auto& row : edit.detached)
            retained.push_back(&row);
    return retained;
}

bool LocalListModel::applyMetadata(const std::string& raw_path, const int hint_row,
                                   LocalTrackRow metadata) {
    const auto row = rowOfPath(raw_path, hint_row);
    if (row < 0) {
        return false;
    }
    metadata.logical_reference = rows_[static_cast<std::size_t>(row)].logical_reference;
    metadata.selection = rows_[static_cast<std::size_t>(row)].selection;
    metadata.segment = rows_[static_cast<std::size_t>(row)].segment;
    metadata.raw_path = raw_path;
    metadata.probed = true;
    rows_[static_cast<std::size_t>(row)] = std::move(metadata);
    emitRowChanged(row);
    return true;
}

bool LocalListModel::applyProbeRows(const std::string& raw_path, const int hint_row,
                                    std::vector<LocalTrackRow> rows) {
    if (rows.empty()) {
        return false;
    }
    const auto is_provisional = [&raw_path](const LocalTrackRow& row) {
        return row.raw_path == raw_path && !row.probed;
    };
    auto target = -1;
    if (hint_row >= 0 && hint_row < static_cast<int>(rows_.size()) &&
        is_provisional(rows_[static_cast<std::size_t>(hint_row)])) {
        target = hint_row;
    } else {
        const auto found = std::ranges::find_if(rows_, is_provisional);
        if (found != rows_.end()) {
            target = static_cast<int>(std::distance(rows_.begin(), found));
        }
    }
    if (target < 0) {
        return false;
    }

    for (auto& row : rows) {
        row.raw_path = raw_path;
        row.probed = true;
    }
    rows_[static_cast<std::size_t>(target)] = std::move(rows.front());
    emitRowChanged(target);
    if (rows.size() > 1U) {
        clearHistory();
        const auto first_inserted = target + 1;
        const auto last_inserted = target + static_cast<int>(rows.size()) - 1;
        beginInsertRows({}, first_inserted, last_inserted);
        rows_.insert(rows_.begin() + first_inserted, std::make_move_iterator(rows.begin() + 1),
                     std::make_move_iterator(rows.end()));
        endInsertRows();
    }
    refreshCurrentRow();
    return true;
}

core::Result<std::size_t>
LocalListModel::applyCommittedMetadata(const std::string& raw_path,
                                       const metadata::MetadataDocument& document,
                                       const core::LocalSourceRevision& published_revision) {
    const auto matches = [&raw_path](const LocalTrackRow& row) { return row.raw_path == raw_path; };
    auto retained_rows = retainedRows();
    if (std::ranges::any_of(retained_rows, [&](const auto* row) {
            return matches(*row) && row->logical_reference &&
                   std::ranges::any_of(row->metadata.fields, [](const auto& field) {
                       return field.provenance == metadata::FieldProvenance::cached_snapshot;
                   });
        })) {
        clearHistory();
        retained_rows.clear();
        emit historyDiscarded(
            tr("List undo was cleared because a removed logical track needs fresh metadata."));
    }
    const auto affected = static_cast<std::size_t>(std::ranges::count_if(rows_, matches));
    if (affected == 0U &&
        !std::ranges::any_of(retained_rows, [&](const auto* row) { return matches(*row); })) {
        return std::size_t{0U};
    }
    auto candidates = retained_rows;
    for (auto& row : rows_)
        candidates.push_back(&row);
    const auto ambiguous = std::ranges::any_of(candidates, [&](const LocalTrackRow* row) {
        return matches(*row) && row->logical_reference &&
               std::ranges::any_of(row->metadata.fields, [](const metadata::MetadataField& field) {
                   return field.provenance == metadata::FieldProvenance::cached_snapshot;
               });
    });
    if (ambiguous) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Logical tracks must be freshly probed before metadata commit",
            .context = {{"source_path", raw_path}},
        });
    }

    for (auto* candidate : candidates) {
        auto& row = *candidate;
        if (!matches(row)) {
            continue;
        }
        std::vector<metadata::MetadataField> retained;
        retained.reserve(row.metadata.fields.size());
        std::ranges::copy_if(
            row.metadata.fields, std::back_inserter(retained),
            [](const metadata::MetadataField& field) { return !replaceable_source_field(field); });
        row.metadata = document;
        row.metadata.fields.insert(row.metadata.fields.end(),
                                   std::make_move_iterator(retained.begin()),
                                   std::make_move_iterator(retained.end()));
        row.source_revision = published_revision;
        row.probed = true;
        project_display_metadata(row);
    }
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (matches(rows_[index]))
            emitRowChanged(static_cast<int>(index));
    }
    trimHistory();
    refreshCurrentRow();
    return affected;
}

std::size_t
LocalListModel::applyCueReplayGain(const std::string& reference, const bool prefix_match,
                                   const std::vector<CueReplayGainFieldUpdate>& fields) {
    if (fields.empty()) {
        return 0U;
    }
    const auto matches = [&](const LocalTrackRow& row) {
        return row.logical_reference &&
               (prefix_match ? row.logical_reference->starts_with(reference)
                             : *row.logical_reference == reference);
    };
    const auto update_row = [&fields](LocalTrackRow& row) {
        for (const auto& field : fields) {
            std::erase_if(row.metadata.fields, [&field](const metadata::MetadataField& existing) {
                return existing.provenance == metadata::FieldProvenance::segment &&
                       existing.canonical_name == field.canonical_name;
            });
            if (field.value) {
                row.metadata.fields.push_back(metadata::MetadataField{
                    .canonical_name = field.canonical_name,
                    .native_name = field.display_name,
                    .values = {*field.value},
                    .qualifier = {},
                    .provenance = metadata::FieldProvenance::segment,
                });
            }
        }
    };
    std::size_t affected = 0U;
    for (std::size_t index = 0U; index < rows_.size(); ++index) {
        if (!matches(rows_[index])) {
            continue;
        }
        update_row(rows_[index]);
        ++affected;
        emitRowChanged(static_cast<int>(index));
    }
    for (auto* row : retainedRows()) {
        if (matches(*row)) {
            update_row(*row);
        }
    }
    return affected;
}

std::size_t
LocalListModel::applySidecarLoudness(const std::string& raw_path,
                                     const SidecarRowIdentity& identity,
                                     const std::vector<CueReplayGainFieldUpdate>& fields) {
    if (fields.empty()) {
        return 0U;
    }
    const auto matches = [&](const LocalTrackRow& row) {
        const auto start = row.segment ? std::optional{row.segment->start_sample} : std::nullopt;
        const auto end = row.segment ? row.segment->end_sample : std::nullopt;
        return row.raw_path == raw_path && row.selection.stream_index == identity.stream_index &&
               row.selection.subsong_index == identity.subsong_index &&
               start == identity.start_sample && end == identity.end_sample;
    };
    const auto update_row = [&fields](LocalTrackRow& row) {
        for (const auto& field : fields) {
            std::erase_if(row.metadata.fields, [&field](const metadata::MetadataField& existing) {
                return existing.provenance == metadata::FieldProvenance::sidecar &&
                       existing.canonical_name == field.canonical_name;
            });
            if (field.value) {
                row.metadata.fields.push_back(metadata::MetadataField{
                    .canonical_name = field.canonical_name,
                    .native_name = field.display_name,
                    .values = {*field.value},
                    .qualifier = {},
                    .provenance = metadata::FieldProvenance::sidecar,
                });
            }
        }
    };
    std::size_t affected = 0U;
    for (std::size_t index = 0U; index < rows_.size(); ++index) {
        if (!matches(rows_[index])) {
            continue;
        }
        update_row(rows_[index]);
        ++affected;
        emitRowChanged(static_cast<int>(index));
    }
    for (auto* row : retainedRows()) {
        if (matches(*row)) {
            update_row(*row);
        }
    }
    return affected;
}

core::Result<std::size_t>
LocalListModel::applyCommittedRelocation(const std::string& source_raw_path,
                                         const std::string& target_raw_path,
                                         const core::LocalSourceRevision& previous_revision,
                                         const core::LocalSourceRevision& published_revision) {
    if (source_raw_path.empty() || target_raw_path.empty() || source_raw_path == target_raw_path ||
        previous_revision.inode == 0U || published_revision.inode == 0U) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Committed relocation requires distinct paths and valid revisions",
            .context = {},
        });
    }
    const auto matches_source = [&source_raw_path](const LocalTrackRow& row) {
        return row.raw_path == source_raw_path;
    };
    auto candidates = retainedRows();
    if (std::ranges::any_of(candidates, [&](const auto* row) {
            return matches_source(*row) && row->source_revision != previous_revision;
        })) {
        clearHistory();
        candidates.clear();
        emit historyDiscarded(
            tr("List undo was cleared because a removed file's revision changed."));
    }
    for (auto& row : rows_)
        candidates.push_back(&row);
    const auto affected = static_cast<std::size_t>(std::ranges::count_if(rows_, matches_source));
    if (!std::ranges::any_of(candidates, [&](const auto* row) { return matches_source(*row); })) {
        return std::size_t{0U};
    }
    if (std::ranges::any_of(rows_, [&target_raw_path](const LocalTrackRow& row) {
            return row.raw_path == target_raw_path;
        })) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Relocation target already exists in the local list",
            .context = {{"target_path", target_raw_path}},
        });
    }
    if (std::ranges::any_of(candidates, [&](const LocalTrackRow* row) {
            return matches_source(*row) && row->source_revision != previous_revision;
        })) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Relocated rows no longer identify the published source",
            .context = {{"source_path", source_raw_path}},
        });
    }

    if (current_source_.raw_path == source_raw_path) {
        current_source_.raw_path = target_raw_path;
    }
    std::vector<int> changed_rows;
    for (int index = 0; index < rowCount(); ++index) {
        if (matches_source(rows_[static_cast<std::size_t>(index)]))
            changed_rows.push_back(index);
    }
    for (auto* candidate : candidates) {
        auto& row = *candidate;
        if (!matches_source(row)) {
            continue;
        }
        row.raw_path = target_raw_path;
        row.source_revision = published_revision;
    }
    for (const auto index : changed_rows)
        emitRowChanged(index);
    trimHistory();
    refreshCurrentRow();
    return affected;
}

void LocalListModel::setCurrentPath(std::string raw_path, const int hint_row) {
    setCurrentSource(
        LocalTrackSource{.raw_path = std::move(raw_path), .selection = {}, .segment = std::nullopt},
        hint_row);
}

void LocalListModel::setCurrentSource(LocalTrackSource source, const int hint_row) {
    const auto previous = current_row_;
    current_source_ = std::move(source);
    current_row_ = current_source_.raw_path.empty() ? -1 : rowOfSource(current_source_, hint_row);
    if (previous >= 0 && previous < static_cast<int>(rows_.size())) {
        emitCurrentRowChanged(previous);
    }
    if (current_row_ >= 0) {
        emitCurrentRowChanged(current_row_);
    }
}

LocalTrackSource LocalListModel::source(const int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& track = rows_[static_cast<std::size_t>(row)];
    return LocalTrackSource{
        .raw_path = track.raw_path, .selection = track.selection, .segment = track.segment};
}

std::string LocalListModel::rawPath(const int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    return rows_[static_cast<std::size_t>(row)].raw_path;
}

int LocalListModel::rowOfPath(const std::string& raw_path, const int hint_row) const {
    if (hint_row >= 0 && hint_row < static_cast<int>(rows_.size()) &&
        rows_[static_cast<std::size_t>(hint_row)].raw_path == raw_path) {
        return hint_row;
    }
    const auto found = std::ranges::find(rows_, raw_path, &LocalTrackRow::raw_path);
    if (found == rows_.end()) {
        return -1;
    }
    return static_cast<int>(std::distance(rows_.begin(), found));
}

int LocalListModel::rowOfSource(const LocalTrackSource& source, const int hint_row) const {
    const auto matches = [&source](const LocalTrackRow& row) {
        return row.raw_path == source.raw_path && row.selection == source.selection &&
               row.segment == source.segment;
    };
    if (hint_row >= 0 && hint_row < static_cast<int>(rows_.size()) &&
        matches(rows_[static_cast<std::size_t>(hint_row)])) {
        return hint_row;
    }
    const auto found = std::ranges::find_if(rows_, matches);
    return found == rows_.end() ? -1 : static_cast<int>(std::distance(rows_.begin(), found));
}

int LocalListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int LocalListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : local_column_count;
}

QVariant LocalListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ui::track_source_role:
        return QByteArray(row.raw_path.data(), static_cast<qsizetype>(row.raw_path.size()));
    case ui::track_id_role:
    case ui::track_position_role:
        return index.row();
    case ui::track_duration_ms_role:
        return static_cast<qlonglong>(row.duration_ms.value_or(0));
    case ui::track_current_role:
        return index.row() == current_row_;
    case ui::track_album_artist_role:
        return display_utf8(row.album_artist.empty() ? row.artist : row.album_artist);
    case ui::track_priority_role:
        return {};
    case ui::track_rating_role:
        return QVariant::fromValue(row.rating);
    case ui::track_album_rating_role:
        return QVariant::fromValue(row.album_rating);
    case ui::track_album_artwork_role:
        return QVariant::fromValue(artwork_.value(groupKey(index.row())));
    case ui::track_album_artwork_key_role:
        return groupKey(index.row());
    case ui::track_album_group_start_role: {
        const auto row_index = static_cast<std::size_t>(index.row());
        const auto begins_group = (row_index == 0U || !same_album(rows_[row_index - 1U], row)) &&
                                  row_index + 1U < rows_.size() &&
                                  same_album(row, rows_[row_index + 1U]);
        return begins_group;
    }
    default:
        break;
    }
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case local_artwork_column:
            return {};
        case local_artist_column:
            return display_utf8(row.artist);
        case local_track_number_column:
            return display_utf8(row.track_number);
        case local_title_column:
            return row.title.empty() ? escaped(file_name_of(row.raw_path))
                                     : display_utf8(row.title);
        case local_album_column:
            return display_utf8(row.album);
        case local_date_column:
            return display_utf8(row.date);
        case local_length_column:
            return row.duration_ms ? format_duration(*row.duration_ms) : QString{};
        case local_rating_column:
            return ui::track_rating_stars(row.rating);
        default:
            return {};
        }
    }
    if (role == Qt::ForegroundRole && index.column() == local_rating_column && row.rating > 0U) {
        return QBrush{ui::ratingStarColor()};
    }
    if (role == Qt::ToolTipRole) {
        return escaped(row.raw_path);
    }
    return {};
}

QVariant LocalListModel::headerData(const int section, const Qt::Orientation orientation,
                                    const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
        section >= local_column_count) {
        return {};
    }
    return QString::fromLatin1(ui::track_column_headers.at(static_cast<std::size_t>(section)));
}

Qt::ItemFlags LocalListModel::flags(const QModelIndex& index) const {
    auto item_flags = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        item_flags |= Qt::ItemIsDragEnabled;
    }
    return item_flags;
}

Qt::DropActions LocalListModel::supportedDropActions() const {
    // Drops are executed by the view's typed callbacks, never by the model,
    // but Qt only tracks and paints the drop indicator for actions the target
    // model advertises.
    return Qt::MoveAction | Qt::CopyAction;
}

QString LocalListModel::groupKey(const int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    // Must mirror the shared delegate's grouping: album artist (with artist
    // fallback), album, and date, null-separated.
    const auto& track = rows_[static_cast<std::size_t>(row)];
    const auto& artist = track.album_artist.empty() ? track.artist : track.album_artist;
    return display_utf8(artist) + QChar::Null + display_utf8(track.album) + QChar::Null +
           display_utf8(track.date);
}

void LocalListModel::setArtwork(const QString& key, QImage image) {
    // A null image clears the entry. Inserting it instead would leave
    // hasArtwork() true, and the artwork reload after an invalidation
    // (metadata commit, ReplayGain write, conversion) would skip its
    // delivery — the album then shows the placeholder for the session.
    if (image.isNull()) {
        artwork_.remove(key);
    } else {
        artwork_.insert(key, std::move(image));
    }
    if (!rows_.empty()) {
        emit dataChanged(index(0, local_artwork_column),
                         index(static_cast<int>(rows_.size()) - 1, local_artwork_column),
                         {ui::track_album_artwork_role});
    }
}

void LocalListModel::refreshCurrentRow() {
    const auto previous = current_row_;
    current_row_ =
        current_source_.raw_path.empty() ? -1 : rowOfSource(current_source_, current_row_);
    if (previous != current_row_) {
        if (previous >= 0 && previous < static_cast<int>(rows_.size())) {
            emitCurrentRowChanged(previous);
        }
        if (current_row_ >= 0) {
            emitCurrentRowChanged(current_row_);
        }
    }
}

void LocalListModel::emitRowChanged(const int row) {
    emit dataChanged(index(row, 0), index(row, local_column_count - 1));
}

void LocalListModel::emitCurrentRowChanged(const int row) {
    emit dataChanged(index(row, 0), index(row, local_column_count - 1), {ui::track_current_role});
}

} // namespace trackknife::bench
