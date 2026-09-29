// SPDX-License-Identifier: GPL-3.0-only

#include "quick/lists_model.hpp"

namespace trackknife::quick {

using Json = EngineClient::Json;

ListsModel::ListsModel(EngineClient& client, QObject* parent)
    : QAbstractListModel(parent), client_(client) {
    // A save touches a list several times in a row; one listing covers them.
    debounce_.setSingleShot(true);
    debounce_.setInterval(150);
    connect(&debounce_, &QTimer::timeout, this, &ListsModel::refresh);
    client_.onEvent([this](const std::string& name, const Json& data) {
        if (name != "list.changed") {
            return;
        }
        debounce_.start();
        emit listChanged(QString::fromStdString(data.value("id", std::string{})));
    });
    connect(&client_, &EngineClient::connectedChanged, this, [this] {
        if (client_.connected()) {
            refresh();
        }
    });
}

void ListsModel::refresh() {
    client_.call(QStringLiteral("list.all"), Json::object(), [this](const auto& answer) {
        if (!answer) {
            return;
        }
        std::vector<Summary> lists;
        for (const auto& summary : answer->value("lists", Json::array())) {
            lists.push_back(Summary{
                .id = QString::fromStdString(summary.value("id", std::string{})),
                .name = QString::fromStdString(summary.value("name", std::string{})),
                .saved = summary.value("kind", std::string{}) == "saved",
                .tracks = summary.value("tracks", 0),
                .modified_ms = summary.value("modified_ms", qint64{0}),
            });
        }
        beginResetModel();
        lists_ = std::move(lists);
        endResetModel();
        emit loaded();
    });
}

int ListsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(lists_.size());
}

QVariant ListsModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    const auto& list = lists_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole:
        return list.id;
    case NameRole:
        return list.name;
    case KindRole:
        return list.saved ? QStringLiteral("saved") : QStringLiteral("working");
    case SavedRole:
        return list.saved;
    case TracksRole:
        return list.tracks;
    case ModifiedRole:
        return list.modified_ms;
    default:
        return {};
    }
}

QHash<int, QByteArray> ListsModel::roleNames() const {
    return {{IdRole, "listId"}, {NameRole, "name"}, {KindRole, "kind"}, {TracksRole, "tracks"}, {SavedRole, "saved"},
            {ModifiedRole, "modified"}};
}

int ListsModel::indexOf(const QString& id) const {
    for (std::size_t row = 0; row < lists_.size(); ++row) {
        if (lists_[row].id == id) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

QString ListsModel::idAt(const int row) const {
    return row >= 0 && row < rowCount() ? lists_[static_cast<std::size_t>(row)].id : QString{};
}

QString ListsModel::nameAt(const int row) const {
    return row >= 0 && row < rowCount() ? lists_[static_cast<std::size_t>(row)].name : QString{};
}

QString ListsModel::nameOf(const QString& id) const { return nameAt(indexOf(id)); }

bool ListsModel::isSaved(const QString& id) const {
    const auto row = indexOf(id);
    return row >= 0 && lists_[static_cast<std::size_t>(row)].saved;
}

QString ListsModel::newest() const {
    const Summary* best = nullptr;
    for (const auto& list : lists_) {
        const auto better = best == nullptr || (best->saved && !list.saved) ||
                            (best->saved == list.saved && list.modified_ms > best->modified_ms);
        if (better) {
            best = &list;
        }
    }
    return best != nullptr ? best->id : QString{};
}

} // namespace trackknife::quick
