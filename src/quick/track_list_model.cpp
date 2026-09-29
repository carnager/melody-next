// SPDX-License-Identifier: GPL-3.0-only

#include "quick/track_list_model.hpp"

#include "quick/cover_provider.hpp"
#include "quick/engine_session.hpp"
#include "quick/format.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/persistence/rating_identity.hpp"

#include <QSet>

#include <algorithm>

#include <filesystem>

namespace trackknife::quick {

using Json = EngineClient::Json;

namespace {

// Where an album's tracks live: two albums of one name in two folders are
// two albums.
[[nodiscard]] std::string folderOf(const QString& encoded_path) {
    const auto raw = protocol::decode_raw_path(encoded_path.toStdString());
    return raw ? std::filesystem::path{*raw}.parent_path().native() : std::string{};
}

// "3", "03" or "3/12".
[[nodiscard]] int leadingNumber(const std::string& text) {
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            break;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

[[nodiscard]] std::string firstField(const Json& fields, const char* name) {
    const auto found = fields.find(name);
    if (found == fields.end() || !found->is_array() || found->empty()) {
        return {};
    }
    const auto& pair = found->front();
    return pair.is_array() && !pair.empty() && pair.front().is_string() ? pair.front().get<std::string>()
                                                                        : std::string{};
}

constexpr std::size_t facts_chunk = 400;

} // namespace

TrackListModel::TrackListModel(QObject* parent) : QAbstractListModel(parent) {}

void TrackListModel::show(EngineSession* session, const QString& id) {
    if (session == session_ && id == list_id_) {
        return;
    }
    disconnect(reconnected_);
    disconnect(rated_);
    session_ = session;
    list_id_ = id;
    if (session_ != nullptr) {
        // The engine restarted, or came back: the list is asked for again.
        reconnected_ = connect(session_, &EngineSession::connectedChanged, this, [this] {
            if (session_->connected()) {
                reload();
            }
        });
        rated_ = connect(session_, &EngineSession::ratingChanged, this,
                         [this](const QString& hash, const int rating) { adoptRating(hash.toStdString(), rating); });
    }
    emit listIdChanged();
    reload();
}

void TrackListModel::reload() {
    const auto generation = ++generation_;
    const auto shown_list = list_id_;
    if (session_ == nullptr || list_id_.isEmpty()) {
        beginResetModel();
        rows_.clear();
        endResetModel();
        emit loaded();
        return;
    }
    loading_ = true;
    emit loadingChanged();
    session_->client().call(QStringLiteral("list.get"), Json{{"id", list_id_.toStdString()}},
                 [this, generation](const auto& answer) {
                     if (generation != generation_) {
                         return;
                     }
                     loading_ = false;
                     emit loadingChanged();
                     revision_ = answer ? answer->value("revision", std::uint64_t{0}) : 0;
                     adoptItems(answer ? answer->value("items", Json::array()) : Json::array(),
                                !rows_.empty() && shown_ == list_id_);
                     shown_ = list_id_;
                 });
}

void TrackListModel::adoptItems(const Json& items, const bool same_list) {
    std::vector<Row> rows;
    rows.reserve(items.size() + items.size() / 8);
    std::string group_key;
    std::size_t header = 0;
    const auto text = [](const Json& item, const char* key) {
        return QString::fromStdString(item.value(key, std::string{}));
    };
    for (const auto& item : items) {
        Row row;
        row.entry = text(item, "entry");
        row.path = text(item, "path");
        row.title = text(item, "title");
        row.artist = text(item, "artist");
        row.album = text(item, "album");
        if (const auto duration = item.find("duration_ms"); duration != item.end() && duration->is_number()) {
            row.duration_ms = duration->get<qint64>();
        }
        auto key = row.album.toStdString() + '\0' + folderOf(row.path);
        if (rows.empty() || key != group_key) {
            group_key = std::move(key);
            header = rows.size();
            rows.push_back(Row{.header = true, .artist = row.artist, .album = row.album});
        }
        auto& head = rows[header];
        if (head.artist != row.artist) {
            head.artist = QStringLiteral("Various artists");
        }
        if (head.path.isEmpty()) {
            head.path = row.path;
        }
        ++head.group_tracks;
        head.group_duration_ms += row.duration_ms;
        row.number = head.group_tracks;
        row.item = item;
        rows.push_back(std::move(row));
    }
    if (same_list) {
        emit refreshing();
    }
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    emit loaded();

    // Track numbers and dates are the library's, not the list's: asked for
    // after the rows show, a chunk at a time.
    const auto generation = generation_;
    Json paths = Json::array();
    for (const auto& row : rows_) {
        if (!row.header) {
            paths.push_back(row.path.toStdString());
        }
        if (paths.size() == facts_chunk) {
            session_->client().call(QStringLiteral("catalogue.cached_tracks"), Json{{"paths", std::move(paths)}},
                         [this, generation](const auto& answer) {
                             if (answer) {
                                 adoptFacts(answer->value("tracks", Json::array()), generation);
                             }
                         });
            paths = Json::array();
        }
    }
    if (!paths.empty()) {
        session_->client().call(QStringLiteral("catalogue.cached_tracks"), Json{{"paths", std::move(paths)}},
                     [this, generation](const auto& answer) {
                         if (answer) {
                             adoptFacts(answer->value("tracks", Json::array()), generation);
                         }
                     });
    }
}

void TrackListModel::adoptFacts(const Json& tracks, const std::uint64_t generation) {
    if (generation != generation_) {
        return;
    }
    // Matched by path; a list may hold one file twice, and both rows take it.
    QHash<QString, const Json*> by_path;
    for (const auto& track : tracks) {
        by_path.insert(QString::fromStdString(track.value("path", std::string{})), &track);
    }
    int header = -1;
    for (int at = 0; at < rowCount(); ++at) {
        auto& row = rows_[static_cast<std::size_t>(at)];
        if (row.header) {
            header = at;
            continue;
        }
        const auto found = by_path.find(row.path);
        if (found == by_path.end()) {
            continue;
        }
        const auto& facts = **found;
        const auto fields = facts.value("fields", Json::object());
        if (const auto number = leadingNumber(firstField(fields, "tracknumber")); number > 0) {
            row.number = number;
        }
        row.date = QString::fromStdString(facts.value("date", std::string{}));
        // The keys ratings are stored by follow the tags, not the file
        // (ADR-0179); worked out as the engine does, from the tags it sent.
        metadata::MetadataDocument document;
        for (const auto& [name, values] : fields.items()) {
            metadata::MetadataField field{.canonical_name = name,
                                          .native_name = name,
                                          .values = {},
                                          .qualifier = {},
                                          .provenance = metadata::FieldProvenance::cached_snapshot};
            for (const auto& pair : values) {
                if (pair.is_array() && !pair.empty() && pair.front().is_string()) {
                    field.values.push_back(pair.front().get<std::string>());
                }
            }
            document.fields.push_back(std::move(field));
        }
        const auto raw_path = protocol::decode_raw_path(row.path.toStdString());
        auto identity = persistence::rating_identity(document, raw_path ? *raw_path : std::string{});
        row.rating_hash = std::move(identity.track_hash);
        row.rating = std::max(0, facts.value("rating", -1));
        if (header >= 0) {
            auto& head = rows_[static_cast<std::size_t>(header)];
            if (head.date.isEmpty()) {
                head.date = row.date;
            }
            if (head.rating_hash.empty()) {
                head.rating_hash = std::move(identity.album_hash);
                head.rating = std::max(0, facts.value("album_rating", -1));
            }
            if (const auto album_artist = firstField(fields, "albumartist"); !album_artist.empty()) {
                head.artist = QString::fromStdString(album_artist);
            }
        }
    }
    if (!rows_.empty()) {
        emit dataChanged(index(0), index(rowCount() - 1));
    }
}

int TrackListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant TrackListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case HeaderRole:
        return row.header;
    case EntryRole:
        return row.entry;
    case PathRole:
        return row.path;
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case AlbumRole:
        return row.album;
    case DateRole:
        return row.date;
    case NumberRole:
        return row.number;
    case DurationRole:
        return static_cast<qreal>(row.duration_ms) / 1000.0;
    case CoverRole:
        return session_ != nullptr ? CoverProvider::forPath(session_->index(), row.path) : QString{};
    case GroupTracksRole:
        return row.group_tracks;
    case GroupDurationRole:
        return static_cast<qreal>(row.group_duration_ms) / 1000.0;
    case RatingRole:
        return row.rating_hash.empty() ? -1 : row.rating;
    default:
        return {};
    }
}

