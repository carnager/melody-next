// SPDX-License-Identifier: GPL-3.0-only

#include "quick/engine_session.hpp"

#include "quick/cover_provider.hpp"

#include <QVariantMap>
#include "trackknife/core/stable_id.hpp"

namespace trackknife::quick {

using Json = EngineClient::Json;

EngineSession::EngineSession(const int index, QString key, const bool local, QString fallback,
                             EngineClient::Connector connector, QObject* parent)
    : QObject(parent), index_(index), key_(std::move(key)), local_(local), fallback_(std::move(fallback)),
      client_(std::move(connector)), player_(client_), lists_(client_), library_(client_, *this),
      up_next_(client_, index), folders_(client_, *this) {
    connect(&client_, &EngineClient::connectedChanged, this, &EngineSession::connectedChanged);
    connect(&player_, &PlayerState::queueChanged, &up_next_, &UpNextModel::refresh);
    client_.onEvent([this](const std::string& name, const Json& data) {
        if (name == "catalogue.rating_changed") {
            emit ratingChanged(QString::fromStdString(data.value("hash", std::string{})), data.value("rating", 0));
            return;
        }
        // Only this window's scan: another client's jobs are its own.
        if (scan_job_.isEmpty() || QString::fromStdString(data.value("job_id", std::string{})) != scan_job_) {
            return;
        }
        if (name == "job.progress") {
            scan_progress_ = tr("%1 files looked at, %2 indexed, %3 failed")
                                 .arg(data.value("visited", 0))
                                 .arg(data.value("indexed", 0))
                                 .arg(data.value("failed", 0));
            emit scanChanged();
        } else if (name == "job.finished") {
            const auto outcome = data.value("outcome", Json::object());
            scan_job_.clear();
            if (outcome.contains("error")) {
                scan_progress_ = QString::fromStdString(outcome.value("error", std::string{}));
            } else {
                scan_progress_ = (outcome.value("cancelled", false) ? tr("Stopped: %1 indexed, %2 failed")
                                                                    : tr("Done: %1 indexed, %2 failed"))
                                     .arg(outcome.value("indexed", 0))
                                     .arg(outcome.value("failed", 0));
            }
            emit scanChanged();
            refreshRoots();
            library_.refresh();
        }
    });
    connect(&client_, &EngineClient::connectedChanged, this, [this] {
        if (client_.connected()) {
            refreshRoots();
        } else if (!scan_job_.isEmpty()) {
            // Its events went with the connection.
            scan_job_.clear();
            scan_progress_.clear();
            emit scanChanged();
        }
    });
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

void EngineSession::setRating(const QString& hash, const bool album, const int rating) {
    client_.command(QStringLiteral("catalogue.set_rating"),
                    Json{{"hash", hash.toStdString()}, {"album", album}, {"rating", std::clamp(rating, 0, 10)}},
                    [this](const auto& answer) {
                        if (!answer) {
                            emit failed(QString::fromStdString(answer.error().message));
                        }
                    });
}

void EngineSession::duplicateList(const QString& id, const QString& name) {
    client_.call(QStringLiteral("list.get"), Json{{"id", id.toStdString()}}, [this, name](const auto& answer) {
        if (!answer) {
            emit failed(QString::fromStdString(answer.error().message));
            return;
        }
        Json items = Json::array();
        for (const auto& item : answer->value("items", Json::array())) {
            items.push_back(copiedItem(item));
        }
        client_.command(QStringLiteral("list.save"),
                        Json{{"name", name.trimmed().toStdString()}, {"kind", "working"}, {"items", std::move(items)}},
                        [this](const auto& saved) {
                            if (!saved) {
                                emit failed(QString::fromStdString(saved.error().message));
                                return;
                            }
                            emit listCreated(QString::fromStdString(saved->value("id", std::string{})));
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

namespace {

// A raw path from the wire, as text a person reads.
[[nodiscard]] QString shownPath(const std::string& encoded) {
    const auto raw = protocol::decode_raw_path(encoded);
    return raw ? QString::fromStdString(protocol::displayable_text(*raw)) : QString{};
}

} // namespace

void EngineSession::refreshRoots() {
    client_.call(QStringLiteral("catalogue.roots"), Json::object(), [this](const auto& answer) {
        if (!answer) {
            return;
        }
        QVariantList roots;
        for (const auto& root : answer->value("roots", Json::array())) {
            const auto path = root.value("path", std::string{});
            roots.push_back(QVariantMap{{QStringLiteral("path"), QString::fromStdString(path)},
                                        {QStringLiteral("name"), shownPath(path)},
                                        {QStringLiteral("available"), root.value("available", true)},
                                        {QStringLiteral("error"), QString::fromStdString(root.value("error", std::string{}))}});
        }
        roots_ = std::move(roots);
        emit rootsChanged();
    });
}

void EngineSession::addRoot(const QString& path) {
    client_.command(QStringLiteral("catalogue.add_root"), Json{{"path", path.toStdString()}},
                    [this](const auto& answer) {
                        if (!answer) {
                            emit failed(QString::fromStdString(answer.error().message));
                        }
                        refreshRoots();
                    });
}

void EngineSession::removeRoot(const QString& path) {
    client_.command(QStringLiteral("catalogue.remove_root"), Json{{"path", path.toStdString()}},
                    [this](const auto& answer) {
                        if (!answer) {
                            emit failed(QString::fromStdString(answer.error().message));
                        }
                        refreshRoots();
                        library_.refresh();
                    });
}

void EngineSession::scan() {
    if (scanning()) {
        return;
    }
    client_.command(QStringLiteral("job.submit"), Json{{"job", "catalogue.scan"}, {"params", Json::object()}},
                    [this](const auto& answer) {
                        if (!answer) {
                            emit failed(QString::fromStdString(answer.error().message));
                            return;
                        }
                        scan_job_ = QString::fromStdString(answer->value("job_id", std::string{}));
                        scan_progress_ = tr("Starting…");
                        emit scanChanged();
                    });
}

void EngineSession::cancelScan() {
    if (scanning()) {
        client_.command(QStringLiteral("job.cancel"), Json{{"job_id", scan_job_.toStdString()}});
    }
}

void EngineSession::listFolders(const QString& path) {
    Json params = Json::object();
    if (!path.isEmpty()) {
        params["path"] = path.toStdString();
    }
    client_.call(QStringLiteral("folders.list"), std::move(params), [this](const auto& listed) {
        QVariantMap result;
        if (!listed) {
            result[QStringLiteral("error")] = QString::fromStdString(listed.error().message);
        } else {
            const auto here = listed->value("path", std::string{});
            const auto raw_here = protocol::decode_raw_path(here);
            result[QStringLiteral("path")] = QString::fromStdString(here);
            result[QStringLiteral("name")] = shownPath(here);
            const auto parent = listed->find("parent");
            result[QStringLiteral("parent")] = parent != listed->end() && parent->is_string()
                                                   ? QString::fromStdString(parent->template get<std::string>())
                                                   : QString{};
            QVariantList folders;
            for (const auto& name : listed->value("folders", Json::array())) {
                const auto raw_name = protocol::decode_raw_path(name.template get<std::string>());
                if (!raw_name || !raw_here) {
                    continue;
                }
                const auto child = *raw_here == "/" ? "/" + *raw_name : *raw_here + "/" + *raw_name;
                folders.push_back(QVariantMap{
                    {QStringLiteral("path"), QString::fromStdString(protocol::encode_raw_path(child))},
                    {QStringLiteral("name"), QString::fromStdString(protocol::displayable_text(*raw_name))}});
            }
            result[QStringLiteral("folders")] = folders;
        }
        emit foldersListed(result);
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
