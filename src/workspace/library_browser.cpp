// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/library_browser.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "uicommon/local_artwork.hpp"
#include "uicommon/local_files_mime_data.hpp"

#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int entry_role = library_entry_role;
constexpr int query_role = Qt::UserRole + 2;
constexpr int loaded_role = Qt::UserRole + 3;
constexpr int more_role = library_more_role;
// The line an empty top level shows, so its text can follow what is learned
// later about the library's folders.
constexpr int empty_state_role = Qt::UserRole + 20;

QString text(const std::string& value) { return QString::fromUtf8(value); }
std::string bytes(const QString& value) { return value.toUtf8().toStdString(); }
QString pathLabel(const std::string& value) { return text(core::display_raw_path(value)); }
QByteArray entryKey(const persistence::LibraryEntry& entry) {
    return QByteArray::number(static_cast<int>(entry.kind)) + ':' +
           QByteArray::fromStdString(entry.key);
}

// The tree: children loaded when a row opens, an album's cover and its
// quiet second line, and a drag that carries the entries.
class LibraryModel final : public QStandardItemModel {
  public:
    LibraryModel(LibraryBrowser* browser, std::function<void(const QModelIndex&)> fetch)
        : QStandardItemModel(browser), browser_(browser), fetch_(std::move(fetch)) {}
    bool canFetchMore(const QModelIndex& parent) const override {
        return parent.isValid() && parent.data(query_role).isValid() &&
               !parent.data(loaded_role).toBool();
    }
    bool hasChildren(const QModelIndex& parent = {}) const override {
        return canFetchMore(parent) || QStandardItemModel::hasChildren(parent);
    }
    void fetchMore(const QModelIndex& parent) override {
        if (canFetchMore(parent))
            fetch_(parent);
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (role == library_secondary_role) {
            // Text, empty where there is none: a view binding it as a
            // string must not be handed nothing.
            const auto value = QStandardItemModel::data(index, entry_role);
            if (!value.isValid())
                return QString{};
            const auto entry = value.value<persistence::LibraryEntry>();
            // An artist's album count is the row's quiet count instead.
            if (entry.kind == persistence::LibraryEntryKind::artist)
                return QString{};
            const auto tracks =
                LibraryBrowser::tr("%1 track%2").arg(entry.tracks).arg(entry.tracks == 1U ? "" : "s");
            // Under its artist, an album need not name them again.
            if (entry.kind == persistence::LibraryEntryKind::album)
                return index.parent().isValid() ? tracks
                                                : text(entry.artist) + QStringLiteral(" · ") + tracks;
            return QString{};
        }
        if (role == library_kind_role || role == library_count_role ||
            role == library_rating_role || role == library_available_role) {
            const auto value = QStandardItemModel::data(index, entry_role);
            if (!value.isValid()) {
                return role == library_kind_role || role == library_count_role ? QVariant{QString{}}
                       : role == library_available_role ? QVariant{false}
                                                        : QVariant{0U};
            }
            const auto entry = value.value<persistence::LibraryEntry>();
            switch (role) {
            case library_kind_role:
                return entry.kind == persistence::LibraryEntryKind::artist  ? QStringLiteral("artist")
                       : entry.kind == persistence::LibraryEntryKind::album ? QStringLiteral("album")
                                                                            : QStringLiteral("track");
            case library_count_role:
                return entry.kind == persistence::LibraryEntryKind::artist
                           ? QString::number(entry.albums)
                           : QString{};
            case library_rating_role:
                return entry.rating;
            default:
                return entry.available > 0U;
            }
        }
        if (role == library_cover_key_role || role == Qt::DecorationRole) {
            const auto value = QStandardItemModel::data(index, entry_role);
            if (value.isValid()) {
                const auto entry = value.value<persistence::LibraryEntry>();
                if (entry.kind == persistence::LibraryEntryKind::album) {
                    const auto key = text(entry.key);
                    if (role == library_cover_key_role) {
                        return key;
                    }
                    if (browser_ && browser_->hasCover(key)) {
                        const auto cover = browser_->cover(key);
                        if (!cover.isNull()) {
                            return QIcon{QPixmap::fromImage(cover)};
                        }
                    }
                }
            }
            if (role == library_cover_key_role) {
                return QString{};
            }
        }
        return QStandardItemModel::data(index, role);
    }
    QHash<int, QByteArray> roleNames() const override {
        auto names = QStandardItemModel::roleNames();
        names.insert(library_kind_role, "kind");
        names.insert(library_count_role, "count");
        names.insert(library_rating_role, "rating");
        names.insert(library_available_role, "available");
        names.insert(library_secondary_role, "secondary");
        names.insert(library_cover_key_role, "coverKey");
        names.insert(library_more_role, "more");
        names.insert(Qt::ToolTipRole, "tooltip");
        return names;
    }
    QStringList mimeTypes() const override { return {ui::LocalFilesMimeData::mimeType()}; }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        auto entries = LibraryBrowser::selectedEntries(indexes);
        if (entries.empty() || entries.size() > 1'000U ||
            std::ranges::none_of(entries, [](const auto& entry) { return entry.available > 0U; })) {
            return nullptr;
        }
        const auto engine = browser_ ? browser_->engine() : EngineKey::local();
        // The entries themselves too, for a place that wants tagged rows
        // rather than paths: Up Next.
        QVariantList carried;
        for (const auto& entry : entries) {
            carried.push_back(QVariant::fromValue(entry));
        }
        auto* mime = new ui::LocalFilesMimeData{
            [browser = browser_, entries = std::move(entries)](
                ui::LocalFilesMimeData::Completion done) {
                if (browser) {
                    browser->resolveEntries(entries, std::move(done));
                }
            },
            engine};
        mime->setProperty(library_entries_property, carried);
        return mime;
    }

  private:
    QPointer<LibraryBrowser> browser_;
    std::function<void(const QModelIndex&)> fetch_;
};

} // namespace