QHash<int, QByteArray> TrackListModel::roleNames() const {
    return {{HeaderRole, "header"},           {EntryRole, "entry"},       {PathRole, "path"},
            {TitleRole, "title"},             {ArtistRole, "artist"},     {AlbumRole, "album"},
            {DateRole, "date"},               {NumberRole, "number"},     {DurationRole, "duration"},
            {CoverRole, "cover"},             {GroupTracksRole, "groupTracks"},
            {GroupDurationRole, "groupDuration"}, {RatingRole, "rating"}};
}

int TrackListModel::trackCount() const {
    return static_cast<int>(std::ranges::count_if(rows_, [](const Row& row) { return !row.header; }));
}

qreal TrackListModel::totalDuration() const {
    qint64 total = 0;
    for (const auto& row : rows_) {
        if (!row.header) {
            total += row.duration_ms;
        }
    }
    return static_cast<qreal>(total) / 1000.0;
}

const TrackListModel::Row* TrackListModel::track(const int row) const {
    if (row < 0 || row >= rowCount()) {
        return nullptr;
    }
    const auto& found = rows_[static_cast<std::size_t>(row)];
    return found.header ? nullptr : &found;
}

void TrackListModel::play(const int row) {
    const auto* found = track(row);
    if (found == nullptr || session_ == nullptr || list_id_.isEmpty()) {
        return;
    }
    session_->client().command(QStringLiteral("list.play"),
                    Json{{"id", list_id_.toStdString()}, {"entry", found->entry.toStdString()}});
}

