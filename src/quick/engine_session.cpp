// SPDX-License-Identifier: GPL-3.0-only

#include "quick/engine_session.hpp"

#include "quick/cover_provider.hpp"
#include "trackknife/core/stable_id.hpp"

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

Json EngineSession::newItem(const QueuedTrack& track) {
    return Json{{"entry", core::StableId::random().to_string()},
                {"path", track.path.toStdString()},
                {"duration_ms", track.duration_ms > 0 ? Json(track.duration_ms) : Json(nullptr)},
                {"title", track.title.toStdString()},
                {"artist", track.artist.toStdString()},
                {"album", track.album.toStdString()}};
}

Json EngineSession::copiedItem(const Json& item) {
    auto copy = item;
    copy["entry"] = core::StableId::random().to_string();
    return copy;
}

void EngineSession::editList(const QString& id, ListEdit edit, const std::optional<std::uint64_t> expected) {
    client_.call(QStringLiteral("list.get"), Json{{"id", id.toStdString()}},
                 [this, id, edit = std::move(edit), expected](const auto& answer) {
                     if (!answer) {
                         emit failed(QString::fromStdString(answer.error().message));
                         return;
                     }
                     const auto revision = answer->value("revision", std::uint64_t{0});
                     if (expected && *expected != revision) {
                         emit failed(tr("The list changed elsewhere; nothing was changed. Try again."));
                         return;
                     }
                     const auto before = answer->value("items", Json::array());
                     auto after = edit(before);
                     Json params{{"id", id.toStdString()},
                                 {"name", answer->value("name", std::string{})},
                                 {"kind", answer->value("kind", std::string{"working"})},
                                 {"items", after},
                                 {"revision", revision}};
                     client_.command(QStringLiteral("list.save"), std::move(params),
                                     [this, before, after](const auto& saved) {
                                         if (!saved) {
                                             emit failed(QString::fromStdString(saved.error().message));
                                             return;
                                         }
                                         followPlaying(before, after);
                                     });
                 });
}

void EngineSession::createList(const QString& name) {
    client_.command(QStringLiteral("list.save"),
                    Json{{"name", name.trimmed().toStdString()}, {"kind", "working"}, {"items", Json::array()}},
                    [this](const auto& answer) {
                        if (!answer) {
                            emit failed(QString::fromStdString(answer.error().message));
                            return;
                        }
                        emit listCreated(QString::fromStdString(answer->value("id", std::string{})));
                    });
}

void EngineSession::renameList(const QString& id, const QString& name) {
    client_.command(QStringLiteral("list.rename"),
                    Json{{"id", id.toStdString()}, {"name", name.trimmed().toStdString()}},
                    [this](const auto& answer) {
                        if (!answer) {
                            emit failed(QString::fromStdString(answer.error().message));
                        }
                    });
}

void EngineSession::saveList(const QString& id, const QString& name) {
    client_.call(QStringLiteral("list.get"), Json{{"id", id.toStdString()}},
                 [this, id, name](const auto& answer) {
                     if (!answer) {
                         emit failed(QString::fromStdString(answer.error().message));
                         return;
                     }
                     client_.command(QStringLiteral("list.save"),
                                     Json{{"id", id.toStdString()},
                                          {"name", name.trimmed().toStdString()},
                                          {"kind", "saved"},
                                          {"items", answer->value("items", Json::array())},
                                          {"revision", answer->value("revision", std::uint64_t{0})}},
                                     [this](const auto& saved) {
                                         if (!saved) {
                                             emit failed(QString::fromStdString(saved.error().message));
                                         }
                                     });
                 });
}

void EngineSession::deleteList(const QString& id) {
    client_.command(QStringLiteral("list.delete"), Json{{"id", id.toStdString()}}, [this](const auto& answer) {
        if (!answer) {
            emit failed(QString::fromStdString(answer.error().message));
        }
    });
}

// A list edited while it plays is an edit to what plays: the engine's queue
// is replaced by the list as it now is, without starting anything.
void EngineSession::followPlaying(const Json& before, const Json& after) {
    const auto playing = player_.entry().toStdString();
    if (playing.empty()) {
        return;
    }
    const auto held = std::ranges::any_of(
        before, [&playing](const Json& item) { return item.value("entry", std::string{}) == playing; });
    if (!held) {
        return;
    }
    Json entries = Json::array();
    for (const auto& item : after) {
        auto entry = item;
        entry["group"] = Json{{"artist", item.value("artist", std::string{})},
                              {"album", item.value("album", std::string{})}};
        entries.push_back(std::move(entry));
    }
    client_.command(QStringLiteral("playback.replace_queue"), Json{{"entries", std::move(entries)}});
}

} // namespace trackknife::quick
