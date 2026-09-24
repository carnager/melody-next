// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_library_panel.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "uicommon/library_tree_view.hpp"
#include "uicommon/local_artwork.hpp"
#include "uicommon/local_files_mime_data.hpp"
#include "uicommon/rating_stars.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace trackknife::bench {
namespace {
constexpr int entry_role = library_entry_role;
constexpr int query_role = Qt::UserRole + 2;
constexpr int loaded_role = Qt::UserRole + 3;
constexpr int more_role = Qt::UserRole + 4;
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

std::array<QIcon, 3> libraryActionIcons(const QWidget* widget) {
    return {QIcon::fromTheme(QStringLiteral("list-add"),
                             widget->style()->standardIcon(QStyle::SP_DialogOpenButton)),
            QIcon::fromTheme(QStringLiteral("go-next"),
                             widget->style()->standardIcon(QStyle::SP_ArrowRight)),
            QIcon::fromTheme(QStringLiteral("media-playback-start"),
                             widget->style()->standardIcon(QStyle::SP_MediaPlay))};
}
std::vector<persistence::LibraryEntry> selectedEntries(QModelIndexList indexes) {
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

class LibraryModel final : public QStandardItemModel {
  public:
    LibraryModel(LocalLibraryPanel* panel, std::function<QIcon(const QByteArray&)> artwork,
                 std::function<void(const QModelIndex&)> fetch)
        : QStandardItemModel(panel), panel_(panel), artwork_(std::move(artwork)),
          fetch_(std::move(fetch)) {}
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
        if (role == ui::LibraryTreeDelegate::secondaryTextRole) {
            const auto value = QStandardItemModel::data(index, entry_role);
            if (!value.isValid())
                return {};
            const auto entry = value.value<persistence::LibraryEntry>();
            // An artist's album count is the row's quiet count instead.
            if (entry.kind == persistence::LibraryEntryKind::artist)
                return {};
            const auto tracks =
                tr("%1 track%2").arg(entry.tracks).arg(entry.tracks == 1U ? "" : "s");
            // Under its artist, an album need not name them again.
            if (entry.kind == persistence::LibraryEntryKind::album)
                return index.parent().isValid() ? tracks
                                                 : text(entry.artist) + QStringLiteral(" · ") + tracks;
            return {};
        }
        if (role == Qt::DecorationRole) {
            const auto value = QStandardItemModel::data(index, entry_role);
            if (value.isValid()) {
                const auto entry = value.value<persistence::LibraryEntry>();
                if (entry.kind == persistence::LibraryEntryKind::album) {
                    const auto cover = artwork_(QByteArray::fromStdString(entry.key));
                    if (!cover.isNull()) {
                        return cover;
                    }
                }
            }
        }
        return QStandardItemModel::data(index, role);
    }
    QStringList mimeTypes() const override { return {ui::LocalFilesMimeData::mimeType()}; }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        auto entries = selectedEntries(indexes);
        if (entries.empty() || entries.size() > 1'000U ||
            std::ranges::none_of(entries, [](const auto& entry) { return entry.available > 0U; })) {
            return nullptr;
        }
        const bool remote = panel_ && panel_->remote();
        // The entries themselves too, for a place that wants tagged rows
        // rather than paths: Up Next.
        QVariantList carried;
        for (const auto& entry : entries) {
            carried.push_back(QVariant::fromValue(entry));
        }
        auto* mime = new ui::LocalFilesMimeData{[panel = panel_, entries = std::move(entries)](
                                              ui::LocalFilesMimeData::Completion done) {
                                              if (panel) {
                                                  panel->resolveEntries(entries, std::move(done));
                                              }
                                          },
                                          remote};
        mime->setProperty(library_entries_property, carried);
        return mime;
    }

  private:
    QPointer<LocalLibraryPanel> panel_;
    std::function<QIcon(const QByteArray&)> artwork_;
    std::function<void(const QModelIndex&)> fetch_;
};

} // namespace

