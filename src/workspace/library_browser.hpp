// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/catalogue_source.hpp"
#include "bench/engine_key.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/query/tkq.hpp"

#include <QCache>
#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QStandardItemModel>
#include <QThreadPool>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// The tree's items carry their persistence::LibraryEntry under this role.
inline constexpr int library_entry_role = Qt::UserRole + 1;
// The album key an album row's cover is found by (a QString), for a view
// that draws covers itself.
inline constexpr int library_cover_key_role = Qt::UserRole + 21;
// A row's quiet second text: an album's artist and track count.
inline constexpr int library_secondary_role = Qt::UserRole + 6;
// "Show more…": activating it loads the next page.
inline constexpr int library_more_role = Qt::UserRole + 4;
// For a view that draws rows itself: "artist", "album", "track" or none; an
// artist's album count; the album's or track's rating; whether any of it is
// there to play.
inline constexpr int library_kind_role = Qt::UserRole + 23;
inline constexpr int library_count_role = Qt::UserRole + 24;
inline constexpr int library_rating_role = Qt::UserRole + 25;
inline constexpr int library_available_role = Qt::UserRole + 26;

// Set on a library drag's data: the dragged entries, as a QVariantList of
// persistence::LibraryEntry.
inline constexpr const char* library_entries_property = "trackknife-library-entries";

enum class LocalLibraryAction { append, next, replace, new_list, request_next, request_end };

