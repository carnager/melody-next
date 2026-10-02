// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/queue_item_delegate.hpp"

#include "uicommon/track_row_roles.hpp"

#include <QApplication>
#include <QPainter>
#include <QRegularExpression>
#include <QStyle>
#include <QTableView>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace trackknife::ui {
namespace {

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

// A lone track is an album of its own (ADR-0250).
[[nodiscard]] TrackGroupColumns groupColumns(const QObject* owner) {
    return {.album = configuredColumn(owner, track_album_column_property, track_album_column),
            .date = configuredColumn(owner, track_date_column_property, track_date_column),
            .lone_tracks_grouped = true};
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

void paintInitialsTile(QPainter* painter, const QRect& rect, const QString& name,
                       const QPalette& palette) {
    QString initials;
    for (const auto& word :
         name.split(QRegularExpression{QStringLiteral("\\s+")}, Qt::SkipEmptyParts)) {
        for (const auto character : word) {
            if (character.toUpper() != character.toLower() ||
                (character >= QLatin1Char('0') && character <= QLatin1Char('9'))) {
                initials += character.toUpper();
                break;
            }
        }
        if (initials.size() == 2) {
            break;
        }
    }
    // A stable hue: hash = hash * 31 + each UTF-16 unit, kept to 32 bits,
    // so a name is the same colour on every run and every machine.
    std::uint32_t hash = 0U;
    for (const auto unit : name) {
        hash = hash * 31U + unit.unicode();
    }
    const auto fill =
        initials.isEmpty()
            ? blended(palette.color(QPalette::Base), palette.color(QPalette::Text), 0.12)
            : QColor::fromHslF(static_cast<float>(hash % 360U) / 360.0F, 0.42F, 0.40F);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(rect, 3, 3);
    auto font = painter->font();
    font.setPixelSize(std::max(8, static_cast<int>(rect.height() * 0.38)));
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    painter->setPen(initials.isEmpty() ? palette.color(QPalette::PlaceholderText)
                                       : QColor{0xf4, 0xf4, 0xf4});
    painter->drawText(rect, Qt::AlignCenter, initials);
    painter->restore();
}

QColor groupHairline(const QPalette& palette) {
    auto line = palette.color(QPalette::Text);
    line.setAlpha(38);
    return line;
}

void paintAlbumHeader(QPainter* painter, const QRect& rect, const QPalette& palette,
                      const QFont& font, const AlbumHeaderText& text, const bool separated) {
    painter->save();
    painter->fillRect(rect, palette.base());
    if (separated) {
        const auto y = rect.top() + QueueItemDelegate::hairline_offset;
        painter->setPen(groupHairline(palette));
        painter->drawLine(QPoint{rect.left(), y}, QPoint{rect.right(), y});
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
        painter->drawText(
            QPoint{area.left() + used, baseline},
            detail_metrics.elidedText(text.details, Qt::ElideRight, area.width() - used));
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
        // Albums are told apart by their headers, not lines between them.
        paintAlbumHeader(painter, header_rect, option.palette, option.font, {}, false);
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
            }
            painter->restore();
        } else if (!side_artwork && index.column() == title_column) {
            paintAlbumHeader(
                painter, header_rect, option.palette, option.font,
                albumHeaderText(*index.model(), index.row(), album_column, date_column), false);
        }
        item.rect.setTop(item.rect.top() + QueueItemDelegate::album_header_height);
    }

