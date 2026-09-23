// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/library_tree_view.hpp"

#include "uicommon/rating_stars.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QToolTip>

#include <algorithm>
#include <array>
#include <utility>

namespace trackknife::ui {

LibraryTreeView::LibraryTreeView(QWidget* parent) : QTreeView(parent) {
    setMouseTracking(true);
    setUniformRowHeights(false);
    setIconSize(QSize{32, 32});
    setIndentation(18);
    setAnimated(true);
}

void LibraryTreeView::setActionCallback(std::function<void(const QModelIndex&, int)> callback) {
    action_callback_ = std::move(callback);
}

void LibraryTreeView::setActionLabels(std::array<QString, 3> labels) {
    action_labels_ = std::move(labels);
}

void LibraryTreeView::setActionsAvailable(std::function<bool(const QModelIndex&)> available) {
    actions_available_ = std::move(available);
}

bool LibraryTreeView::actionsAvailable(const QModelIndex& index) const {
    return index.isValid() && (!actions_available_ || actions_available_(index));
}

QModelIndex LibraryTreeView::hoverIndex() const { return hover_index_; }

int LibraryTreeView::hoverAction() const noexcept { return hover_action_; }

void LibraryTreeView::completePendingExpansions() {
    std::erase_if(pending_expansions_, [this](const QPersistentModelIndex& pending) {
        if (!pending.isValid() || model()->rowCount(pending) <= 0) {
            return !pending.isValid();
        }
        setExpanded(pending, true);
        return true;
    });
}

void LibraryTreeView::cancelPendingExpansions() { pending_expansions_.clear(); }

QRect LibraryTreeView::actionRect(const QRect& row_rect, const int action) {
    constexpr int action_count = 3;
    constexpr int action_extent = 24;
    constexpr int right_margin = 4;
    const auto left = row_rect.right() + 1 - right_margin - action_count * action_extent;
    return {left + action * action_extent, row_rect.center().y() - action_extent / 2, action_extent,
            action_extent};
}

void LibraryTreeView::mousePressEvent(QMouseEvent* event) {
    pressed_index_ = QPersistentModelIndex{};
    pressed_action_ = -1;
    pressed_expanded_ = false;
    drag_started_ = false;
    if (event->button() == Qt::LeftButton) {
        const auto index = indexAt(event->position().toPoint());
        if (index.isValid()) {
            pressed_index_ = index;
            pressed_position_ = event->position().toPoint();
            pressed_action_ = actionAt(index, pressed_position_);
            pressed_expanded_ = isExpanded(index);
            if (pressed_action_ >= 0) {
                selectionModel()->setCurrentIndex(index, selectionModel()->isSelected(index)
                                                             ? QItemSelectionModel::NoUpdate
                                                             : QItemSelectionModel::ClearAndSelect |
                                                                   QItemSelectionModel::Rows);
                event->accept();
                return;
            }
        }
    }
    // Let QAbstractItemView retain its normal selection and drag threshold
    // bookkeeping. Branch toggling and inline actions are resolved on release.
    QTreeView::mousePressEvent(event);
}

void LibraryTreeView::mouseDoubleClickEvent(QMouseEvent* event) { event->accept(); }

void LibraryTreeView::mouseMoveEvent(QMouseEvent* event) {
    const auto old_index = QModelIndex{hover_index_};
    const auto old_action = hover_action_;
    const auto index = indexAt(event->position().toPoint());
    hover_index_ = index;
    hover_action_ = actionAt(index, event->position().toPoint());
    if (old_index != index || old_action != hover_action_) {
        if (old_index.isValid()) {
            viewport()->update(visualRect(old_index));
        }
        if (index.isValid()) {
            viewport()->update(visualRect(index));
        }
    }
    if (pressed_action_ < 0 || !event->buttons().testFlag(Qt::LeftButton)) {
        QTreeView::mouseMoveEvent(event);
    }
}

void LibraryTreeView::mouseReleaseEvent(QMouseEvent* event) {
    const auto index = indexAt(event->position().toPoint());
    const auto action = actionAt(index, event->position().toPoint());
    const auto same_press = pressed_index_.isValid() && QModelIndex{pressed_index_} == index;
    const auto within_click_distance =
        (pressed_position_ - event->position().toPoint()).manhattanLength() <
        QApplication::startDragDistance();
    if (event->button() == Qt::LeftButton && pressed_action_ >= 0) {
        if (!drag_started_ && same_press && within_click_distance && action == pressed_action_ &&
            action_callback_) {
            action_callback_(index, action);
        }
        event->accept();
        return;
    }
    QTreeView::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton || drag_started_ || !same_press ||
        !within_click_distance) {
        return;
    }
    // QTreeView owns clicks on its disclosure arrow and toggles the branch
    // during its normal mouse handling. Do not toggle the same branch a second
    // time on release; clicks on the remainder of the row still use our larger
    // branch target below.
    if (isExpanded(index) != pressed_expanded_) {
        return;
    }
    if (pressed_action_ < 0 && model()->hasChildren(index)) {
        toggleBranch(index);
        event->accept();
    }
}

