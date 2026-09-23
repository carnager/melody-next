// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/query/tkq.hpp"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace trackknife::bench {

struct DynamicPlaylistDefinition {
    QString id{};
    QString name{};
    QString profile{};
    QString source{QStringLiteral("rules")};
    QString query{QStringLiteral("rating GREATER 6")};
    QString artist{};
    QString track{};
    QString user{};
    QString tag{};
    int limit{100};
    bool shuffle{false};
};

inline constexpr int lastfm_candidate_limit = 500;

struct RecommendationTrack {
    QString artist{};
    QString title;
    QString mbid;
};

// Profile-bound definitions, independently versioned from ordinary playlists.
// No key or password is stored in a definition.
core::Result<QVector<DynamicPlaylistDefinition>> loadDynamicPlaylists(const QString& profile);
core::Result<void> saveDynamicPlaylists(const QString& profile,
                                        const QVector<DynamicPlaylistDefinition>& definitions);
core::Result<QVector<RecommendationTrack>> parseLastFmTracks(const QByteArray& data, int limit);

class DynamicPlaylistService final : public QObject {
    Q_OBJECT
  public:
    using Tracks = std::vector<LocalTrackRow>;
    using Result = core::Result<Tracks>;
    using Completion = std::function<void(Result)>;
    using Search = std::function<void(query::CompiledTkq, core::CancellationToken, Completion)>;
    DynamicPlaylistService(Search search, QObject* parent = nullptr);
    void refresh(DynamicPlaylistDefinition definition, const QString& lastfm_key);
    void cancel();
    std::size_t matchedPoolSize() const { return matched_pool_size_; }
    // Same matching path as a provider response, without network access.
    void matchRecommendations(DynamicPlaylistDefinition definition,
                              QVector<RecommendationTrack> candidates);
  signals:
    void progress(const QString& message);
    void finished(const Tracks& tracks, int unmatched, const QString& error);

  private:
    void matchNext(quint64 generation);
    void complete();
    void fail(const QString& message);
    Search search_;
    core::CancellationSource cancellation_;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    quint64 generation_{0};
    DynamicPlaylistDefinition definition_;
    QVector<RecommendationTrack> candidates_;
    qsizetype candidate_index_{0};
    int unmatched_{0};
    Tracks result_;
    std::size_t matched_pool_size_{0};
};
// The library the catalogue source chose -- this process's or an engine's --
// never a database opened here.
DynamicPlaylistService::Result queryDynamicLibrary(const engine::Catalogue& catalogue,
                                                   const query::CompiledTkq& compiled,
                                                   const core::CancellationToken& cancellation);
} // namespace trackknife::bench