std::vector<persistence::LibraryEntry> LibraryBrowser::selectedEntries(QModelIndexList indexes) {
    if (indexes.size() > 1'000) {
        return {};
    }
    // Tree order, independent of Ctrl-click order. Parent selections subsume
    // selected descendants; search album/track overlap is deduplicated by path.
    const auto position = [](QModelIndex index) {
        std::vector<int> rows;
        while (index.isValid()) {
            rows.push_back(index.row());
            index = index.parent();
        }
        std::ranges::reverse(rows);
        return rows;
    };
    std::ranges::sort(indexes,
                      [&](const auto& a, const auto& b) { return position(a) < position(b); });
    std::vector<persistence::LibraryEntry> entries;
    for (const auto& index : indexes) {
        if (index.column() != 0 || !index.data(entry_role).isValid()) {
            continue;
        }
        bool covered = false;
        for (auto parent = index.parent(); parent.isValid(); parent = parent.parent()) {
            if (indexes.contains(parent) && parent.data(entry_role).isValid()) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            entries.push_back(index.data(entry_role).value<persistence::LibraryEntry>());
        }
    }
    return entries;
}

LibraryBrowser::LibraryBrowser(const CatalogueSource& catalogues, EngineKey engine,
                               QObject* parent)
    : QObject(parent), catalogues_(&catalogues), engine_(std::move(engine)) {
    pool_.setMaxThreadCount(2);
    artwork_pool_.setMaxThreadCount(1);
    status_ = tr("Press Refresh to scan your music folders.");
    query_mode_ = QSettings{}.value(QStringLiteral("library/query-mode"), false).toBool();
    newest_first_ = QSettings{}.value(QStringLiteral("library/newest-first"), false).toBool();
    model_ = new LibraryModel(this, [this](const QModelIndex& index) { fetch(index); });
    search_timer_.setSingleShot(true);
    search_timer_.setInterval(200);
    connect(&search_timer_, &QTimer::timeout, this, &LibraryBrowser::reload);
    connect(&query_watcher_, &QFutureWatcherBase::finished, this, [this] {
        querying_ = false;
        auto outcome = query_watcher_.result();
        auto done = std::exchange(completion_, {});
        if (!stopped_) {
            // Every query may have reconnected, or found the engine gone, so
            // the label says which after each one rather than only at start.
            refreshSourceLabel();
            if (done) {
                done(std::move(outcome));
            }
            pump();
        }
    });
    connect(&scan_watcher_, &QFutureWatcherBase::finished, this, [this] {
        scanning_ = false;
        emit scanningChanged();
        poll_timer_.stop();
        if (stopped_) {
            return;
        }
        const auto outcome = scan_watcher_.result();
        if (!outcome.error.isEmpty()) {
            setStatus(outcome.error);
        } else if (outcome.result.cancelled) {
            setStatus(tr("Scan stopped. Completed updates were kept."));
        } else if (outcome.result.incomplete) {
            setStatus(tr("Scan incomplete. %1 files could not be read; previous entries were kept.")
                          .arg(progress_->failed.load()));
        } else {
            const auto updated = progress_->indexed.load();
            setStatus(updated == 0U   ? tr("Library up to date.")
                      : updated == 1U ? tr("Library up to date. 1 file updated.")
                                      : tr("Library up to date. %1 files updated.").arg(updated));
        }
        invalidateCovers();
        reload();
        loadRoots();
        emit libraryContentChanged();
    });
    poll_timer_.setInterval(200);
    connect(&poll_timer_, &QTimer::timeout, this, &LibraryBrowser::updateProgress);
    change_timer_.setSingleShot(true);
    change_timer_.setInterval(250);
    connect(&change_timer_, &QTimer::timeout, this, [this] {
        invalidateCovers();
        reload();
        loadRoots();
        emit libraryContentChanged();
    });
    connect(&artwork_watcher_, &QFutureWatcherBase::finished, this, [this] {
        artwork_running_ = false;
        if (stopped_) {
            return;
        }
        if (artwork_job_generation_ == artwork_generation_ &&
            !artwork_cancellation_.is_cancellation_requested()) {
            artwork_cache_.insert(artwork_key_, new QImage(artwork_watcher_.result()));
            emit coverLoaded(artwork_key_);
        }
        pumpCovers();
    });
    reload();
    loadRoots();
    refreshSourceLabel();
    // Said once in the status line too, where a user is actually looking when
    // a folder they added does not appear. The source label keeps saying it
    // afterwards.
    if (catalogues_ != nullptr && !catalogues_->usingEngine()) {
        QTimer::singleShot(0, this, [this] { setStatus(catalogues_->describe()); });
    }
}

LibraryBrowser::~LibraryBrowser() { stop(); }

QString LibraryBrowser::name() const {
    return catalogues_ != nullptr ? catalogues_->name() : QString{};
}

void LibraryBrowser::setStatus(const QString& status) {
    if (status_ != status) {
        status_ = status;
        emit statusChanged();
    }
}

