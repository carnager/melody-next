// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/track_grouping.hpp"

#include <QAbstractItemModel>
#include <QStringList>

#include <algorithm>

namespace trackknife::ui {

QString trackGroupKey(const QAbstractItemModel& model, const int row,
                      const TrackGroupColumns columns) {
    if (row < 0 || row >= model.rowCount()) {
        return {};
    }
    return model.index(row, 0).data(track_album_artist_role).toString() + QChar::Null +
           model.index(row, columns.album).data().toString() + QChar::Null +
           model.index(row, columns.date).data().toString();
}

bool inTrackGroup(const QAbstractItemModel& model, const int row, const TrackGroupColumns columns) {
    if (row < 0 || row >= model.rowCount()) {
        return false;
    }
    if (columns.lone_tracks_grouped) {
        return true;
    }
    const auto key = trackGroupKey(model, row, columns);
    return (row > 0 && key == trackGroupKey(model, row - 1, columns)) ||
           (row + 1 < model.rowCount() && key == trackGroupKey(model, row + 1, columns));
}

bool beginsTrackGroup(const QAbstractItemModel& model, const int row,
                      const TrackGroupColumns columns) {
    if (row < 0 || row >= model.rowCount()) {
        return false;
    }
    if (columns.lone_tracks_grouped) {
        return row == 0 || trackGroupKey(model, row, columns) !=
                               trackGroupKey(model, row - 1, columns);
    }
    const auto cached = model.index(row, 0).data(track_album_group_start_role);
    if (cached.isValid()) {
        return cached.toBool();
    }
    const auto key = trackGroupKey(model, row, columns);
    return (row == 0 || key != trackGroupKey(model, row - 1, columns)) &&
           row + 1 < model.rowCount() && key == trackGroupKey(model, row + 1, columns);
}

bool beginsLooseRun(const QAbstractItemModel& model, const int row,
                    const TrackGroupColumns columns) {
    return row > 0 && row < model.rowCount() && !inTrackGroup(model, row, columns) &&
           inTrackGroup(model, row - 1, columns);
}

QString trackDiscStart(const QAbstractItemModel& model, const int row) {
    return row >= 0 && row < model.rowCount()
               ? model.index(row, 0).data(track_disc_start_role).toString()
               : QString{};
}

int trackGroupSpacing(const QAbstractItemModel& model, const int row,
                      const TrackGroupColumns columns) {
    const auto disc = trackDiscStart(model, row).isEmpty() ? 0 : disc_header_height;
    if (beginsTrackGroup(model, row, columns)) {
        return album_header_height + disc;
    }
    return (beginsLooseRun(model, row, columns) ? loose_run_gap : 0) + disc;
}

std::pair<int, int> trackGroupRange(const QAbstractItemModel& model, const int row,
                                    const TrackGroupColumns columns) {
    if (row < 0 || row >= model.rowCount()) {
        return {-1, -1};
    }
    const auto key = trackGroupKey(model, row, columns);
    auto first = row;
    while (first > 0 && trackGroupKey(model, first - 1, columns) == key) {
        --first;
    }
    auto last = row;
    while (last + 1 < model.rowCount() && trackGroupKey(model, last + 1, columns) == key) {
        ++last;
    }
    return {first, last};
}

QHash<int, int> fitTrackColumns(const QList<int>& visible, QList<int> expanding,
                                const std::function<int(int)>& minimum,
                                const std::function<int(int)>& preferred, const int available) {
    QHash<int, int> target_widths;
    if (visible.isEmpty()) {
        return target_widths;
    }
    expanding.removeIf([&visible](const int column) { return !visible.contains(column); });
    if (expanding.isEmpty()) {
        expanding.push_back(visible.back());
    }
    int fixed_total = 0;
    int expanding_minimum_total = 0;
    for (const auto column : visible) {
        if (expanding.contains(column)) {
            expanding_minimum_total += minimum(column);
        } else {
            const auto width = preferred(column);
            target_widths.insert(column, width);
            fixed_total += width;
        }
    }
    auto available_for_expanding = available - fixed_total;
    if (available_for_expanding < expanding_minimum_total) {
        // A narrow viewport may require compact columns to participate too.
        expanding = visible;
        target_widths.clear();
        expanding_minimum_total = 0;
        for (const auto column : expanding) {
            expanding_minimum_total += minimum(column);
        }
        available_for_expanding = available;
    }
    const auto distributable = std::max(0, available_for_expanding - expanding_minimum_total);
    int total_weight = 0;
    for (const auto column : expanding) {
        total_weight += std::max(1, preferred(column) - minimum(column));
    }
    auto remaining = available_for_expanding;
    for (qsizetype index = 0; index < expanding.size(); ++index) {
        const auto column = expanding.at(index);
        int width = minimum(column);
        if (index + 1 == expanding.size()) {
            width = std::max(width, remaining);
        } else if (total_weight > 0) {
            const auto weight = std::max(1, preferred(column) - minimum(column));
            width += distributable * weight / total_weight;
        }
        target_widths.insert(column, width);
        remaining -= width;
    }
    for (const auto column : visible) {
        if (!target_widths.contains(column)) {
            target_widths.insert(column, minimum(column));
        }
    }
    return target_widths;
}

namespace {

[[nodiscard]] QString formattedTrackNumber(const QString& raw) {
    const auto trimmed = raw.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const auto number_text = trimmed.section(QLatin1Char('/'), 0, 0).trimmed();
    bool numeric = false;
    const auto number = number_text.toUInt(&numeric);
    return numeric ? QStringLiteral("%1").arg(number, 2, 10, QLatin1Char('0')) : number_text;
}

} // namespace

TrackCell trackCell(const QAbstractItemModel& model, const int row, const int column,
                    const TrackCellContext& context) {
    TrackCell cell;
    const auto index = model.index(row, column);
    if (!index.isValid()) {
        return cell;
    }
    const auto hidden = [&context](const int at) { return context.hidden && context.hidden(at); };
    cell.text = index.data().toString();
    cell.artwork_cell = column == context.artwork;
    cell.current = model.index(row, 0).data(track_current_role).toBool();
    cell.in_group = inTrackGroup(model, row, context.groups);
    if (cell.current && column == (context.side_artwork ? context.title : context.artwork)) {
        cell.playing_icon = true;
    }
    // Numbers right-aligned and quiet, so they end where the titles begin.
    if (column == context.number) {
        cell.right_aligned = true;
        cell.quiet = !cell.current;
    }
    const auto album_artist = model.index(row, 0).data(track_album_artist_role).toString();
    if (cell.in_group && !cell.artwork_cell) {
        if (column == context.groups.album || column == context.groups.date) {
            cell.text.clear();
        } else if (column == context.artist) {
            if (cell.text == album_artist) {
                cell.text.clear();
            } else if (!cell.current) {
                cell.quiet = true;
            }
        }
    }
    // A lone track's number means nothing without its album, so its cover
    // takes the number's place, just before the title. With no number
    // column, it keeps to the cover column.
    cell.inline_cover = context.side_artwork && !cell.in_group &&
                        (!hidden(context.number) ? column == context.number
                                                 : column == context.artwork);
    if (cell.inline_cover && !cell.artwork_cell) {
        cell.text.clear();
    }
    if (cell.artwork_cell) {
        cell.text.clear();
    } else if (column == context.title) {
        if (!context.separate_number) {
            const auto number =
                formattedTrackNumber(model.index(row, context.number).data().toString());
            if (!number.isEmpty()) {
                cell.text = QStringLiteral("%1 %2").arg(number, cell.text);
            }
        }
        const auto artist = model.index(row, context.artist).data().toString();
        QStringList extra;
        if (hidden(context.artist) && !artist.isEmpty() &&
            (!cell.in_group || artist != album_artist)) {
            extra << artist;
        }
        if (!cell.in_group && hidden(context.groups.album)) {
            const auto album = model.index(row, context.groups.album).data().toString();
            if (!album.isEmpty()) {
                extra << album;
            }
        }
        cell.suffix = extra.join(QStringLiteral(" · "));
    }
    return cell;
}

QString formatTrackDuration(const qint64 milliseconds) {
    const auto seconds = std::max<qint64>(0, milliseconds / 1'000);
    const auto hours = seconds / 3'600;
    const auto minutes = (seconds / 60) % 60;
    const auto remainder = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(remainder, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(remainder, 2, 10, QLatin1Char('0'));
}

AlbumHeaderText albumHeaderText(const QAbstractItemModel& model, const int first_row,
                                const int album_column, const int date_column) {
    const TrackGroupColumns columns{.album = album_column, .date = date_column};
    const auto group = trackGroupKey(model, first_row, columns);
    int tracks = 0;
    qint64 duration = 0;
    for (int row = first_row; row < model.rowCount() && trackGroupKey(model, row, columns) == group;
         ++row) {
        ++tracks;
        duration += model.index(row, 0).data(track_duration_ms_role).toLongLong();
    }
    const auto artist = model.index(first_row, 0).data(track_album_artist_role).toString();
    const auto album = model.index(first_row, album_column).data().toString();
    const auto date = model.index(first_row, date_column).data().toString();
    QStringList details;
    details << (artist.isEmpty() ? QStringLiteral("Unknown artist") : artist);
    if (!date.isEmpty()) {
        details << date;
    }
    details << (tracks == 1 ? QStringLiteral("1 track") : QStringLiteral("%1 tracks").arg(tracks));
    details << formatTrackDuration(duration);
    return {.album = album.isEmpty() ? QStringLiteral("Unknown album") : album,
            .details = details.join(QStringLiteral(" · "))};
}

} // namespace trackknife::ui
