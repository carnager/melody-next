// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QVariantMap>

#include <cstdint>
#include <vector>

namespace trackknife::quick {

// Rows known by a lasting key, each a map of named values. Given the rows
// as they now stand, it reports what became of each -- removed, moved,
// inserted, changed -- rather than starting over, so a view keeps its
// delegates and can animate each row's coming, going and moving.
class KeyedRowsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    // `roles`: the names each row's map is read by, in role order.
    explicit KeyedRowsModel(const QList<QByteArray>& roles, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const { return static_cast<int>(keys_.size()); }

    // `keys` and `rows` in step, keys unique.
    void update(const std::vector<std::uint64_t>& keys, const QList<QVariantMap>& rows);

  signals:
    void countChanged();

  private:
    QList<QByteArray> roles_;
    std::vector<std::uint64_t> keys_;
    QList<QVariantMap> rows_;
};

} // namespace trackknife::quick