void LibraryBrowser::setQueryError(const QString& error) {
    if (query_error_ != error) {
        query_error_ = error;
        emit queryErrorChanged();
    }
}

// ADR-0220: which library this is. Three distinct states, and the difference
// between the last two is exactly what was previously invisible -- a
// configured engine that is not answering looks identical to no engine at
// all.
void LibraryBrowser::refreshSourceLabel() {
    if (catalogues_ == nullptr) {
        return;
    }
    const auto described = catalogues_->describe();
    const auto shown = !catalogues_->reachable();
    const auto tooltip =
        catalogues_->usingEngine()
            ? tr("Folders, scanning, search and covers come from that engine.")
            : tr("Choose an engine in Settings → Library, or leave it empty to use this "
                 "computer's. It is tried again on the next library action.");
    if (described != source_ || shown != source_shown_ || tooltip != source_tooltip_) {
        source_ = described;
        source_shown_ = shown;
        source_tooltip_ = tooltip;
        emit sourceChanged();
    }
}

void LibraryBrowser::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    lifetime_cancellation_.request_cancellation();
    view_cancellation_.request_cancellation();
    scan_cancellation_.request_cancellation();
    artwork_cancellation_.request_cancellation();
    search_timer_.stop();
    poll_timer_.stop();
    change_timer_.stop();
    tasks_.clear();
    pool_.waitForDone();
    artwork_pool_.waitForDone();
}

void LibraryBrowser::enqueue(Task task) {
    if (stopped_) {
        return;
    }
    if (tasks_.size() >= 64U) {
        setStatus(tr("Please wait for the current library requests."));
        return;
    }
    tasks_.push_back(std::move(task));
    pump();
}

void LibraryBrowser::pump() {
    if (querying_ || tasks_.empty() || stopped_) {
        return;
    }
    auto task = std::move(tasks_.front());
    tasks_.pop_front();
    completion_ = std::move(task.done);
    querying_ = true;
    query_watcher_.setFuture(
        QtConcurrent::run(&pool_, [catalogues = catalogues_, work = std::move(task.work)] {
            // ADR-0220: the task is given the core's front door, never the
            // database, and never learns which side of a socket it is on.
            auto catalogue = catalogues->open();
            return work(*catalogue);
        }));
}

void LibraryBrowser::setSearch(const QString& text) {
    if (text == search_) {
        return;
    }
    search_ = text;
    emit searchChanged();
    setStatus(tr("Searching…"));
    search_timer_.start();
}

void LibraryBrowser::setQueryMode(const bool enabled) {
    if (enabled == query_mode_) {
        return;
    }
    query_mode_ = enabled;
    QSettings{}.setValue(QStringLiteral("library/query-mode"), enabled);
    setQueryError({});
    emit searchChanged();
    reload();
}

void LibraryBrowser::setNewestFirst(const bool on) {
    if (on == newest_first_) {
        return;
    }
    newest_first_ = on;
    QSettings{}.setValue(QStringLiteral("library/newest-first"), on);
    emit searchChanged();
    reload();
}

void LibraryBrowser::locatePath(std::string raw_path, bool album) {
    search_timer_.stop();
    ++generation_;
    const auto generation = generation_;
    setStatus(tr("Locating in library…"));
    enqueue(
        {[raw_path = std::move(raw_path),
          cancellation = lifetime_cancellation_.token()](engine::Catalogue& library) {
             Outcome outcome;
             persistence::LibraryQuery query;
             query.kind = persistence::LibraryEntryKind::album;
             query.raw_path = raw_path;
             auto page = library.query(query, cancellation);
             if (page)
                 outcome.page = std::move(*page);
             else
                 outcome.error = text(page.error().message);
             return outcome;
         },
         [this, generation, album](Outcome outcome) {
             // A failed or empty lookup is a fact about the queried path, true
             // however the tree changed while the query ran. Reported before
             // the staleness check, which guards the navigation below.
             if (!outcome.error.isEmpty()) {
                 setStatus(outcome.error);
                 return;
             }
             if (outcome.page.entries.empty()) {
                 setStatus(!engine_.isLocal()
                               ? tr("This file is not in this library yet; it is found after the "
                                    "engine's next scan.")
                               : tr("This file is not in the local library. Add its folder and "
                                    "Refresh first."));
                 return;
             }
             if (generation != generation_)
                 return;
             auto entry = outcome.page.entries.front();
             search_.clear();
             // Found under its artist, so the artist tree, not the newest.
             if (newest_first_) {
                 newest_first_ = false;
                 QSettings{}.setValue(QStringLiteral("library/newest-first"), false);
             }
             emit searchChanged();
             expanded_entries_.clear();
             current_entry_.clear();
             reload();
             locate_artist_ = entry.artist;
             if (!album) {
                 entry.kind = persistence::LibraryEntryKind::artist;
                 entry.key = entry.artist;
             }
             locate_target_ = std::move(entry);
         }});
}

QString LibraryBrowser::emptyLibraryText() const {
    if (!has_roots_) {
        return tr("The library is empty");
    }
    return *has_roots_ ? tr("Nothing indexed yet — press Refresh to scan your folders")
                       : tr("No music folders yet — choose Folders… to add one");
}

