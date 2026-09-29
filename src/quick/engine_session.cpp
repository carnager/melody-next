// SPDX-License-Identifier: GPL-3.0-only

#include "quick/engine_session.hpp"

#include "quick/cover_provider.hpp"

namespace trackknife::quick {

using Json = EngineClient::Json;

EngineSession::EngineSession(const int index, QString key, const bool local, QString fallback,
                             EngineClient::Connector connector, QObject* parent)
    : QObject(parent), index_(index), key_(std::move(key)), local_(local), fallback_(std::move(fallback)),
      client_(std::move(connector)), player_(client_), lists_(client_), library_(client_, *this),
      up_next_(client_, index) {
    connect(&client_, &EngineClient::connectedChanged, this, &EngineSession::connectedChanged);
    connect(&player_, &PlayerState::queueChanged, &up_next_, &UpNextModel::refresh);
}

QString EngineSession::name() const {
    if (local_) {
        return tr("This computer");
    }
    const auto announced = client_.announcedName();
    return announced.isEmpty() ? fallback_ : announced;
}

QString EngineSession::coverForPath(const QString& encoded_path) const {
    return CoverProvider::forPath(index_, encoded_path);
}

void EngineSession::enqueue(const std::vector<QueuedTrack>& tracks) {
    if (tracks.empty()) {
        return;
    }
    Json entries = Json::array();
    for (const auto& track : tracks) {
        up_next_.remember(track);
        entries.push_back(Json{{"entry", track.entry.toStdString()},
                               {"path", track.path.toStdString()},
                               {"duration_ms", track.duration_ms},
                               {"title", track.title.toStdString()},
                               {"group",
                                Json{{"artist", track.artist.toStdString()},
                                     {"album", track.album.toStdString()},
                                     {"date", track.date.toStdString()}}}});
    }
    // One command lane, so the requests never reach the engine before the
    // entries they name.
    client_.command(QStringLiteral("playback.enqueue"), Json{{"entries", std::move(entries)}});
    for (const auto& track : tracks) {
        client_.command(QStringLiteral("playback.request"), Json{{"entry", track.entry.toStdString()}});
    }
}

} // namespace trackknife::quick