LocalLibraryPanel::LocalLibraryPanel(const CatalogueSource& catalogues, QWidget* parent)
    : QWidget(parent), catalogues_(&catalogues) {
    setObjectName(QStringLiteral("bench-local-library"));
    pool_.setMaxThreadCount(2);
    artwork_pool_.setMaxThreadCount(1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto* search_row = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("local-library-search"));
    search_->setPlaceholderText(tr("Search albums and tracks"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("Search local library"));
    search_row->addWidget(search_, 1);
    // ADR-0150: the explicit query toggle switches the field into the tkq
    // dialect; word search stays byte-for-byte what it was when off, and a
    // malformed query is an inline error, never a silent word search. A
    // checkbox, not a button: its state must be legible at a glance.
    query_toggle_ = new QCheckBox(tr("Query"), this);
    query_toggle_->setObjectName(QStringLiteral("local-library-query-toggle"));
    query_toggle_->setToolTip(
        tr("Interpret the search as a tkq query, e.g. genre HAS jazz AND date GREATER 1990"));
    query_toggle_->setChecked(
        QSettings{}.value(QStringLiteral("library/query-mode"), false).toBool());
    connect(query_toggle_, &QCheckBox::toggled, this, [this](const bool enabled) {
        QSettings{}.setValue(QStringLiteral("library/query-mode"), enabled);
        search_->setPlaceholderText(enabled ? tr("tkq query, e.g. genre HAS jazz")
                                            : tr("Search albums and tracks"));
        query_error_->hide();
        reloadTree();
    });
    search_row->addWidget(query_toggle_);
    layout->addLayout(search_row);
    query_error_ = new QLabel(this);
    query_error_->setObjectName(QStringLiteral("local-library-query-error"));
    query_error_->setWordWrap(true);
    query_error_->hide();
    layout->addWidget(query_error_);
    if (query_toggle_->isChecked()) {
        search_->setPlaceholderText(tr("tkq query, e.g. genre HAS jazz"));
    }
    // Refresh and the library's folders are icons on the search row: used
    // now and then, they need not take a row of their own.
    const auto icon_button = [this](const QString& name, const QString& icon,
                                    const QString& text) {
        auto* button = new QToolButton(this);
        button->setObjectName(name);
        button->setText(text);
        button->setToolTip(text);
        button->setAccessibleName(text);
        button->setIcon(QIcon::fromTheme(icon));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setIconSize(QSize{16, 16});
        return button;
    };
    auto* folders = icon_button(QStringLiteral("local-library-folders"),
                                QStringLiteral("folder"), tr("Folders…"));
    folders->setToolTip(tr("Choose which folders belong to your music library"));
    connect(folders, &QToolButton::clicked, this, &LocalLibraryPanel::showFolders);
    scan_button_ = icon_button(QStringLiteral("local-library-scan"),
                               QStringLiteral("view-refresh"), tr("Refresh"));
    connect(scan_button_, &QToolButton::clicked, this, [this] {
        if (scanning_) {
            scan_cancellation_.request_cancellation();
            change_timer_->stop();
            status_->setText(tr("Stopping scan…"));
        } else {
            startScan();
        }
    });
    newest_toggle_ = icon_button(QStringLiteral("local-library-newest"),
                                 QStringLiteral("document-open-recent"), tr("Recently added"));
    newest_toggle_->setCheckable(true);
    newest_toggle_->setToolTip(tr("Show albums newest first, as they came into the library"));
    newest_toggle_->setChecked(
        QSettings{}.value(QStringLiteral("library/newest-first"), false).toBool());
    connect(newest_toggle_, &QToolButton::toggled, this, [this](const bool on) {
        QSettings{}.setValue(QStringLiteral("library/newest-first"), on);
        reloadTree();
    });
    search_row->addWidget(newest_toggle_);
    search_row->addWidget(scan_button_);
    search_row->addWidget(folders);
    auto* library_view = new ui::LibraryTreeView(this);
    tree_ = library_view;
    tree_->setObjectName(QStringLiteral("local-library-tree"));
    tree_->setAccessibleName(tr("Local artists, albums, and tracks"));
    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setDefaultDropAction(Qt::CopyAction);
    tree_->setExpandsOnDoubleClick(false);
    library_view->setActionLabels({tr("Append to current list"), tr("Insert next in current list"),
                                   tr("Replace list and play")});
    library_view->setActionsAvailable([](const QModelIndex& index) {
        return index.data(entry_role).isValid() &&
               index.data(entry_role).value<persistence::LibraryEntry>().available > 0U;
    });
    library_view->setActionCallback([this](const QModelIndex& index, int action) {
        requestAction(index, static_cast<LocalLibraryAction>(action));
    });
    tree_->setItemDelegate(new ui::LibraryTreeDelegate(
        library_view, libraryActionIcons(this), [](const QModelIndex& index) {
            const auto value = index.data(entry_role);
            const auto entry = value.value<persistence::LibraryEntry>();
            return ui::LibraryTreeDelegate::Presentation{
                .track = value.isValid() && entry.kind == persistence::LibraryEntryKind::track,
                .album = value.isValid() && entry.kind == persistence::LibraryEntryKind::album,
                .root = !index.parent().isValid(),
                .artist = value.isValid() && entry.kind == persistence::LibraryEntryKind::artist,
                .secondary = index.data(ui::LibraryTreeDelegate::secondaryTextRole).toString(),
                .count = value.isValid() && entry.kind == persistence::LibraryEntryKind::artist
                             ? QString::number(entry.albums)
                             : QString{},
                .album_rating = value.isValid() ? entry.rating : 0U};
        }));
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    model_ = new LibraryModel(
        this,
        [this](const QByteArray& key) {
            const auto* icon = artwork_cache_.object(key);
            return icon ? *icon : QIcon{};
        },
        [this](const QModelIndex& index) {
            model_->setData(index, true, loaded_role);
            loadChildren(QPersistentModelIndex{index},
                         index.data(query_role).value<persistence::LibraryQuery>());
        });
    tree_->setModel(model_);
    tree_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        auto* item = model_->itemFromIndex(index);
        if (item != nullptr && item->data(entry_role).isValid()) {
            expanded_entries_.insert(
                entryKey(item->data(entry_role).value<persistence::LibraryEntry>()));
        }
        if (item == nullptr || item->data(loaded_role).toBool() ||
            !item->data(query_role).isValid()) {
            return;
        }
        item->setData(true, loaded_role);
        loadChildren(QPersistentModelIndex{index},
                     item->data(query_role).value<persistence::LibraryQuery>());
    });
    connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        if (index.data(entry_role).isValid()) {
            expanded_entries_.remove(
                entryKey(index.data(entry_role).value<persistence::LibraryEntry>()));
        }
    });
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& index) {
                if (index.data(entry_role).isValid()) {
                    current_entry_ =
                        entryKey(index.data(entry_role).value<persistence::LibraryEntry>());
                }
            });
    connect(tree_, &QTreeView::activated, this, &LocalLibraryPanel::activate);
    connect(tree_, &QTreeView::customContextMenuRequested, this,
            &LocalLibraryPanel::showContextMenu);
    // The footer: one small, quiet line of news, under a hairline.
    auto* footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("local-library-footer"));
    footer->setFrameShape(QFrame::NoFrame);
    {
        const auto ground = palette().color(QPalette::Window);
        const auto ink = palette().color(QPalette::Text);
        const auto mix = [](const int a, const int b) { return (a * 88 + b * 12) / 100; };
        footer->setStyleSheet(
            QStringLiteral("QFrame#local-library-footer { border-top: 1px solid %1; }")
                .arg(QColor::fromRgb(mix(ground.red(), ink.red()), mix(ground.green(), ink.green()),
                                     mix(ground.blue(), ink.blue()))
                         .name()));
    }
    auto* footer_layout = new QVBoxLayout(footer);
    footer_layout->setContentsMargins(4, 6, 4, 2);
    footer_layout->setSpacing(2);
    auto small = font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    status_ = new QLabel(tr("Press Refresh to scan your music folders."), footer);
    status_->setObjectName(QStringLiteral("local-library-status"));
    status_->setWordWrap(true);
    status_->setFont(small);
    status_->setForegroundRole(QPalette::PlaceholderText);
    footer_layout->addWidget(status_);
    // Which library this is: the tab above says so while it answers, so this
    // line appears only when it does not (ADR-0220), and in full colour.
    source_label_ = new QLabel(footer);
    source_label_->setObjectName(QStringLiteral("local-library-source"));
    source_label_->setWordWrap(true);
    source_label_->setTextFormat(Qt::PlainText);
    source_label_->setFont(small);
    source_label_->hide();
    footer_layout->addWidget(source_label_);
    layout->addWidget(footer);
    search_timer_ = new QTimer(this);
    search_timer_->setSingleShot(true);
    search_timer_->setInterval(200);
    connect(search_, &QLineEdit::textChanged, search_timer_, qOverload<>(&QTimer::start));
    connect(search_, &QLineEdit::textChanged, this, [this] { status_->setText(tr("Searching…")); });
    connect(search_timer_, &QTimer::timeout, this, &LocalLibraryPanel::reloadTree);
    // ADR-0140: Enter keeps the current hits as a durable list tab; the
    // live-filtered tree stays the transient default.
    connect(search_, &QLineEdit::returnPressed, this, &LocalLibraryPanel::commitSearch);
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
        setProperty("scanning", false);
        scan_button_->setText(tr("Refresh"));
        scan_button_->setToolTip(tr("Refresh"));
        scan_button_->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
        poll_timer_->stop();
        if (stopped_) {
            return;
        }
        const auto outcome = scan_watcher_.result();
        if (!outcome.error.isEmpty()) {
            status_->setText(outcome.error);
        } else if (outcome.result.cancelled) {
            status_->setText(tr("Scan stopped. Completed updates were kept."));
        } else if (outcome.result.incomplete) {
            status_->setText(
                tr("Scan incomplete. %1 files could not be read; previous entries were kept.")
                    .arg(progress_->failed.load()));
        } else {
            const auto updated = progress_->indexed.load();
            status_->setText(updated == 0U ? tr("Library up to date.")
                             : updated == 1U
                                 ? tr("Library up to date. 1 file updated.")
                                 : tr("Library up to date. %1 files updated.").arg(updated));
        }
        invalidateArtwork();
        reloadTree();
        loadRoots();
        emit libraryContentChanged();
    });
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(200);
    connect(poll_timer_, &QTimer::timeout, this, &LocalLibraryPanel::updateProgress);
    change_timer_ = new QTimer(this);
    change_timer_->setSingleShot(true);
    change_timer_->setInterval(250);
    connect(change_timer_, &QTimer::timeout, this, [this] {
        invalidateArtwork();
        reloadTree();
        loadRoots();
        emit libraryContentChanged();
    });
    artwork_timer_ = new QTimer(this);
    artwork_timer_->setSingleShot(true);
    artwork_timer_->setInterval(0);
    connect(artwork_timer_, &QTimer::timeout, this, &LocalLibraryPanel::updateArtwork);
    tree_->viewport()->installEventFilter(this);
    connect(tree_->verticalScrollBar(), &QScrollBar::valueChanged, artwork_timer_,
            qOverload<>(&QTimer::start));
    connect(tree_, &QTreeView::expanded, artwork_timer_, qOverload<>(&QTimer::start));
    connect(tree_, &QTreeView::collapsed, artwork_timer_, qOverload<>(&QTimer::start));
    connect(model_, &QAbstractItemModel::rowsInserted, artwork_timer_, qOverload<>(&QTimer::start));
    connect(&artwork_watcher_, &QFutureWatcherBase::finished, this, [this] {
        artwork_running_ = false;
        if (stopped_) {
            return;
        }
        if (artwork_job_generation_ == artwork_generation_ &&
            !artwork_cancellation_.is_cancellation_requested()) {
            const auto image = artwork_watcher_.result();
            artwork_cache_.insert(
                artwork_key_,
                new QIcon(image.isNull() ? QIcon{} : QIcon{QPixmap::fromImage(image)}));
            tree_->viewport()->update();
        }
        artwork_timer_->start();
    });
    reloadTree();
    loadRoots();
    refreshSourceLabel();
    // Said once in the status line too, where a user is actually looking when
    // a folder they added does not appear. The source label keeps saying it
    // afterwards.
    if (catalogues_ != nullptr && !catalogues_->usingEngine()) {
        QTimer::singleShot(0, this, [this] { status_->setText(catalogues_->describe()); });
    }
}