void LibraryBrowser::reload() {
    locate_target_.reset();
    ++generation_;
    artwork_cancellation_.request_cancellation();
    view_cancellation_.request_cancellation();
    view_cancellation_ = core::CancellationSource{};
    std::erase_if(tasks_, [](const Task& task) { return task.view_query; });
    if (previous_search_ != search_) {
        previous_search_ = search_;
        expanded_entries_.clear();
        current_entry_.clear();
    }
    emit reloadStarted();
    model_->clear();
    const auto query_text = bytes(search_.trimmed());
    if (query_text.empty()) {
        setQueryError({});
        // Recently added: albums newest first, where artists would be.
        if (newest_first_) {
            persistence::LibraryQuery newest;
            newest.kind = persistence::LibraryEntryKind::album;
            newest.newest_first = true;
            newest.limit = 500;
            loadChildren({}, newest);
            return;
        }
        loadChildren({}, {});
        return;
    }
    // ADR-0150: in query mode the text compiles as tkq; a malformed query
    // is a visible error and never degrades into the word search below.
    if (query_mode_) {
        auto compiled = query::compile_tkq(query_text);
        if (!compiled) {
            setQueryError(text(compiled.error().message));
            setStatus(tr("Invalid query"));
            return;
        }
        setQueryError({});
        auto* group = new QStandardItem(tr("Tracks"));
        group->setEditable(false);
        group->setDragEnabled(false);
        group->setData(true, loaded_role);
        group->setColumnCount(1);
        model_->appendRow(group);
        loadFilterChildren(QPersistentModelIndex{group->index()},
                           std::make_shared<query::CompiledTkq>(std::move(*compiled)));
        emit expandRequested(group->index());
        return;
    }
    for (const auto kind :
         {persistence::LibraryEntryKind::album, persistence::LibraryEntryKind::track}) {
        auto* group = new QStandardItem(
            kind == persistence::LibraryEntryKind::album ? tr("Albums") : tr("Tracks"));
        group->setEditable(false);
        group->setDragEnabled(false);
        group->setData(true, loaded_role);
        group->setColumnCount(1);
        model_->appendRow(group);
        persistence::LibraryQuery query;
        query.kind = kind;
        query.text = query_text;
        loadChildren(QPersistentModelIndex{group->index()}, query);
        emit expandRequested(group->index());
    }
}

void LibraryBrowser::fetch(const QModelIndex& index) {
    auto* item = model_->itemFromIndex(index);
    if (item == nullptr || item->data(loaded_role).toBool() || !item->data(query_role).isValid()) {
        return;
    }
    item->setData(true, loaded_role);
    loadChildren(QPersistentModelIndex{index},
                 item->data(query_role).value<persistence::LibraryQuery>());
}

void LibraryBrowser::noteExpanded(const QModelIndex& index, const bool expanded) {
    if (!index.data(entry_role).isValid()) {
        return;
    }
    const auto key = entryKey(index.data(entry_role).value<persistence::LibraryEntry>());
    if (expanded) {
        expanded_entries_.insert(key);
        fetch(index);
    } else {
        expanded_entries_.remove(key);
    }
}

void LibraryBrowser::noteCurrent(const QModelIndex& index) {
    if (index.data(entry_role).isValid()) {
        current_entry_ = entryKey(index.data(entry_role).value<persistence::LibraryEntry>());
    }
}

