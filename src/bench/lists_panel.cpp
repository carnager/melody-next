// SPDX-License-Identifier: GPL-3.0-only

#include "lists_panel.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QScrollBar>

namespace trackknife::bench {

namespace {

constexpr int id_role = Qt::UserRole;
constexpr int remote_role = Qt::UserRole + 1;
constexpr int other_role = Qt::UserRole + 2;

} // namespace

ListsPanel::ListsPanel(QWidget* parent) : QTreeWidget(parent) {
    setObjectName(QStringLiteral("bench-lists-panel"));
    setAccessibleName(QStringLiteral("Lists"));
    setColumnCount(2);
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(10);
    setUniformRowHeights(true);
    setFrameShape(QFrame::NoFrame);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DropOnly);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
    header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
                if (presenting_ || item == nullptr) {
                    return;
                }
                if (const auto other = item->data(0, other_role); other.isValid()) {
                    if (auto* widget = qobject_cast<QWidget*>(other.value<QObject*>());
                        widget != nullptr) {
                        emit otherChosen(widget);
                    }
                    return;
                }
                if (!idOf(item).isEmpty()) {
                    emit listChosen(remoteOf(item), idOf(item));
                }
            });
}

void ListsPanel::present(const std::vector<Group>& groups, const std::vector<Other>& others,
                         const QString& current_id, const QWidget* current_other) {
    presenting_ = true;
    const auto scrolled = verticalScrollBar()->value();
    clear();
    const auto quiet = palette().color(QPalette::Disabled, QPalette::Text);
    const auto heading = [this](const QString& text) {
        auto* item = new QTreeWidgetItem(this, {text});
        item->setFlags(Qt::ItemIsEnabled);
        auto font = item->font(0);
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 0.85);
        item->setFont(0, font);
        item->setFirstColumnSpanned(true);
        return item;
    };
    QTreeWidgetItem* chosen = nullptr;
    for (const auto& group : groups) {
        auto* parent = heading(group.name);
        for (const auto& list : group.lists) {
            auto* item = new QTreeWidgetItem(
                parent, {list.name + (list.dirty ? QStringLiteral(" *") : QString{}),
                         list.tracks >= 0 ? QString::number(list.tracks) : QString{}});
            item->setData(0, id_role, list.id);
            item->setData(0, remote_role, group.remote);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
            item->setForeground(1, quiet);
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            if (list.playing) {
                item->setIcon(0, QIcon::fromTheme(QStringLiteral("media-playback-start")));
            }
            // A working list is one nobody has named to keep.
            if (!list.saved) {
                auto font = item->font(0);
                font.setItalic(true);
                item->setFont(0, font);
            }
            item->setToolTip(0, QStringLiteral("%1%2%3").arg(
                                    list.saved ? QStringLiteral("Saved list")
                                               : QStringLiteral("Working list, not saved"),
                                    list.open ? QStringLiteral(" · open here") : QString{},
                                    list.dirty ? QStringLiteral(" · modified") : QString{}));
            if (list.id == current_id && current_other == nullptr) {
                chosen = item;
            }
        }
        if (group.lists.empty()) {
            auto* note = new QTreeWidgetItem(
                parent, {group.note.isEmpty() ? QStringLiteral("No lists") : group.note});
            note->setFlags(Qt::NoItemFlags);
            note->setFirstColumnSpanned(true);
        }
        parent->setExpanded(true);
    }
    if (!others.empty()) {
        auto* parent = heading(QStringLiteral("Other tabs"));
        for (const auto& other : others) {
            auto* item = new QTreeWidgetItem(parent, {other.name});
            item->setData(0, other_role,
                          QVariant::fromValue(static_cast<QObject*>(other.widget.data())));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (other.widget == current_other) {
                chosen = item;
            }
        }
        parent->setExpanded(true);
    }
    if (chosen != nullptr) {
        setCurrentItem(chosen);
    }
    verticalScrollBar()->setValue(scrolled);
    presenting_ = false;
}

QString ListsPanel::idOf(const QTreeWidgetItem* item) {
    return item == nullptr ? QString{} : item->data(0, id_role).toString();
}

bool ListsPanel::remoteOf(const QTreeWidgetItem* item) {
    return item != nullptr && item->data(0, remote_role).toBool();
}

QTreeWidgetItem* ListsPanel::itemFor(const QString& id) const {
    for (int group = 0; group < topLevelItemCount(); ++group) {
        auto* parent = topLevelItem(group);
        for (int index = 0; index < parent->childCount(); ++index) {
            if (idOf(parent->child(index)) == id) {
                return parent->child(index);
            }
        }
    }
    return nullptr;
}

bool ListsPanel::offer(QDropEvent* event) {
    auto* item = itemAt(event->position().toPoint());
    if (item == nullptr || idOf(item).isEmpty() || !drop_handler_ ||
        !drop_handler_(event, remoteOf(item), idOf(item))) {
        event->ignore();
        return false;
    }
    return true;
}

void ListsPanel::dragEnterEvent(QDragEnterEvent* event) {
    // Taken for now: whether it lands depends on the row it is moved over.
    event->acceptProposedAction();
}

void ListsPanel::dragMoveEvent(QDragMoveEvent* event) {
    QTreeWidget::dragMoveEvent(event);
    static_cast<void>(offer(event));
}

void ListsPanel::dropEvent(QDropEvent* event) { static_cast<void>(offer(event)); }

} // namespace trackknife::bench