// ADR-0220: which library this panel is showing. Three distinct states, and
// the difference between the last two is exactly what was previously
// invisible -- a configured engine that is not answering looks identical to
// no engine at all.
void LocalLibraryPanel::refreshSourceLabel() {
    if (source_label_ == nullptr || catalogues_ == nullptr) {
        return;
    }
    source_label_->setText(catalogues_->describe());
    source_label_->setVisible(!catalogues_->reachable());
    source_label_->setToolTip(catalogues_->usingEngine()
                                  ? tr("Folders, scanning, search and covers come from that "
                                       "engine.")
                                  : tr("Choose an engine in Settings → Library, or leave it empty "
                                       "to use this computer's. It is tried again on the next "
                                       "library action."));
}

LocalLibraryPanel::~LocalLibraryPanel() { stop(); }

void LocalLibraryPanel::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    lifetime_cancellation_.request_cancellation();
    view_cancellation_.request_cancellation();
    scan_cancellation_.request_cancellation();
    artwork_cancellation_.request_cancellation();
    artwork_timer_->stop();
    search_timer_->stop();
    poll_timer_->stop();
    change_timer_->stop();
    tasks_.clear();
    pool_.waitForDone();
    artwork_pool_.waitForDone();
}

void LocalLibraryPanel::enqueue(Task task) {
    if (stopped_) {
        return;
    }
    if (tasks_.size() >= 64U) {
        status_->setText(tr("Please wait for the current library requests."));
        return;
    }
    tasks_.push_back(std::move(task));
    pump();
}