void LibraryBrowser::loadChildren(const QPersistentModelIndex& parent,
                                  persistence::LibraryQuery query) {
    // Browsing shows the complete level in one load -- the default 200-row
    // page left the list cut off behind a manual "Show more…" row. The bound
    // matches the tkq result cap and keeps the more-row as a never-expected
    // safety valve.
    query.limit = 100'000U;
    const auto generation = generation_;
    const auto root = !parent.isValid();
    enqueue(
        {[query, cancellation = view_cancellation_.token()](engine::Catalogue& library) {
             Outcome outcome;
             const auto result = library.query(query, cancellation);
             if (result) {
                 outcome.page = *result;
             } else {
                 outcome.error = text(result.error().message);
             }
             return outcome;
         },
         [this, parent, root, query, generation](Outcome outcome) mutable {
             if (generation != generation_ || (!root && !parent.isValid())) {
                 return;
             }
             auto* target = root ? model_->invisibleRootItem() : model_->itemFromIndex(parent);
             if (target == nullptr) {
                 return;
             }
             if (!outcome.error.isEmpty()) {
                 setStatus(outcome.error);
                 target->setData(false, loaded_role);
                 return;
             }
             if (query.offset == 0U) {
                 target->removeRows(0, target->rowCount());
             }
             if (!scanning_ && status_ == tr("Searching…")) {
                 setStatus(search_.trimmed().isEmpty() ? tr("Browse artists and albums.")
                                                       : tr("Search results"));
             }
             for (const auto& entry : outcome.page.entries) {
                 auto label = text(entry.label);
                 if (entry.available == 0U) {
                     label += tr(" — unavailable");
                 } else if (entry.available < entry.tracks) {
                     label += tr(" — %1 unavailable").arg(entry.tracks - entry.available);
                 }
                 auto* item = new QStandardItem(label);
                 item->setEditable(false);
                 item->setDragEnabled(entry.available > 0U);
                 item->setDropEnabled(false);
                 item->setIcon(QIcon::fromTheme(entry.kind == persistence::LibraryEntryKind::artist
                                                    ? QStringLiteral("avatar-default")
                                                : entry.kind == persistence::LibraryEntryKind::album
                                                    ? QStringLiteral("media-optical-audio")
                                                    : QStringLiteral("audio-x-generic")));
                 item->setData(QVariant::fromValue(entry), entry_role);
                 // A track by its file, an album by whose it is, an artist
                 // by name alone.
                 item->setToolTip(entry.kind == persistence::LibraryEntryKind::track
                                      ? pathLabel(entry.key)
                                  : entry.album.empty() ? text(entry.artist)
                                                        : text(entry.artist + " — " + entry.album));
                 if (entry.kind != persistence::LibraryEntryKind::track) {
                     persistence::LibraryQuery children;
                     if (entry.kind == persistence::LibraryEntryKind::artist) {
                         children.kind = persistence::LibraryEntryKind::album;
                         children.artist = entry.key;
                     } else {
                         children.kind = persistence::LibraryEntryKind::track;
                         children.album_key = entry.key;
                     }
                     item->setData(QVariant::fromValue(children), query_role);
                     // Its column before its rows: added with the first row,
                     // it is a column insertion under a nested row, which
                     // QML's tree adapter cannot follow.
                     item->setColumnCount(1);
                 }
                 target->appendRow(item);
                 if (locate_target_) {
                     const bool found =
                         entry.kind == locate_target_->kind && entry.key == locate_target_->key;
                     if (found) {
                         emit currentRequested(item->index(), true);
                         locate_target_.reset();
                         setStatus(tr("Located in library."));
                     }
                     if (found || (entry.kind == persistence::LibraryEntryKind::artist &&
                                   entry.key == locate_artist_)) {
                         emit expandRequested(item->index());
                     }
                 }
                 if (current_entry_ == entryKey(entry)) {
                     emit currentRequested(item->index(), false);
                 }
                 if (expanded_entries_.contains(entryKey(entry))) {
                     emit expandRequested(item->index());
                 }
             }
             if (outcome.page.more) {
                 query.offset += outcome.page.entries.size();
                 auto* more = new QStandardItem(tr("Show more…"));
                 more->setEditable(false);
                 more->setDragEnabled(false);
                 more->setData(true, more_role);
                 more->setData(QVariant::fromValue(query), query_role);
                 target->appendRow(more);
                 if (locate_target_) {
                     bool seek_more = query.kind == persistence::LibraryEntryKind::album &&
                                      query.artist == locate_artist_;
                     if (query.kind == persistence::LibraryEntryKind::artist) {
                         seek_more = true;
                         for (int row = 0; row < target->rowCount(); ++row) {
                             const auto value = target->child(row)->data(entry_role);
                             if (value.isValid() &&
                                 value.value<persistence::LibraryEntry>().key == locate_artist_) {
                                 seek_more = false;
                                 break;
                             }
                         }
                     }
                     if (seek_more) {
                         target->removeRow(more->row());
                         loadChildren(parent, query);
                     }
                 }
             } else if (target->rowCount() == 0) {
                 // Why it is empty, at the top: a search that found nothing
                 // is not a library with no folders.
                 const bool top = !parent.isValid() && query.text.empty();
                 auto* empty = new QStandardItem(top ? emptyLibraryText() : tr("No matches"));
                 empty->setEnabled(false);
                 empty->setData(top, empty_state_role);
                 target->appendRow(empty);
             }
             emit levelLoaded();
         },
         true});
}

void LibraryBrowser::loadFilterChildren(const QPersistentModelIndex& parent,
                                        std::shared_ptr<const query::CompiledTkq> compiled) {
    const auto generation = generation_;
    enqueue({[compiled, cancellation = view_cancellation_.token()](engine::Catalogue& library) {
                 Outcome outcome;
                 auto result = library.filter(*compiled, 0U, 200U, cancellation);
                 if (result) {
                     outcome.page = std::move(*result);
                 } else {
                     outcome.error = text(result.error().message);
                 }
                 return outcome;
             },
             [this, parent, generation](Outcome outcome) {
                 if (generation != generation_ || !parent.isValid()) {
                     return;
                 }
                 auto* target = model_->itemFromIndex(parent);
                 if (target == nullptr) {
                     return;
                 }
                 if (!outcome.error.isEmpty()) {
                     setStatus(outcome.error);
                     return;
                 }
                 if (!scanning_ && status_ == tr("Searching…")) {
                     setStatus(tr("Query results"));
                 }
                 for (const auto& entry : outcome.page.entries) {
                     auto* item = new QStandardItem(text(entry.label));
                     item->setEditable(false);
                     item->setDragEnabled(true);
                     item->setDropEnabled(false);
                     item->setIcon(QIcon::fromTheme(QStringLiteral("audio-x-generic")));
                     item->setData(QVariant::fromValue(entry), entry_role);
                     item->setToolTip(pathLabel(entry.key));
                     target->appendRow(item);
                 }
                 if (outcome.page.more) {
                     // The tree shows the first page; Enter keeps the complete
                     // bounded result set as a list.
                     auto* more = new QStandardItem(
                         tr("Showing the first %1 matches — press Enter to keep them all")
                             .arg(outcome.page.entries.size()));
                     more->setEnabled(false);
                     target->appendRow(more);
                 } else if (target->rowCount() == 0) {
                     auto* empty = new QStandardItem(tr("No matches"));
                     empty->setEnabled(false);
                     target->appendRow(empty);
                 }
                 emit levelLoaded();
             },
             true});
}

void LibraryBrowser::activate(const QModelIndex& index) {
    auto* item = model_->itemFromIndex(index);
    if (item == nullptr) {
        return;
    }
    if (item->data(more_role).toBool()) {
        const auto query = item->data(query_role).value<persistence::LibraryQuery>();
        const QPersistentModelIndex parent{index.parent()};
        model_->removeRow(index.row(), index.parent());
        loadChildren(parent, query);
        return;
    }
    request({index}, static_cast<int>(LocalLibraryAction::append));
}

