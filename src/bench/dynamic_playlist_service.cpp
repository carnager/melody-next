// SPDX-License-Identifier: GPL-3.0-only
#include "bench/dynamic_playlist_service.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/query/tkq_melody.hpp"
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>
#include <QtConcurrentRun>
#include <algorithm>
#include <numeric>
#include <random>

namespace trackknife::bench {
namespace {
core::Error error(const QString& message) {
    return {
        .code = core::ErrorCode::invalid_argument, .message = message.toStdString(), .context = {}};
}
QString key(const QString& profile) { return QStringLiteral("dynamic-playlists/v1/") + profile; }
bool validSource(const QString& source) {
    return source == QStringLiteral("rules") || source == QStringLiteral("similar") ||
           source == QStringLiteral("loved") || source == QStringLiteral("top") ||
           source == QStringLiteral("tag");
}
QString quoted(QString value) {
    value.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}
QString text(const mpd::Track& track, const char* field) {
    const auto value = track.metadata.first(field);
    return value ? QString::fromUtf8(value->data(), static_cast<qsizetype>(value->size()))
                 : QString{};
}
QString text(const LocalTrackRow& track, const char* field) {
    return QString::fromStdString(std::string_view(field) == "Artist" ? track.artist : track.title);
}
const std::string& identity(const LocalTrackRow& track) { return track.raw_path; }
const std::string& identity(const mpd::Track& track) { return track.uri; }
DynamicPlaylistService::Tracks emptyTracks(const DynamicPlaylistDefinition& definition) {
    if (definition.profile == QStringLiteral("local"))
        return std::vector<LocalTrackRow>{};
    return std::vector<mpd::Track>{};
}
QString historyKey(const DynamicPlaylistDefinition& definition) {
    const auto context =
        QJsonDocument(QJsonArray{definition.profile, definition.id, definition.source,
                                 definition.artist, definition.track, definition.user,
                                 definition.tag})
            .toJson(QJsonDocument::Compact);
    return QStringLiteral("dynamic-playlists/recent-v1/") +
           QString::fromLatin1(
               QCryptographicHash::hash(context, QCryptographicHash::Sha256).toHex());
}
QString identityHash(const std::string& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(QByteArray(value.data(), static_cast<qsizetype>(value.size())),
                                 QCryptographicHash::Sha256)
            .toHex());
}
QString normalized(const QString& value) {
    return value.normalized(QString::NormalizationForm_C).simplified().toCaseFolded();
}
} // namespace