void LocalLibraryPanel::pump() {
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

void LocalLibraryPanel::locatePath(std::string raw_path, bool album) {
    search_timer_->stop();
    ++generation_;
    const auto generation = generation_;
    status_->setText(tr("Locating in library…"));
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
             // however the tree changed while the query ran. Report it before
             // the staleness check, which guards the navigation below: a
             // background refreshLibrary() bumps generation_ through
             // reloadTree(), and dropping the whole result left the panel
             // stuck on "Locating in library…" with no outcome.
             if (!outcome.error.isEmpty()) {
                 status_->setText(outcome.error);
                 return;
             }
             if (outcome.page.entries.empty()) {
                 status_->setText(remote()
                                      ? tr("This file is not in this library yet; it is found "
                                           "after the engine's next scan.")
                                      : tr("This file is not in the local library. Add its folder "
                                           "and Refresh first."));
                 return;
             }
             if (generation != generation_)
                 return;
             auto entry = outcome.page.entries.front();
             {
                 const QSignalBlocker blocker{search_};
                 search_->clear();
             }
             // Found under its artist, so the artist tree, not the newest.
             if (newest_toggle_ != nullptr && newest_toggle_->isChecked()) {
                 const QSignalBlocker blocker{newest_toggle_};
                 newest_toggle_->setChecked(false);
                 QSettings{}.setValue(QStringLiteral("library/newest-first"), false);
             }
             expanded_entries_.clear();
             current_entry_.clear();
             reloadTree();
             locate_artist_ = entry.artist;
             if (!album) {
                 entry.kind = persistence::LibraryEntryKind::artist;
                 entry.key = entry.artist;
             }
             locate_target_ = std::move(entry);
         }});
}

QString LocalLibraryPanel::emptyLibraryText() const {
    if (!has_roots_) {
        return tr("The library is empty");
    }
    return *has_roots_ ? tr("Nothing indexed yet — press Refresh to scan your folders")
                       : tr("No music folders yet — choose Folders… to add one");
}