void LibraryBrowser::request(const QModelIndexList& indexes, const int action) {
    auto entries = selectedEntries(indexes);
    if (entries.empty() && indexes.size() > 1'000) {
        setStatus(tr("Select at most 1,000 library entries."));
    }
    if (!entries.empty()) {
        emit actionRequested(std::move(entries), static_cast<LocalLibraryAction>(action));
    }
}

void LibraryBrowser::addToList(const QModelIndexList& indexes, const QString& id) {
    auto entries = selectedEntries(indexes);
    if (!entries.empty()) {
        emit addToListRequested(std::move(entries), id);
    }
}

void LibraryBrowser::requestRatings(std::vector<std::string> hashes,
                                    std::function<void(std::vector<unsigned>)> ready) {
    if (hashes.empty() || !ready) {
        return;
    }
    enqueue({.work =
                 [hashes = std::move(hashes)](engine::Catalogue& library) {
                     Outcome outcome;
                     auto ratings = library.ratings(hashes);
                     if (!ratings) {
                         outcome.error = text(ratings.error().message);
                     } else {
                         outcome.ratings = std::move(*ratings);
                     }
                     return outcome;
                 },
             .done =
                 [ready = std::move(ready)](Outcome outcome) {
                     if (outcome.error.isEmpty()) {
                         ready(std::move(outcome.ratings));
                     }
                 },
             .view_query = false});
}

void LibraryBrowser::storeRating(std::string hash, const bool album, const unsigned rating) {
    if (hash.empty()) {
        return;
    }
    enqueue({.work =
                 [hash = std::move(hash), album, rating](engine::Catalogue& library) {
                     Outcome outcome;
                     if (auto stored = library.set_rating(hash, album, rating); !stored) {
                         outcome.error = text(stored.error().message);
                     }
                     return outcome;
                 },
             .done =
                 [this](const Outcome& outcome) {
                     if (!outcome.error.isEmpty()) {
                         setStatus(outcome.error);
                         return;
                     }
                     emit ratingsChanged();
                 },
             .view_query = false});
}

void LibraryBrowser::rate(const QModelIndex& index, const int rating) {
    if (!index.data(entry_role).isValid()) {
        return;
    }
    auto entry = index.data(entry_role).value<persistence::LibraryEntry>();
    const auto value = static_cast<unsigned>(std::clamp(rating, 0, 10));
    storeRating(entry.rating_hash, entry.kind == persistence::LibraryEntryKind::album, value);
    entry.rating = value;
    model_->setData(index, QVariant::fromValue(entry), entry_role);
}

void LibraryBrowser::resolveEntries(std::vector<persistence::LibraryEntry> entries,
                                    std::function<void(std::vector<std::string>)> completion) {
    if (entries.empty() || entries.size() > 1'000U) {
        setStatus(tr("Select between 1 and 1,000 library entries."));
        return;
    }
    setStatus(tr("Loading library selection…"));
    enqueue({[entries = std::move(entries),
              cancellation = lifetime_cancellation_.token()](engine::Catalogue& library) {
                 Outcome outcome;
                 std::unordered_set<std::string> seen;
                 std::size_t resolved = 0;
                 std::size_t unavailable = 0;
                 for (const auto& entry : entries) {
                     unavailable += entry.tracks - entry.available;
                     persistence::LibraryQuery query;
                     if (entry.kind == persistence::LibraryEntryKind::artist) {
                         query.artist = entry.key;
                     } else if (entry.kind == persistence::LibraryEntryKind::album) {
                         query.album_key = entry.key;
                     } else {
                         query.raw_path = entry.key;
                     }
                     auto paths = library.paths(query, cancellation);
                     if (!paths) {
                         outcome.error = text(paths.error().message);
                         return outcome;
                     }
                     resolved += paths->size();
                     if (resolved > 100'000U) {
                         outcome.error = tr("This selection exceeds the 100,000-file limit.");
                         return outcome;
                     }
                     for (auto& path : *paths) {
                         if (seen.insert(path).second) {
                             outcome.paths.push_back(std::move(path));
                         }
                     }
                 }
                 outcome.unavailable = unavailable;
                 return outcome;
             },
             [this, completion = std::move(completion)](Outcome outcome) {
                 if (!outcome.error.isEmpty()) {
                     setStatus(outcome.error);
                     return;
                 }
                 if (outcome.paths.empty()) {
                     setStatus(tr("These files are unavailable. Reconnect the folder and refresh "
                                  "the library."));
                     return;
                 }
                 setStatus(outcome.unavailable > 0U ? tr("Unavailable files were skipped.")
                                                    : tr("Library selection loaded."));
                 completion(std::move(outcome.paths));
             }});
}

void LibraryBrowser::resolveEntryRows(std::vector<persistence::LibraryEntry> entries,
                                      std::function<void(std::vector<LocalTrackRow>)> completion) {
    resolveEntries(std::move(entries), [this, completion = std::move(completion)](
                                           std::vector<std::string> paths) {
        enqueue({[paths = std::move(paths),
                  cancellation = lifetime_cancellation_.token()](engine::Catalogue& library) {
                     Outcome outcome;
                     auto cached = library.cached_tracks(paths, cancellation);
                     if (!cached) {
                         outcome.error = text(cached.error().message);
                         return outcome;
                     }
                     for (auto& track : *cached) {
                         outcome.rows.push_back(cached_library_row(std::move(track)));
                     }
                     return outcome;
                 },
                 [this, completion](Outcome outcome) {
                     if (!outcome.error.isEmpty()) {
                         setStatus(outcome.error);
                         return;
                     }
                     completion(std::move(outcome.rows));
                 }});
    });
}

