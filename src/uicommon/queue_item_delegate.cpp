// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/queue_item_delegate.hpp"

#include "uicommon/track_row_roles.hpp"

#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QTableView>

#include <algorithm>
#include <cmath>

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

// A colour between two, `amount` of the way from `from` to `to`.
[[nodiscard]] QColor blended(const QColor& from, const QColor& to, const double amount) {
    const auto mix = [amount](const int a, const int b) {
        return static_cast<int>(std::lround(a + (b - a) * amount));
    };
    return QColor::fromRgb(mix(from.red(), to.red()), mix(from.green(), to.green()),
                           mix(from.blue(), to.blue()));
}

} // namespace

AlbumHeaderText albumHeaderText(const QAbstractItemModel& model, const int first_row,
                                const int album_column, const int date_column) {
    const auto key = [&](const int row) {
        return model.index(row, 0).data(track_album_artist_role).toString() + QChar::Null +
               model.index(row, album_column).data().toString() + QChar::Null +
               model.index(row, date_column).data().toString();
    };
    const auto group = key(first_row);
    int tracks = 0;
    qint64 duration = 0;
    for (int row = first_row; row < model.rowCount() && key(row) == group; ++row) {
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
    details << formatDuration(duration);
    return {.album = album.isEmpty() ? QStringLiteral("Unknown album") : album,
            .details = details.join(QStringLiteral(" · "))};
}

void paintAlbumHeader(QPainter* painter, const QRect& rect, const QPalette& palette,
                      const QFont& font, const AlbumHeaderText& text, const bool separated) {
    painter->save();
    painter->fillRect(rect, palette.base());
    if (separated) {
        auto hairline = palette.color(QPalette::Mid);
        hairline.setAlpha(110);
        painter->setPen(hairline);
        painter->drawLine(rect.topLeft(), rect.topRight());
    }
    auto album_font = font;
    album_font.setWeight(QFont::DemiBold);
    album_font.setPointSizeF(font.pointSizeF() * 1.08);
    const QFontMetrics album_metrics{album_font};
    // Sat on the rows below rather than floating mid-strip.
    const auto baseline = rect.bottom() - 8;
    const auto area = rect.adjusted(6, 0, -8, 0);
    const auto album = album_metrics.elidedText(text.album, Qt::ElideRight, area.width());
    painter->setFont(album_font);
    painter->setPen(palette.color(QPalette::Text));
    painter->drawText(QPoint{area.left(), baseline}, album);
    const auto used = album_metrics.horizontalAdvance(album) + 12;
    if (used < area.width()) {
        const QFontMetrics detail_metrics{font};
        painter->setFont(font);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(QPoint{area.left() + used, baseline},
                          detail_metrics.elidedText(text.details, Qt::ElideRight,
                                                    area.width() - used));
    }
    painter->restore();
}

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
    const auto album_column =
        configuredColumn(this, track_album_column_property, track_album_column);
    const auto date_column = configuredColumn(this, track_date_column_property, track_date_column);
    const auto group_start = beginsAlbum(index);
    if (group_start) {
        const QRect header_rect{option.rect.x(), option.rect.y(), option.rect.width(),
                                QueueItemDelegate::album_header_height};
        // Side artwork: the view draws the whole header over the row, beside
        // the cover. Here each cell draws only its share of the strip.
        paintAlbumHeader(painter, header_rect, option.palette, option.font, {}, index.row() > 0);
        if (!side_artwork && index.column() == artwork_column) {
            const auto cover = index.data(track_album_artwork_role).value<QImage>();
            const QRect cover_bounds{header_rect.left() + 6,
                                     header_rect.center().y() - album_cover_extent / 2,
                                     album_cover_extent, album_cover_extent};
            painter->save();
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
            painter->restore();
        } else if (!side_artwork && index.column() == title_column) {
            paintAlbumHeader(painter, header_rect, option.palette, option.font,
                             albumHeaderText(*index.model(), index.row(), album_column, date_column),
                             index.row() > 0);
        }
        item.rect.setTop(item.rect.top() + QueueItemDelegate::album_header_height);
    }

    const auto current_track = index.data(track_current_role).toBool();
    const auto in_group = !isSingleTrackGroup(index, this);
    if (!artwork_cell) {
        // Selection as a tint, so the text keeps its colour and the row that
        // plays still reads as playing inside a selection.
        if (selected) {
            item.state &= ~QStyle::State_Selected;
            item.backgroundBrush = blended(item.palette.color(QPalette::Base),
                                           item.palette.color(QPalette::Highlight), 0.32);
        }
        // Focus is one outline around the current row, drawn by the view,
        // not a box around each cell.
        item.state &= ~QStyle::State_HasFocus;
    }
    if (current_track) {
        item.font.setWeight(QFont::DemiBold);
        const auto accent = item.palette.color(QPalette::Highlight).lighter(115);
        item.palette.setColor(QPalette::Text, accent);
        item.palette.setColor(QPalette::HighlightedText, accent);
        if (index.column() == (side_artwork ? title_column : artwork_column)) {
            item.icon =
                QIcon::fromTheme(QStringLiteral("media-playback-start"),
                                 QApplication::style()->standardIcon(QStyle::SP_MediaPlay));
            item.decorationSize = QSize{14, 14};
        }
    }
    // Numbers right-aligned and quiet, so they end where the titles begin.
    if (index.column() == configuredColumn(this, track_number_column_property,
                                           track_number_column)) {
        item.displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
        if (!current_track) {
            item.palette.setColor(QPalette::Text, item.palette.color(QPalette::PlaceholderText));
        }
    }
    // Inside an album, what the header already says is not said again on
    // every row: album and date go, and the artist stays only where it
    // differs from the album's -- a compilation's tracks, quietly.
    if (in_group && !artwork_cell) {
        if (index.column() == album_column || index.column() == date_column) {
            item.text.clear();
        } else if (index.column() == artist_column) {
            if (item.text == index.data(track_album_artist_role).toString()) {
                item.text.clear();
            } else if (!current_track) {
                item.palette.setColor(QPalette::Text,
                                      item.palette.color(QPalette::PlaceholderText));
            }
        }
    }
    const auto inline_artwork =
        index.column() == artwork_column && side_artwork && isSingleTrackGroup(index, this);
    // What a hidden column would have said, after the title and quieter: a
    // compilation track's artist, and a lone track's artist and album.
    QString title_suffix;
    if (artwork_cell) {
        item.text.clear();
    } else if (index.column() == title_column) {
        item.text = trackLabel(index, this);
        const auto hidden = [view](const int column) {
            return view != nullptr && view->isColumnHidden(column);
        };
        const auto artist = index.siblingAtColumn(artist_column).data().toString();
        QStringList extra;
        if (hidden(artist_column) && !artist.isEmpty() &&
            (!in_group || artist != index.data(track_album_artist_role).toString())) {
            extra << artist;
        }
        if (!in_group && hidden(album_column)) {
            const auto album = index.siblingAtColumn(album_column).data().toString();
            if (!album.isEmpty()) {
                extra << album;
            }
        }
        title_suffix = extra.join(QStringLiteral(" · "));
    }
    const auto* widget = item.widget;
    auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
    if (title_suffix.isEmpty()) {
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
    } else {
        const auto title = item.text;
        item.text.clear();
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        item.text = title;
        const auto area =
            item_style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget).adjusted(2, 0, -2, 0);
        const QFontMetrics title_metrics{item.font};
        const auto shown = title_metrics.elidedText(title, Qt::ElideRight, area.width());
        painter->save();
        painter->setFont(item.font);
        painter->setPen(item.palette.color(QPalette::Text));
        painter->drawText(area, Qt::AlignVCenter | Qt::AlignLeft, shown);
        const auto used = title_metrics.horizontalAdvance(shown);
        const auto rest = area.adjusted(used, 0, 0, 0);
        if (rest.width() > 12) {
            const QFontMetrics suffix_metrics{option.font};
            painter->setFont(option.font);
            painter->setPen(current_track ? item.palette.color(QPalette::Text)
                                          : item.palette.color(QPalette::PlaceholderText));
            painter->drawText(rest, Qt::AlignVCenter | Qt::AlignLeft,
                              suffix_metrics.elidedText(QStringLiteral(" — ") + title_suffix,
                                                        Qt::ElideRight, rest.width()));
        }
        painter->restore();
    }
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

} // namespace trackknife::ui
