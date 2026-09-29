// SPDX-License-Identifier: GPL-3.0-only

#include "quick/up_next_model.hpp"

#include "quick/cover_provider.hpp"

#include <QSet>

namespace trackknife::quick {

using Json = EngineClient::Json;

UpNextModel::UpNextModel(EngineClient& client, const int session, QObject* parent)
    : QAbstractListModel(parent), client_(client), session_(session) {
    connect(&client_, &EngineClient::connectedChanged, this, [this] {
        if (client_.connected()) {
            refresh();
        }
    });
}

void UpNextModel::remember(const QueuedTrack& track) { known_.insert(track.entry, track); }

void UpNextModel::refresh() {
    const auto generation = ++generation_;
    client_.call(QStringLiteral("playback.requests"), Json::object(), [this, generation](const auto& requests) {
        if (generation != generation_ || !requests) {
            return;
        }
        std::vector<QString> entries;
        for (const auto& value : requests->value("entries", Json::array())) {
            if (value.is_string()) {
                entries.push_back(QString::fromStdString(value.template get<std::string>()));
            }
        }
        const auto unknown = std::ranges::any_of(entries, [this](const QString& entry) { return !known_.contains(entry); });
        if (!unknown) {
            adopt(entries);
            return;
        }
        // Requested by another client, or before this window opened: what
        // they are is in the engine's queue.
        client_.call(QStringLiteral("playback.queue"), Json::object(),
                     [this, generation, entries](const auto& queue) {
                         if (generation != generation_) {
                             return;
                         }
                         if (queue) {
                             for (const auto& item : queue->value("entries", Json::array())) {
                                 const auto group = item.value("group", Json::object());
                                 const auto text = [](const Json& object, const char* key) {
                                     return QString::fromStdString(object.value(key, std::string{}));
                                 };
                                 remember(QueuedTrack{.entry = text(item, "entry"),
                                                      .path = text(item, "path"),
                                                      .title = text(item, "title"),
                                                      .artist = text(group, "artist"),
                                                      .album = text(group, "album"),
                                                      .date = text(group, "date"),
                                                      .duration_ms = item.value("duration_ms", std::int64_t{0})});
                             }
                         }
                         adopt(entries);
                     });
    });
}

// Changes rows one at a time where it can, so the view animates what moved
// rather than redrawing the whole list.
void UpNextModel::adopt(const std::vector<QString>& entries) {
    const QSet<QString> wanted(entries.begin(), entries.end());
    for (auto row = rowCount() - 1; row >= 0; --row) {
        if (!wanted.contains(rows_[static_cast<std::size_t>(row)].entry)) {
            beginRemoveRows({}, row, row);
            rows_.erase(rows_.begin() + row);
            endRemoveRows();
        }
    }
    const auto lookup = [this](const QString& entry) {
        auto track = known_.value(entry);
        track.entry = entry;
        if (track.title.isEmpty()) {
            track.title = QStringLiteral("Unknown track");
        }
        return track;
    };
    // What remains must be in the new order for inserting alone to get there.
    std::size_t kept = 0;
    for (const auto& entry : entries) {
        if (kept < rows_.size() && rows_[kept].entry == entry) {
            ++kept;
        }
    }
    if (kept != rows_.size()) {
        beginResetModel();
        rows_.clear();
        for (const auto& entry : entries) {
            rows_.push_back(lookup(entry));
        }
        endResetModel();
        return;
    }
    for (std::size_t at = 0; at < entries.size(); ++at) {
        if (at < rows_.size() && rows_[at].entry == entries[at]) {
            continue;
        }
        beginInsertRows({}, static_cast<int>(at), static_cast<int>(at));
        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(at), lookup(entries[at]));
        endInsertRows();
    }
}

void UpNextModel::sendOrder() {
    Json entries = Json::array();
    for (const auto& row : rows_) {
        entries.push_back(row.entry.toStdString());
    }
    client_.command(QStringLiteral("playback.set_requests"), Json{{"entries", std::move(entries)}});
}

void UpNextModel::remove(const int row) {
    if (row < 0 || row >= rowCount()) {
        return;
    }
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    endRemoveRows();
    sendOrder();
}

void UpNextModel::move(const int from, const int to) {
    if (from < 0 || from >= rowCount() || to < 0 || to >= rowCount() || from == to) {
        return;
    }
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    auto moving = rows_[static_cast<std::size_t>(from)];
    rows_.erase(rows_.begin() + from);
    rows_.insert(rows_.begin() + to, std::move(moving));
    endMoveRows();
    sendOrder();
}

void UpNextModel::clear() {
    if (rows_.empty()) {
        return;
    }
    beginRemoveRows({}, 0, rowCount() - 1);
    rows_.clear();
    endRemoveRows();
    client_.command(QStringLiteral("playback.clear_requests"));
}

int UpNextModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant UpNextModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case EntryRole:
        return row.entry;
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case AlbumRole:
        return row.album;
    case DurationRole:
        return static_cast<qreal>(row.duration_ms) / 1000.0;
    case CoverRole:
        return CoverProvider::forPath(session_, row.path);
    default:
        return {};
    }
}

QHash<int, QByteArray> UpNextModel::roleNames() const {
    return {{EntryRole, "entry"}, {TitleRole, "title"},       {ArtistRole, "artist"},
            {AlbumRole, "album"}, {DurationRole, "duration"}, {CoverRole, "cover"}};
}

} // namespace trackknife::quick
