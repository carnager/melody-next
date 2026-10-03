// SPDX-License-Identifier: GPL-3.0-only
#include "bench/dynamic_playlist_service.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/query/tkq.hpp"
#include <QCryptographicHash>
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
QString text(const LocalTrackRow& track, const char* field) {
    return QString::fromStdString(std::string_view(field) == "Artist" ? track.artist : track.title);
}
const std::string& identity(const LocalTrackRow& track) { return track.raw_path; }
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
bool validCount(const int count) { return count >= 0 && count <= 500; }
bool validSelection(const DynamicPlaylistDefinition& d) {
    return d.limit >= 1 && d.limit <= 500 && validCount(d.groups) && validCount(d.per_group) &&
           (d.groups == 0 || !d.group_by.trimmed().isEmpty());
}
} // namespace

engine::DynamicSelection DynamicPlaylistDefinition::selection() const {
    const bool grouped = groups > 0 && !group_by.trimmed().isEmpty();
    return engine::DynamicSelection{
        .query = query.toStdString(),
        .limit = static_cast<std::size_t>(std::clamp(limit, 1, 500)),
        .shuffle = shuffle,
        .group_by = grouped ? group_by.toStdString() : std::string{},
        .groups = grouped ? static_cast<std::size_t>(groups) : 0U,
        .per_group = grouped ? static_cast<std::size_t>(std::max(per_group, 0)) : 0U};
}

QVector<DynamicPlaylistDefinition> shippedDynamicPlaylists(const QString& profile) {
    const auto rule = [&profile](const char* id, const char* name, const char* query,
                                 const int limit, const bool shuffle, const char* group_by = "",
                                 const int groups = 0, const int per_group = 0) {
        return DynamicPlaylistDefinition{.id = QStringLiteral("shipped:") + QLatin1String(id),
                                         .name = QString::fromUtf8(name),
                                         .profile = profile,
                                         .source = QStringLiteral("rules"),
                                         .query = QString::fromUtf8(query),
                                         .limit = limit,
                                         .shuffle = shuffle,
                                         .group_by = QString::fromUtf8(group_by),
                                         .groups = groups,
                                         .per_group = per_group};
    };
    return {
        rule("random-tracks", "Random tracks", "ALL", 50, true),
        rule("random-album", "Random album", "ALL", 500, false,
             "%albumartist% \u2014 %album% %date%", 1, 0),
        rule("random-artists", "Random artists", "ALL", 30, true, "%albumartist%", 10, 3),
        rule("rated-8", "Rated 8 and higher", "rating GREATER 7", 100, true),
        rule("unrated", "Unrated", "rating MISSING", 100, true),
        rule("recently-added", "Recently added",
             "dayssinceadded LESS 31 SORT BY $num($info(dayssinceadded),6)", 100, false),
        rule("not-heard-in-a-year", "Not heard in a year",
             "HISTORY(dayssinceplayed) GREATER 365 OR HISTORY(dayssinceplayed) MISSING", 100,
             true),
    };
}

core::Result<QVector<DynamicPlaylistDefinition>> dynamicPlaylistCatalog(const QString& profile) {
    auto saved = loadDynamicPlaylists(profile);
    if (!saved)
        return std::unexpected(saved.error());
    auto catalog = shippedDynamicPlaylists(profile);
    catalog.append(*saved);
    return catalog;
}

