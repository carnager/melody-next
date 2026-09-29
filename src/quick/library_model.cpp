// SPDX-License-Identifier: GPL-3.0-only

#include "quick/library_model.hpp"

#include "quick/cover_provider.hpp"
#include "quick/engine_session.hpp"
#include "trackknife/core/stable_id.hpp"

#include <algorithm>

namespace trackknife::quick {

using Json = EngineClient::Json;

namespace {

constexpr std::size_t page_size = 200;
constexpr std::size_t search_albums = 100;
constexpr std::size_t search_tracks = 300;
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

void LibraryModel::setQueryMode(const bool on) {
    if (on == query_mode_) {
        return;
    }
    query_mode_ = on;
    emit searchChanged();
    if (!search_.trimmed().isEmpty()) {
        restart();
    }
}

void LibraryModel::setOrder(const int order) {
    const auto clamped = std::clamp(order, 0, 2);
    if (clamped == order_) {
        return;
    }
    order_ = clamped;
    emit searchChanged();
    if (search_.trimmed().isEmpty()) {
        restart();
    }
}

void LibraryModel::refresh() { restart(); }

void LibraryModel::setLoading(const bool loading, const QString& error) {
    loading_ = loading;
    error_ = error;
    emit loadingChanged();
}

void LibraryModel::restart() {
    ++generation_;
    loaded_ = 0;
    more_ = true;
    loading_ = false;
    error_.clear();
    // Before the reset: a view told of it asks for more at once, and that
    // request must be the only one.
    beginResetModel();
    rows_.clear();
    endResetModel();
    if (search_.trimmed().isEmpty() || query_mode_) {
        requestPage();
    } else {
        more_ = false;
        requestSearch();
    }
}

LibraryModel::Row LibraryModel::rowFrom(const Json& entry, const Kind kind) {
    const auto text = [&entry](const char* key) { return QString::fromStdString(entry.value(key, std::string{})); };
    Row row;
    row.kind = kind;
    row.key = text("key");
    row.title = kind == Kind::album ? text("label") : text("title");
    if (row.title.isEmpty()) {
        row.title = text("label");
    }
    row.artist = text("artist");
    row.album = text("album");
    row.date = text("date");
    row.tracks = entry.value("tracks", 0);
    row.number = entry.value("track_number", 0);
    // -1 when unknown -- as an engine from before its query results were
    // whole rows says of every result.
    row.duration_ms = std::max<std::int64_t>(0, entry.value("duration_ms", std::int64_t{0}));
    row.rating = static_cast<int>(entry.value("rating", 0U));
    return row;
}

void LibraryModel::append(std::vector<Row> rows) {
    if (rows.empty()) {
        return;
    }
    const auto first = rowCount();
    beginInsertRows({}, first, first + static_cast<int>(rows.size()) - 1);
    std::ranges::move(rows, std::back_inserter(rows_));
    endInsertRows();
}

// Browsing albums, or a query's tracks: a page at a time.
void LibraryModel::requestPage() {
    if (loading_ || !more_ || !client_.connected()) {
        return;
    }
    setLoading(true);
    const auto generation = generation_;
    const auto query = query_mode_ && !search_.trimmed().isEmpty();
    Json params{{"offset", loaded_}, {"limit", page_size}};
    QString method;
    if (query) {
        method = QStringLiteral("catalogue.filter");
        params["query"] = search_.trimmed().toStdString();
    } else {
        method = QStringLiteral("catalogue.query");
        params["kind"] = album_kind;
        if (order_ == newestFirst) {
            params["newest_first"] = true;
        } else if (order_ == atRandom) {
            params["random"] = true;
        }
    }
    client_.call(method, std::move(params), [this, generation, query](const auto& answer) {
        if (generation != generation_) {
            return;
        }
        if (!answer) {
            more_ = false;
            // A query that does not compile says why, and shows nothing else.
            setLoading(false, QString::fromStdString(answer.error().message));
            return;
        }
        const auto entries = answer->value("entries", Json::array());
        // A random pick is one pick; asking again would pick again.
        more_ = answer->value("more", false) && order_ != atRandom;
        std::vector<Row> rows;
        for (const auto& entry : entries) {
            rows.push_back(rowFrom(entry, query ? Kind::track : Kind::album));
        }
        loaded_ += entries.size();
        append(std::move(rows));
        setLoading(false);
    });
}

// Words: the albums they find, then the tracks.
void LibraryModel::requestSearch() {
    if (!client_.connected()) {
        return;
    }
    setLoading(true);
    const auto generation = generation_;
    const auto words = search_.trimmed().toStdString();
    client_.call(
        QStringLiteral("catalogue.query"), Json{{"kind", album_kind}, {"text", words}, {"limit", search_albums}},
        [this, generation, words](const auto& albums) {
            if (generation != generation_) {
                return;
            }
            std::vector<Row> rows;
            if (albums && !albums->value("entries", Json::array()).empty()) {
                rows.push_back(Row{.kind = Kind::section, .title = tr("Albums")});
                for (const auto& entry : albums->value("entries", Json::array())) {
                    rows.push_back(rowFrom(entry, Kind::album));
                }
            }
            append(std::move(rows));
            client_.call(QStringLiteral("catalogue.query"),
                         Json{{"kind", track_kind}, {"text", words}, {"limit", search_tracks}},
                         [this, generation](const auto& tracks) {
                             if (generation != generation_) {
                                 return;
                             }
                             std::vector<Row> rows;
                             if (tracks && !tracks->value("entries", Json::array()).empty()) {
                                 rows.push_back(Row{.kind = Kind::section, .title = tr("Tracks")});
                                 for (const auto& entry : tracks->value("entries", Json::array())) {
                                     rows.push_back(rowFrom(entry, Kind::track));
                                 }
                             }
                             append(std::move(rows));
                             setLoading(false, tracks ? QString{}
                                                      : QString::fromStdString(tracks.error().message));
                         });
        });
}

bool LibraryModel::canFetchMore(const QModelIndex& parent) const {
    return !parent.isValid() && more_ && !loading_ && (search_.trimmed().isEmpty() || query_mode_);
}

void LibraryModel::fetchMore(const QModelIndex& parent) {
    if (!parent.isValid()) {
        requestPage();
    }
}

int LibraryModel::rowOfAlbum(const QString& key) const {
    for (std::size_t row = 0; row < rows_.size(); ++row) {
        if (rows_[row].kind == Kind::album && rows_[row].key == key) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

void LibraryModel::toggle(const int row) {
    if (row < 0 || row >= rowCount() || rows_[static_cast<std::size_t>(row)].kind != Kind::album) {
        return;
    }
    auto& album = rows_[static_cast<std::size_t>(row)];
    if (album.expanded) {
        album.expanded = false;
        auto end = row + 1;
        while (end < rowCount() && rows_[static_cast<std::size_t>(end)].kind == Kind::child) {
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
    const auto album_title = album.title;
    const auto generation = generation_;
    client_.call(QStringLiteral("catalogue.query"),
                 Json{{"kind", track_kind}, {"album_key", key.toStdString()}, {"limit", 1000}},
                 [this, key, album_title, generation](const auto& answer) {
                     if (generation != generation_ || !answer) {
                         return;
                     }
                     // Found again by key: rows may have moved while this was asked.
                     const auto at = rowOfAlbum(key);
                     if (at < 0 || !rows_[static_cast<std::size_t>(at)].expanded) {
                         return;
                     }
                     std::vector<Row> tracks;
                     for (const auto& entry : answer->value("entries", Json::array())) {
                         auto track = rowFrom(entry, Kind::child);
                         if (track.album.isEmpty()) {
                             track.album = album_title;
                         }
                         tracks.push_back(std::move(track));
                     }
                     if (tracks.empty()) {
                         return;
                     }
                     beginInsertRows({}, at + 1, at + static_cast<int>(tracks.size()));
                     rows_.insert(rows_.begin() + at + 1, tracks.begin(), tracks.end());
                     endInsertRows();
                 });
}

void LibraryModel::resolve(const int row, std::function<void(std::vector<QueuedTrack>)> done) {
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto& picked = rows_[static_cast<std::size_t>(row)];
    const auto toQueued = [](const Row& track) {
        return QueuedTrack{.entry = QString::fromStdString(core::StableId::random().to_string()),
                           .path = track.key,
                           .title = track.title,
                           .artist = track.artist,
                           .album = track.album,
                           .date = track.date,
                           .duration_ms = track.duration_ms};
    };
    if (picked.kind == Kind::section) {
        return;
    }
    if (picked.kind != Kind::album) {
        done({toQueued(picked)});
        return;
    }
    const auto album = picked.title;
    client_.call(QStringLiteral("catalogue.query"),
                 Json{{"kind", track_kind}, {"album_key", picked.key.toStdString()}, {"limit", 1000}},
                 [album, toQueued, done = std::move(done)](const auto& answer) {
                     if (!answer) {
                         return;
                     }
                     std::vector<QueuedTrack> tracks;
                     for (const auto& entry : answer->value("entries", Json::array())) {
                         auto track = rowFrom(entry, Kind::child);
                         if (track.album.isEmpty()) {
                             track.album = album;
                         }
                         tracks.push_back(toQueued(track));
                     }
                     done(std::move(tracks));
                 });
}

void LibraryModel::askPaths(const int row) {
    resolve(row, [this](const std::vector<QueuedTrack>& tracks) {
        QStringList paths;
        for (const auto& track : tracks) {
            paths.push_back(track.path);
        }
        emit pathsReady(paths);
    });
}

void LibraryModel::enqueue(const int row) {
    resolve(row, [this](const std::vector<QueuedTrack>& tracks) { session_.enqueue(tracks); });
}

void LibraryModel::addToList(const int row, const QString& listId, const int position) {
    resolve(row, [this, listId, position](const std::vector<QueuedTrack>& tracks) {
        Json added = Json::array();
        for (const auto& track : tracks) {
            added.push_back(EngineSession::newItem(track));
        }
        session_.editList(listId, [added, position](const Json& items) {
            Json result = Json::array();
            const auto at = position < 0 ? items.size() : std::min<std::size_t>(position, items.size());
            for (std::size_t index = 0; index <= items.size(); ++index) {
                if (index == at) {
                    for (const auto& item : added) {
                        result.push_back(item);
                    }
                }
                if (index < items.size()) {
                    result.push_back(items[index]);
                }
            }
            return result;
        });
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
    case KindRole:
        switch (row.kind) {
        case Kind::album:
            return QStringLiteral("album");
        case Kind::child:
            return QStringLiteral("child");
        case Kind::track:
            return QStringLiteral("track");
        case Kind::section:
            return QStringLiteral("section");
        }
        return {};
    case KeyRole:
        return row.key;
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case AlbumRole:
        return row.album;
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
        return row.kind == Kind::album   ? CoverProvider::forAlbum(session_.index(), row.key)
               : row.kind == Kind::track ? CoverProvider::forPath(session_.index(), row.key)
                                         : QString{};
    case RatingRole:
        return row.rating;
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryModel::roleNames() const {
    return {{KindRole, "kind"},         {KeyRole, "key"},           {TitleRole, "title"},
            {ArtistRole, "artist"},     {AlbumRole, "album"},       {DateRole, "date"},
            {TracksRole, "tracks"},     {NumberRole, "number"},     {DurationRole, "duration"},
            {ExpandedRole, "expanded"}, {CoverRole, "cover"},       {RatingRole, "rating"}};
}

} // namespace trackknife::quick
