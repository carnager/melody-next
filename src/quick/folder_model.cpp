// SPDX-License-Identifier: GPL-3.0-only

#include "quick/folder_model.hpp"

#include "quick/cover_provider.hpp"
#include "quick/engine_session.hpp"
#include "trackknife/core/stable_id.hpp"

namespace trackknife::quick {

using Json = EngineClient::Json;

namespace {

// Enough for any album folder, and a whole library's worth in a few pages.
constexpr std::size_t inventory_page = 5000;
// A folder dropped into a list is a deliberate act, but not an unbounded one.
constexpr std::size_t folder_limit = 20000;
constexpr std::size_t facts_chunk = 400;

} // namespace

FolderModel::FolderModel(EngineClient& client, EngineSession& session, QObject* parent)
    : QAbstractListModel(parent), client_(client), session_(session) {
    connect(&client_, &EngineClient::connectedChanged, this, [this] {
        if (client_.connected()) {
            refresh();
        }
    });
}

void FolderModel::refresh() { go(path_); }

void FolderModel::up() {
    if (!path_.isEmpty()) {
        go(parent_);
    }
}

void FolderModel::open(const int row) {
    if (row >= 0 && row < rowCount() && rows_[static_cast<std::size_t>(row)].folder) {
        go(rows_[static_cast<std::size_t>(row)].key);
    }
}

void FolderModel::go(const QString& path) {
    const auto generation = ++generation_;
    loading_ = true;
    error_.clear();
    emit loadingChanged();
    Json params = Json::object();
    if (!path.isEmpty()) {
        params["path"] = path.toStdString();
    }
    client_.call(QStringLiteral("catalogue.folder"), std::move(params), [this, generation](const auto& answer) {
        if (generation != generation_) {
            return;
        }
        loading_ = false;
        if (!answer) {
            // An engine from before folders were browsable says so plainly.
            error_ = answer.error().code == core::ErrorCode::unsupported ||
                             answer.error().message.find("unknown method") != std::string::npos
                         ? tr("This engine cannot be browsed by folder yet; it needs updating.")
                         : QString::fromStdString(answer.error().message);
            emit loadingChanged();
            return;
        }
        const auto text = [](const Json& object, const char* key) {
            const auto found = object.find(key);
            return found != object.end() && found->is_string() ? QString::fromStdString(found->template get<std::string>())
                                                               : QString{};
        };
        std::vector<Row> rows;
        for (const auto& folder : answer->value("folders", Json::array())) {
            rows.push_back(Row{.folder = true, .key = text(folder, "path"), .title = text(folder, "name")});
        }
        for (const auto& track : answer->value("tracks", Json::array())) {
            auto title = text(track, "title");
            if (title.isEmpty()) {
                title = text(track, "label");
            }
            rows.push_back(Row{.folder = false,
                               .key = text(track, "key"),
                               .title = title,
                               .artist = text(track, "artist"),
                               .album = text(track, "album"),
                               .number = track.value("track_number", 0),
                               .duration_ms = std::max<qint64>(0, track.value("duration_ms", qint64{0}))});
        }
        beginResetModel();
        rows_ = std::move(rows);
        path_ = text(*answer, "path");
        name_ = text(*answer, "name");
        parent_ = text(*answer, "parent");
        endResetModel();
        emit loadingChanged();
        emit moved();
    });
}

// Every indexed path under a folder, a page at a time, then what they are.
void FolderModel::collect(QString folder, QString after, std::shared_ptr<Json> paths,
                          std::function<void(std::vector<QueuedTrack>)> done) {
    Json params{{"path", folder.toStdString()}, {"limit", inventory_page}};
    if (!after.isEmpty()) {
        params["after"] = after.toStdString();
    }
    client_.call(QStringLiteral("catalogue.inventory"), std::move(params),
                 [this, folder, paths, done = std::move(done)](const auto& answer) mutable {
                     if (!answer) {
                         emit session_.failed(QString::fromStdString(answer.error().message));
                         return;
                     }
                     QString last;
                     for (const auto& entry : answer->value("entries", Json::array())) {
                         last = QString::fromStdString(entry.value("path", std::string{}));
                         if (entry.value("available", true)) {
                             paths->push_back(last.toStdString());
                         }
                     }
                     if (answer->value("more", false) && paths->size() < folder_limit && !last.isEmpty()) {
                         collect(folder, last, paths, std::move(done));
                         return;
                     }
                     // What each is, a chunk at a time, in the order found.
                     auto tracks = std::make_shared<std::vector<QueuedTrack>>(paths->size());
                     auto pending = std::make_shared<std::size_t>(0);
                     const auto count = paths->size();
                     if (count == 0) {
                         done({});
                         return;
                     }
                     for (std::size_t first = 0; first < count; first += facts_chunk) {
                         ++*pending;
                         Json chunk = Json::array();
                         for (auto at = first; at < std::min(count, first + facts_chunk); ++at) {
                             chunk.push_back((*paths)[at]);
                         }
                         client_.call(QStringLiteral("catalogue.cached_tracks"), Json{{"paths", chunk}},
                                      [first, tracks, pending, paths, done](const auto& facts) {
                                          auto at = first;
                                          for (const auto& track : facts ? facts->value("tracks", Json::array())
                                                                         : Json::array()) {
                                              const auto index = at++;
                                              (*tracks)[index] = QueuedTrack{
                                                  .entry = QString::fromStdString(core::StableId::random().to_string()),
                                                  .path = QString::fromStdString((*paths)[index].template get<std::string>()),
                                                  .title = QString::fromStdString(track.value("title", std::string{})),
                                                  .artist = QString::fromStdString(track.value("artist", std::string{})),
                                                  .album = QString::fromStdString(track.value("album", std::string{})),
                                                  .date = QString::fromStdString(track.value("date", std::string{})),
                                                  .duration_ms = std::max<qint64>(0, track.value("duration_ms", qint64{0}))};
                                          }
                                          if (--*pending == 0) {
                                              std::vector<QueuedTrack> found;
                                              for (auto& track : *tracks) {
                                                  if (!track.path.isEmpty()) {
                                                      found.push_back(std::move(track));
                                                  }
                                              }
                                              done(std::move(found));
                                          }
                                      });
                     }
                 });
}

void FolderModel::resolve(const int row, std::function<void(std::vector<QueuedTrack>)> done) {
    if (row < 0 || row >= rowCount()) {
        return;
    }
    const auto& picked = rows_[static_cast<std::size_t>(row)];
    if (picked.folder) {
        collect(picked.key, {}, std::make_shared<Json>(Json::array()), std::move(done));
        return;
    }
    done({QueuedTrack{.entry = QString::fromStdString(core::StableId::random().to_string()),
                      .path = picked.key,
                      .title = picked.title,
                      .artist = picked.artist,
                      .album = picked.album,
                      .date = {},
                      .duration_ms = picked.duration_ms}});
}

void FolderModel::enqueue(const int row) {
    resolve(row, [this](const std::vector<QueuedTrack>& tracks) { session_.enqueue(tracks); });
}

void FolderModel::addToList(const int row, const QString& listId, const int position) {
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

int FolderModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant FolderModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case KindRole:
        return row.folder ? QStringLiteral("folder") : QStringLiteral("track");
    case KeyRole:
        return row.key;
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case AlbumRole:
        return row.album;
    case NumberRole:
        return row.number;
    case DurationRole:
        return static_cast<qreal>(row.duration_ms) / 1000.0;
    case CoverRole:
        return row.folder ? QString{} : CoverProvider::forPath(session_.index(), row.key);
    default:
        return {};
    }
}

QHash<int, QByteArray> FolderModel::roleNames() const {
    return {{KindRole, "kind"},     {KeyRole, "key"},       {TitleRole, "title"},       {ArtistRole, "artist"},
            {AlbumRole, "album"},   {NumberRole, "number"}, {DurationRole, "duration"}, {CoverRole, "cover"}};
}

} // namespace trackknife::quick