void adoptDynamicPlaylists(const QString& profile) {
    const auto done = QStringLiteral("dynamic-playlists/adopted-v1");
    QSettings settings;
    if (settings.value(done).toBool())
        return;
    settings.beginGroup(QStringLiteral("dynamic-playlists/v1"));
    const auto profiles = settings.allKeys();
    settings.endGroup();
    auto kept = loadDynamicPlaylists(profile);
    if (!kept)
        return;
    QStringList moved;
    for (const auto& other : profiles) {
        if (other == profile)
            continue;
        const auto found = loadDynamicPlaylists(other);
        if (!found)
            continue;
        for (auto definition : *found) {
            if (std::ranges::any_of(*kept, [&definition](const auto& known) {
                    return known.id == definition.id;
                }))
                continue;
            definition.profile = profile;
            kept->push_back(std::move(definition));
        }
        moved.push_back(other);
    }
    if (!moved.isEmpty() && !saveDynamicPlaylists(profile, *kept))
        return;
    for (const auto& other : moved)
        settings.remove(key(other));
    settings.setValue(done, true);
}

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
                                    .shuffle = o["shuffle"].toBool(),
                                    .group_by = o["group_by"].toString(),
                                    .groups = o["groups"].toInt(),
                                    .per_group = o["per_group"].toInt()};
        if (d.id.isEmpty() || d.name.isEmpty() || d.shipped() || !validSource(d.source) ||
            !validSelection(d) || o["dialect"].toString() != QStringLiteral("tkq") ||
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
        if (d.profile != profile || d.id.isEmpty() || d.shipped() || d.name.trimmed().isEmpty() ||
            !validSource(d.source) || !validSelection(d))
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
                                   {"group_by", d.group_by},
                                   {"groups", d.groups},
                                   {"per_group", d.per_group},
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
    result_.clear();
    unmatched_ = 0;
    if (!validSource(definition_.source) || !validSelection(definition_)) {
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
        // ADR-0258: the engine selects; only what it chose comes back.
        search_(definition_.selection(), cancellation_.token(), [self, generation](Result tracks) {
            if (!self || generation != self->generation_)
                return;
            if (!tracks) {
                self->fail(QString::fromStdString(tracks.error().message));
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
    result_.clear();
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
    const engine::DynamicSelection selection{.query = compiled->source,
                                             .limit = engine::dynamic_selection_limit,
                                             .shuffle = false,
                                             .group_by = {},
                                             .groups = 0U,
                                             .per_group = 0U};
    search_(selection, cancellation_.token(), [self, generation, candidate](Result tracks) {
        if (!self || generation != self->generation_)
            return;
        if (!tracks) {
            self->fail(QString::fromStdString(tracks.error().message));
            return;
        }
        auto& matches = *tracks;
        auto& result = self->result_;
        std::erase_if(matches, [&candidate](const auto& t) {
            return normalized(text(t, "Artist")) != normalized(candidate.artist) ||
                   normalized(text(t, "Title")) != normalized(candidate.title);
        });
        std::ranges::sort(matches,
                          [](const auto& a, const auto& b) { return identity(a) < identity(b); });
        if (matches.empty())
            ++self->unmatched_;
        else if (std::ranges::none_of(result, [&matches](const auto& t) {
                     return identity(t) == identity(matches.front());
                 }))
            result.push_back(std::move(matches.front()));
        QTimer::singleShot(0, self, [self, generation] {
            if (self)
                self->matchNext(generation);
        });
    });
}
void DynamicPlaylistService::complete() {
    matched_pool_size_ = result_.size();
    if (definition_.source != QStringLiteral("rules")) {
        // Membership varies independently of display ordering. Prefer every unseen
        // match before reusing any track from the last successful refresh.
        QSettings settings;
        const auto key = historyKey(definition_);
        const auto previous = settings.value(key).toStringList();
        const QSet<QString> recent(previous.begin(), previous.end());
        QStringList selected_ids;
        auto& rows = result_;
        std::vector<std::size_t> indices(rows.size());
        std::iota(indices.begin(), indices.end(), 0U);
        std::mt19937 generator(std::random_device{}());
        std::shuffle(indices.begin(), indices.end(), generator);
        std::stable_partition(indices.begin(), indices.end(), [&](std::size_t index) {
            return !recent.contains(identityHash(identity(rows[index])));
        });
        indices.resize(std::min(indices.size(), static_cast<std::size_t>(definition_.limit)));
        if (definition_.shuffle)
            std::shuffle(indices.begin(), indices.end(), generator);
        else
            std::ranges::sort(indices);
        Tracks selected;
        selected.reserve(indices.size());
        for (const auto index : indices) {
            selected_ids.push_back(identityHash(identity(rows[index])));
            selected.push_back(std::move(rows[index]));
        }
        rows = std::move(selected);
        // Empty, failed and cancelled refreshes must not forget the last selection.
        if (!selected_ids.isEmpty())
            settings.setValue(key, selected_ids);
        emit finished(result_, unmatched_, {});
        return;
    }
    // Rules: as the engine selected them.
    emit finished(result_, unmatched_, {});
}

DynamicPlaylistService::Result selectDynamicLibrary(const engine::Catalogue& catalogue,
                                                    const engine::DynamicSelection& selection,
                                                    const core::CancellationToken& cancellation) {
    auto selected = catalogue.select(selection, {}, cancellation);
    if (!selected)
        return std::unexpected(selected.error());
    auto cached = catalogue.cached_tracks(selected->paths, cancellation);
    if (!cached)
        return std::unexpected(cached.error());
    std::vector<LocalTrackRow> rows;
    rows.reserve(cached->size());
    for (auto& entry : *cached)
        rows.push_back(cached_library_row(std::move(entry)));
    return rows;
}
} // namespace trackknife::bench
