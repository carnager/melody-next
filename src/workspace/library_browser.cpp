// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/library_browser.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "uicommon/local_artwork.hpp"
#include "uicommon/local_files_mime_data.hpp"
#include "workspace/library_view_definitions.hpp"

#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
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
// Its row was said by describeItem() -- not by a search's flat results.
constexpr int described_role = Qt::UserRole + 21;

QString text(const std::string& value) { return QString::fromUtf8(value); }
std::string bytes(const QString& value) { return value.toUtf8().toStdString(); }
QString pathLabel(const std::string& value) { return text(core::display_raw_path(value)); }
QByteArray entryKey(const persistence::LibraryEntry& entry) {
    auto key = QByteArray::number(static_cast<int>(entry.kind)) + ':';
    // A view's node by where it is: one artist under two genres is two rows.
    if (entry.view_node) {
        if (entry.view_node->folder) {
            return key + QByteArray::fromStdString(*entry.view_node->folder);
        }
        for (const auto& label : entry.view_node->view_path) {
            key += QByteArray::fromStdString(label) + '\x1f';
        }
        return key;
    }
    return key + QByteArray::fromStdString(entry.key);
}

// The tree: children loaded when a row opens, an album's cover and its
// quiet second line, and a drag that carries the entries.
class LibraryModel final : public LibraryTreeModel {
  public:
    LibraryModel(LibraryBrowser* browser, std::function<void(const QModelIndex&)> fetch)
        : LibraryTreeModel(browser), browser_(browser), fetch_(std::move(fetch)) {}
    void fetchMore(const QModelIndex& parent) override {
        if (canFetchMore(parent))
            fetch_(parent);
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        const auto* row = index.isValid() ? rowAt(index) : nullptr;
        if (row == nullptr) {
            return {};
        }
        if (role == library_secondary_role) {
            // Text, empty where there is none: a view binding it as a
            // string must not be handed nothing.
            if (!row->entry)
                return QString{};
            const auto& entry = *row->entry;
            // An artist's album count is the row's quiet count instead, and
            // so is a view's group's.
            if (entry.kind == persistence::LibraryEntryKind::artist ||
                entry.kind == persistence::LibraryEntryKind::group)
                return QString{};
            const auto tracks = LibraryBrowser::tr("%1 track%2")
                                    .arg(entry.tracks)
                                    .arg(entry.tracks == 1U ? "" : "s");
            // Under its artist, an album need not name them again.
            if (entry.kind == persistence::LibraryEntryKind::album)
                return index.parent().isValid()
                           ? tracks
                           : text(entry.artist) + QStringLiteral(" · ") + tracks;
            return QString{};
        }
        if (role == library_kind_role || role == library_count_role ||
            role == library_rating_role || role == library_available_role) {
            if (!row->entry) {
                return role == library_kind_role || role == library_count_role ? QVariant{QString{}}
                       : role == library_available_role                        ? QVariant{false}
                                                                               : QVariant{0U};
            }
            const auto& entry = *row->entry;
            switch (role) {
            case library_kind_role:
                return entry.kind == persistence::LibraryEntryKind::artist
                           ? QStringLiteral("artist")
                       : entry.kind == persistence::LibraryEntryKind::album
                           ? QStringLiteral("album")
                       : entry.kind == persistence::LibraryEntryKind::group
                           ? QStringLiteral("group")
                           : QStringLiteral("track");
            case library_count_role:
                return entry.kind == persistence::LibraryEntryKind::artist ||
                               entry.kind == persistence::LibraryEntryKind::group
                           ? QString::number(entry.albums)
                           : QString{};
            case library_rating_role:
                return entry.rating;
            default:
                return entry.available > 0U;
            }
        }
        if (role == library_cover_key_role || role == Qt::DecorationRole) {
            if (row->entry) {
                const auto& entry = *row->entry;
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
        switch (role) {
        case Qt::DisplayRole:
        case Qt::EditRole:
            return row->text;
        case Qt::DecorationRole:
            return row->icon.isNull() ? QVariant{} : QVariant{row->icon};
        case Qt::ToolTipRole:
            return row->tooltip.isNull() ? QVariant{} : QVariant{row->tooltip};
        case entry_role:
            return row->entry ? QVariant::fromValue(*row->entry) : QVariant{};
        case query_role:
            return row->query ? QVariant::fromValue(*row->query) : QVariant{};
        case loaded_role:
            return row->loaded;
        case more_role:
            return row->more;
        case described_role:
            return row->described;
        case empty_state_role:
            return row->empty_state ? QVariant{*row->empty_state} : QVariant{};
        default:
            return {};
        }
    }
    QHash<int, QByteArray> roleNames() const override {
        auto names = LibraryTreeModel::roleNames();
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
        auto* mime =
            new ui::LocalFilesMimeData{[browser = browser_, entries = std::move(entries)](
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

LibraryBrowser::LibraryBrowser(const CatalogueSource& catalogues, EngineKey engine, QObject* parent)
    : QObject(parent), catalogues_(&catalogues), engine_(std::move(engine)) {
    pool_.setMaxThreadCount(2);
    artwork_pool_.setMaxThreadCount(1);
    status_ = tr("Press Refresh to scan your music folders.");
    query_mode_ = QSettings{}.value(QStringLiteral("library/query-mode"), false).toBool();
    newest_first_ = QSettings{}.value(QStringLiteral("library/newest-first"), false).toBool();
    // Recently added was a toggle before it was a view.
    adoptView(QSettings{}
                  .value(QStringLiteral("library/view"),
                         newest_first_ ? recent_library_view_id : default_library_view_id)
                  .toString(),
              false);
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
        refreshInPlace();
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

std::shared_ptr<engine::Catalogue> LibraryBrowser::openCatalogue() const {
    return catalogues_->openDeferred();
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
        QtConcurrent::run(&pool_, [catalogue = openCatalogue(), work = std::move(task.work)] {
            // ADR-0220: the task is given the core's front door, never the
            // database, and never learns which side of a socket it is on.
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
    setViewId(on ? recent_library_view_id : default_library_view_id);
}

void LibraryBrowser::adoptView(const QString& id, const bool remember) {
    const auto views = libraryViews();
    auto found = std::ranges::find(views, id, &LibraryViewDefinition::id);
    if (found == views.end()) {
        found = std::ranges::find(views, default_library_view_id, &LibraryViewDefinition::id);
    }
    view_id_ = found->id;
    view_ = found->own_tree ? std::vector<persistence::LibraryViewLevel>{} : found->levels;
    newest_first_ = view_id_ == recent_library_view_id;
    folders_ = view_id_ == folders_library_view_id;
    if (!remember) {
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("library/view"), view_id_);
    settings.setValue(QStringLiteral("library/newest-first"), newest_first_);
}

void LibraryBrowser::setViewId(const QString& id) {
    if (id == view_id_) {
        return;
    }
    adoptView(id);
    expanded_entries_.clear();
    current_entry_.clear();
    emit searchChanged();
    reload();
}

void LibraryBrowser::previewLevels(std::vector<persistence::LibraryViewLevel> levels) {
    view_id_.clear();
    view_ = std::move(levels);
    newest_first_ = false;
    folders_ = false;
    reload();
}

void LibraryBrowser::previewView(const QString& id) {
    adoptView(id, false);
    // Marked a preview, so an edit of the saved views leaves it alone.
    view_id_.clear();
    reload();
}

void LibraryBrowser::refreshViews() {
    // A preview shows what it is given, not a saved view.
    if (view_id_.isEmpty()) {
        return;
    }
    const auto shown = std::make_pair(view_id_, view_);
    adoptView(view_id_);
    if (std::make_pair(view_id_, view_) != shown) {
        emit searchChanged();
        reload();
    }
}

void LibraryBrowser::locatePath(std::string raw_path, bool album) {
    search_timer_.stop();
    ++generation_;
    const auto generation = generation_;
    setStatus(tr("Locating in library…"));
    // ADR-0273: found in the layout shown, not by switching to another --
    // a view asks the engine for the way to the file through its levels.
    auto view = view_;
    enqueue({[raw_path, view,
              cancellation = lifetime_cancellation_.token()](engine::Catalogue& library) {
                 Outcome outcome;
                 persistence::LibraryQuery query;
                 query.kind = persistence::LibraryEntryKind::album;
                 query.raw_path = raw_path;
                 auto page = library.query(query, cancellation);
                 if (!page) {
                     outcome.error = text(page.error().message);
                     return outcome;
                 }
                 outcome.page = std::move(*page);
                 if (!view.empty() && !outcome.page.entries.empty()) {
                     persistence::LibraryQuery way;
                     way.kind = persistence::LibraryEntryKind::group;
                     way.view = view;
                     way.raw_path = raw_path;
                     auto located = library.query(way, cancellation);
                     if (!located) {
                         outcome.error = text(located.error().message);
                         return outcome;
                     }
                     outcome.way = std::move(*located);
                 }
                 return outcome;
             },
             [this, generation, album, raw_path](Outcome outcome) {
                 // A failed or empty lookup is a fact about the queried path, true
                 // however the tree changed while the query ran. Reported before
                 // the staleness check, which guards the navigation below.
                 if (!outcome.error.isEmpty()) {
                     setStatus(outcome.error);
                     return;
                 }
                 if (outcome.page.entries.empty()) {
                     setStatus(
                         !engine_.isLocal()
                             ? tr("This file is not in this library yet; it is found after the "
                                  "engine's next scan.")
                             : tr("This file is not in the local library. Add its folder and "
                                  "Refresh first."));
                     return;
                 }
                 if (generation != generation_)
                     return;
                 const auto found = outcome.page.entries.front();
                 auto locating = this->locating(found, raw_path, album, outcome.way);
                 if (!locating) {
                     setStatus(view_.empty() ? tr("This file is not in this view.")
                               : !outcome.way.located
                                   ? tr("This engine cannot locate in a view yet; update "
                                        "its melodyd.")
                                   : tr("This file is not in this view."));
                     return;
                 }
                 search_.clear();
                 emit searchChanged();
                 expanded_entries_.clear();
                 current_entry_.clear();
                 reload();
                 locating_ = std::move(locating);
             }});
}

LibraryBrowser::Locating LibraryBrowser::locating(const persistence::LibraryEntry& album_entry,
                                                  const std::string& raw_path, const bool album,
                                                  const persistence::LibraryPage& way) const {
    using Kind = persistence::LibraryEntryKind;
    // The library's folders: every folder the file is under, down to its own.
    if (folders_) {
        const auto folder = std::filesystem::path{raw_path}.parent_path().native();
        return [folder](const persistence::LibraryEntry& entry, std::size_t) {
            if (entry.kind == Kind::track || entry.view_value.empty()) {
                return Step::none;
            }
            if (entry.view_value == folder) {
                return Step::target;
            }
            const auto& under = entry.view_value;
            const bool inside = folder.size() > under.size() && folder.starts_with(under) &&
                                (under.ends_with('/') || folder[under.size()] == '/');
            return inside ? Step::on_way : Step::none;
        };
    }
    // A view: its node at each level, as the engine found the way; an
    // artist at the level that names it, an album -- or an artist no level
    // names -- at the last.
    if (!view_.empty()) {
        if (!way.located || way.entries.empty()) {
            return {};
        }
        std::vector<std::string> labels;
        auto target = way.entries.size() - 1U;
        for (std::size_t depth = 0; depth < way.entries.size(); ++depth) {
            const auto& node = way.entries[depth];
            labels.push_back(node.view_value);
            if (!album && target == way.entries.size() - 1U && !album_entry.artist.empty() &&
                node.view_value == album_entry.artist) {
                target = depth;
            }
        }
        labels.resize(target + 1U);
        return [labels](const persistence::LibraryEntry& entry, const std::size_t depth) {
            if (depth >= labels.size() || entry.kind == Kind::track ||
                entry.view_value != labels[depth]) {
                return Step::none;
            }
            return depth + 1U == labels.size() ? Step::target : Step::on_way;
        };
    }
    // Recently added lists albums, newest first: the album, whichever asked.
    if (newest_first_) {
        return [key = album_entry.key](const persistence::LibraryEntry& entry,
                                       const std::size_t depth) {
            return depth == 0U && entry.kind == Kind::album && entry.key == key ? Step::target
                                                                                : Step::none;
        };
    }
    // The artist tree: the artist, and under it the album.
    return [artist = album_entry.artist, key = album_entry.key,
            album](const persistence::LibraryEntry& entry, const std::size_t depth) {
        if (depth == 0U && entry.kind == Kind::artist && entry.key == artist) {
            return album ? Step::on_way : Step::target;
        }
        if (album && depth == 1U && entry.kind == Kind::album && entry.key == key) {
            return Step::target;
        }
        return Step::none;
    };
}

QString LibraryBrowser::emptyLibraryText() const {
    if (!has_roots_) {
        return tr("The library is empty");
    }
    return *has_roots_ ? tr("Nothing indexed yet — press Refresh to scan your folders")
                       : tr("No music folders yet — choose Folders… to add one");
}

void LibraryBrowser::reload() {
    locating_ = {};
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
        // ADR-0254: the engine's folders, from the library's roots down --
        // on whichever machine it runs.
        if (folders_) {
            persistence::LibraryQuery top;
            top.kind = persistence::LibraryEntryKind::group;
            top.folders = true;
            loadChildren({}, top);
            return;
        }
        // ADR-0254: a view's first level, grouped by the engine.
        if (!view_.empty()) {
            persistence::LibraryQuery top;
            top.kind = persistence::LibraryEntryKind::group;
            top.view = view_;
            loadChildren({}, top);
            return;
        }
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
        // In a view, the query narrows it: what it keeps, grouped as ever.
        if (!view_.empty()) {
            persistence::LibraryQuery top;
            top.kind = persistence::LibraryEntryKind::group;
            top.view = view_;
            top.view_filter = query_text;
            loadChildren({}, top);
            return;
        }
        auto group = std::make_unique<LibraryTreeModel::Row>();
        group->text = tr("Tracks");
        group->loaded = true;
        const auto index = model_->indexOf(model_->append(model_->root(), std::move(group)));
        loadFilterChildren(QPersistentModelIndex{index},
                           std::make_shared<query::CompiledTkq>(std::move(*compiled)));
        emit expandRequested(index);
        return;
    }
    for (const auto kind :
         {persistence::LibraryEntryKind::album, persistence::LibraryEntryKind::track}) {
        auto group = std::make_unique<LibraryTreeModel::Row>();
        group->text = kind == persistence::LibraryEntryKind::album ? tr("Albums") : tr("Tracks");
        group->loaded = true;
        persistence::LibraryQuery query;
        query.kind = kind;
        query.text = query_text;
        // Loaded already, so not fetched again -- kept to be asked again.
        group->query = query;
        const auto index = model_->indexOf(model_->append(model_->root(), std::move(group)));
        loadChildren(QPersistentModelIndex{index}, query);
        emit expandRequested(index);
    }
}

void LibraryBrowser::fetch(const QModelIndex& index) {
    auto* item = index.isValid() ? model_->rowAt(index) : nullptr;
    if (item == nullptr || item->loaded || !item->query) {
        return;
    }
    item->loaded = true;
    loadChildren(QPersistentModelIndex{index}, *item->query);
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

// ADR-0254: a view's node opens, and resolves, as the view lists it -- under
// a genre, an album is the genre's part of it.
void LibraryBrowser::prepareEntry(persistence::LibraryEntry& entry,
                                  const persistence::LibraryQuery& query) {
    if ((!query.view.empty() || query.folders) &&
        entry.kind != persistence::LibraryEntryKind::track) {
        auto node = query;
        node.kind = persistence::LibraryEntryKind::group;
        node.offset = 0U;
        if (query.folders) {
            node.folder = entry.view_value;
        } else {
            node.view_path.push_back(entry.view_value);
        }
        entry.view_node = std::move(node);
    }
}

// An entry's row as it shows: its label, icon, tooltip, and what opening it
// lists. Its children and whether they are loaded are left as they are.
void LibraryBrowser::describeItem(LibraryTreeModel::Row& item,
                                  const persistence::LibraryEntry& entry) {
    auto label = entry.label.empty() && entry.view_node ? tr("Unknown") : text(entry.label);
    if (entry.available == 0U) {
        label += tr(" — unavailable");
    } else if (entry.available < entry.tracks) {
        label += tr(" — %1 unavailable").arg(entry.tracks - entry.available);
    }
    item.text = label;
    item.draggable = entry.available > 0U;
    item.icon = kindIcon(entry.kind);
    item.entry = entry;
    item.described = true;
    // A track by its file, an album by whose it is, an artist by name alone.
    item.tooltip = (entry.kind == persistence::LibraryEntryKind::track ? pathLabel(entry.key)
                    : entry.kind == persistence::LibraryEntryKind::group
                        ? tr("%1 album%2 · %3 track%4")
                              .arg(entry.albums)
                              .arg(entry.albums == 1U ? "" : "s")
                              .arg(entry.tracks)
                              .arg(entry.tracks == 1U ? "" : "s")
                    : entry.album.empty() ? text(entry.artist)
                                          : text(entry.artist + " — " + entry.album));
    if (entry.view_node) {
        item.query = *entry.view_node;
    } else if (entry.kind != persistence::LibraryEntryKind::track) {
        persistence::LibraryQuery children;
        if (entry.kind == persistence::LibraryEntryKind::artist) {
            children.kind = persistence::LibraryEntryKind::album;
            children.artist = entry.key;
        } else {
            children.kind = persistence::LibraryEntryKind::track;
            children.album_key = entry.key;
        }
        item.query = std::move(children);
    }
}

// Looked up in the theme once; a theme icon follows a change of theme.
const QIcon& LibraryBrowser::kindIcon(const persistence::LibraryEntryKind kind) {
    const auto at = static_cast<int>(kind);
    if (const auto found = kind_icons_.constFind(at); found != kind_icons_.constEnd()) {
        return *found;
    }
    return *kind_icons_.insert(at, QIcon::fromTheme(kind == persistence::LibraryEntryKind::artist
                                                        ? QStringLiteral("avatar-default")
                                                    : kind == persistence::LibraryEntryKind::album
                                                        ? QStringLiteral("media-optical-audio")
                                                    : kind == persistence::LibraryEntryKind::group
                                                        ? QStringLiteral("folder-music")
                                                        : QStringLiteral("audio-x-generic")));
}

std::optional<persistence::LibraryQuery> LibraryBrowser::topQuery() const {
    if (!search_.trimmed().isEmpty()) {
        return std::nullopt;
    }
    persistence::LibraryQuery top;
    if (folders_) {
        top.kind = persistence::LibraryEntryKind::group;
        top.folders = true;
    } else if (!view_.empty()) {
        top.kind = persistence::LibraryEntryKind::group;
        top.view = view_;
    } else if (newest_first_) {
        top.kind = persistence::LibraryEntryKind::album;
        top.newest_first = true;
        top.limit = 500;
    }
    return top;
}

void LibraryBrowser::applyChanges(const std::vector<std::string>& albums, const bool everything) {
    if (stopped_) {
        return;
    }
    if (everything) {
        invalidateCovers();
    } else if (!albums.empty()) {
        // Asked again when shown; the old one stays until the new arrives.
        for (const auto& album : albums) {
            const auto key = text(album);
            artwork_cache_.remove(key);
            emit coverLoaded(key);
        }
        pumpCovers();
    }
    change_timer_.start();
}

void LibraryBrowser::refreshInPlace() {
    if (stopped_) {
        return;
    }
    // A query's results, or a tree not yet shown: as a new search.
    if ((query_mode_ && !search_.trimmed().isEmpty()) || model_->rowCount() == 0) {
        reload();
        return;
    }
    std::vector<std::pair<QPersistentModelIndex, persistence::LibraryQuery>> levels;
    if (const auto top = topQuery()) {
        levels.emplace_back(QPersistentModelIndex{}, *top);
    }
    std::vector<const LibraryTreeModel::Row*> pending{&model_->root()};
    while (!pending.empty()) {
        const auto* item = pending.back();
        pending.pop_back();
        for (int row = 0; row < item->count(); ++row) {
            const auto* child = item->child(row);
            if (child->loaded && child->query && !child->more) {
                levels.emplace_back(QPersistentModelIndex{model_->indexOf(*child)}, *child->query);
                pending.push_back(child);
            }
        }
    }
    for (auto& [parent, query] : levels) {
        mergeChildren(parent, std::move(query));
    }
}

void LibraryBrowser::mergeChildren(const QPersistentModelIndex& parent,
                                   persistence::LibraryQuery query) {
    if (query.limit < 100'000U && !query.newest_first) {
        query.limit = 100'000U;
    }
    query.offset = 0U;
    const auto generation = generation_;
    const auto root = !parent.isValid();
    enqueue({[query, cancellation = view_cancellation_.token()](engine::Catalogue& library) {
                 Outcome outcome;
                 const auto result = library.query(query, cancellation);
                 if (result) {
                     outcome.page = *result;
                 } else {
                     outcome.error = text(result.error().message);
                 }
                 return outcome;
             },
             [this, parent, root, query, generation](Outcome outcome) {
                 if (generation != generation_ || (!root && !parent.isValid()) ||
                     !outcome.error.isEmpty()) {
                     return;
                 }
                 auto* target = model_->rowAt(root ? QModelIndex{} : QModelIndex{parent});
                 if (target == nullptr) {
                     return;
                 }
                 mergeLevel(*target, std::move(outcome.page), query, root);
             },
             true});
}

void LibraryBrowser::mergeLevel(LibraryTreeModel::Row& target, persistence::LibraryPage page,
                                const persistence::LibraryQuery& query, const bool root) {
    // Rows that are no entry -- "Show more…", "No matches" -- go;
    // what stays is matched by its entry.
    for (int row = target.count() - 1; row >= 0; --row) {
        if (!target.child(row)->entry) {
            model_->remove(target, row, 1);
        }
    }
    // The rows there are, by their entries' keys, in their order: each
    // entry finds its row by a lookup, not by reading every row after it.
    struct Kept {
        std::vector<LibraryTreeModel::Row*> rows;
        std::size_t next{0U};
    };
    QHash<QByteArray, Kept> kept;
    kept.reserve(target.count());
    for (int row = 0; row < target.count(); ++row) {
        auto* child = target.child(row);
        kept[entryKey(*child->entry)].rows.push_back(child);
    }
    int at = 0;
    for (auto& entry : page.entries) {
        prepareEntry(entry, query);
        const auto key = entryKey(entry);
        // The first row of that key not yet placed: rows before `at` are.
        LibraryTreeModel::Row* found = nullptr;
        bool first_of_key = false;
        if (const auto slot = kept.find(key); slot != kept.end()) {
            while (slot->next < slot->rows.size() && found == nullptr) {
                auto* candidate = slot->rows[slot->next++];
                if (candidate->position() >= at) {
                    found = candidate;
                    // Only the first of a key's rows is compared; another is
                    // said again, as every row once was.
                    first_of_key = slot->next == 1U;
                }
            }
        }
        LibraryTreeModel::Row* item = found;
        if (found != nullptr && found->position() > at) {
            // Moved: taken with what it holds, and opened again.
            model_->move(target, found->position(), at);
            if (expanded_entries_.contains(key)) {
                emit expandRequested(model_->indexOf(*found));
            }
        } else if (found == nullptr) {
            auto row = std::make_unique<LibraryTreeModel::Row>();
            describeItem(*row, entry);
            item = &model_->insert(target, at, std::move(row));
            if (expanded_entries_.contains(key)) {
                emit expandRequested(model_->indexOf(*item));
            }
            ++at;
            continue;
        }
        // Said again as it is now: counts, ratings, what is available --
        // its rows and whether they are open kept. A row saying it already
        // is left as it is.
        if (!first_of_key || !item->described || !(*item->entry == entry)) {
            describeItem(*item, entry);
            model_->changed(*item);
        }
        ++at;
    }
    if (target.count() > at) {
        model_->remove(target, at, target.count() - at);
    }
    if (page.more) {
        auto next = query;
        next.offset = page.entries.size();
        auto more = std::make_unique<LibraryTreeModel::Row>();
        more->text = tr("Show more…");
        more->more = true;
        more->query = std::move(next);
        model_->append(target, std::move(more));
    } else if (target.count() == 0) {
        const bool top = root && query.text.empty();
        auto empty = std::make_unique<LibraryTreeModel::Row>();
        empty->text = top ? emptyLibraryText() : tr("No matches");
        empty->enabled = false;
        empty->empty_state = top;
        model_->append(target, std::move(empty));
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
    enqueue({[query, cancellation = view_cancellation_.token()](engine::Catalogue& library) {
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
                 auto* target = model_->rowAt(root ? QModelIndex{} : QModelIndex{parent});
                 if (target == nullptr) {
                     return;
                 }
                 if (!outcome.error.isEmpty()) {
                     setStatus(outcome.error);
                     target->loaded = false;
                     model_->changed(*target);
                     return;
                 }
                 if (query.offset == 0U) {
                     model_->remove(*target, 0, target->count());
                 }
                 if (!scanning_ && status_ == tr("Searching…")) {
                     setStatus(search_.trimmed().isEmpty() ? tr("Browse artists and albums.")
                                                           : tr("Search results"));
                 }
                 // ADR-0273: how deep these rows are, for the way being located.
                 std::size_t depth = 0U;
                 for (QModelIndex above = parent; above.isValid(); above = above.parent()) {
                     ++depth;
                 }
                 // The level in at once -- one insertion however many rows --
                 // and what opens, or is current, said of the rows once there.
                 std::vector<std::unique_ptr<LibraryTreeModel::Row>> rows;
                 rows.reserve(outcome.page.entries.size());
                 for (auto& entry : outcome.page.entries) {
                     prepareEntry(entry, query);
                     auto row = std::make_unique<LibraryTreeModel::Row>();
                     describeItem(*row, entry);
                     rows.push_back(std::move(row));
                 }
                 auto* first = model_->append(*target, std::move(rows));
                 bool on_the_way = false;
                 for (std::size_t added = 0; added < outcome.page.entries.size(); ++added) {
                     const auto& entry = outcome.page.entries[added];
                     const auto index = model_->index(first->position() + static_cast<int>(added),
                                                      0, model_->indexOf(*target));
                     if (locating_) {
                         const auto step = locating_(entry, depth);
                         if (step != Step::none) {
                             on_the_way = true;
                             emit expandRequested(index);
                         }
                         if (step == Step::target) {
                             emit currentRequested(index, true);
                             locating_ = {};
                             setStatus(tr("Located in library."));
                         }
                     }
                     if (current_entry_ == entryKey(entry)) {
                         emit currentRequested(index, false);
                     }
                     if (expanded_entries_.contains(entryKey(entry))) {
                         emit expandRequested(index);
                     }
                 }
                 if (outcome.page.more) {
                     query.offset += outcome.page.entries.size();
                     // Not among these: further on, then.
                     if (locating_ && !on_the_way) {
                         loadChildren(parent, query);
                     } else {
                         auto more = std::make_unique<LibraryTreeModel::Row>();
                         more->text = tr("Show more…");
                         more->more = true;
                         more->query = query;
                         model_->append(*target, std::move(more));
                     }
                 } else if (target->count() == 0) {
                     // Why it is empty, at the top: a search that found nothing
                     // is not a library with no folders.
                     const bool top = !parent.isValid() && query.text.empty();
                     auto empty = std::make_unique<LibraryTreeModel::Row>();
                     empty->text = top ? emptyLibraryText() : tr("No matches");
                     empty->enabled = false;
                     empty->empty_state = top;
                     model_->append(*target, std::move(empty));
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
                 auto* target = model_->rowAt(parent);
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
                 std::vector<std::unique_ptr<LibraryTreeModel::Row>> rows;
                 rows.reserve(outcome.page.entries.size());
                 const auto icon = QIcon::fromTheme(QStringLiteral("audio-x-generic"));
                 for (const auto& entry : outcome.page.entries) {
                     auto row = std::make_unique<LibraryTreeModel::Row>();
                     row->text = text(entry.label);
                     row->draggable = true;
                     row->icon = icon;
                     row->entry = entry;
                     row->tooltip = pathLabel(entry.key);
                     rows.push_back(std::move(row));
                 }
                 model_->append(*target, std::move(rows));
                 if (outcome.page.more) {
                     // The tree shows the first page; Enter keeps the complete
                     // bounded result set as a list.
                     auto more = std::make_unique<LibraryTreeModel::Row>();
                     more->text = tr("Showing the first %1 matches — press Enter to keep them all")
                                      .arg(outcome.page.entries.size());
                     more->enabled = false;
                     model_->append(*target, std::move(more));
                 } else if (target->count() == 0) {
                     auto empty = std::make_unique<LibraryTreeModel::Row>();
                     empty->text = tr("No matches");
                     empty->enabled = false;
                     model_->append(*target, std::move(empty));
                 }
                 emit levelLoaded();
             },
             true});
}

void LibraryBrowser::activate(const QModelIndex& index) {
    auto* item = index.isValid() ? model_->rowAt(index) : nullptr;
    if (item == nullptr) {
        return;
    }
    if (item->more && item->query) {
        const auto query = *item->query;
        const QPersistentModelIndex parent{index.parent()};
        model_->remove(*item->parent(), item->position(), 1);
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

void LibraryBrowser::addToNewList(const QModelIndexList& indexes, const QString& name) {
    auto entries = selectedEntries(indexes);
    if (!entries.empty()) {
        emit newListRequested(std::move(entries), name);
    }
}

QString LibraryBrowser::newListName(const QModelIndexList& indexes) {
    const auto entries = selectedEntries(indexes);
    return entries.size() == 1U ? QString::fromStdString(entries.front().label)
                                : tr("Library selection");
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
    if (auto* row = model_->rowAt(index); row != nullptr && row->entry) {
        row->entry = std::move(entry);
        model_->changed(*row);
    }
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
                     if (entry.view_node) {
                         query = *entry.view_node;
                     } else if (entry.kind == persistence::LibraryEntryKind::artist) {
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
        emit searchStarted(query_text);
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
                         emit searchFailed(query_text, outcome.error);
                         return;
                     }
                     if (outcome.rows.empty()) {
                         setStatus(tr("No search results to keep."));
                         emit searchFailed(query_text, tr("Nothing matches “%1”").arg(query_text));
                         return;
                     }
                     setStatus(tr("Search kept as a new tab."));
                     emit searchCommitted(query_text, std::move(outcome.rows));
                 }});
        return;
    }
    emit searchStarted(query_text);
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
                     emit searchFailed(query_text, outcome.error);
                     return;
                 }
                 if (outcome.rows.empty()) {
                     setStatus(tr("No search results to keep."));
                     emit searchFailed(query_text, tr("Nothing matches “%1”").arg(query_text));
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

std::vector<std::string> LibraryBrowser::rootPaths() const {
    std::vector<std::string> paths;
    paths.reserve(roots_.size());
    for (const auto& root : roots_) {
        paths.push_back(root.raw_path);
    }
    return paths;
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
    enqueue(
        {[](engine::Catalogue& library) {
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
             for (int row = 0; row < model_->root().count(); ++row) {
                 if (auto* item = model_->root().child(row); item->empty_state.value_or(false)) {
                     item->text = emptyLibraryText();
                     model_->changed(*item);
                 }
             }
             std::size_t offline = 0;
             roots_.clear();
             for (const auto& root : outcome.roots) {
                 const auto unavailable = !root.available && !root.error.empty();
                 if (unavailable) {
                     ++offline;
                 }
                 roots_.push_back(Root{.raw_path = root.raw_path,
                                       .label = pathLabel(root.raw_path) +
                                                (root.available ? QString{}
                                                 : unavailable  ? tr(" — unavailable")
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
        // Not told which albums: every cover is asked again.
        invalidateCovers();
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
    scan_watcher_.setFuture(QtConcurrent::run(&pool_, [catalogue = openCatalogue(),
                                                       cancellation = scan_cancellation_.token(),
                                                       progress = progress_] {
        ScanOutcome outcome;
        // ADR-0220: ask the core, do not open its database. Remotely it
        // becomes a job, and the same counters are fed from its progress.
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
            QtConcurrent::run(&artwork_pool_, [catalogue = openCatalogue(), key = key.toStdString(),
                                               cancellation = artwork_cancellation_.token()] {
                if (cancellation.is_cancellation_requested()) {
                    return QImage{};
                }
                // ADR-0220: ask the core, do not open its database. Read by
                // the engine, where the files are: a remote library shows its
                // covers with nothing of it mounted here.
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