void LibraryTreeView::keyPressEvent(QKeyEvent* event) {
    const auto index = currentIndex();
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && index.isValid() &&
        model()->hasChildren(index)) {
        toggleBranch(index);
        event->accept();
        return;
    }
    QTreeView::keyPressEvent(event);
}

void LibraryTreeView::leaveEvent(QEvent* event) {
    const auto old_index = QModelIndex{hover_index_};
    hover_index_ = QPersistentModelIndex{};
    hover_action_ = -1;
    if (old_index.isValid()) {
        viewport()->update(visualRect(old_index));
    }
    QTreeView::leaveEvent(event);
}

bool LibraryTreeView::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const auto index = indexAt(help->pos());
        const auto action = actionAt(index, help->pos());
        if (action >= 0) {
            QToolTip::showText(help->globalPos(), action_labels_[static_cast<std::size_t>(action)],
                               this, actionRect(visualRect(index), action));
            return true;
        }
    }
    return QTreeView::viewportEvent(event);
}

void LibraryTreeView::startDrag(const Qt::DropActions supported_actions) {
    drag_started_ = true;
    QTreeView::startDrag(supported_actions);
}

void LibraryTreeView::toggleBranch(const QModelIndex& index) {
    const auto pending = QPersistentModelIndex{index};
    const auto found = std::ranges::find(pending_expansions_, pending);
    if (found != pending_expansions_.end()) {
        pending_expansions_.erase(found);
        return;
    }
    if (isExpanded(index)) {
        setExpanded(index, false);
        return;
    }
    if (model()->canFetchMore(index)) {
        pending_expansions_.push_back(pending);
        model()->fetchMore(index);
        return;
    }
    setExpanded(index, true);
}

int LibraryTreeView::actionAt(const QModelIndex& index, const QPoint& position) const {
    if (!actionsAvailable(index)) {
        return -1;
    }
    const auto row_rect = visualRect(index);
    for (int action = 0; action < 3; ++action) {
        if (actionRect(row_rect, action).contains(position)) {
            return action;
        }
    }
    return -1;
}

LibraryTreeDelegate::LibraryTreeDelegate(
    LibraryTreeView* view, std::array<QIcon, 3> action_icons,
    std::function<Presentation(const QModelIndex&)> presentation)
    : QStyledItemDelegate(view), view_(view), action_icons_(std::move(action_icons)),
      presentation_(std::move(presentation)) {}

QSize LibraryTreeDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const {
    auto size = QStyledItemDelegate::sizeHint(option, index);
    const auto row = presentation_(index);
    size.setHeight(row.track ? 26 : row.album ? 40 : row.artist ? 30 : row.root ? 34 : 30);
    return size;
}

void LibraryTreeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    auto item = option;
    initStyleOption(&item, index);
    const auto icon = item.icon;
    const auto primary = item.text;
    const auto row = presentation_(index);
    const auto secondary = row.secondary;
    item.text.clear();
    item.icon = {};
    item.features &= ~QStyleOptionViewItem::HasDecoration;
    // Selection as a tint, as in the lists: the text keeps its colour.
    const bool selected = item.state.testFlag(QStyle::State_Selected);
    if (selected) {
        item.state &= ~QStyle::State_Selected;
        const auto base = item.palette.color(QPalette::Base);
        const auto accent = item.palette.color(QPalette::Highlight);
        const auto mix = [](const int a, const int b) { return (a * 68 + b * 32) / 100; };
        item.backgroundBrush = QColor::fromRgb(mix(base.red(), accent.red()),
                                               mix(base.green(), accent.green()),
                                               mix(base.blue(), accent.blue()));
    }
    item.state &= ~QStyle::State_HasFocus;
    const auto* widget = item.widget;
    auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
    item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

    const bool is_track = row.track;
    const bool is_album = row.album;
    const bool show_actions =
        view_ != nullptr && view_->actionsAvailable(index) &&
        (view_->hoverIndex() == index || (view_->hasFocus() && view_->currentIndex() == index));
    const auto icon_extent = is_track ? 16 : is_album ? 30 : 22;
    auto content = item.rect.adjusted(5, 2, -8, -2);
    const auto icon_rect =
        QRect{content.left(), content.center().y() - icon_extent / 2, icon_extent, icon_extent};
    if (row.artist) {
        // No picture of the artist to show: their initials on a quiet tile.
        QString initials;
        for (const auto character : primary) {
            if (character.isLetterOrNumber()) {
                initials += character.toUpper();
                if (initials.size() == 2) {
                    break;
                }
            }
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(item.palette.color(QPalette::Mid));
        painter->drawRoundedRect(icon_rect, 3, 3);
        auto tile_font = item.font;
        tile_font.setPointSizeF(std::max(6.0, item.font.pointSizeF() * 0.72));
        tile_font.setWeight(QFont::DemiBold);
        painter->setFont(tile_font);
        painter->setPen(item.palette.color(QPalette::PlaceholderText));
        painter->drawText(icon_rect, Qt::AlignCenter, initials);
        painter->restore();
    } else if (!icon.isNull()) {
        const auto mode =
            item.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled;
        icon.paint(painter, icon_rect, Qt::AlignCenter, mode);
        if (row.album) {
            paintRatingOverlay(painter, icon_rect, row.album_rating);
        }
    }
    content.setLeft(icon_rect.right() + (is_track ? 6 : 8));
    if (show_actions) {
        content.setRight(LibraryTreeView::actionRect(item.rect, 0).left() - 5);
    }

    const auto foreground = item.palette.color(QPalette::Text);
    auto muted = item.palette.color(QPalette::PlaceholderText);
    muted.setAlpha(selected ? 255 : 220);
    // The count, quiet at the end -- unless the actions have that place.
    if (!row.count.isEmpty() && !show_actions) {
        auto count_font = item.font;
        count_font.setPointSizeF(std::max(7.0, item.font.pointSizeF() - 1.0));
        const QFontMetrics count_metrics{count_font};
        const auto width = count_metrics.horizontalAdvance(row.count);
        painter->save();
        painter->setFont(count_font);
        painter->setPen(muted);
        painter->drawText(QRect{content.right() - width, content.top(), width, content.height()},
                          Qt::AlignRight | Qt::AlignVCenter, row.count);
        painter->restore();
        content.setRight(content.right() - width - 8);
    }
    auto primary_font = item.font;
    auto secondary_font = item.font;
    secondary_font.setPointSizeF(std::max(7.0, item.font.pointSizeF() - 1.0));
    const QFontMetrics primary_metrics{primary_font};
    const QFontMetrics secondary_metrics{secondary_font};
    const auto primary_height = primary_metrics.height();
    const auto secondary_height = secondary.isEmpty() ? 0 : secondary_metrics.height();
    const auto text_height = primary_height + secondary_height;
    const auto text_top = content.center().y() - text_height / 2;

    painter->save();
    painter->setPen(foreground);
    painter->setFont(primary_font);
    painter->drawText(QRect{content.left(), text_top, std::max(0, content.width()), primary_height},
                      Qt::AlignLeft | Qt::AlignVCenter,
                      primary_metrics.elidedText(primary, Qt::ElideRight, content.width()));
    if (!secondary.isEmpty()) {
        painter->setPen(muted);
        painter->setFont(secondary_font);
        painter->drawText(QRect{content.left(), text_top + primary_height,
                                std::max(0, content.width()), secondary_height},
                          Qt::AlignLeft | Qt::AlignVCenter,
                          secondary_metrics.elidedText(secondary, Qt::ElideRight, content.width()));
    }
    painter->restore();

    if (!show_actions) {
        return;
    }
    for (int action = 0; action < 3; ++action) {
        const auto action_rect = LibraryTreeView::actionRect(item.rect, action);
        const bool hovered = view_->hoverIndex() == index && view_->hoverAction() == action;
        if (hovered) {
            auto fill = item.palette.color(QPalette::Highlight);
            fill.setAlpha(item.state.testFlag(QStyle::State_Selected) ? 90 : 42);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(action_rect.adjusted(2, 2, -2, -2), 4.0, 4.0);
            painter->restore();
        }
        const auto action_icon_rect = action_rect.adjusted(6, 6, -6, -6);
        action_icons_[static_cast<std::size_t>(action)].paint(
            painter, action_icon_rect, Qt::AlignCenter,
            item.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled);
    }
}

} // namespace trackknife::ui