void LocalLibraryPanel::reloadTree() {
    locate_target_.reset();
    ++generation_;
    artwork_cancellation_.request_cancellation();
    view_cancellation_.request_cancellation();
    view_cancellation_ = core::CancellationSource{};
    std::erase_if(tasks_, [](const Task& task) { return task.view_query; });
    if (previous_search_ != search_->text()) {
        previous_search_ = search_->text();
        expanded_entries_.clear();
        current_entry_.clear();
    }
    static_cast<ui::LibraryTreeView*>(tree_)->cancelPendingExpansions();
    model_->clear();
    const auto query_text = bytes(search_->text().trimmed());
    if (query_text.empty()) {
        if (query_error_ != nullptr) {
            query_error_->hide();
        }
        // Recently added: albums newest first, where artists would be.
        if (newest_toggle_ != nullptr && newest_toggle_->isChecked()) {
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
    if (query_toggle_ != nullptr && query_toggle_->isChecked()) {
        auto compiled = query::compile_tkq(query_text);
        if (!compiled) {
            query_error_->setText(text(compiled.error().message));
            query_error_->show();
            status_->setText(tr("Invalid query"));
            return;
        }
        query_error_->hide();
        auto* group = new QStandardItem(tr("Tracks"));
        group->setEditable(false);
        group->setDragEnabled(false);
        group->setData(true, loaded_role);
        model_->appendRow(group);
        loadFilterChildren(QPersistentModelIndex{group->index()},
                           std::make_shared<query::CompiledTkq>(std::move(*compiled)));
        tree_->expand(group->index());
        return;
    }
    for (const auto kind :
         {persistence::LibraryEntryKind::album, persistence::LibraryEntryKind::track}) {
        auto* group = new QStandardItem(
            kind == persistence::LibraryEntryKind::album ? tr("Albums") : tr("Tracks"));
        group->setEditable(false);
        group->setDragEnabled(false);
        group->setData(true, loaded_role);
        model_->appendRow(group);
        persistence::LibraryQuery query;
        query.kind = kind;
        query.text = query_text;
        loadChildren(QPersistentModelIndex{group->index()}, query);
        tree_->expand(group->index());
    }
}

void LocalLibraryPanel::loadChildren(const QPersistentModelIndex& parent,
                                     persistence::LibraryQuery query) {
    // Browsing shows the complete level in one load -- the default 200-row
    // page left the list cut off behind
    // a manual "Show more…" row. The bound matches the tkq result cap and
    // keeps the more-row as a never-expected safety valve.
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
                 status_->setText(outcome.error);
                 target->setData(false, loaded_role);
                 return;
             }
             if (query.offset == 0U) {
                 target->removeRows(0, target->rowCount());
             }
             if (!scanning_ && status_->text() == tr("Searching…")) {
                 status_->setText(search_->text().trimmed().isEmpty()
                                      ? tr("Browse artists and albums.")
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
                 item->setToolTip(entry.kind == persistence::LibraryEntryKind::track
                                      ? pathLabel(entry.key)
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
                 }
                 target->appendRow(item);
                 if (locate_target_) {
                     const bool found =
                         entry.kind == locate_target_->kind && entry.key == locate_target_->key;
                     if (found) {
                         tree_->setCurrentIndex(item->index());
                         tree_->scrollTo(item->index());
                         tree_->setFocus();
                         locate_target_.reset();
                         status_->setText(tr("Located in library."));
                     }
                     if (found || (entry.kind == persistence::LibraryEntryKind::artist &&
                                   entry.key == locate_artist_)) {
                         tree_->expand(item->index());
                     }
                 }
                 if (current_entry_ == entryKey(entry)) {
                     tree_->setCurrentIndex(item->index());
                 }
                 if (expanded_entries_.contains(entryKey(entry))) {
                     tree_->expand(item->index());
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
             static_cast<ui::LibraryTreeView*>(tree_)->completePendingExpansions();
         },
         true});
}

void LocalLibraryPanel::loadFilterChildren(const QPersistentModelIndex& parent,
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
                     status_->setText(outcome.error);
                     return;
                 }
                 if (!scanning_ && status_->text() == tr("Searching…")) {
                     status_->setText(tr("Query results"));
                 }
                 for (const auto& entry : outcome.page.entries) {
                     auto label = text(entry.label);
                     auto* item = new QStandardItem(label);
                     item->setEditable(false);
                     item->setDragEnabled(true);
                     item->setDropEnabled(false);
                     item->setIcon(QIcon::fromTheme(QStringLiteral("audio-x-generic")));
                     item->setData(QVariant::fromValue(entry), entry_role);
                     item->setToolTip(pathLabel(entry.key));
                     target->appendRow(item);
                 }
                 if (outcome.page.more) {
                     // Package-1 paging: the tree shows the first page; Enter
                     // keeps the complete bounded result set as a tab.
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
             },
             true});
}

void LocalLibraryPanel::activate(const QModelIndex& index) {
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
    requestAction(index, LocalLibraryAction::append);
}

void LocalLibraryPanel::requestAction(const QModelIndex& index, LocalLibraryAction action) {
    if (!index.data(entry_role).isValid()) {
        return;
    }
    if (!tree_->selectionModel()->isSelected(index)) {
        tree_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                            QItemSelectionModel::Rows);
    }
    auto entries = selectedEntries(tree_->selectionModel()->selectedRows());
    if (entries.empty() && tree_->selectionModel()->selectedRows().size() > 1'000) {
        status_->setText(tr("Select at most 1,000 library entries."));
    }
    if (!entries.empty()) {
        emit actionRequested(std::move(entries), action);
    }
}

