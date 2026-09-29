// SPDX-License-Identifier: GPL-3.0-only

#include "quick/track_rows_model.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/local_list_model.hpp"

#include <QBrush>
#include <QColor>

#include <algorithm>
#include <ranges>

namespace trackknife::quick {
namespace {

// Qt::KeyboardModifiers as QML hands them over.
[[nodiscard]] bool has(const int modifiers, const Qt::KeyboardModifier modifier) {
    return (modifiers & modifier) != 0;
}

} // namespace

TrackRowsModel::TrackRowsModel(QObject* parent) : QAbstractListModel(parent) {}

void TrackRowsModel::setSource(bench::LocalListModel* source, const ui::TrackViewLayout& layout) {
    if (source_ != nullptr) {
        disconnect(source_, nullptr, this, nullptr);
    }
    beginResetModel();
    source_ = source;
    layout_ = layout;
    selected_.clear();
    current_.clear();
    anchor_.clear();
    endResetModel();
    refit();
    emit countChanged();
    emit selectionChanged();
    emit currentRowChanged();
    if (source_ == nullptr) {
        return;
    }
    // Any change can move where a group begins or ends, so what every row
    // shows is looked at again; only the rows on screen are redrawn.
    connect(source_, &QAbstractItemModel::rowsAboutToBeInserted, this,
            [this](const QModelIndex&, const int first, const int last) {
                beginInsertRows({}, first, last);
            });
    connect(source_, &QAbstractItemModel::rowsInserted, this, [this] {
        endInsertRows();
        emit countChanged();
        refreshAll();
    });
    connect(source_, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex&, const int first, const int last) {
                beginRemoveRows({}, first, last);
            });
    connect(source_, &QAbstractItemModel::rowsRemoved, this, [this] {
        endRemoveRows();
        emit countChanged();
        selectionEdited();
        refreshAll();
    });
    connect(source_, &QAbstractItemModel::rowsAboutToBeMoved, this,
            [this](const QModelIndex&, const int first, const int last, const QModelIndex&,
                   const int destination) {
                beginMoveRows({}, first, last, {}, destination);
            });
    connect(source_, &QAbstractItemModel::rowsMoved, this, [this] {
        endMoveRows();
        refreshAll();
    });
    connect(source_, &QAbstractItemModel::modelAboutToBeReset, this, [this] { beginResetModel(); });
    connect(source_, &QAbstractItemModel::modelReset, this, [this] {
        endResetModel();
        emit countChanged();
        selectionEdited();
    });
    connect(source_, &QAbstractItemModel::layoutChanged, this, [this] {
        refreshAll();
        emit currentRowChanged();
    });
    connect(source_, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& top_left, const QModelIndex& bottom_right) {
                // A row's album changing can make or break the groups beside it.
                const auto first = std::max(0, top_left.row() - 1);
                const auto last = std::min(rowCount() - 1, bottom_right.row() + 1);
                if (first <= last) {
                    emit dataChanged(index(first), index(last));
                }
            });
}

void TrackRowsModel::setLayout(const ui::TrackViewLayout& layout) {
    layout_ = layout;
    refit();
    refreshAll();
}

void TrackRowsModel::setViewportWidth(const int width) {
    if (width != viewport_width_) {
        viewport_width_ = width;
        refit();
        refreshAll();
    }
}

bool TrackRowsModel::grouped() const {
    return layout_.presentation == ui::TrackViewPresentation::albums_side_artwork ||
           layout_.presentation == ui::TrackViewPresentation::albums_header_artwork;
}

bool TrackRowsModel::sideArtwork() const {
    return layout_.presentation == ui::TrackViewPresentation::albums_side_artwork;
}

