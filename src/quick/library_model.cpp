// SPDX-License-Identifier: GPL-3.0-only

#include "quick/library_model.hpp"

#include "quick/cover_provider.hpp"
#include "quick/engine_session.hpp"
#include "trackknife/core/stable_id.hpp"

namespace trackknife::quick {

using Json = EngineClient::Json;

namespace {

constexpr std::size_t page_size = 200;
constexpr int album_kind = 1;
constexpr int track_kind = 2;

} // namespace

LibraryModel::LibraryModel(EngineClient& client, EngineSession& session, QObject* parent)
    : QAbstractListModel(parent), client_(client), session_(session) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(200);
    connect(&debounce_, &QTimer::timeout, this, &LibraryModel::restart);
    connect(&client_, &EngineClient::connectedChanged, this, [this] {
        if (client_.connected()) {
            restart();
        }
    });
}

void LibraryModel::setSearch(const QString& text) {
    if (text == search_) {
        return;
    }
    search_ = text;
    emit searchChanged();
    debounce_.start();
}

void LibraryModel::restart() {
    ++generation_;
    albums_loaded_ = 0;
    more_ = true;
    loading_ = false;
    // Before the reset: a view told of it asks for more at once, and that
    // request must be the only one.
    beginResetModel();
    rows_.clear();
    endResetModel();
    requestPage();
}

LibraryModel::Row LibraryModel::rowFrom(const Json& entry) {
    const auto text = [&entry](const char* key) { return QString::fromStdString(entry.value(key, std::string{})); };
    Row row;
    row.track = entry.value("kind", 0) == track_kind;
    row.key = text("key");
    row.title = row.track ? text("title") : text("label");
    if (row.title.isEmpty()) {
        row.title = text("label");
    }
    row.artist = text("artist");
    row.date = text("date");
    row.tracks = entry.value("tracks", 0);
    row.number = entry.value("track_number", 0);
    row.duration_ms = entry.value("duration_ms", std::int64_t{0});
    return row;
}

void LibraryModel::requestPage() {
    if (loading_ || !more_ || !client_.connected()) {
        return;
    }
    loading_ = true;
    emit loadingChanged();
    const auto generation = generation_;
    Json params{{"kind", album_kind}, {"offset", albums_loaded_}, {"limit", page_size}};
    if (!search_.trimmed().isEmpty()) {
        params["text"] = search_.trimmed().toStdString();
    }
    client_.call(QStringLiteral("catalogue.query"), std::move(params), [this, generation](const auto& answer) {
        if (generation != generation_) {
            return;
        }
        loading_ = false;
        emit loadingChanged();
        if (!answer) {
            more_ = false;
            return;
        }
        const auto entries = answer->value("entries", Json::array());
        more_ = answer->value("more", false);
        if (entries.empty()) {
            return;
        }
        const auto first = rowCount();
        beginInsertRows({}, first, first + static_cast<int>(entries.size()) - 1);
        for (const auto& entry : entries) {
            rows_.push_back(rowFrom(entry));
        }
        endInsertRows();
        albums_loaded_ += entries.size();
    });
}

bool LibraryModel::canFetchMore(const QModelIndex& parent) const {
    return !parent.isValid() && more_ && !loading_;
}

void LibraryModel::fetchMore(const QModelIndex& parent) {
    if (!parent.isValid()) {
        requestPage();
    }
}

int LibraryModel::rowOfAlbum(const QString& key) const {
    for (std::size_t row = 0; row < rows_.size(); ++row) {
        if (!rows_[row].track && rows_[row].key == key) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

void LibraryModel::toggle(const int row) {
    if (row < 0 || row >= rowCount() || rows_[static_cast<std::size_t>(row)].track) {
        return;
    }
    auto& album = rows_[static_cast<std::size_t>(row)];
    if (album.expanded) {
        album.expanded = false;
        auto end = row + 1;
        while (end < rowCount() && rows_[static_cast<std::size_t>(end)].track) {
            ++end;
        }
        if (end > row + 1) {
            beginRemoveRows({}, row + 1, end - 1);
            rows_.erase(rows_.begin() + row + 1, rows_.begin() + end);
            endRemoveRows();
        }
        emit dataChanged(index(row), index(row));
        return;
    }
    album.expanded = true;
    emit dataChanged(index(row), index(row));
    const auto key = album.key;
    const auto generation = generation_;
    client_.call(QStringLiteral("catalogue.query"),
                 Json{{"kind", track_kind}, {"album_key", key.toStdString()}, {"limit", 1000}},
                 [this, key, generation](const auto& answer) {
                     if (generation != generation_ || !answer) {
                         return;
                     }
                     // Found again by key: rows may have moved while this was asked.
                     const auto at = rowOfAlbum(key);
                     if (at < 0 || !rows_[static_cast<std::size_t>(at)].expanded) {
                         return;
                     }
                     const auto entries = answer->value("entries", Json::array());
                     if (entries.empty()) {
                         return;
                     }
                     beginInsertRows({}, at + 1, at + static_cast<int>(entries.size()));
                     std::vector<Row> tracks;
                     for (const auto& entry : entries) {
                         tracks.push_back(rowFrom(entry));
                     }
                     rows_.insert(rows_.begin() + at + 1, tracks.begin(), tracks.end());
                     endInsertRows();
                 });
}

void LibraryModel::enqueue(const int row) {
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto& picked = rows_[static_cast<std::size_t>(row)];
    const auto toQueued = [](const Row& track, const QString& album) {
        return QueuedTrack{.entry = QString::fromStdString(core::StableId::random().to_string()),
                           .path = track.key,
                           .title = track.title,
                           .artist = track.artist,
                           .album = album,
                           .date = track.date,
                           .duration_ms = track.duration_ms};
    };
    if (picked.track) {
        QString album;
        for (auto at = row; at >= 0; --at) {
            if (!rows_[static_cast<std::size_t>(at)].track) {
                album = rows_[static_cast<std::size_t>(at)].title;
                break;
            }
        }
        session_.enqueue(std::vector<QueuedTrack>{toQueued(picked, album)});
        return;
    }
    const auto album = picked.title;
    client_.call(QStringLiteral("catalogue.query"),
                 Json{{"kind", track_kind}, {"album_key", picked.key.toStdString()}, {"limit", 1000}},
                 [this, album, toQueued](const auto& answer) {
                     if (!answer) {
                         return;
                     }
                     std::vector<QueuedTrack> tracks;
                     for (const auto& entry : answer->value("entries", Json::array())) {
                         tracks.push_back(toQueued(rowFrom(entry), album));
                     }
                     session_.enqueue(tracks);
                 });
}

int LibraryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant LibraryModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case TrackRole:
        return row.track;
    case KeyRole:
        return row.key;
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case DateRole:
        return row.date;
    case TracksRole:
        return row.tracks;
    case NumberRole:
        return row.number;
    case DurationRole:
        return static_cast<qreal>(row.duration_ms) / 1000.0;
    case ExpandedRole:
        return row.expanded;
    case CoverRole:
        return row.track ? QString{} : CoverProvider::forAlbum(session_.index(), row.key);
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryModel::roleNames() const {
    return {{TrackRole, "track"},   {KeyRole, "key"},         {TitleRole, "title"},
            {ArtistRole, "artist"}, {DateRole, "date"},       {TracksRole, "tracks"},
            {NumberRole, "number"}, {DurationRole, "duration"}, {ExpandedRole, "expanded"},
            {CoverRole, "cover"}};
}

} // namespace trackknife::quick