void LocalLibraryPanel::showContextMenu(const QPoint& position) {
    const auto index = tree_->indexAt(position);
    if (!index.data(entry_role).isValid()) {
        return;
    }
    if (!tree_->selectionModel()->isSelected(index)) {
        tree_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                            QItemSelectionModel::Rows);
    }
    const auto entries = selectedEntries(tree_->selectionModel()->selectedRows());
    const bool available =
        entries.size() <= 1'000U &&
        std::ranges::any_of(entries, [](const auto& entry) { return entry.available > 0U; });
    auto* menu = new QMenu(tree_);
    menu->setObjectName(QStringLiteral("local-library-context-menu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const std::array labels{tr("Append to current list"),
                            tr("Insert next in current list"),
                            tr("Replace list and play"),
                            tr("Open in new tab"),
                            tr("Play next (Up Next)"),
                            tr("Add to Up Next")};
    const auto icons = libraryActionIcons(this);
    for (int action = 0; action < static_cast<int>(labels.size()); ++action) {
        auto* command =
            menu->addAction(action < 3    ? icons[static_cast<std::size_t>(action)]
                            : action == 3 ? QIcon::fromTheme(QStringLiteral("tab-new"))
                                          : QIcon::fromTheme(QStringLiteral("media-playlist-append")),
                            labels[static_cast<std::size_t>(action)]);
        command->setObjectName(QStringLiteral("action-local-library-%1").arg(action));
        command->setEnabled(available);
        connect(command, &QAction::triggered, this, [this, entries, action] {
            emit actionRequested(entries, static_cast<LocalLibraryAction>(action));
        });
    }
    // ADR-0179: rate the targeted track or album entry by content identity.
    const auto target_entry = index.data(entry_role).value<persistence::LibraryEntry>();
    if (target_entry.kind != persistence::LibraryEntryKind::artist &&
        !target_entry.rating_hash.empty()) {
        menu->addSeparator();
        auto* rate_menu = menu->addMenu(target_entry.kind == persistence::LibraryEntryKind::album
                                            ? tr("Rate album")
                                            : tr("Rate track"));
        rate_menu->setObjectName(QStringLiteral("local-library-rate-menu"));
        const QPersistentModelIndex target{index};
        for (unsigned rating = 0U; rating <= 10U; rating += 2U) {
            QAction* choice = nullptr;
            if (rating == 0U) {
                choice = rate_menu->addAction(ui::ratingMenuLabel(rating));
                choice->setCheckable(true);
            } else {
                auto* stars = new ui::RatingMenuAction(rating, rate_menu);
                rate_menu->addAction(stars);
                choice = stars;
            }
            choice->setObjectName(QStringLiteral("action-local-library-rate-%1").arg(rating));
            choice->setChecked(target_entry.rating == rating);
            connect(choice, &QAction::triggered, this, [this, target, target_entry, rating] {
                storeRating(target_entry.rating_hash,
                            target_entry.kind == persistence::LibraryEntryKind::album, rating);
                if (target.isValid()) {
                    auto updated = target_entry;
                    updated.rating = rating;
                    model_->setData(target, QVariant::fromValue(updated), entry_role);
                }
            });
        }
    }
    if (model_->hasChildren(index)) {
        menu->addSeparator();
        const QPersistentModelIndex target{index};
        menu->addAction(tree_->isExpanded(index) ? tr("Collapse") : tr("Expand"), this,
                        [this, target] {
                            if (target.isValid()) {
                                tree_->setExpanded(target, !tree_->isExpanded(target));
                            }
                        });
    }
    menu->popup(tree_->viewport()->mapToGlobal(position));
}

void LocalLibraryPanel::requestRatings(std::vector<std::string> hashes,
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

void LocalLibraryPanel::storeRating(std::string hash, const bool album, const unsigned rating) {
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
                         status_->setText(outcome.error);
                         return;
                     }
                     emit ratingsChanged();
                 },
             .view_query = false});
}

void LocalLibraryPanel::resolveEntries(std::vector<persistence::LibraryEntry> entries,
                                       std::function<void(std::vector<std::string>)> completion) {
    if (entries.empty() || entries.size() > 1'000U) {
        status_->setText(tr("Select between 1 and 1,000 library entries."));
        return;
    }
    status_->setText(tr("Loading library selection…"));
    enqueue(
        {[entries = std::move(entries),
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
                 status_->setText(outcome.error);
                 return;
             }
             if (outcome.paths.empty()) {
                 status_->setText(tr(
                     "These files are unavailable. Reconnect the folder and refresh the library."));
                 return;
             }
             status_->setText(outcome.unavailable > 0U ? tr("Unavailable files were skipped.")
                                                       : tr("Library selection loaded."));
             completion(std::move(outcome.paths));
         }});
}

void LocalLibraryPanel::resolveEntryRows(
    std::vector<persistence::LibraryEntry> entries,
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
                         status_->setText(outcome.error);
                         return;
                     }
                     completion(std::move(outcome.rows));
                 }});
    });
}

void LocalLibraryPanel::commitSearch() {
    const auto query_text = search_->text().trimmed();
    if (query_text.isEmpty()) {
        return;
    }
    status_->setText(tr("Collecting search results…"));
    // ADR-0150: a committed query resolves through the structured filter;
    // the resulting tab is the same ADR-0140 snapshot as a word search.
    if (query_toggle_ != nullptr && query_toggle_->isChecked()) {
        auto compiled = query::compile_tkq(bytes(query_text));
        if (!compiled) {
            query_error_->setText(text(compiled.error().message));
            query_error_->show();
            status_->setText(tr("Invalid query"));
            return;
        }
        query_error_->hide();
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
                         status_->setText(outcome.error);
                         return;
                     }
                     if (outcome.rows.empty()) {
                         status_->setText(tr("No search results to keep."));
                         return;
                     }
                     status_->setText(tr("Search kept as a new tab."));
                     emit searchCommitted(query_text, std::move(outcome.rows));
                 }});
        return;
    }
    enqueue({[query = bytes(query_text),
              cancellation = lifetime_cancellation_.token()](engine::Catalogue& library) {
                 Outcome outcome;
                 std::unordered_set<std::string> seen;
                 // Album-name matches first (whole matching albums), then the
                 // remaining track-title matches — the tree's presentation order.
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
                     status_->setText(outcome.error);
                     return;
                 }
                 if (outcome.rows.empty()) {
                     status_->setText(tr("No search results to keep."));
                     return;
                 }
                 status_->setText(tr("Search kept as a new tab."));
                 emit searchCommitted(query_text, std::move(outcome.rows));
             }});
}

