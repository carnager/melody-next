// SPDX-License-Identifier: GPL-3.0-only

#include "quick/list_tabs_model.hpp"

#include "bench/bench_main_window_helpers.hpp"

#include <algorithm>
#include <ranges>

namespace trackknife::quick {

ListTabsModel::ListTabsModel(bench::Workspace& workspace, QObject* parent)
    : QAbstractListModel(parent), workspace_(workspace) {}

int ListTabsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(tabs_.size());
}

QVariant ListTabsModel::data(const QModelIndex& index, const int role) const {
    auto* tab = at(index.row());
    if (tab == nullptr) {
        return {};
    }
    const auto chrome = workspace_.tabChrome(*tab);
    switch (role) {
    case Qt::DisplayRole:
    case text_role:
        return chrome.text;
    case Qt::ToolTipRole:
    case tooltip_role:
        return chrome.tooltip;
    case playing_role:
        return chrome.playing;
    case remote_role:
        return chrome.remote;
    case pinned_role:
        return tab->document.pinned;
    case dirty_role:
        return tab->document.dirty;
    case name_role:
        return bench::displayText(tab->document.name);
    case document_role:
        return bench::document_text(tab->document.id);
    case model_role:
        return QVariant::fromValue<QObject*>(tab->model);
    default:
        return {};
    }
}

QHash<int, QByteArray> ListTabsModel::roleNames() const {
    return {{text_role, "text"},         {tooltip_role, "tooltip"},   {playing_role, "playing"},
            {remote_role, "remote"},     {pinned_role, "pinned"},
            {dirty_role, "dirty"},       {name_role, "name"},     {document_role, "document"},
            {model_role, "listModel"}};
}

bench::Workspace::ListTab* ListTabsModel::at(const int row) const {
    return row >= 0 && row < static_cast<int>(tabs_.size()) ? tabs_[static_cast<std::size_t>(row)]
                                                            : nullptr;
}

int ListTabsModel::indexOf(const bench::Workspace::ListTab* tab) const {
    const auto found = std::ranges::find(tabs_, tab);
    return found == tabs_.end() ? -1 : static_cast<int>(found - tabs_.begin());
}

std::size_t ListTabsModel::rankOf(const bench::Workspace::ListTab& tab) const {
    const auto engine = bench::EngineKey::of(tab.document);
    const auto& engines = workspace_.engines_;
    const auto found = std::ranges::find(engines, engine, [](const auto& link) { return link->key; });
    // An engine not reached now sorts after every one that is.
    return found == engines.end() ? engines.size() : static_cast<std::size_t>(found - engines.begin());
}

int ListTabsModel::insert(bench::Workspace::ListTab& tab) {
    const auto rank = rankOf(tab);
    auto at_row = tabs_.size();
    for (std::size_t row = 0; row < tabs_.size(); ++row) {
        if (rankOf(*tabs_[row]) > rank) {
            at_row = row;
            break;
        }
    }
    const auto row = static_cast<int>(at_row);
    beginInsertRows({}, row, row);
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at_row), &tab);
    endInsertRows();
    return row;
}

void ListTabsModel::remove(const int row) {
    if (at(row) == nullptr) {
        return;
    }
    beginRemoveRows({}, row, row);
    tabs_.erase(tabs_.begin() + row);
    endRemoveRows();
}

void ListTabsModel::move(const int from, const int to) {
    if (at(from) == nullptr || at(to) == nullptr || from == to) {
        return;
    }
    // A tab stays among its engine's (ADR-0234): moved into another
    // engine's group, it is not moved.
    if (rankOf(*tabs_[static_cast<std::size_t>(from)]) !=
        rankOf(*tabs_[static_cast<std::size_t>(to)])) {
        return;
    }
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    auto* tab = tabs_[static_cast<std::size_t>(from)];
    tabs_.erase(tabs_.begin() + from);
    tabs_.insert(tabs_.begin() + to, tab);
    endMoveRows();
}

void ListTabsModel::refresh(const bench::Workspace::ListTab& tab) {
    const auto row = indexOf(&tab);
    if (row >= 0) {
        emit dataChanged(index(row), index(row));
    }
}

} // namespace trackknife::quick