void TrackListModel::enqueue(const QList<int>& rows) {
    std::vector<QueuedTrack> tracks;
    for (const auto row : rows) {
        if (const auto* found = track(row)) {
            tracks.push_back(QueuedTrack{.entry = found->entry,
                                         .path = found->path,
                                         .title = found->title,
                                         .artist = found->artist,
                                         .album = found->album,
                                         .date = found->date,
                                         .duration_ms = found->duration_ms});
        }
    }
    if (session_ != nullptr) {
        session_->enqueue(tracks);
    }
}

int TrackListModel::rowOfEntry(const QString& entry) const {
    if (entry.isEmpty()) {
        return -1;
    }
    for (std::size_t row = 0; row < rows_.size(); ++row) {
        if (!rows_[row].header && rows_[row].entry == entry) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

QString TrackListModel::describe(const QList<int>& rows) const {
    std::vector<const Row*> picked;
    for (const auto row : rows) {
        if (const auto* found = track(row)) {
            picked.push_back(found);
        }
    }
    if (picked.empty()) {
        const auto count = trackCount();
        return count == 0 ? QStringLiteral("Empty list")
                          : QStringLiteral("%1 tracks · %2").arg(count).arg(formatDuration(totalDuration()));
    }
    if (picked.size() == 1) {
        const auto& row = *picked.front();
        auto line = row.artist + QStringLiteral(" — ") + row.title;
        if (!row.album.isEmpty()) {
            line += QStringLiteral(" · ") + row.album;
        }
        if (!row.date.isEmpty()) {
            line += QStringLiteral(" (") + row.date + QLatin1Char(')');
        }
        return line + QStringLiteral(" · ") + formatDuration(static_cast<qreal>(row.duration_ms) / 1000.0);
    }
    qint64 total = 0;
    for (const auto* row : picked) {
        total += row->duration_ms;
    }
    return QStringLiteral("%1 tracks selected · %2")
        .arg(picked.size())
        .arg(formatDuration(static_cast<qreal>(total) / 1000.0));
}

int TrackListModel::itemIndexAt(const int row) const {
    int index = 0;
    for (int at = 0; at < std::min(row, rowCount()); ++at) {
        if (!rows_[static_cast<std::size_t>(at)].header) {
            ++index;
        }
    }
    return index;
}

std::vector<std::size_t> TrackListModel::itemIndices(const QList<int>& rows) const {
    std::vector<std::size_t> indices;
    for (const auto row : rows) {
        if (track(row) != nullptr) {
            indices.push_back(static_cast<std::size_t>(itemIndexAt(row)));
        }
    }
    std::ranges::sort(indices);
    const auto [first, last] = std::ranges::unique(indices);
    indices.erase(first, last);
    return indices;
}

void TrackListModel::moveRows(const QList<int>& rows, const int before) {
    const auto picked = itemIndices(rows);
    if (session_ == nullptr || picked.empty()) {
        return;
    }
    session_->editList(
        list_id_,
        [picked, before](const Json& items) {
            Json moving = Json::array();
            Json staying = Json::array();
            std::size_t insert_at = 0;
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (std::ranges::binary_search(picked, index)) {
                    moving.push_back(items[index]);
                } else {
                    if (static_cast<int>(index) < before) {
                        ++insert_at;
                    }
                    staying.push_back(items[index]);
                }
            }
            Json result = Json::array();
            for (std::size_t index = 0; index <= staying.size(); ++index) {
                if (index == insert_at) {
                    for (const auto& item : moving) {
                        result.push_back(item);
                    }
                }
                if (index < staying.size()) {
                    result.push_back(staying[index]);
                }
            }
            return result;
        },
        revision_);
}

void TrackListModel::removeRows(const QList<int>& rows) {
    const auto picked = itemIndices(rows);
    if (session_ == nullptr || picked.empty()) {
        return;
    }
    session_->editList(
        list_id_,
        [picked](const Json& items) {
            Json kept = Json::array();
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (!std::ranges::binary_search(picked, index)) {
                    kept.push_back(items[index]);
                }
            }
            return kept;
        },
        revision_);
}

