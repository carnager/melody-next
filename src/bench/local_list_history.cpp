// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_list_model.hpp"
#include "uicommon/list_persistence_service.hpp"

#include <QDateTime>
#include <QLocale>
#include <QTimer>

#include <algorithm>

namespace trackknife::bench {

void LocalListModel::setListeningHistoryService(ui::ListPersistenceService* service) {
    if (listening_service_ == service)
        return;
    if (listening_service_)
        disconnect(listening_service_, nullptr, this, nullptr);
    listening_service_ = service;
    listening_busy_ = false;
    if (service)
        connect(service, &ui::ListPersistenceService::listeningHistoryChanged, this,
                &LocalListModel::invalidateListeningHistory);
    invalidateListeningHistory();
}

void LocalListModel::invalidateListeningHistory() {
    ++listening_generation_;
    const auto cached = listening_cache_.keys();
    listening_cache_.clear();
    listening_pending_.clear();
    listening_overflow_ = false;
    listening_timer_->stop();
    if (!cached.empty() && rowCount() > 0) {
        const auto [first, last] = std::minmax_element(cached.begin(), cached.end());
        emit dataChanged(index(std::min(*first, rowCount() - 1), local_play_count_column),
                         index(std::min(*last, rowCount() - 1), local_last_played_column),
                         {Qt::DisplayRole, Qt::ToolTipRole});
    }
}

QVariant LocalListModel::listeningHistoryData(const QModelIndex& index, const int role) const {
    if (role == Qt::TextAlignmentRole)
        return QVariant::fromValue(Qt::Alignment{
            (index.column() == local_play_count_column ? Qt::AlignRight : Qt::AlignLeft) |
            Qt::AlignVCenter});
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (!listening_service_)
        return role == Qt::ToolTipRole ? tr("Listening history service is unavailable.")
                                       : QStringLiteral("—");
    if (!row.source_revision || row.source_revision->inode == 0)
        return role == Qt::ToolTipRole
                   ? tr("Listening history is unavailable until a source revision is known.")
                   : QStringLiteral("—");
    auto* cell = listening_cache_.object(index.row());
    if (!cell && listening_pending_.size() < 64) {
        cell = new ListeningCell;
        listening_cache_.insert(index.row(), cell);
        listening_pending_.insert(index.row());
        if (!listening_busy_ && !listening_timer_->isActive())
            listening_timer_->start(0);
    } else if (!cell) {
        listening_overflow_ = true;
    }
    if (!cell || !cell->loaded)
        return role == Qt::ToolTipRole ? tr("Loading listening history…") : QStringLiteral("…");
    if (!cell->error.isEmpty())
        return role == Qt::ToolTipRole ? cell->error : QStringLiteral("—");
    const auto count = cell->history ? cell->history->play_count : 0;
    const auto time = cell->history ? cell->history->last_played_ms : 0;
    if (role == Qt::ToolTipRole) {
        if (time == 0)
            return tr("No qualified local listens recorded.");
        return tr("Local play count: %1. Last qualified listen: %2")
            .arg(QLocale{}.toString(static_cast<qulonglong>(count)))
            .arg(QDateTime::fromMSecsSinceEpoch(time).toString(Qt::ISODate));
    }
    if (index.column() == local_play_count_column)
        return QLocale{}.toString(static_cast<qulonglong>(count));
    return time == 0
               ? tr("Never")
               : QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(time), QLocale::ShortFormat);
}

void LocalListModel::dispatchListeningHistory() {
    if (listening_busy_ || !listening_service_ || listening_pending_.empty())
        return;
    const auto requested = listening_pending_.values();
    listening_pending_.clear();
    std::vector<persistence::ListItem> sources;
    sources.reserve(static_cast<std::size_t>(requested.size()));
    for (const auto row_index : requested) {
        const auto& row = rows_[static_cast<std::size_t>(row_index)];
        persistence::ListItem source;
        source.source = persistence::ListSource::local;
        source.source_reference = row.raw_path;
        source.source_revision = row.source_revision;
        source.source_selection = persistence::ListItemSourceSelection{row.selection.stream_index,
                                                                       row.selection.subsong_index};
        if (row.segment)
            source.segment =
                persistence::ListItemSegment{row.segment->start_sample, row.segment->end_sample};
        sources.push_back(std::move(source));
    }
    listening_busy_ = true;
    const QPointer self{this};
    listening_service_->loadListeningHistory(
        std::move(sources),
        [self, requested, service = listening_service_,
         generation = listening_generation_](auto results, const QString& error) {
            if (!self || self->listening_service_ != service)
                return;
            self->listening_busy_ = false;
            if (generation == self->listening_generation_) {
                auto reply_error = error;
                if (reply_error.isEmpty() &&
                    results.size() != static_cast<std::size_t>(requested.size()))
                    reply_error = tr("Incomplete listening-history reply.");
                for (qsizetype n = 0; n < requested.size(); ++n) {
                    if (auto* cell = self->listening_cache_.object(requested[n])) {
                        cell->loaded = true;
                        cell->error = reply_error;
                        if (error.isEmpty() && static_cast<std::size_t>(n) < results.size())
                            cell->history = std::move(results[static_cast<std::size_t>(n)]);
                    }
                }
                const auto [first, last] = std::minmax_element(requested.begin(), requested.end());
                // A tall viewport may display more rows than one batch admits.
                // Repaint its history cells so overflow rows can request the next
                // batch, without scanning rows or invalidating album geometry.
                const bool overflow = self->listening_overflow_;
                self->listening_overflow_ = false;
                emit self->dataChanged(
                    self->index(overflow ? 0 : *first, local_play_count_column),
                    self->index(overflow ? self->rowCount() - 1 : *last, local_last_played_column),
                    {Qt::DisplayRole, Qt::ToolTipRole});
            }
            if (!self->listening_pending_.empty())
                self->listening_timer_->start(0);
        });
}

} // namespace trackknife::bench