void LibraryBrowser::commitSearch() {
    const auto query_text = search_.trimmed();
    if (query_text.isEmpty()) {
        return;
    }
    setStatus(tr("Collecting search results…"));
    // ADR-0150: a committed query resolves through the structured filter;
    // the resulting list is the same ADR-0140 snapshot as a word search.
    if (query_mode_) {
        auto compiled = query::compile_tkq(bytes(query_text));
        if (!compiled) {
            setQueryError(text(compiled.error().message));
            setStatus(tr("Invalid query"));
            return;
        }
        setQueryError({});
        enqueue({[shared = std::make_shared<query::CompiledTkq>(std::move(*compiled)),
                  cancellation = lifetime_cancellation_.token()](engine::Catalogue& library) {
                     Outcome outcome;
                     auto paths = library.filter_paths(*shared, cancellation);
                     if (paths) {
                         auto cached = library.cached_tracks(*paths, cancellation);
                         if (!cached) {
                             outcome.error = text(cached.error().message);
                             return outcome;
                         }
                         for (auto& track : *cached) {
                             outcome.rows.push_back(cached_library_row(std::move(track)));
                         }
                     } else {
                         outcome.error = text(paths.error().message);
                     }
                     return outcome;
                 },
                 [this, query_text](Outcome outcome) {
                     if (!outcome.error.isEmpty()) {
                         setStatus(outcome.error);
                         return;
                     }
                     if (outcome.rows.empty()) {
                         setStatus(tr("No search results to keep."));
                         return;
                     }
                     setStatus(tr("Search kept as a new tab."));
                     emit searchCommitted(query_text, std::move(outcome.rows));
                 }});
        return;
    }
    enqueue({[query = bytes(query_text),
              cancellation = lifetime_cancellation_.token()](engine::Catalogue& library) {
                 Outcome outcome;
                 std::unordered_set<std::string> seen;
                 // Album-name matches first (whole matching albums), then the
                 // remaining track-title matches -- the tree's order.
                 for (const auto kind : {persistence::LibraryEntryKind::album,
                                         persistence::LibraryEntryKind::track}) {
                     persistence::LibraryQuery entry_query;
                     entry_query.kind = kind;
                     entry_query.text = query;
                     auto paths = library.paths(entry_query, cancellation);
                     if (!paths) {
                         outcome.error = text(paths.error().message);
                         return outcome;
                     }
                     for (auto& path : *paths) {
                         if (seen.insert(path).second) {
                             outcome.paths.push_back(std::move(path));
                         }
                     }
                 }
                 auto cached = library.cached_tracks(outcome.paths, cancellation);
                 if (!cached) {
                     outcome.error = text(cached.error().message);
                     return outcome;
                 }
                 for (auto& track : *cached) {
                     outcome.rows.push_back(cached_library_row(std::move(track)));
                 }
                 return outcome;
             },
             [this, query_text](Outcome outcome) {
                 if (!outcome.error.isEmpty()) {
                     setStatus(outcome.error);
                     return;
                 }
                 if (outcome.rows.empty()) {
                     setStatus(tr("No search results to keep."));
                     return;
                 }
                 setStatus(tr("Search kept as a new tab."));
                 emit searchCommitted(query_text, std::move(outcome.rows));
             }});
}

QVariantList LibraryBrowser::roots() const {
    QVariantList list;
    for (const auto& root : roots_) {
        list.push_back(QVariantMap{{QStringLiteral("label"), root.label},
                                   {QStringLiteral("tooltip"), root.tooltip}});
    }
    return list;
}

void LibraryBrowser::addRoot(const QString& path) {
    if (!path.trimmed().isEmpty()) {
        addRoot(QFile::encodeName(path.trimmed()).toStdString());
    }
}

void LibraryBrowser::addRoot(std::string raw_path) {
    enqueue({[raw_path = std::move(raw_path)](engine::Catalogue& library) {
                 Outcome outcome;
                 const auto result = library.add_root(raw_path);
                 if (!result) {
                     outcome.error = text(result.error().message);
                 }
                 return outcome;
             },
             [this](Outcome outcome) {
                 if (outcome.error.isEmpty()) {
                     setStatus(tr("Folder added. Press Refresh to scan for music."));
                     loadRoots();
                     refreshLibrary();
                 } else {
                     setStatus(outcome.error);
                 }
                 roots_error_ = outcome.error;
                 emit rootsChanged();
             }});
}

void LibraryBrowser::removeRoot(const int index) {
    if (index >= 0 && index < static_cast<int>(roots_.size())) {
        removeRoot(roots_[static_cast<std::size_t>(index)].raw_path);
    }
}

void LibraryBrowser::removeRoot(std::string raw_path) {
    enqueue({[raw_path = std::move(raw_path)](engine::Catalogue& library) {
                 Outcome outcome;
                 auto result = library.remove_root(raw_path);
                 if (!result) {
                     outcome.error = text(result.error().message);
                 }
                 return outcome;
             },
             [this](Outcome outcome) {
                 if (outcome.error.isEmpty()) {
                     loadRoots();
                     reload();
                 } else {
                     setStatus(outcome.error);
                 }
                 roots_error_ = outcome.error;
                 emit rootsChanged();
             }});
}