// One engine's library as the Sources panel browses it (ADR-0220, ADR-0234):
// artists, their albums and tracks loaded a level at a time; a word search or
// a tkq query; albums newest first; the library's folders and a scan of them;
// covers for the albums in view. Everything is asked of the engine's
// catalogue, one request at a time, off the UI thread. The window's library
// panel draws it; the view reports what it shows and the browser says what
// to expand and where the cursor goes.
class LibraryBrowser final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString queryError READ queryError NOTIFY queryErrorChanged)
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)
    Q_PROPERTY(bool sourceShown READ sourceShown NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceTooltip READ sourceTooltip NOTIFY sourceChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool queryMode READ queryMode WRITE setQueryMode NOTIFY searchChanged)
    Q_PROPERTY(bool newestFirst READ newestFirst WRITE setNewestFirst NOTIFY searchChanged)
    Q_PROPERTY(QString viewId READ viewId WRITE setViewId NOTIFY searchChanged)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
    Q_PROPERTY(QVariantList roots READ roots NOTIFY rootsChanged)
    Q_PROPERTY(QString rootsError READ rootsError NOTIFY rootsChanged)

  public:
    struct Root {
        std::string raw_path;
        QString label;
        QString tooltip;
    };

    explicit LibraryBrowser(const CatalogueSource& catalogues, EngineKey engine,
                            QObject* parent = nullptr);
    ~LibraryBrowser() override;

    [[nodiscard]] const EngineKey& engine() const noexcept { return engine_; }
    void setEngine(EngineKey engine) { engine_ = std::move(engine); }
    [[nodiscard]] const CatalogueSource* catalogues() const { return catalogues_; }
    [[nodiscard]] QStandardItemModel* model() const { return model_; }

    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString queryError() const { return query_error_; }
    [[nodiscard]] QString source() const { return source_; }
    [[nodiscard]] bool sourceShown() const { return source_shown_; }
    [[nodiscard]] QString sourceTooltip() const { return source_tooltip_; }
    [[nodiscard]] bool scanning() const { return scanning_; }
    [[nodiscard]] bool queryMode() const { return query_mode_; }
    [[nodiscard]] bool newestFirst() const { return newest_first_; }
    // ADR-0254: the library view shown, by its definition's id.
    [[nodiscard]] QString viewId() const { return view_id_; }
    [[nodiscard]] QString search() const { return search_; }
    [[nodiscard]] QVariantList roots() const;
    [[nodiscard]] const std::vector<Root>& rootList() const { return roots_; }
    [[nodiscard]] QString rootsError() const { return roots_error_; }
    // Whether it is this computer's library, or that of an engine elsewhere,
    // whose folders are typed as that machine sees them.
    [[nodiscard]] Q_INVOKABLE bool remote() const {
        return catalogues_ != nullptr && catalogues_->role() == CatalogueSource::Role::remote;
    }
    [[nodiscard]] Q_INVOKABLE QString name() const;
    [[nodiscard]] Q_INVOKABLE QString engineText() const { return engine_.text(); }

    // The search, as typed: it is looked up a moment after the last key.
    void setSearch(const QString& text);
    void setQueryMode(bool enabled);
    void setNewestFirst(bool on);
    // Shows the view `id`; one that is gone shows the artist tree.
    void setViewId(const QString& id);
    // The views were edited: the one shown is read again.
    void refreshViews();
    // Shows `levels`, or the view `id`, without choosing it: an editor's
    // preview, which the next start does not show.
    void previewLevels(std::vector<persistence::LibraryViewLevel> levels);
    void previewView(const QString& id);
    // ADR-0140: the whole result of the search, kept as a list
    // (searchCommitted). Enter does it.
    Q_INVOKABLE void commitSearch();
    // Everything looked up again, a moment from now; a scan is the Refresh
    // button's.
    Q_INVOKABLE void refreshLibrary();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void toggleScan();
    void startScan();
    void stop();

    // A row's children, when it is expanded; the next page, for "Show more…".
    Q_INVOKABLE void fetch(const QModelIndex& index);
    Q_INVOKABLE void activate(const QModelIndex& index);
    // The view says what it shows, so a reload shows it again: what is
    // expanded, and where its cursor is.
    Q_INVOKABLE void noteExpanded(const QModelIndex& index, bool expanded);
    Q_INVOKABLE void noteCurrent(const QModelIndex& index);
    // The album rows in view, whose covers are to be loaded.
    Q_INVOKABLE void wantCovers(const QStringList& album_keys);
    [[nodiscard]] QImage cover(const QString& album_key) const;
    [[nodiscard]] bool hasCover(const QString& album_key) const;
    void invalidateCovers();

    // A file's album, or its artist, found in the tree and shown.
    void locatePath(std::string raw_path, bool album);
    // Entries -- artists, albums, tracks -- as the paths of their tracks,
    // or as rows built from the engine's index (ADR-0227).
    void resolveEntries(std::vector<persistence::LibraryEntry> entries,
                        std::function<void(std::vector<std::string>)> completion);
    void resolveEntryRows(std::vector<persistence::LibraryEntry> entries,
                          std::function<void(std::vector<LocalTrackRow>)> completion);
    // Entries asked for from the rows a view has selected: in tree order,
    // those under a selected row left to it; at most 1,000.
    [[nodiscard]] static std::vector<persistence::LibraryEntry>
    selectedEntries(QModelIndexList indexes);
    // The entries of `indexes`, asked for as `action` (actionRequested),
    // appended to the list with `id` (addToListRequested), or made a new list
    // called `name` (newListRequested).
    Q_INVOKABLE void request(const QModelIndexList& indexes, int action);
    Q_INVOKABLE void addToList(const QModelIndexList& indexes, const QString& id);
    Q_INVOKABLE void addToNewList(const QModelIndexList& indexes, const QString& name);
    // What a new list of `indexes` is called unless named: the one entry's
    // label, or "Library selection".
    [[nodiscard]] Q_INVOKABLE static QString newListName(const QModelIndexList& indexes);

    // ADR-0179: ratings by content identity, on the library's queue.
    void requestRatings(std::vector<std::string> hashes,
                        std::function<void(std::vector<unsigned>)> ready);
    void storeRating(std::string hash, bool album, unsigned rating);
    // A row's rating, stored and shown.
    Q_INVOKABLE void rate(const QModelIndex& index, int rating);

    // The library's folders.
    Q_INVOKABLE void addRoot(const QString& path);
    void addRoot(std::string raw_path);
    void removeRoot(std::string raw_path);
    Q_INVOKABLE void removeRoot(int index);
    Q_INVOKABLE void loadRoots();

  signals:
    void statusChanged();
    void queryErrorChanged();
    void sourceChanged();
    void scanningChanged();
    void searchChanged();
    void rootsChanged();
    // For the view: an index to open, or to make its cursor (and focus the
    // tree when `focus`); a reload began, or a level arrived.
    void expandRequested(const QModelIndex& index);
    void currentRequested(const QModelIndex& index, bool focus);
    void reloadStarted();
    void levelLoaded();
    void coverLoaded(const QString& album_key);
    // What the window is asked to do with entries.
    void actionRequested(std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action);
    void addToListRequested(std::vector<persistence::LibraryEntry> entries, const QString& id);
    void newListRequested(std::vector<persistence::LibraryEntry> entries, const QString& name);
    void searchCommitted(QString query, std::vector<LocalTrackRow> rows);
    void ratingsChanged();
    void libraryContentChanged();

  private:
    struct Outcome {
        persistence::LibraryPage page;
        std::vector<persistence::LibraryRoot> roots;
        std::vector<std::string> paths;
        std::vector<LocalTrackRow> rows;
        std::vector<unsigned> ratings;
        QString error;
        std::size_t unavailable{0};
    };
    struct Task {
        // ADR-0220: queued work is handed the core's front door, not the
        // database.
        std::function<Outcome(engine::Catalogue&)> work;
        std::function<void(Outcome)> done;
        bool view_query{false};
    };
    struct ScanOutcome {
        persistence::LibraryScanResult result;
        QString error;
    };

    void enqueue(Task task);
    void pump();
    void loadChildren(const QPersistentModelIndex& parent, persistence::LibraryQuery query);
    void loadFilterChildren(const QPersistentModelIndex& parent,
                            std::shared_ptr<const query::CompiledTkq> compiled);
    void refreshSourceLabel();
    void updateProgress();
    void pumpCovers();
    void setStatus(const QString& status);
    void setQueryError(const QString& error);
    [[nodiscard]] QString emptyLibraryText() const;

    const CatalogueSource* catalogues_{nullptr};
    EngineKey engine_;
    QStandardItemModel* model_{nullptr};
    QThreadPool pool_;
    QFutureWatcher<Outcome> query_watcher_;
    QFutureWatcher<ScanOutcome> scan_watcher_;
    QThreadPool artwork_pool_;
    QFutureWatcher<QImage> artwork_watcher_;
    core::CancellationSource artwork_cancellation_;
    QCache<QString, QImage> artwork_cache_{256};
    QString artwork_key_;
    QStringList wanted_covers_;
    std::size_t artwork_generation_{0};
    std::size_t artwork_job_generation_{0};
    bool artwork_running_{false};
    std::deque<Task> tasks_;
    std::function<void(Outcome)> completion_;
    core::CancellationSource lifetime_cancellation_;
    core::CancellationSource view_cancellation_;
    core::CancellationSource scan_cancellation_;
    std::shared_ptr<persistence::LibraryScanProgress> progress_;
    QTimer search_timer_;
    QTimer poll_timer_;
    QTimer change_timer_;
    QString status_;
    QString query_error_;
    QString source_;
    QString source_tooltip_;
    bool source_shown_{false};
    QString search_;
    QString previous_search_;
    bool query_mode_{false};
    bool newest_first_{false};
    QString view_id_;
    // The shown view's levels; none for the artist tree, Recently added and
    // Folders.
    std::vector<persistence::LibraryViewLevel> view_;
    bool folders_{false};
    // Settles view_id_, view_ and newest_first_ on `id`, saving the choice
    // when `remember`.
    void adoptView(const QString& id, bool remember = true);
    std::optional<bool> has_roots_;
    std::vector<Root> roots_;
    QString roots_error_;
    std::size_t generation_{0};
    QSet<QByteArray> expanded_entries_;
    QByteArray current_entry_;
    std::optional<persistence::LibraryEntry> locate_target_;
    std::string locate_artist_;
    bool querying_{false};
    bool scanning_{false};
    bool stopped_{false};
};

} // namespace trackknife::bench

Q_DECLARE_METATYPE(trackknife::bench::LocalLibraryAction)