void TrackListModel::copyToList(const QList<int>& rows, const QString& listId) {
    if (session_ == nullptr || listId == list_id_) {
        return;
    }
    Json copies = Json::array();
    for (const auto row : rows) {
        if (const auto* found = track(row)) {
            copies.push_back(EngineSession::copiedItem(found->item));
        }
    }
    if (copies.empty()) {
        return;
    }
    session_->editList(listId, [copies](const Json& items) {
        auto result = items;
        for (const auto& item : copies) {
            result.push_back(item);
        }
        return result;
    });
}

void TrackListModel::rate(const int row, const int rating) {
    if (session_ == nullptr || row < 0 || row >= rowCount()) {
        return;
    }
    const auto& found = rows_[static_cast<std::size_t>(row)];
    if (found.rating_hash.empty()) {
        return;
    }
    session_->setRating(QString::fromStdString(found.rating_hash), found.header, std::clamp(rating, 0, 10));
}

void TrackListModel::adoptRating(const std::string& hash, const int rating) {
    for (int at = 0; at < rowCount(); ++at) {
        auto& row = rows_[static_cast<std::size_t>(at)];
        if (row.rating_hash == hash && row.rating != rating) {
            row.rating = rating;
            emit dataChanged(index(at), index(at), {RatingRole});
        }
    }
}

QList<int> TrackListModel::groupRows(const int headerRow) const {
    QList<int> rows;
    if (headerRow < 0 || headerRow >= rowCount() || !rows_[static_cast<std::size_t>(headerRow)].header) {
        return rows;
    }
    for (int row = headerRow + 1; row < rowCount() && !rows_[static_cast<std::size_t>(row)].header; ++row) {
        rows.push_back(row);
    }
    return rows;
}

} // namespace trackknife::quick