    if (beginsLooseRun(index)) {
        painter->save();
        painter->fillRect(QRect{option.rect.x(), option.rect.y(), option.rect.width(),
                                QueueItemDelegate::loose_run_gap},
                          option.palette.base());
        const auto y = option.rect.y() + QueueItemDelegate::hairline_offset;
        painter->setPen(groupHairline(option.palette));
        painter->drawLine(QPoint{option.rect.left(), y}, QPoint{option.rect.right(), y});
        painter->restore();
        item.rect.setTop(item.rect.top() + QueueItemDelegate::loose_run_gap);
    }
    if (const auto disc = discStart(index); !disc.isEmpty()) {
        // The disc's name where its tracks start, in line with their titles.
        const QRect strip{option.rect.x(), item.rect.top(), option.rect.width(),
                          disc_header_height};
        painter->save();
        painter->fillRect(strip, option.palette.base());
        if (index.column() == title_column) {
            auto font = option.font;
            font.setWeight(QFont::DemiBold);
            font.setPointSizeF(font.pointSizeF() * 0.92);
            painter->setFont(font);
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
            const auto area = strip.adjusted(6, 0, -8, -4);
            painter->drawText(area, Qt::AlignLeft | Qt::AlignBottom,
                              QFontMetrics{font}.elidedText(disc, Qt::ElideRight, area.width()));
        }
        painter->restore();
        item.rect.setTop(item.rect.top() + disc_header_height);
    }
    TrackCellContext context{
        .groups = groupColumns(this),
        .artwork = artwork_column,
        .artist = artist_column,
        .number = configuredColumn(this, track_number_column_property, track_number_column),
        .title = title_column,
        .side_artwork = side_artwork,
        .separate_number = configuredFlag(this, track_separate_number_property),
        .hidden =
            [view](const int column) { return view != nullptr && view->isColumnHidden(column); },
    };
    const auto cell = trackCell(*index.model(), index.row(), index.column(), context);
    const auto current_track = cell.current;
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
        // The row's text takes the accent; a cell with a colour of its own
        // -- the rating's stars -- keeps it.
        if (!index.data(Qt::ForegroundRole).isValid()) {
            const auto accent = item.palette.color(QPalette::Highlight).lighter(115);
            item.palette.setColor(QPalette::Text, accent);
            item.palette.setColor(QPalette::HighlightedText, accent);
        }
        if (cell.playing_icon) {
            item.icon = QIcon::fromTheme(QStringLiteral("media-playback-start"),
                                         QApplication::style()->standardIcon(QStyle::SP_MediaPlay));
            item.decorationSize = QSize{14, 14};
        }
    }
    if (cell.right_aligned) {
        item.displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
    }
    if (cell.quiet) {
        item.palette.setColor(QPalette::Text, item.palette.color(QPalette::PlaceholderText));
    }
    item.text = cell.text;
    const auto inline_artwork = cell.inline_cover;
    const auto& title_suffix = cell.suffix;
    const auto* widget = item.widget;
    auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
    if (title_suffix.isEmpty()) {
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
    } else {
        const auto title = item.text;
        item.text.clear();
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        item.text = title;
        const auto area = item_style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget)
                              .adjusted(2, 0, -2, 0);
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
            std::max(0, std::min({20, item.rect.width() - 4, item.rect.height() - 2}));
        if (extent > 0) {
            const QRect target{item.rect.right() - extent - 4, item.rect.center().y() - extent / 2,
                               extent, extent};
            const auto cover = index.siblingAtColumn(artwork_column)
                                   .data(track_album_artwork_role)
                                   .value<QImage>();
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
                   (beginsAlbum(index)      ? album_header_height
                    : beginsLooseRun(index) ? loose_run_gap
                                            : 0) +
                   (discStart(index).isEmpty() ? 0 : disc_header_height));
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
    return trackGroupRange(*index.model(), index.row(), groupColumns(this));
}

bool QueueItemDelegate::beginsLooseRun(const QModelIndex& index) const {
    return index.isValid() && ui::beginsLooseRun(*index.model(), index.row(), groupColumns(this));
}

bool QueueItemDelegate::beginsAlbum(const QModelIndex& index) const {
    return index.isValid() && beginsTrackGroup(*index.model(), index.row(), groupColumns(this));
}

QString discStart(const QModelIndex& index) {
    return index.isValid() ? trackDiscStart(*index.model(), index.row()) : QString{};
}

} // namespace trackknife::ui
