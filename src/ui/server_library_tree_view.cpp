// SPDX-License-Identifier: GPL-3.0-only

#include "ui/server_library_tree_view.hpp"

#include "ui/server_library_tree_model.hpp"

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

static_assert(ServerLibraryTreeDelegate::secondaryTextRole ==
              ServerLibraryTreeModel::SecondaryTextRole);

ServerLibraryTreeView::ServerLibraryTreeView(QWidget* parent) : QTreeView(parent) {
    setMouseTracking(true);
    setUniformRowHeights(false);
    setIconSize(QSize{32, 32});
    setIndentation(18);
    setAnimated(true);
}

void ServerLibraryTreeView::setActionCallback(
    std::function<void(const QModelIndex&, int)> callback) {
    action_callback_ = std::move(callback);
}

void ServerLibraryTreeView::setActionLabels(std::array<QString, 3> labels) {
    action_labels_ = std::move(labels);
}

void ServerLibraryTreeView::setActionsAvailable(std::function<bool(const QModelIndex&)> available) {
    actions_available_ = std::move(available);
}

bool ServerLibraryTreeView::actionsAvailable(const QModelIndex& index) const {
    return index.isValid() && (!actions_available_ || actions_available_(index));
}

QModelIndex ServerLibraryTreeView::hoverIndex() const { return hover_index_; }

int ServerLibraryTreeView::hoverAction() const noexcept { return hover_action_; }

void ServerLibraryTreeView::completePendingExpansions() {
    std::erase_if(pending_expansions_, [this](const QPersistentModelIndex& pending) {
        if (!pending.isValid() || model()->rowCount(pending) <= 0) {
            return !pending.isValid();
        }
        setExpanded(pending, true);
        return true;
    });
}

void ServerLibraryTreeView::cancelPendingExpansions() { pending_expansions_.clear(); }

QRect ServerLibraryTreeView::actionRect(const QRect& row_rect, const int action) {
    constexpr int action_count = 3;
    constexpr int action_extent = 24;
    constexpr int right_margin = 4;
    const auto left = row_rect.right() + 1 - right_margin - action_count * action_extent;
    return {left + action * action_extent, row_rect.center().y() - action_extent / 2, action_extent,
            action_extent};
}

void ServerLibraryTreeView::mousePressEvent(QMouseEvent* event) {
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

void ServerLibraryTreeView::mouseDoubleClickEvent(QMouseEvent* event) { event->accept(); }

void ServerLibraryTreeView::mouseMoveEvent(QMouseEvent* event) {
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

void ServerLibraryTreeView::mouseReleaseEvent(QMouseEvent* event) {
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

void ServerLibraryTreeView::keyPressEvent(QKeyEvent* event) {
    const auto index = currentIndex();
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && index.isValid() &&
        model()->hasChildren(index)) {
        toggleBranch(index);
        event->accept();
        return;
    }
    QTreeView::keyPressEvent(event);
}

void ServerLibraryTreeView::leaveEvent(QEvent* event) {
    const auto old_index = QModelIndex{hover_index_};
    hover_index_ = QPersistentModelIndex{};
    hover_action_ = -1;
    if (old_index.isValid()) {
        viewport()->update(visualRect(old_index));
    }
    QTreeView::leaveEvent(event);
}

bool ServerLibraryTreeView::viewportEvent(QEvent* event) {
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

void ServerLibraryTreeView::startDrag(const Qt::DropActions supported_actions) {
    drag_started_ = true;
    QTreeView::startDrag(supported_actions);
}

void ServerLibraryTreeView::toggleBranch(const QModelIndex& index) {
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

int ServerLibraryTreeView::actionAt(const QModelIndex& index, const QPoint& position) const {
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

ServerLibraryTreeDelegate::ServerLibraryTreeDelegate(
    ServerLibraryTreeView* view, std::array<QIcon, 3> action_icons,
    std::function<Presentation(const QModelIndex&)> presentation)
    : QStyledItemDelegate(view), view_(view), action_icons_(std::move(action_icons)),
      presentation_(std::move(presentation)) {
    if (!presentation_) {
        presentation_ = [](const QModelIndex& index) {
            return Presentation{
                .track = static_cast<ServerLibraryTreeModel::NodeKind>(
                             index.data(ServerLibraryTreeModel::KindRole).toInt()) ==
                         ServerLibraryTreeModel::NodeKind::track,
                .album = index.data(ServerLibraryTreeModel::AlbumRole).toBool(),
                .root = index.data(ServerLibraryTreeModel::LevelRole).toULongLong() == 0U,
                .secondary = index.data(ServerLibraryTreeModel::SecondaryTextRole).toString()};
        };
    }
}

QSize ServerLibraryTreeDelegate::sizeHint(const QStyleOptionViewItem& option,
                                          const QModelIndex& index) const {
    auto size = QStyledItemDelegate::sizeHint(option, index);
    const auto row = presentation_(index);
    size.setHeight(row.track ? 34 : row.album ? 42 : row.root ? 46 : 38);
    return size;
}

void ServerLibraryTreeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
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
    const auto* widget = item.widget;
    auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
    item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

    const bool is_track = row.track;
    const bool is_album = row.album;
    const bool show_actions =
        view_ != nullptr && view_->actionsAvailable(index) &&
        (view_->hoverIndex() == index || (view_->hasFocus() && view_->currentIndex() == index));
    const auto icon_extent = is_track ? 20 : is_album ? 28 : 32;
    auto content = item.rect.adjusted(5, 3, -5, -3);
    const auto icon_rect =
        QRect{content.left(), content.center().y() - icon_extent / 2, icon_extent, icon_extent};
    if (!icon.isNull()) {
        const auto mode =
            item.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled;
        icon.paint(painter, icon_rect, Qt::AlignCenter, mode);
    }
    content.setLeft(icon_rect.right() + (is_track ? 6 : 8));
    if (show_actions) {
        content.setRight(ServerLibraryTreeView::actionRect(item.rect, 0).left() - 5);
    }

    const auto foreground = item.palette.color(
        item.state.testFlag(QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);
    auto muted =
        item.palette.color(item.state.testFlag(QStyle::State_Selected) ? QPalette::HighlightedText
                                                                       : QPalette::PlaceholderText);
    muted.setAlpha(205);
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
        const auto action_rect = ServerLibraryTreeView::actionRect(item.rect, action);
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