void TrackRowsModel::refit() {
    hidden_.clear();
    QList<int> visible;
    QHash<int, int> preferred;
    QHash<int, int> minimum;
    for (const auto& column : layout_.columns) {
        const auto logical = bench::trackColumnLogical(column.id);
        const auto spec =
            std::ranges::find(bench::track_column_specs, logical, &bench::TrackColumnSpec::logical);
        if (logical < 0 || spec == bench::track_column_specs.end()) {
            continue;
        }
        // Listening history is for a list of this computer's own: the other
        // models do not have it.
        if (!column.visible) {
            hidden_.insert(logical);
            continue;
        }
        visible.push_back(logical);
        preferred.insert(logical, column.width);
        minimum.insert(logical, spec->minimum_width);
    }
    // Header sections are never narrower than 24.
    const auto minimum_of = [&minimum](const int column) {
        return std::max(24, minimum.value(column, 24));
    };
    const auto preferred_of = [&preferred, &minimum_of](const int column) {
        return std::max(minimum_of(column), preferred.value(column, 100));
    };
    const auto widths = ui::fitTrackColumns(
        visible, {bench::local_artist_column, bench::local_title_column, bench::local_album_column},
        minimum_of, preferred_of, viewport_width_);
    QVariantList columns;
    for (const auto logical : visible) {
        columns.push_back(QVariantMap{
            {QStringLiteral("logical"), logical},
            {QStringLiteral("id"), bench::trackColumnId(logical)},
            {QStringLiteral("header"),
             QString::fromLatin1(ui::track_column_headers[static_cast<std::size_t>(logical)])},
            {QStringLiteral("width"), widths.value(logical, minimum_of(logical))},
            {QStringLiteral("minimum"), minimum_of(logical)},
        });
    }
    columns_ = std::move(columns);
    emit layoutChanged();
}

void TrackRowsModel::refreshAll() {
    if (rowCount() > 0) {
        emit dataChanged(index(0), index(rowCount() - 1));
    }
}

int TrackRowsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || source_ == nullptr ? 0 : source_->rowCount();
}

ui::TrackCellContext TrackRowsModel::cellContext() const {
    return {
        .groups = {.album = bench::local_album_column, .date = bench::local_date_column, .lone_tracks_grouped = true},
        .artwork = bench::local_artwork_column,
        .artist = bench::local_artist_column,
        .number = bench::local_track_number_column,
        .title = bench::local_title_column,
        .side_artwork = sideArtwork(),
        .separate_number = true,
        .hidden = [this](const int column) { return hidden_.contains(column); },
    };
}

QVariantList TrackRowsModel::cellsOf(const int row) const {
    QVariantList cells;
    const auto context = cellContext();
    const auto is_grouped = grouped();
    for (const auto& column : columns_) {
        const auto logical = column.toMap().value(QStringLiteral("logical")).toInt();
        const auto at = source_->index(row, logical);
        QVariantMap cell;
        cell.insert(QStringLiteral("logical"), logical);
        const auto foreground = at.data(Qt::ForegroundRole);
        cell.insert(QStringLiteral("foreground"),
                    foreground.isValid() ? foreground.value<QBrush>().color().name()
                                         : QString{});
        cell.insert(QStringLiteral("tooltip"), at.data(Qt::ToolTipRole).toString());
        if (is_grouped) {
            const auto shown = ui::trackCell(*source_, row, logical, context);
            cell.insert(QStringLiteral("text"), shown.text);
            cell.insert(QStringLiteral("suffix"), shown.suffix);
            cell.insert(QStringLiteral("quiet"), shown.quiet);
            cell.insert(QStringLiteral("rightAligned"), shown.right_aligned);
            cell.insert(QStringLiteral("current"), shown.current);
            cell.insert(QStringLiteral("playingIcon"), shown.playing_icon);
            cell.insert(QStringLiteral("inlineCover"), shown.inline_cover);
            cell.insert(QStringLiteral("artworkCell"), shown.artwork_cell);
        } else {
            // Plain columns draw the model as it is, as a plain delegate does.
            const auto alignment = at.data(Qt::TextAlignmentRole);
            cell.insert(QStringLiteral("text"), at.data().toString());
            cell.insert(QStringLiteral("rightAligned"),
                        alignment.isValid() && (alignment.toInt() & Qt::AlignRight) != 0);
        }
        cells.push_back(cell);
    }
    return cells;
}

