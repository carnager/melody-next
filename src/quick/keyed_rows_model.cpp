// SPDX-License-Identifier: GPL-3.0-only
#include "keyed_rows_model.hpp"

#include <algorithm>
#include <iterator>
#include <unordered_set>

namespace trackknife::quick {

KeyedRowsModel::KeyedRowsModel(const QList<QByteArray>& roles, QObject* parent)
    : QAbstractListModel(parent), roles_(roles) {}

int KeyedRowsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant KeyedRowsModel::data(const QModelIndex& index, const int role) const {
    const auto at = role - Qt::UserRole;
    if (!index.isValid() || index.row() >= rows_.size() || at < 0 || at >= roles_.size()) {
        return {};
    }
    return rows_.at(index.row()).value(QString::fromLatin1(roles_.at(at)));
}

QHash<int, QByteArray> KeyedRowsModel::roleNames() const {
    QHash<int, QByteArray> names;
    for (qsizetype at = 0; at < roles_.size(); ++at) {
        names.insert(Qt::UserRole + static_cast<int>(at), roles_.at(at));
    }
    return names;
}

void KeyedRowsModel::update(const std::vector<std::uint64_t>& keys,
                            const QList<QVariantMap>& rows) {
    const auto before = count();
    // Gone: last first, so the rows before keep their places.
    const std::unordered_set<std::uint64_t> kept(keys.begin(), keys.end());
    for (auto row = count() - 1; row >= 0; --row) {
        if (!kept.contains(keys_[static_cast<std::size_t>(row)])) {
            beginRemoveRows({}, row, row);
            keys_.erase(keys_.begin() + row);
            rows_.removeAt(row);
            endRemoveRows();
        }
    }
    // Then, place by place: the row there already, one moved up from
    // further down, or a new one.
    for (int row = 0; row < static_cast<int>(keys.size()); ++row) {
        const auto key = keys[static_cast<std::size_t>(row)];
        const auto& values = rows.at(row);
        if (row < count() && keys_[static_cast<std::size_t>(row)] == key) {
            if (rows_.at(row) != values) {
                rows_[row] = values;
                emit dataChanged(index(row), index(row));
            }
            continue;
        }
        const auto found = std::find(keys_.begin() + row, keys_.end(), key);
        if (found != keys_.end()) {
            const auto from = static_cast<int>(std::distance(keys_.begin(), found));
            beginMoveRows({}, from, from, {}, row);
            keys_.erase(found);
            keys_.insert(keys_.begin() + row, key);
            rows_.move(from, row);
            endMoveRows();
            if (rows_.at(row) != values) {
                rows_[row] = values;
                emit dataChanged(index(row), index(row));
            }
            continue;
        }
        beginInsertRows({}, row, row);
        keys_.insert(keys_.begin() + row, key);
        rows_.insert(row, values);
        endInsertRows();
    }
    if (count() != before) {
        emit countChanged();
    }
}

} // namespace trackknife::quick