core::Result<QVector<DynamicPlaylistDefinition>> loadDynamicPlaylists(const QString& profile) {
    QVector<DynamicPlaylistDefinition> result;
    const auto bytes = QSettings{}.value(key(profile)).toByteArray();
    if (bytes.isEmpty())
        return result;
    const auto doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject() || doc.object().value(QStringLiteral("version")).toInt() != 1 ||
        !doc.object().value(QStringLiteral("definitions")).isArray())
        return std::unexpected(
            error(QStringLiteral("Cannot read this dynamic-playlist catalog version")));
    for (const auto& entry : doc.object().value(QStringLiteral("definitions")).toArray()) {
        const auto o = entry.toObject();
        DynamicPlaylistDefinition d{.id = o["id"].toString(),
                                    .name = o["name"].toString(),
                                    .profile = profile,
                                    .source = o["source"].toString(),
                                    .query = o["query"].toString(),
                                    .artist = o["artist"].toString(),
                                    .track = o["track"].toString(),
                                    .user = o["user"].toString(),
                                    .tag = o["tag"].toString(),
                                    .limit = o["limit"].toInt(),
                                    .shuffle = o["shuffle"].toBool()};
        if (d.id.isEmpty() || d.name.isEmpty() || !validSource(d.source) || d.limit < 1 ||
            d.limit > 500 || o["dialect"].toString() != QStringLiteral("tkq") ||
            o["dialect_version"].toInt() != 1 || o["compiler_schema"].toInt() != 1)
            return std::unexpected(
                error(QStringLiteral("Invalid or unsupported dynamic-playlist definition")));
        result.push_back(std::move(d));
    }
    return result;
}
core::Result<void> saveDynamicPlaylists(const QString& profile,
                                        const QVector<DynamicPlaylistDefinition>& definitions) {
    if (profile.isEmpty())
        return std::unexpected(error(QStringLiteral("Connect to a saved server profile first")));
    if (const auto existing = loadDynamicPlaylists(profile); !existing)
        return std::unexpected(existing.error());
    QJsonArray entries;
    for (const auto& d : definitions) {
        if (d.profile != profile || d.id.isEmpty() || d.name.trimmed().isEmpty() ||
            !validSource(d.source) || d.limit < 1 || d.limit > 500)
            return std::unexpected(error(QStringLiteral("Invalid dynamic-playlist definition")));
        entries.append(QJsonObject{{"id", d.id},
                                   {"name", d.name},
                                   {"source", d.source},
                                   {"query", d.query},
                                   {"artist", d.artist},
                                   {"track", d.track},
                                   {"user", d.user},
                                   {"tag", d.tag},
                                   {"limit", d.limit},
                                   {"shuffle", d.shuffle},
                                   {"dialect", "tkq"},
                                   {"dialect_version", 1},
                                   {"compiler_schema", 1}});
    }
    QSettings settings;
    settings.setValue(key(profile),
                      QJsonDocument(QJsonObject{{"version", 1}, {"definitions", entries}})
                          .toJson(QJsonDocument::Compact));
    // Uses the same settings persistence contract as the rest of the workspace.
    return {};
}
core::Result<QVector<RecommendationTrack>> parseLastFmTracks(const QByteArray& data,
                                                             const int limit) {
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return std::unexpected(error(QStringLiteral("Last.fm returned invalid JSON")));
    const auto root = doc.object();
    if (root.contains("error"))
        return std::unexpected(
            error(root["message"].toString(QStringLiteral("Last.fm request failed"))));
    QJsonValue tracks;
    for (const auto& field : {"similartracks", "lovedtracks", "toptracks", "tracks"})
        if (root.contains(field))
            tracks = root[field].toObject()["track"];
    if (!tracks.isArray() && !tracks.isObject())
        return std::unexpected(error(QStringLiteral("Last.fm response has no track list")));
    const auto array = tracks.isArray() ? tracks.toArray() : QJsonArray{tracks};
    QVector<RecommendationTrack> result;
    for (const auto& value : array) {
        const auto o = value.toObject();
        auto artist = o["artist"].isObject() ? o["artist"].toObject()["name"].toString()
                                             : o["artist"].toString();
        const auto title = o["name"].toString();
        if (artist.isEmpty() || title.isEmpty())
            continue;
        result.push_back({artist, title, o["mbid"].toString()});
        if (result.size() >= std::clamp(limit, 1, lastfm_candidate_limit))
            break;
    }
    return result;
}
DynamicPlaylistService::DynamicPlaylistService(Search search, QObject* parent)
    : QObject(parent), search_(std::move(search)), network_(new QNetworkAccessManager(this)) {}
