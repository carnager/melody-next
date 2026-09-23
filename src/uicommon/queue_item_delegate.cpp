// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/queue_item_delegate.hpp"

#include "uicommon/track_row_roles.hpp"

#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QTableView>

#include <algorithm>

namespace trackknife::ui {
namespace {

constexpr int track_row_height = 22;
constexpr int album_cover_extent = 22;

[[nodiscard]] int configuredColumn(const QObject* owner, const char* property, const int fallback) {
    const auto* view = qobject_cast<const QTableView*>(owner->parent());
    if (view == nullptr || !view->property(property).isValid()) {
        return fallback;
    }
    return view->property(property).toInt();
}

[[nodiscard]] bool configuredFlag(const QObject* owner, const char* property) {
    const auto* view = qobject_cast<const QTableView*>(owner->parent());
    return view != nullptr && view->property(property).toBool();
}

[[nodiscard]] QString groupKey(const QModelIndex& index, const QObject* owner) {
    const auto album_column =
        configuredColumn(owner, track_album_column_property, track_album_column);
    const auto date_column = configuredColumn(owner, track_date_column_property, track_date_column);
    return index.data(track_album_artist_role).toString() + QChar::Null +
           index.siblingAtColumn(album_column).data().toString() + QChar::Null +
           index.siblingAtColumn(date_column).data().toString();
}

[[nodiscard]] QString formatDuration(const qint64 milliseconds) {
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

[[nodiscard]] QString formattedTrackNumber(const QModelIndex& index, const QObject* owner) {
    const auto number_column =
        configuredColumn(owner, track_number_column_property, track_number_column);
    const auto raw_number = index.siblingAtColumn(number_column).data().toString().trimmed();
    if (raw_number.isEmpty()) {
        return {};
    }

    const auto number_text = raw_number.section(QLatin1Char('/'), 0, 0).trimmed();
    bool numeric = false;
    const auto number = number_text.toUInt(&numeric);
    return numeric ? QStringLiteral("%1").arg(number, 2, 10, QLatin1Char('0')) : number_text;
}

[[nodiscard]] QString trackLabel(const QModelIndex& index, const QObject* owner) {
    const auto title_column =
        configuredColumn(owner, track_title_column_property, track_title_column);
    const auto title = index.siblingAtColumn(title_column).data().toString();
    if (configuredFlag(owner, track_separate_number_property)) {
        return title;
    }
    const auto number = formattedTrackNumber(index, owner);
    return number.isEmpty() ? title : QStringLiteral("%1 %2").arg(number, title);
}

[[nodiscard]] bool isSingleTrackGroup(const QModelIndex& index, const QObject* owner) {
    if (!index.isValid()) {
        return false;
    }
    const auto key = groupKey(index, owner);
    const auto previous_matches =
        index.row() > 0 && key == groupKey(index.sibling(index.row() - 1, 0), owner);
    const auto next_matches = index.row() + 1 < index.model()->rowCount() &&
                              key == groupKey(index.sibling(index.row() + 1, 0), owner);
    return !previous_matches && !next_matches;
}

} // namespace

QueueItemDelegate::QueueItemDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void QueueItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    auto item = option;
    initStyleOption(&item, index);
    item.state &= ~QStyle::State_MouseOver;
    const auto* view = qobject_cast<const QTableView*>(parent());
    const auto artwork_column =
        configuredColumn(this, track_artwork_column_property, track_marker_column);
    const auto artist_column =
        configuredColumn(this, track_artist_column_property, track_artist_column);
    const auto title_column =
        configuredColumn(this, track_title_column_property, track_title_column);
    const auto length_column =
        configuredColumn(this, track_length_column_property, track_length_column);
    const auto side_artwork = configuredFlag(this, track_side_artwork_property);
    const auto artwork_cell = index.column() == artwork_column;
    const auto selected = item.state.testFlag(QStyle::State_Selected);
    if (artwork_cell) {
        // The cover/status gutter is visually separate from the metadata-row
        // selection and playback bands. Row selection remains visible from the
        // first metadata column onward.
        item.state &= ~(QStyle::State_HasFocus | QStyle::State_Selected);
    }
    if (view != nullptr && view->property("trackknife-hover-row").isValid() &&
        view->property("trackknife-hover-row").toInt() == index.row()) {
        item.state |= QStyle::State_MouseOver;
    }
    const auto group_start = beginsAlbum(index);
    if (group_start) {
        const QRect header_rect{option.rect.x(), option.rect.y(), option.rect.width(),
                                QueueItemDelegate::album_header_height};
        painter->save();
        painter->fillRect(header_rect, option.palette.alternateBase());
        painter->setPen(option.palette.mid().color());
        painter->drawLine(header_rect.bottomLeft(), header_rect.bottomRight());

        if (index.column() == artwork_column && !side_artwork) {
            const auto cover = index.data(track_album_artwork_role).value<QImage>();
            const QRect cover_bounds{header_rect.left() + 6,
                                     header_rect.center().y() - album_cover_extent / 2,
                                     album_cover_extent, album_cover_extent};
            if (!cover.isNull()) {
                const auto fitted = cover.size().scaled(cover_bounds.size(), Qt::KeepAspectRatio);
                const QRect target{cover_bounds.center().x() - fitted.width() / 2,
                                   cover_bounds.center().y() - fitted.height() / 2, fitted.width(),
                                   fitted.height()};
                painter->drawImage(target, cover);
            } else {
                const auto icon =
                    QIcon::fromTheme(QStringLiteral("media-optical-audio"),
                                     QApplication::style()->standardIcon(QStyle::SP_FileIcon));
                icon.paint(painter, cover_bounds, Qt::AlignCenter, QIcon::Normal);
            }
        } else if (index.column() == (side_artwork ? artist_column : title_column)) {
            auto font = option.font;
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(option.palette.text().color());
            painter->drawText(header_rect.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                              option.fontMetrics.elidedText(albumTitle(index), Qt::ElideRight,
                                                            header_rect.width() - 8));
        } else if (index.column() == length_column) {
            auto font = option.font;
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(option.palette.text().color());
            painter->drawText(header_rect.adjusted(4, 0, -6, 0), Qt::AlignVCenter | Qt::AlignRight,
                              formatDuration(albumDurationMs(index)));
        }
        painter->restore();
        item.rect.setTop(item.rect.top() + QueueItemDelegate::album_header_height);
    }

