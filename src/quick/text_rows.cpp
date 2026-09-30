// SPDX-License-Identifier: GPL-3.0-only

#include "quick/text_rows.hpp"

#include <algorithm>

namespace trackknife::quick {

void TextRows::setRows(const QVariantList& rows) {
    beginResetModel();
    rows_ = rows;
    columns_ = 0;
    for (const auto& row : rows_) {
        columns_ = std::max(columns_, static_cast<int>(row.toStringList().size()));
    }
    endResetModel();
    emit rowsChanged();
}

int TextRows::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int TextRows::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : columns_;
}

QVariant TextRows::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || (role != Qt::DisplayRole && role != Qt::ToolTipRole)) {
        return {};
    }
    const auto cells = rows_.value(index.row()).toStringList();
    return cells.value(index.column());
}

} // namespace trackknife::quick