void LocalLibraryPanel::addRoot(std::string raw_path) {
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
                     status_->setText(tr("Folder added. Press Refresh to scan for music."));
                     loadRoots();
                     refreshLibrary();
                 } else {
                     status_->setText(outcome.error);
                 }
                 if (folders_widget_) {
                     roots_error_->setText(outcome.error);
                 }
             }});
}

void LocalLibraryPanel::showFolders() {
    if (receivers(SIGNAL(manageFoldersRequested())) > 0) {
        emit manageFoldersRequested();
        return;
    }
    if (folders_dialog_) {
        folders_dialog_->raise();
        folders_dialog_->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("local-library-folders-dialog"));
    dialog->setWindowTitle(catalogues_ != nullptr &&
                                   catalogues_->role() == CatalogueSource::Role::remote
                               ? tr("Library folders on %1").arg(catalogues_->name())
                               : tr("Local library folders"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(560, 320);
    folders_dialog_ = dialog;
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(createFoldersWidget(dialog));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}

QWidget* LocalLibraryPanel::createFoldersWidget(QWidget* parent) {
    auto* widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("local-library-folders-settings"));
    folders_widget_ = widget;
    auto* layout = new QVBoxLayout(widget);
    const bool remote =
        catalogues_ != nullptr && catalogues_->role() == CatalogueSource::Role::remote;
    auto* explanation = new QLabel(
        remote ? tr("Folders on %1 for its engine to index. Give each path as that machine "
                    "sees it. Folder changes are saved immediately. Removing a folder leaves its "
                    "files untouched.")
                     .arg(catalogues_->name())
               : tr("Choose the folders to browse and search as your local music library. "
                    "Folder changes are saved immediately. Removing a folder leaves its files "
                    "untouched. "),
        widget);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* scan_note =
        new QLabel(tr("Only Refresh in the Library sidebar scans your folders for music."), widget);
    scan_note->setWordWrap(true);
    layout->addWidget(scan_note);
    roots_list_ = new QListWidget(widget);
    roots_list_->setObjectName(QStringLiteral("local-library-roots"));
    layout->addWidget(roots_list_, 1);
    roots_error_ = new QLabel(widget);
    roots_error_->setObjectName(QStringLiteral("local-library-folder-error"));
    roots_error_->setWordWrap(true);
    layout->addWidget(roots_error_);
    auto* buttons = new QDialogButtonBox(widget);
    auto* add = buttons->addButton(tr("Add folder…"), QDialogButtonBox::ActionRole);
    auto* remove = buttons->addButton(tr("Remove"), QDialogButtonBox::ActionRole);
    add->setObjectName(QStringLiteral("local-library-folder-add"));
    remove->setObjectName(QStringLiteral("local-library-folder-remove"));
    remove->setEnabled(false);
    connect(roots_list_, &QListWidget::currentRowChanged, remove,
            [remove](int row) { remove->setEnabled(row >= 0); });
    connect(add, &QPushButton::clicked, this, [this] {
        if (catalogues_ != nullptr && catalogues_->role() == CatalogueSource::Role::remote) {
            // ADR-0227: a folder on the remote machine cannot be browsed from
            // here yet -- this computer's file dialog would offer this
            // computer's folders. Until the engine can list its own, the path
            // is typed as that machine sees it, and the engine checks it.
            bool accepted = false;
            const auto path = QInputDialog::getText(
                folders_widget_, tr("Add music folder"),
                tr("Folder on %1, as that machine sees it:").arg(catalogues_->name()),
                QLineEdit::Normal, {}, &accepted);
            if (accepted && !path.trimmed().isEmpty()) {
                addRoot(QFile::encodeName(path.trimmed()).toStdString());
            }
            return;
        }
        const auto path =
            QFileDialog::getExistingDirectory(folders_widget_, tr("Add music folder"));
        if (!path.isEmpty()) {
            addRoot(QFile::encodeName(path).toStdString());
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!folders_widget_ || roots_list_->currentItem() == nullptr) {
            return;
        }
        const auto path =
            roots_list_->currentItem()->data(Qt::UserRole).toByteArray().toStdString();
        enqueue({[path](engine::Catalogue& library) {
                     Outcome outcome;
                     auto result = library.remove_root(path);
                     if (!result) {
                         outcome.error = text(result.error().message);
                     }
                     return outcome;
                 },
                 [this](Outcome outcome) {
                     if (outcome.error.isEmpty()) {
                         loadRoots();
                         reloadTree();
                     } else {
                         status_->setText(outcome.error);
                     }
                     if (folders_widget_) {
                         roots_error_->setText(outcome.error);
                     }
                 }});
    });
    layout->addWidget(buttons);
    loadRoots();
    return widget;
}

void LocalLibraryPanel::loadRoots() {
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
                     status_->setText(outcome.error);
                     return;
                 }
                 has_roots_ = !outcome.roots.empty();
                 if (outcome.roots.empty()) {
                     status_->setText(tr("Choose Folders… to add your music collection."));
                 }
                 // The empty line may have been drawn before this was known.
                 for (int row = 0; row < model_->rowCount(); ++row) {
                     if (auto* item = model_->item(row); item && item->data(empty_state_role).toBool()) {
                         item->setText(emptyLibraryText());
                     }
                 }
                 std::size_t offline = 0;
                 if (folders_widget_) {
                     roots_list_->clear();
                 }
                 for (const auto& root : outcome.roots) {
                     const auto unavailable = !root.available && !root.error.empty();
                     if (unavailable) {
                         ++offline;
                     }
                     if (!folders_widget_) {
                         continue;
                     }
                     auto* item = new QListWidgetItem(pathLabel(root.raw_path) +
                                                          (root.available ? QString{}
                                                           : unavailable  ? tr(" — unavailable")
                                                                          : tr(" — not scanned")),
                                                      roots_list_);
                     item->setData(Qt::UserRole, QByteArray::fromStdString(root.raw_path));
                     item->setToolTip(text(root.error));
                 }
                 if (offline > 0U && !scanning_) {
                     status_->setText(
                         tr("%1 folders unavailable. Cached music is still shown.").arg(offline));
                 }
             }});
}

