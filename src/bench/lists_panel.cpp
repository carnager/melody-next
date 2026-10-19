// SPDX-License-Identifier: GPL-3.0-only

#include "lists_panel.hpp"

#include "bench/themed_icon.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QScrollBar>

#include <algorithm>

namespace trackknife::bench {

namespace {

constexpr int id_role = Qt::UserRole;
constexpr int engine_role = Qt::UserRole + 1;
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
                    emit listChosen(engineOf(item), idOf(item));
                }
            });
}

void ListsPanel::present(const std::vector<Group>& groups, const std::vector<Other>& others,
                         const QString& current_id, const QWidget* current_other) {
    // ADR-0298: one engine's lists, and nothing else -- the library's
    // Playlists -- drawn flat, as the library's rows are.
    if (groups.size() == 1U && others.empty()) {
        presentFlat(groups.front(), current_id);
        return;
    }
    setIndentation(10);
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
            item->setData(0, engine_role, group.engine.text());
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

void ListsPanel::presentFlat(const Group& group, const QString& current_id) {
    presenting_ = true;
    const auto scrolled = verticalScrollBar()->value();
    clear();
    setIndentation(0);
    const auto quiet = palette().color(QPalette::PlaceholderText);
    // As high as the library's rows, so the two tabs read alike.
    const auto height = fontMetrics().height() + 12;
    const auto playlist_icon = themedIcon(u"view-media-playlist|audio-x-generic|sp:SP_FileIcon");
    const auto favourite_icon = themedIcon(u"emblem-favorite|starred|sp:SP_DialogYesButton");
    const auto playing_icon = themedIcon(u"media-playback-start|sp:SP_MediaPlay");
    QTreeWidgetItem* chosen = nullptr;
    for (const auto& list : group.lists) {
        auto* item = new QTreeWidgetItem(
            this, {list.name + (list.dirty ? QStringLiteral(" *") : QString{}),
                   list.tracks >= 0 ? QString::number(list.tracks) : QString{}});
        item->setData(0, id_role, list.id);
        item->setData(0, engine_role, group.engine.text());
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
        item->setSizeHint(0, QSize{0, height});
        item->setForeground(1, quiet);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setIcon(0, list.playing ? playing_icon
                         : list.name.compare(QStringLiteral("Favorites"), Qt::CaseInsensitive) == 0
                             ? favourite_icon
                             : playlist_icon);
        if (list.open) {
            auto font = item->font(0);
            font.setWeight(QFont::DemiBold);
            item->setFont(0, font);
        }
        item->setToolTip(0, QStringLiteral("%1%2%3").arg(
                                list.tracks == 1
                                    ? QStringLiteral("1 track")
                                    : QStringLiteral("%1 tracks").arg(std::max(list.tracks, 0)),
                                list.open ? QStringLiteral(" · open") : QString{},
                                list.dirty ? QStringLiteral(" · modified") : QString{}));
        if (list.id == current_id) {
            chosen = item;
        }
    }
    if (group.lists.empty()) {
        auto* note = new QTreeWidgetItem(
            this, {group.note.isEmpty() ? QStringLiteral("No lists") : group.note});
        note->setFlags(Qt::NoItemFlags);
        note->setFirstColumnSpanned(true);
        note->setForeground(0, quiet);
        note->setTextAlignment(0, Qt::AlignCenter);
        note->setSizeHint(0, QSize{0, height * 3});
    }
    if (chosen != nullptr) {
        setCurrentItem(chosen);
    }
    setFilter(filter_);
    verticalScrollBar()->setValue(scrolled);
    presenting_ = false;
}

void ListsPanel::setFilter(const QString& text) {
    filter_ = text.trimmed();
    for (auto* item : findItems(QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive)) {
        if (!idOf(item).isEmpty()) {
            item->setHidden(!filter_.isEmpty() &&
                            !item->text(0).contains(filter_, Qt::CaseInsensitive));
        }
    }
}

QString ListsPanel::idOf(const QTreeWidgetItem* item) {
    return item == nullptr ? QString{} : item->data(0, id_role).toString();
}

EngineKey ListsPanel::engineOf(const QTreeWidgetItem* item) {
    return item != nullptr ? EngineKey::fromText(item->data(0, engine_role).toString())
                           : EngineKey{};
}

QTreeWidgetItem* ListsPanel::itemFor(const QString& id) const {
    for (int top = 0; top < topLevelItemCount(); ++top) {
        auto* item = topLevelItem(top);
        if (idOf(item) == id) {
            return item;
        }
        for (int index = 0; index < item->childCount(); ++index) {
            if (idOf(item->child(index)) == id) {
                return item->child(index);
            }
        }
    }
    return nullptr;
}

bool ListsPanel::offer(QDropEvent* event) {
    auto* item = itemAt(event->position().toPoint());
    if (item == nullptr || idOf(item).isEmpty() || !drop_handler_ ||
        !drop_handler_(event, engineOf(item), idOf(item))) {
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