    const auto current_track = index.data(track_current_role).toBool();
    if (current_track) {
        item.font.setBold(true);
        if (!selected && !artwork_cell) {
            const auto base = item.palette.color(QPalette::Base);
            const auto accent = item.palette.color(QPalette::Highlight);
            const auto playback_fill = QColor::fromRgb((base.red() * 3 + accent.red()) / 4,
                                                       (base.green() * 3 + accent.green()) / 4,
                                                       (base.blue() * 3 + accent.blue()) / 4);
            item.backgroundBrush = playback_fill;
        }
        if (index.column() == (side_artwork ? title_column : artwork_column)) {
            item.icon =
                QIcon::fromTheme(QStringLiteral("media-playback-pause"),
                                 QApplication::style()->standardIcon(QStyle::SP_MediaPause));
            item.decorationSize = QSize{14, 14};
        }
    }
    const auto inline_artwork =
        index.column() == artwork_column && side_artwork && isSingleTrackGroup(index, this);
    if (artwork_cell) {
        item.text.clear();
    } else if (index.column() == title_column) {
        item.text = trackLabel(index, this);
    }
    const auto* widget = item.widget;
    auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
    item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
    if (inline_artwork) {
        const auto extent =
            std::max(0, std::min({18, item.rect.width() - 4, item.rect.height() - 4}));
        if (extent > 0) {
            const QRect target{item.rect.right() - extent - 2, item.rect.center().y() - extent / 2,
                               extent, extent};
            const auto cover = index.data(track_album_artwork_role).value<QImage>();
            if (!cover.isNull()) {
                const auto fitted = cover.size().scaled(target.size(), Qt::KeepAspectRatio);
                const QRect centered{target.center().x() - fitted.width() / 2,
                                     target.center().y() - fitted.height() / 2, fitted.width(),
                                     fitted.height()};
                painter->drawImage(centered, cover);
            } else {
                const auto icon =
                    QIcon::fromTheme(QStringLiteral("media-optical-audio"),
                                     QApplication::style()->standardIcon(QStyle::SP_FileIcon));
                icon.paint(painter, target, Qt::AlignCenter, QIcon::Disabled);
            }
        }
    }
    if (current_track && !selected && !artwork_cell) {
        auto playback_line = item.palette.color(QPalette::Highlight);
        playback_line.setAlpha(180);
        painter->save();
        painter->setPen(playback_line);
        painter->drawLine(item.rect.topLeft(), item.rect.topRight());
        painter->drawLine(item.rect.bottomLeft(), item.rect.bottomRight());
        painter->restore();
    }
}

QSize QueueItemDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    auto size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(track_row_height, size.height()) +
                   (beginsAlbum(index) ? album_header_height : 0));
    return size;
}