void LibraryBrowser::loadRoots() {
    enqueue({[](engine::Catalogue& library) {
                 Outcome outcome;
                 auto roots = library.roots();
                 if (roots) {
                     outcome.roots = std::move(*roots);
                 } else {
                     outcome.error = text(roots.error().message);
                 }
                 return outcome;
             },
             [this](Outcome outcome) {
                 if (!outcome.error.isEmpty()) {
                     setStatus(outcome.error);
                     return;
                 }
                 has_roots_ = !outcome.roots.empty();
                 if (outcome.roots.empty()) {
                     setStatus(tr("Choose Folders… to add your music collection."));
                 }
                 // The empty line may have been drawn before this was known.
                 for (int row = 0; row < model_->rowCount(); ++row) {
                     if (auto* item = model_->item(row);
                         item && item->data(empty_state_role).toBool()) {
                         item->setText(emptyLibraryText());
                     }
                 }
                 std::size_t offline = 0;
                 roots_.clear();
                 for (const auto& root : outcome.roots) {
                     const auto unavailable = !root.available && !root.error.empty();
                     if (unavailable) {
                         ++offline;
                     }
                     roots_.push_back(Root{
                         .raw_path = root.raw_path,
                         .label = pathLabel(root.raw_path) + (root.available ? QString{}
                                                              : unavailable ? tr(" — unavailable")
                                                                            : tr(" — not scanned")),
                         .tooltip = text(root.error)});
                 }
                 emit rootsChanged();
                 if (offline > 0U && !scanning_) {
                     setStatus(tr("%1 folders unavailable. Cached music is still shown.").arg(offline));
                 }
             }});
}

void LibraryBrowser::refreshLibrary() {
    if (!stopped_) {
        change_timer_.start();
    }
}

void LibraryBrowser::toggleScan() {
    if (scanning_) {
        scan_cancellation_.request_cancellation();
        change_timer_.stop();
        setStatus(tr("Stopping scan…"));
    } else {
        startScan();
    }
}

void LibraryBrowser::startScan() {
    if (stopped_ || scanning_) {
        return;
    }
    scanning_ = true;
    emit scanningChanged();
    scan_cancellation_ = core::CancellationSource{};
    progress_ = std::make_shared<persistence::LibraryScanProgress>();
    poll_timer_.start();
    updateProgress();
    scan_watcher_.setFuture(QtConcurrent::run(&pool_, [catalogues = catalogues_,
                                                       cancellation = scan_cancellation_.token(),
                                                       progress = progress_] {
        ScanOutcome outcome;
        // ADR-0220: ask the core, do not open its database. Remotely it
        // becomes a job, and the same counters are fed from its progress.
        auto catalogue = catalogues->open();
        auto result = catalogue->scan(cancellation, *progress);
        if (result) {
            outcome.result = *result;
        } else {
            outcome.error = text(result.error().message);
        }
        return outcome;
    }));
}

void LibraryBrowser::updateProgress() {
    if (!progress_) {
        return;
    }
    if (scan_cancellation_.is_cancellation_requested()) {
        setStatus(tr("Stopping scan…"));
        return;
    }
    setStatus(tr("Scanning… %1 entries checked, %2 files updated, %3 unreadable")
                  .arg(progress_->visited.load())
                  .arg(progress_->indexed.load())
                  .arg(progress_->failed.load()));
}

// --- Covers -------------------------------------------------------------------

QImage LibraryBrowser::cover(const QString& album_key) const {
    const auto* image = artwork_cache_.object(album_key);
    return image != nullptr ? *image : QImage{};
}

bool LibraryBrowser::hasCover(const QString& album_key) const {
    return artwork_cache_.contains(album_key);
}

void LibraryBrowser::invalidateCovers() {
    ++artwork_generation_;
    artwork_cancellation_.request_cancellation();
    artwork_cache_.clear();
    emit coverLoaded({});
}

void LibraryBrowser::wantCovers(const QStringList& album_keys) {
    wanted_covers_ = album_keys;
    if (artwork_running_) {
        // A cover no longer in view is not waited for.
        if (!wanted_covers_.contains(artwork_key_)) {
            artwork_cancellation_.request_cancellation();
        }
        return;
    }
    pumpCovers();
}

void LibraryBrowser::pumpCovers() {
    if (stopped_ || artwork_running_) {
        return;
    }
    for (const auto& key : wanted_covers_) {
        if (artwork_cache_.contains(key)) {
            continue;
        }
        artwork_key_ = key;
        artwork_job_generation_ = artwork_generation_;
        artwork_cancellation_ = core::CancellationSource{};
        artwork_running_ = true;
        artwork_watcher_.setFuture(
            QtConcurrent::run(&artwork_pool_, [catalogues = catalogues_, key = key.toStdString(),
                                               cancellation = artwork_cancellation_.token()] {
                if (cancellation.is_cancellation_requested()) {
                    return QImage{};
                }
                // ADR-0220: ask the core, do not open its database. Read by
                // the engine, where the files are: a remote library shows its
                // covers with nothing of it mounted here.
                auto catalogue = catalogues->open();
                const auto source = catalogue->artwork_source(key, cancellation);
                if (!source || !source->has_value() || cancellation.is_cancellation_requested()) {
                    return QImage{};
                }
                const auto bytes = catalogue->artwork(**source, cancellation);
                return bytes ? ui::artworkThumbnail(*bytes) : QImage{};
            }));
        return;
    }
}

} // namespace trackknife::bench