void DynamicPlaylistService::cancel() {
    ++generation_;
    cancellation_.request_cancellation();
    cancellation_ = core::CancellationSource{};
    if (reply_) {
        auto reply = reply_;
        reply_.clear();
        reply->abort();
    }
}
void DynamicPlaylistService::fail(const QString& message) { emit finished({}, 0, message); }
void DynamicPlaylistService::refresh(DynamicPlaylistDefinition definition,
                                     const QString& lastfm_key) {
    cancel();
    definition_ = std::move(definition);
    result_ = emptyTracks(definition_);
    unmatched_ = 0;
    if (!validSource(definition_.source) || definition_.limit < 1 || definition_.limit > 500) {
        fail(QStringLiteral("Invalid playlist source or limit"));
        return;
    }
    const auto generation = generation_;
    QPointer<DynamicPlaylistService> self(this);
    if (definition_.source == QStringLiteral("rules")) {
        const auto compiled = query::compile_tkq(definition_.query.toStdString());
        if (!compiled) {
            fail(QString::fromStdString(compiled.error().message));
            return;
        }
        emit progress(QStringLiteral("Updating from library tags and ratings…"));
        search_(*compiled, cancellation_.token(), [self, generation](Result tracks) {
            if (!self || generation != self->generation_)
                return;
            if (!tracks) {
                self->fail(QString::fromStdString(tracks.error().message));
                return;
            }
            if (tracks->index() != self->result_.index()) {
                self->fail(QStringLiteral("Library authority changed; refresh the playlist"));
                return;
            }
            self->result_ = std::move(*tracks);
            self->complete();
        });
        return;
    }
    if (lastfm_key.trimmed().isEmpty()) {
        fail(QStringLiteral("Add a Last.fm API key in Settings → Metadata services"));
        return;
    }
    QUrl url(QStringLiteral("https://ws.audioscrobbler.com/2.0/"));
    QUrlQuery args;
    args.addQueryItem(QStringLiteral("api_key"), lastfm_key.trimmed());
    args.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    args.addQueryItem(QStringLiteral("limit"), QString::number(lastfm_candidate_limit));
    if (definition_.source == QStringLiteral("similar")) {
        if (definition_.artist.trimmed().isEmpty() || definition_.track.trimmed().isEmpty()) {
            fail(QStringLiteral("Enter a seed artist and track"));
            return;
        }
        args.addQueryItem(QStringLiteral("method"), QStringLiteral("track.getSimilar"));
        args.addQueryItem(QStringLiteral("artist"), definition_.artist);
        args.addQueryItem(QStringLiteral("track"), definition_.track);
    } else if (definition_.source == QStringLiteral("tag")) {
        if (definition_.tag.trimmed().isEmpty()) {
            fail(QStringLiteral("Enter a Last.fm tag"));
            return;
        }
        args.addQueryItem(QStringLiteral("method"), QStringLiteral("tag.getTopTracks"));
        args.addQueryItem(QStringLiteral("tag"), definition_.tag);
    } else {
        if (definition_.user.trimmed().isEmpty()) {
            fail(QStringLiteral("Enter a Last.fm username"));
            return;
        }
        args.addQueryItem(QStringLiteral("method"), definition_.source == QStringLiteral("loved")
                                                        ? QStringLiteral("user.getLovedTracks")
                                                        : QStringLiteral("user.getTopTracks"));
        args.addQueryItem(QStringLiteral("user"), definition_.user);
    }
    url.setQuery(args);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("User-Agent", "Trackknife/1.0");
    auto* reply = network_->get(request);
    reply_ = reply;
    reply->setReadBufferSize(2 * 1024 * 1024 + 1);
    emit progress(QStringLiteral("Fetching Last.fm tracks…"));
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 2 * 1024 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [self, reply, generation] {
        reply->deleteLater();
        if (!self || generation != self->generation_)
            return;
        self->reply_.clear();
        // Do not echo QNetworkReply's URL-bearing error text: it includes the API key.
        if (reply->error() != QNetworkReply::NoError) {
            self->fail(QStringLiteral(
                "Last.fm request failed or timed out; check the connection and API key"));
            return;
        }
        const auto parsed = parseLastFmTracks(reply->readAll(), lastfm_candidate_limit);
        if (!parsed) {
            self->fail(QString::fromStdString(parsed.error().message));
            return;
        }
        self->matchRecommendations(self->definition_, *parsed);
    });
}
void DynamicPlaylistService::matchRecommendations(DynamicPlaylistDefinition definition,
                                                  QVector<RecommendationTrack> candidates) {
    cancel();
    definition_ = std::move(definition);
    candidates_ = std::move(candidates);
    if (candidates_.size() > lastfm_candidate_limit)
        candidates_.resize(lastfm_candidate_limit);
    candidate_index_ = 0;
    unmatched_ = 0;
    result_ = emptyTracks(definition_);
    matchNext(generation_);
}
void DynamicPlaylistService::matchNext(const quint64 generation) {
    if (generation != generation_)
        return;
    if (candidate_index_ >= candidates_.size()) {
        complete();
        return;
    }
    const auto candidate = candidates_[candidate_index_++];
    emit progress(QStringLiteral("Matching library tracks: %1 / %2")
                      .arg(candidate_index_)
                      .arg(candidates_.size()));
    const auto compiled =
        query::compile_tkq(QStringLiteral("artist IS %1 AND title IS %2")
                               .arg(quoted(candidate.artist), quoted(candidate.title))
                               .toStdString());
    if (!compiled) {
        fail(QStringLiteral("Last.fm returned an invalid artist or title"));
        return;
    }
    QPointer<DynamicPlaylistService> self(this);
    search_(*compiled, cancellation_.token(), [self, generation, candidate](Result tracks) {
        if (!self || generation != self->generation_)
            return;
        if (!tracks) {
            self->fail(QString::fromStdString(tracks.error().message));
            return;
        }
        if (tracks->index() != self->result_.index()) {
            self->fail(QStringLiteral("Library authority changed; refresh the playlist"));
            return;
        }
        std::visit(
            [&](auto& matches) {
                using Rows = std::decay_t<decltype(matches)>;
                auto& result = std::get<Rows>(self->result_);
                std::erase_if(matches, [&candidate](const auto& t) {
                    return normalized(text(t, "Artist")) != normalized(candidate.artist) ||
                           normalized(text(t, "Title")) != normalized(candidate.title);
                });
                std::ranges::sort(matches, [](const auto& a, const auto& b) {
                    return identity(a) < identity(b);
                });
                if (matches.empty())
                    ++self->unmatched_;
                else if (std::ranges::none_of(result, [&matches](const auto& t) {
                             return identity(t) == identity(matches.front());
                         }))
                    result.push_back(std::move(matches.front()));
            },
            *tracks);
        QTimer::singleShot(0, self, [self, generation] {
            if (self)
                self->matchNext(generation);
        });
    });
}
void DynamicPlaylistService::complete() {
    matched_pool_size_ = std::visit([](const auto& rows) { return rows.size(); }, result_);
    if (definition_.source != QStringLiteral("rules")) {
        // Membership varies independently of display ordering. Prefer every unseen
        // match before reusing any track from the last successful refresh.
        QSettings settings;
        const auto key = historyKey(definition_);
        const auto previous = settings.value(key).toStringList();
        const QSet<QString> recent(previous.begin(), previous.end());
        QStringList selected_ids;
        std::visit(
            [this, &recent, &selected_ids](auto& rows) {
                std::vector<std::size_t> indices(rows.size());
                std::iota(indices.begin(), indices.end(), 0U);
                std::mt19937 generator(std::random_device{}());
                std::shuffle(indices.begin(), indices.end(), generator);
                std::stable_partition(indices.begin(), indices.end(), [&](std::size_t index) {
                    return !recent.contains(identityHash(identity(rows[index])));
                });
                indices.resize(
                    std::min(indices.size(), static_cast<std::size_t>(definition_.limit)));
                if (definition_.shuffle)
                    std::shuffle(indices.begin(), indices.end(), generator);
                else
                    std::ranges::sort(indices);
                std::decay_t<decltype(rows)> selected;
                selected.reserve(indices.size());
                for (const auto index : indices) {
                    selected_ids.push_back(identityHash(identity(rows[index])));
                    selected.push_back(std::move(rows[index]));
                }
                rows = std::move(selected);
            },
            result_);
        // Empty, failed and cancelled refreshes must not forget the last selection.
        if (!selected_ids.isEmpty())
            settings.setValue(key, selected_ids);
        emit finished(result_, unmatched_, {});
        return;
    }
    const auto prepare = [shuffle = definition_.shuffle, limit = definition_.limit](Tracks tracks) {
        std::visit(
            [shuffle, limit](auto& rows) {
                if (shuffle) {
                    std::mt19937 generator(std::random_device{}());
                    std::shuffle(rows.begin(), rows.end(), generator);
                }
                if (rows.size() > static_cast<std::size_t>(limit))
                    rows.resize(static_cast<std::size_t>(limit));
            },
            tracks);
        return tracks;
    };
    if (std::visit([](const auto& rows) { return rows.size(); }, result_) <= 500U) {
        result_ = prepare(std::move(result_));
        emit finished(result_, unmatched_, {});
        return;
    }
    // Large rule result sets are sampled/trimmed and released on a bounded worker.
    auto* watcher = new QFutureWatcher<Tracks>(this);
    const auto generation = generation_;
    connect(watcher, &QFutureWatcher<Tracks>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != generation_)
            return;
        result_ = watcher->future().takeResult();
        emit finished(result_, unmatched_, {});
    });
    watcher->setFuture(QtConcurrent::run(prepare, std::move(result_)));
}

DynamicPlaylistService::Result
queryDynamicLocalLibrary(const std::filesystem::path& database, const query::CompiledTkq& compiled,
                         const core::CancellationToken& cancellation) {
    auto library = persistence::LocalLibrary::open(database);
    if (!library)
        return std::unexpected(library.error());
    auto paths = library->filter_paths(compiled, cancellation);
    if (!paths)
        return std::unexpected(paths.error());
    auto cached = library->cached_tracks(*paths, cancellation);
    if (!cached)
        return std::unexpected(cached.error());
    std::vector<LocalTrackRow> rows;
    rows.reserve(cached->size());
    for (auto& entry : *cached)
        rows.push_back(cached_library_row(std::move(entry)));
    return DynamicPlaylistService::Tracks{std::move(rows)};
}
} // namespace trackknife::bench