void LocalLibraryPanel::refreshLibrary() {
    if (!stopped_) {
        change_timer_->start();
    }
}

void LocalLibraryPanel::startScan() {
    if (stopped_ || scanning_) {
        return;
    }
    scanning_ = true;
    setProperty("scanning", true);
    scan_cancellation_ = core::CancellationSource{};
    progress_ = std::make_shared<persistence::LibraryScanProgress>();
    scan_button_->setText(tr("Stop"));
    scan_button_->setToolTip(tr("Stop scanning"));
    scan_button_->setIcon(QIcon::fromTheme(QStringLiteral("process-stop")));
    poll_timer_->start();
    updateProgress();
    scan_watcher_.setFuture(QtConcurrent::run(&pool_, [catalogues = catalogues_,
                                                       cancellation = scan_cancellation_.token(),
                                                       progress = progress_] {
        ScanOutcome outcome;
        // ADR-0220: ask the core, do not open its database. The job shape
        // around this call -- pool, poll timer, token, watcher -- is
        // unchanged whichever side answers; remotely it becomes a job,
        // and the same counters are fed from its progress events.
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

bool LocalLibraryPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tree_->viewport() && artwork_timer_ && !stopped_) {
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize) {
            artwork_timer_->start();
        } else if (event->type() == QEvent::Hide) {
            artwork_timer_->stop();
            artwork_cancellation_.request_cancellation();
        }
    }
    return QWidget::eventFilter(watched, event);
}

QModelIndexList LocalLibraryPanel::visibleAlbums() const {
    QModelIndexList albums;
    auto index = tree_->indexAt(QPoint{tree_->viewport()->width() / 2, 0});
    for (int visited = 0; index.isValid() && visited < 128;
         ++visited, index = tree_->indexBelow(index)) {
        if (tree_->visualRect(index).top() >= tree_->viewport()->height()) {
            break;
        }
        if (index.data(entry_role).isValid()) {
            const auto entry = index.data(entry_role).value<persistence::LibraryEntry>();
            if (entry.kind == persistence::LibraryEntryKind::album && entry.available > 0U) {
                albums.push_back(index);
            }
        }
    }
    return albums;
}

void LocalLibraryPanel::invalidateArtwork() {
    ++artwork_generation_;
    artwork_cancellation_.request_cancellation();
    artwork_cache_.clear();
    tree_->viewport()->update();
}

void LocalLibraryPanel::updateArtwork() {
    if (stopped_ || !tree_->isVisible()) {
        return;
    }
    const auto albums = visibleAlbums();
    if (artwork_running_) {
        if (std::ranges::none_of(albums, [this](const QModelIndex& index) {
                return QByteArray::fromStdString(
                           index.data(entry_role).value<persistence::LibraryEntry>().key) ==
                       artwork_key_;
            })) {
            artwork_cancellation_.request_cancellation();
        }
        return;
    }
    for (const auto& index : albums) {
        const auto key = QByteArray::fromStdString(
            index.data(entry_role).value<persistence::LibraryEntry>().key);
        if (artwork_cache_.contains(key)) {
            continue;
        }
        artwork_key_ = key;
        artwork_job_generation_ = artwork_generation_;
        artwork_cancellation_ = core::CancellationSource{};
        artwork_running_ = true;
        artwork_watcher_.setFuture(
            QtConcurrent::run(&artwork_pool_, [catalogues = catalogues_, key,
                                               cancellation = artwork_cancellation_.token()] {
                if (cancellation.is_cancellation_requested()) {
                    return QImage{};
                }
                // ADR-0220: ask the core, do not open its database.
                //
                // The artwork pool is separate from the query pool, so with a
                // engine connection both share one connection and their calls
                // serialise. Acceptable while covers are the only thing on
                // that pool; it is the first place a second connection would
                // be worth having.
                auto catalogue = catalogues->open();
                const auto source = catalogue->artwork_source(key.toStdString(), cancellation);
                if (!source || !source->has_value() || cancellation.is_cancellation_requested()) {
                    return QImage{};
                }
                // Read by the engine, where the files are: a remote library
                // shows its covers with nothing of it mounted here.
                const auto bytes = catalogue->artwork(**source, cancellation);
                return bytes ? ui::artworkThumbnail(*bytes) : QImage{};
            }));
        return;
    }
}

void LocalLibraryPanel::updateProgress() {
    if (!progress_) {
        return;
    }
    if (scan_cancellation_.is_cancellation_requested()) {
        status_->setText(tr("Stopping scan…"));
        return;
    }
    status_->setText(tr("Scanning… %1 entries checked, %2 files updated, %3 unreadable")
                         .arg(progress_->visited.load())
                         .arg(progress_->indexed.load())
                         .arg(progress_->failed.load()));
}

} // namespace trackknife::bench
