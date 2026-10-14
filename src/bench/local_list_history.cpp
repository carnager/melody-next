// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_list_model.hpp"

#include <QDateTime>
#include <QLocale>

namespace trackknife::bench {

QVariant LocalListModel::listeningHistoryData(const QModelIndex& index, const int role) const {
    if (role == Qt::TextAlignmentRole)
        return QVariant::fromValue(Qt::Alignment{
            (index.column() == local_play_count_column ? Qt::AlignRight : Qt::AlignLeft) |
            Qt::AlignVCenter});
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (!row.history_loaded)
        return role == Qt::ToolTipRole ? tr("Asking this list's engine…") : QStringLiteral("…");
    // ADR-0282: lists::history_field_names order -- play count, then the
    // last listen in milliseconds; below zero where the engine cannot say.
    const auto count = row.history ? (*row.history)[0] : -1;
    const auto time = row.history ? (*row.history)[1] : -1;
    if (count < 0)
        return role == Qt::ToolTipRole ? tr("This list's engine keeps no history of the file.")
                                       : QStringLiteral("—");
    if (role == Qt::ToolTipRole)
        return time <= 0 ? tr("Never heard through.")
                         : tr("Heard through %1 times, last on %2")
                               .arg(QLocale{}.toString(static_cast<qlonglong>(count)))
                               .arg(QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(time),
                                                       QLocale::LongFormat));
    if (index.column() == local_play_count_column)
        return QLocale{}.toString(static_cast<qlonglong>(count));
    return time <= 0
               ? tr("Never")
               : QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(time), QLocale::ShortFormat);
}

void LocalListModel::historyShown(const int first, const int last) {
    if (first < 0 || last < first)
        return;
    emit dataChanged(index(first, local_play_count_column), index(last, local_last_played_column),
                     {Qt::DisplayRole, Qt::ToolTipRole});
}

} // namespace trackknife::bench
