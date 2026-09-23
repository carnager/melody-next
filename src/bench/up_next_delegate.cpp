// SPDX-License-Identifier: GPL-3.0-only

#include "bench/up_next_delegate.hpp"

#include "uicommon/track_row_roles.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace trackknife::bench {
namespace {

[[nodiscard]] QColor mixed(const QColor& from, const QColor& to, const int percent) {
    const auto mix = [percent](const int a, const int b) {
        return (a * (100 - percent) + b * percent) / 100;
    };
    return QColor::fromRgb(mix(from.red(), to.red()), mix(from.green(), to.green()),
                           mix(from.blue(), to.blue()));
}

} // namespace

UpNextDelegate::UpNextDelegate(const int artist_column, const int length_column, QObject* parent)
    : QStyledItemDelegate(parent), artist_column_(artist_column), length_column_(length_column) {}

QSize UpNextDelegate::sizeHint(const QStyleOptionViewItem& option,
                               const QModelIndex& index) const {
    auto size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(row_height);
    return size;
}

void UpNextDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const {
    const auto& palette = option.palette;
    const auto base = palette.color(QPalette::Base);
    painter->save();
    // Selection as a tint, as in the lists.
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    painter->fillRect(option.rect,
                      selected ? mixed(base, palette.color(QPalette::Highlight), 32) : base);

    const auto area = option.rect.adjusted(8, 4, -8, -4);
    const auto extent = std::min(32, area.height());
    const QRect cover_rect{area.left(), area.center().y() - extent / 2, extent, extent};
    const auto cover = index.data(ui::track_album_artwork_role).value<QImage>();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    if (!cover.isNull()) {
        QPainterPath rounded;
        rounded.addRoundedRect(cover_rect, 2, 2);
        painter->save();
        painter->setClipPath(rounded, Qt::IntersectClip);
        painter->drawImage(cover_rect, cover);
        painter->restore();
    } else {
        painter->setPen(Qt::NoPen);
        painter->setBrush(mixed(base, palette.color(QPalette::Text), 10));
        painter->drawRoundedRect(cover_rect, 2, 2);
    }

    const auto length = index.siblingAtColumn(length_column_).data().toString();
    auto small = option.font;
    small.setPointSizeF(std::max(7.0, option.font.pointSizeF() - 1.0));
    const QFontMetrics small_metrics{small};
    const auto length_width = small_metrics.horizontalAdvance(length);
    const QRect text{cover_rect.right() + 10, area.top(),
                     std::max(0, area.right() - cover_rect.right() - 10 - length_width - 8),
                     area.height()};
    const QFontMetrics title_metrics{option.font};
    const auto title = index.data().toString();
    const auto artist = index.siblingAtColumn(artist_column_).data().toString();
    const auto block = title_metrics.height() + (artist.isEmpty() ? 0 : small_metrics.height());
    const auto top = text.center().y() - block / 2;
    painter->setPen(palette.color(QPalette::Text));
    painter->setFont(option.font);
    painter->drawText(QRect{text.left(), top, text.width(), title_metrics.height()},
                      Qt::AlignLeft | Qt::AlignVCenter,
                      title_metrics.elidedText(title, Qt::ElideRight, text.width()));
    painter->setFont(small);
    painter->setPen(palette.color(QPalette::PlaceholderText));
    if (!artist.isEmpty()) {
        painter->drawText(
            QRect{text.left(), top + title_metrics.height(), text.width(), small_metrics.height()},
            Qt::AlignLeft | Qt::AlignVCenter,
            small_metrics.elidedText(artist, Qt::ElideRight, text.width()));
    }
    painter->drawText(QRect{area.right() - length_width, area.top(), length_width, area.height()},
                      Qt::AlignRight | Qt::AlignVCenter, length);
    painter->restore();
}

} // namespace trackknife::bench
