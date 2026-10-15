// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/library_tree_model.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace trackknife::bench {

LibraryTreeModel::Row* LibraryTreeModel::Row::child(const int position) const {
    return position >= 0 && position < count() ? children_[static_cast<std::size_t>(position)].get()
                                               : nullptr;
}

LibraryTreeModel::LibraryTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

LibraryTreeModel::Row* LibraryTreeModel::rowAt(const QModelIndex& index) const {
    if (!index.isValid()) {
        return const_cast<Row*>(&root_);
    }
    return index.model() == this ? static_cast<Row*>(index.internalPointer()) : nullptr;
}

QModelIndex LibraryTreeModel::indexOf(const Row& row) const {
    if (&row == &root_ || row.parent_ == nullptr) {
        return {};
    }
    return createIndex(row.position_, 0, const_cast<Row*>(&row));
}

void LibraryTreeModel::renumber(Row& parent, const int from) {
    for (auto position = static_cast<std::size_t>(std::max(from, 0));
         position < parent.children_.size(); ++position) {
        parent.children_[position]->position_ = static_cast<int>(position);
    }
}

void LibraryTreeModel::clear() {
    beginResetModel();
    root_.children_.clear();
    endResetModel();
}

LibraryTreeModel::Row* LibraryTreeModel::append(Row& parent,
                                                std::vector<std::unique_ptr<Row>> rows) {
    if (rows.empty()) {
        return nullptr;
    }
    const auto first = parent.count();
    beginInsertRows(indexOf(parent), first, first + static_cast<int>(rows.size()) - 1);
    for (auto& row : rows) {
        row->parent_ = &parent;
        parent.children_.push_back(std::move(row));
    }
    renumber(parent, first);
    endInsertRows();
    return parent.child(first);
}

LibraryTreeModel::Row& LibraryTreeModel::append(Row& parent, std::unique_ptr<Row> row) {
    std::vector<std::unique_ptr<Row>> rows;
    rows.push_back(std::move(row));
    return *append(parent, std::move(rows));
}

LibraryTreeModel::Row& LibraryTreeModel::insert(Row& parent, int position,
                                                std::unique_ptr<Row> row) {
    position = std::clamp(position, 0, parent.count());
    beginInsertRows(indexOf(parent), position, position);
    row->parent_ = &parent;
    auto& inserted =
        **parent.children_.insert(std::next(parent.children_.begin(), position), std::move(row));
    renumber(parent, position);
    endInsertRows();
    return inserted;
}

void LibraryTreeModel::move(Row& parent, const int from, const int to) {
    if (from == to || from < 0 || to < 0 || from >= parent.count() || to >= parent.count()) {
        return;
    }
    const auto at = indexOf(parent);
    // Qt names the place before which the row goes, counted before the move.
    if (!beginMoveRows(at, from, from, at, to > from ? to + 1 : to)) {
        return;
    }
    auto taken = std::move(parent.children_[static_cast<std::size_t>(from)]);
    parent.children_.erase(std::next(parent.children_.begin(), from));
    parent.children_.insert(std::next(parent.children_.begin(), to), std::move(taken));
    renumber(parent, std::min(from, to));
    endMoveRows();
}

void LibraryTreeModel::remove(Row& parent, const int first, int count) {
    count = std::min(count, parent.count() - first);
    if (first < 0 || count <= 0) {
        return;
    }
    beginRemoveRows(indexOf(parent), first, first + count - 1);
    parent.children_.erase(std::next(parent.children_.begin(), first),
                           std::next(parent.children_.begin(), first + count));
    renumber(parent, first);
    endRemoveRows();
}

void LibraryTreeModel::changed(const Row& row) {
    const auto at = indexOf(row);
    if (at.isValid()) {
        emit dataChanged(at, at);
    }
}

QModelIndex LibraryTreeModel::index(const int row, const int column,
                                    const QModelIndex& parent) const {
    const auto* above = rowAt(parent);
    if (above == nullptr || column != 0) {
        return {};
    }
    auto* found = above->child(row);
    return found == nullptr ? QModelIndex{} : createIndex(row, 0, found);
}

QModelIndex LibraryTreeModel::parent(const QModelIndex& child) const {
    const auto* row = child.isValid() ? rowAt(child) : nullptr;
    return row == nullptr || row->parent_ == nullptr ? QModelIndex{} : indexOf(*row->parent_);
}

int LibraryTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    const auto* row = rowAt(parent);
    return row == nullptr ? 0 : row->count();
}

int LibraryTreeModel::columnCount(const QModelIndex&) const { return 1; }

Qt::ItemFlags LibraryTreeModel::flags(const QModelIndex& index) const {
    const auto* row = index.isValid() ? rowAt(index) : nullptr;
    if (row == nullptr) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags flags = Qt::ItemIsSelectable;
    if (row->enabled) {
        flags |= Qt::ItemIsEnabled;
    }
    if (row->draggable) {
        flags |= Qt::ItemIsDragEnabled;
    }
    return flags;
}

bool LibraryTreeModel::hasChildren(const QModelIndex& parent) const {
    const auto* row = rowAt(parent);
    return row != nullptr && (row->count() > 0 || canFetchMore(parent));
}

bool LibraryTreeModel::canFetchMore(const QModelIndex& parent) const {
    const auto* row = parent.isValid() ? rowAt(parent) : nullptr;
    return row != nullptr && row->query.has_value() && !row->loaded;
}

} // namespace trackknife::bench