int TrackRowsModel::rowHeight(const int row) const {
    if (source_ == nullptr || row < 0 || row >= rowCount()) {
        return ui::track_row_height;
    }
    const ui::TrackGroupColumns columns{.album = bench::local_album_column,
                                        .date = bench::local_date_column,
                                        .lone_tracks_grouped = true};
    return ui::track_row_height + (grouped() ? ui::trackGroupSpacing(*source_, row, columns) : 0);
}

QVariant TrackRowsModel::data(const QModelIndex& index, const int role) const {
    if (source_ == nullptr || !index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    const auto row = index.row();
    const ui::TrackGroupColumns columns{.album = bench::local_album_column,
                                        .date = bench::local_date_column,
                                        .lone_tracks_grouped = true};
    const auto anchor = source_->index(row, 0);
    switch (role) {
    case cells_role:
        return cellsOf(row);
    case spacing_role:
        return grouped() ? ui::trackGroupSpacing(*source_, row, columns) : 0;
    case group_start_role:
        return grouped() && ui::beginsTrackGroup(*source_, row, columns);
    case loose_run_role:
        return grouped() && ui::beginsLooseRun(*source_, row, columns);
    case disc_role:
        return grouped() ? ui::trackDiscStart(*source_, row) : QString{};
    case header_album_role:
    case header_details_role: {
        if (!grouped() || !ui::beginsTrackGroup(*source_, row, columns)) {
            return QString{};
        }
        const auto text =
            ui::albumHeaderText(*source_, row, bench::local_album_column, bench::local_date_column);
        return role == header_album_role ? text.album : text.details;
    }
    case cover_key_role:
        return anchor.data(ui::track_album_artwork_key_role);
    case cover_offset_role:
    case group_height_role: {
        // Where this row's top is below its group's first row, and how tall
        // the group is: a cover beside the rows is drawn by each row, clipped
        // to it, so it is whole however the list is scrolled.
        if (!ui::inTrackGroup(*source_, row, columns)) {
            return 0;
        }
        const auto [first, last] = ui::trackGroupRange(*source_, row, columns);
        int offset = 0;
        for (int at = first; at < row; ++at) {
            offset += rowHeight(at);
        }
        if (role == cover_offset_role) {
            return offset;
        }
        int height = 0;
        for (int at = first; at <= last && height < 1'000; ++at) {
            height += rowHeight(at);
        }
        return height;
    }
    case album_rating_role:
        return anchor.data(ui::track_album_rating_role);
    case selected_role:
        return selected_.contains(entryOf(row));
    case current_track_role:
        return anchor.data(ui::track_current_role);
    case entry_role:
        return entryOf(row);
    case tooltip_role:
        return anchor.data(Qt::ToolTipRole);
    default:
        return {};
    }
}

QHash<int, QByteArray> TrackRowsModel::roleNames() const {
    return {{cells_role, "cells"},
            {spacing_role, "spacing"},
            {group_start_role, "groupStart"},
            {loose_run_role, "looseRun"},
            {disc_role, "disc"},
            {header_album_role, "headerAlbum"},
            {header_details_role, "headerDetails"},
            {cover_key_role, "coverKey"},
            {cover_offset_role, "coverOffset"},
            {group_height_role, "groupHeight"},
            {album_rating_role, "albumRating"},
            {selected_role, "selected"},
            {current_track_role, "currentTrack"},
            {entry_role, "entry"},
            {tooltip_role, "rowTooltip"}};
}

// --- Selection ----------------------------------------------------------------

QString TrackRowsModel::entryOf(const int row) const {
    if (source_ == nullptr || row < 0 || row >= static_cast<int>(source_->rows().size())) {
        return {};
    }
    return QString::fromStdString(source_->rows()[static_cast<std::size_t>(row)].entry_id.to_string());
}

int TrackRowsModel::rowOfEntry(const QString& entry) const {
    if (source_ == nullptr || entry.isEmpty()) {
        return -1;
    }
    const auto& rows = source_->rows();
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (QString::fromStdString(rows[row].entry_id.to_string()) == entry) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

int TrackRowsModel::currentRow() const { return rowOfEntry(current_); }

std::vector<int> TrackRowsModel::selectedRows() const {
    std::vector<int> rows;
    if (source_ == nullptr) {
        return rows;
    }
    const auto& tracks = source_->rows();
    for (std::size_t row = 0; row < tracks.size(); ++row) {
        if (selected_.contains(QString::fromStdString(tracks[row].entry_id.to_string()))) {
            rows.push_back(static_cast<int>(row));
        }
    }
    return rows;
}

QVariantList TrackRowsModel::selectedRowList() const {
    QVariantList rows;
    for (const auto row : selectedRows()) {
        rows.push_back(row);
    }
    return rows;
}

void TrackRowsModel::selectionEdited() {
    // Entries no longer in the list are no longer selected.
    QSet<QString> present;
    if (source_ != nullptr) {
        for (const auto& row : source_->rows()) {
            present.insert(QString::fromStdString(row.entry_id.to_string()));
        }
    }
    selected_.intersect(present);
    if (!present.contains(current_)) {
        current_.clear();
    }
    emit selectionChanged();
    emit currentRowChanged();
    refreshAll();
}

void TrackRowsModel::press(const int row, const int modifiers) {
    const auto entry = entryOf(row);
    if (entry.isEmpty()) {
        if (!has(modifiers, Qt::ControlModifier) && !selected_.isEmpty()) {
            selected_.clear();
            selectionEdited();
        }
        return;
    }
    if (has(modifiers, Qt::ShiftModifier) && rowOfEntry(anchor_) >= 0) {
        const auto from = rowOfEntry(anchor_);
        if (!has(modifiers, Qt::ControlModifier)) {
            selected_.clear();
        }
        for (int at = std::min(from, row); at <= std::max(from, row); ++at) {
            selected_.insert(entryOf(at));
        }
    } else if (has(modifiers, Qt::ControlModifier)) {
        if (selected_.contains(entry)) {
            selected_.remove(entry);
        } else {
            selected_.insert(entry);
        }
        anchor_ = entry;
    } else {
        selected_ = {entry};
        anchor_ = entry;
    }
    current_ = entry;
    emit selectionChanged();
    emit currentRowChanged();
    refreshAll();
}

void TrackRowsModel::moveCurrent(const int row, const int modifiers) {
    const auto target = std::clamp(row, 0, rowCount() - 1);
    if (rowCount() == 0) {
        return;
    }
    if (has(modifiers, Qt::ControlModifier) && !has(modifiers, Qt::ShiftModifier)) {
        // Ctrl moves the keyboard's place without selecting.
        current_ = entryOf(target);
        emit currentRowChanged();
        refreshAll();
        return;
    }
    press(target, modifiers & Qt::ShiftModifier);
}

void TrackRowsModel::selectAll() {
    if (source_ == nullptr) {
        return;
    }
    for (const auto& row : source_->rows()) {
        selected_.insert(QString::fromStdString(row.entry_id.to_string()));
    }
    emit selectionChanged();
    refreshAll();
}

void TrackRowsModel::selectRows(const QVariantList& rows, const int current) {
    selected_.clear();
    for (const auto& row : rows) {
        if (const auto entry = entryOf(row.toInt()); !entry.isEmpty()) {
            selected_.insert(entry);
        }
    }
    if (current >= 0) {
        current_ = entryOf(current);
        anchor_ = current_;
    } else if (!rows.isEmpty()) {
        current_ = entryOf(rows.front().toInt());
        anchor_ = current_;
    }
    emit selectionChanged();
    emit currentRowChanged();
    refreshAll();
}

// Right-clicking an album's header selects the whole album.
void TrackRowsModel::selectGroup(const int row) {
    if (source_ == nullptr) {
        return;
    }
    const auto [first, last] = ui::trackGroupRange(
        *source_, row, {.album = bench::local_album_column, .date = bench::local_date_column, .lone_tracks_grouped = true});
    QVariantList rows;
    for (int at = first; at <= last && at >= 0; ++at) {
        rows.push_back(at);
    }
    selectRows(rows, first);
}

} // namespace trackknife::quick
