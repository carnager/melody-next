// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/dynamic_playlist_service.hpp"
#include "bench/engine_key.hpp"

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <vector>

namespace trackknife::bench {

class LocalListModel;

// Dynamic playlists (ADR-0145): saved definitions -- library rules or a
// Last.fm source -- refreshed into a list of matching tracks, in the library
// of the engine chosen. Rules follow that library's changes while open; a
// snapshot keeps a result stable. The window's Dynamic playlists dialog draws it.
class DynamicPlaylistSession final : public QObject {
    Q_OBJECT

  public:
    // Which library a refresh searches: that of the engine chosen.
    using LibrarySearch =
        std::function<void(const EngineKey& engine, engine::DynamicSelection,
                           core::CancellationToken, DynamicPlaylistService::Completion)>;
    // A library to choose, by its engine and the name it is shown by.
    struct Library {
        EngineKey engine;
        QString name;
    };
    struct Choice {
        QString label;
        QString value;
    };

    // `libraries`: the engines to search, this computer's first; none, only
    // this computer's.
    DynamicPlaylistSession(QString profile, std::vector<Library> libraries, LibrarySearch search,
                           QObject* parent = nullptr);
    ~DynamicPlaylistSession() override;

    [[nodiscard]] static std::vector<Choice> sources();
    [[nodiscard]] QStringList libraryNames() const;
    [[nodiscard]] QString libraryKey(int index) const;
    [[nodiscard]] int library() const { return library_; }
    // The library the results come from, and so the engine they play on.
    [[nodiscard]] EngineKey engine() const;
    // "New dynamic playlist…" first.
    [[nodiscard]] QStringList catalogNames() const;
    [[nodiscard]] int catalogIndex() const { return catalog_index_; }
    [[nodiscard]] bool catalogWritable() const { return catalog_writable_; }
    // ADR-0258: the definition shown ships with Trackknife: saving it saves
    // a copy, and it cannot be removed.
    [[nodiscard]] bool definitionShipped() const;

    // The definition shown.
    [[nodiscard]] QString name() const { return draft_.name; }
    [[nodiscard]] QString source() const { return draft_.source; }
    [[nodiscard]] QString query() const { return draft_.query; }
    [[nodiscard]] QString artist() const { return draft_.artist; }
    [[nodiscard]] QString track() const { return draft_.track; }
    [[nodiscard]] QString user() const { return draft_.user; }
    [[nodiscard]] QString tag() const { return draft_.tag; }
    [[nodiscard]] int limit() const { return draft_.limit; }
    [[nodiscard]] bool shuffle() const { return draft_.shuffle; }
    [[nodiscard]] QString groupBy() const { return draft_.group_by; }
    [[nodiscard]] int groups() const { return draft_.groups; }
    [[nodiscard]] int perGroup() const { return draft_.per_group; }
    [[nodiscard]] QString shuffleText() const;
    [[nodiscard]] QString shuffleTip() const;

    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool canRefresh() const { return authority_valid_ && !busy_; }
    [[nodiscard]] bool canOpen() const { return open_enabled_; }
    [[nodiscard]] bool authorityValid() const { return authority_valid_; }
    [[nodiscard]] const DynamicPlaylistService::Tracks& tracks() const { return tracks_; }
    [[nodiscard]] LocalListModel* results() const { return results_; }
    [[nodiscard]] QString playlistName() const { return draft_.name.trimmed(); }

    // `index` -1: this computer's.
    void chooseLibrary(int index);
    void followLibrary(const EngineKey& engine);
    void selectDefinition(int index);
    void setName(const QString& name);
    void setSource(const QString& source);
    void setQuery(const QString& query);
    void setArtist(const QString& artist);
    void setTrack(const QString& track);
    void setUser(const QString& user);
    void setTag(const QString& tag);
    void setLimit(int limit);
    void setShuffle(bool on);
    void setGroupBy(const QString& expression);
    void setGroups(int groups);
    void setPerGroup(int per_group);
    void save();
    void remove();
    void refresh();
    void stop();
    // The library searched changed: rules refresh.
    void libraryChanged();
    void invalidateAuthority();
    // While the window is shown: rules refresh now and then.
    void setShown(bool shown) { shown_ = shown; }

  signals:
    void changed();
    void catalogChanged();
    // The definitions were written: a rule saved, edited or removed.
    void definitionsSaved();
    // The results are about to be replaced, and were.
    void resultsAboutToChange();
    void resultsChanged();
    void libraryChosen(const trackknife::ui::EngineKey& engine);

  private:
    [[nodiscard]] DynamicPlaylistDefinition definition() const;
    void loadSelection();
    void discardResults();
    void edited();
    void finished(const DynamicPlaylistService::Tracks& tracks, int unmatched,
                  const QString& error);

    QString profile_;
    std::vector<Library> libraries_;
    int library_{0};
    QVector<DynamicPlaylistDefinition> definitions_;
    int catalog_index_{0};
    bool catalog_writable_{true};
    DynamicPlaylistDefinition draft_;
    QString status_;
    bool auto_refresh_{false};
    bool loading_{false};
    bool busy_{false};
    bool refresh_pending_{false};
    bool authority_valid_{true};
    bool open_enabled_{false};
    bool shown_{true};
    DynamicPlaylistService* service_;
    DynamicPlaylistService::Tracks tracks_;
    LocalListModel* results_;
    QTimer refresh_timer_;
    QTimer poll_;
};

} // namespace trackknife::bench