bool QueueItemDelegate::isAlbumHeaderHit(const QModelIndex& index, const int relative_y) const {
    return index.isValid() && beginsAlbum(index) && relative_y >= 0 &&
           relative_y < album_header_height;
}

std::pair<int, int> QueueItemDelegate::albumRowRange(const QModelIndex& index) const {
    if (!index.isValid()) {
        return {-1, -1};
    }
    const auto key = groupKey(index, this);
    auto first = index.row();
    while (first > 0 && groupKey(index.sibling(first - 1, 0), this) == key) {
        --first;
    }
    auto last = index.row();
    while (last + 1 < index.model()->rowCount() &&
           groupKey(index.sibling(last + 1, 0), this) == key) {
        ++last;
    }
    return {first, last};
}

bool QueueItemDelegate::beginsAlbum(const QModelIndex& index) const {
    if (!index.isValid()) {
        return false;
    }
    const auto cached = index.siblingAtColumn(0).data(track_album_group_start_role);
    if (cached.isValid()) {
        return cached.toBool();
    }
    const auto key = groupKey(index, this);
    const auto begins_group =
        index.row() == 0 || key != groupKey(index.sibling(index.row() - 1, 0), this);
    return begins_group && index.row() + 1 < index.model()->rowCount() &&
           key == groupKey(index.sibling(index.row() + 1, 0), this);
}

QString QueueItemDelegate::albumTitle(const QModelIndex& index) const {
    const auto artist = index.data(track_album_artist_role).toString();
    const auto album = index
                           .siblingAtColumn(configuredColumn(this, track_album_column_property,
                                                             track_album_column))
                           .data()
                           .toString();
    const auto date =
        index.siblingAtColumn(configuredColumn(this, track_date_column_property, track_date_column))
            .data()
            .toString();
    auto title = artist.isEmpty() ? QStringLiteral("Unknown artist") : artist;
    title += QStringLiteral(" — ");
    title += album.isEmpty() ? QStringLiteral("Unknown album") : album;
    if (!date.isEmpty()) {
        title += QStringLiteral(" (%1)").arg(date);
    }
    return title;
}

qint64 QueueItemDelegate::albumDurationMs(const QModelIndex& index) const {
    const auto key = groupKey(index, this);
    qint64 duration = 0;
    for (int row = index.row(); row < index.model()->rowCount(); ++row) {
        const auto current = index.sibling(row, 0);
        if (groupKey(current, this) != key) {
            break;
        }
        duration += current.data(track_duration_ms_role).toLongLong();
    }
    return duration;
}

} // namespace trackknife::ui
