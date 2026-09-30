// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_library_panel.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/settings_dialog.hpp"
#include "uicommon/library_tree_view.hpp"
#include "uicommon/rating_stars.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int entry_role = library_entry_role;

std::array<QIcon, 3> libraryActionIcons(const QWidget* widget) {
    return {QIcon::fromTheme(QStringLiteral("list-add"),
                             widget->style()->standardIcon(QStyle::SP_DialogOpenButton)),
            QIcon::fromTheme(QStringLiteral("go-next"),
                             widget->style()->standardIcon(QStyle::SP_ArrowRight)),
            QIcon::fromTheme(QStringLiteral("media-playback-start"),
                             widget->style()->standardIcon(QStyle::SP_MediaPlay))};
}

} // namespace

LocalLibraryPanel::LocalLibraryPanel(const CatalogueSource& catalogues, EngineKey engine,
                                     QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("bench-local-library"));
    browser_ = new LibraryBrowser(catalogues, std::move(engine), this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto* search_row = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("local-library-search"));
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
    search_row->addWidget(query_toggle_);
    layout->addLayout(search_row);
    query_error_ = new QLabel(this);
    query_error_->setObjectName(QStringLiteral("local-library-query-error"));
    query_error_->setWordWrap(true);
    query_error_->hide();
    layout->addWidget(query_error_);
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
    connect(scan_button_, &QToolButton::clicked, browser_, &LibraryBrowser::toggleScan);
    newest_toggle_ = icon_button(QStringLiteral("local-library-newest"),
                                 QStringLiteral("document-open-recent"), tr("Recently added"));
    newest_toggle_->setCheckable(true);
    newest_toggle_->setToolTip(tr("Show albums newest first, as they came into the library"));
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
        if (!index.data(entry_role).isValid()) {
            return;
        }
        if (!tree_->selectionModel()->isSelected(index)) {
            tree_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
        }
        browser_->request(tree_->selectionModel()->selectedRows(), action);
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
    tree_->setModel(browser_->model());
    tree_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeView::expanded, browser_,
            [this](const QModelIndex& index) { browser_->noteExpanded(index, true); });
    connect(tree_, &QTreeView::collapsed, browser_,
            [this](const QModelIndex& index) { browser_->noteExpanded(index, false); });
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, browser_,
            &LibraryBrowser::noteCurrent);
    connect(tree_, &QTreeView::activated, this, [this](const QModelIndex& index) {
        if (index.data(library_more_role).toBool() || !index.data(entry_role).isValid()) {
            browser_->activate(index);
            return;
        }
        if (!tree_->selectionModel()->isSelected(index)) {
            tree_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
        }
        browser_->request(tree_->selectionModel()->selectedRows(),
                          static_cast<int>(LocalLibraryAction::append));
    });
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
    status_ = new QLabel(browser_->status(), footer);
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

    // What the browser says, shown.
    const auto show_search = [this] {
        {
            const QSignalBlocker blocker{search_};
            if (search_->text() != browser_->search()) {
                search_->setText(browser_->search());
            }
        }
        {
            const QSignalBlocker blocker{query_toggle_};
            query_toggle_->setChecked(browser_->queryMode());
        }
        {
            const QSignalBlocker blocker{newest_toggle_};
            newest_toggle_->setChecked(browser_->newestFirst());
        }
        search_->setPlaceholderText(browser_->queryMode() ? tr("tkq query, e.g. genre HAS jazz")
                                                          : tr("Search albums and tracks"));
    };
    show_search();
    connect(browser_, &LibraryBrowser::searchChanged, this, show_search);
    connect(search_, &QLineEdit::textChanged, browser_, &LibraryBrowser::setSearch);
    connect(query_toggle_, &QCheckBox::toggled, browser_, &LibraryBrowser::setQueryMode);
    connect(newest_toggle_, &QToolButton::toggled, browser_, &LibraryBrowser::setNewestFirst);
    // ADR-0140: Enter keeps the current hits as a durable list; the
    // live-filtered tree stays the transient default.
    connect(search_, &QLineEdit::returnPressed, browser_, &LibraryBrowser::commitSearch);
    connect(browser_, &LibraryBrowser::statusChanged, this,
            [this] { status_->setText(browser_->status()); });
    connect(browser_, &LibraryBrowser::queryErrorChanged, this, [this] {
        query_error_->setText(browser_->queryError());
        query_error_->setVisible(!browser_->queryError().isEmpty());
    });
    const auto show_source = [this] {
        source_label_->setText(browser_->source());
        source_label_->setVisible(browser_->sourceShown());
        source_label_->setToolTip(browser_->sourceTooltip());
    };
    show_source();
    connect(browser_, &LibraryBrowser::sourceChanged, this, show_source);
    connect(browser_, &LibraryBrowser::scanningChanged, this, &LocalLibraryPanel::refreshScanButton);
    connect(browser_, &LibraryBrowser::rootsChanged, this, &LocalLibraryPanel::refreshRoots);
    connect(browser_, &LibraryBrowser::expandRequested, tree_,
            [this](const QModelIndex& index) { tree_->expand(index); });
    connect(browser_, &LibraryBrowser::currentRequested, tree_,
            [this](const QModelIndex& index, const bool focus) {
                tree_->setCurrentIndex(index);
                if (focus) {
                    tree_->scrollTo(index);
                    tree_->setFocus();
                }
            });
    connect(browser_, &LibraryBrowser::reloadStarted, tree_,
            [library_view] { library_view->cancelPendingExpansions(); });
    connect(browser_, &LibraryBrowser::levelLoaded, tree_,
            [library_view] { library_view->completePendingExpansions(); });
    connect(browser_, &LibraryBrowser::coverLoaded, tree_, [this] { tree_->viewport()->update(); });
    // What it asks of the window, passed on.
    connect(browser_, &LibraryBrowser::actionRequested, this, &LocalLibraryPanel::actionRequested);
    connect(browser_, &LibraryBrowser::addToListRequested, this,
            &LocalLibraryPanel::addToListRequested);
    connect(browser_, &LibraryBrowser::searchCommitted, this, &LocalLibraryPanel::searchCommitted);
    connect(browser_, &LibraryBrowser::ratingsChanged, this, &LocalLibraryPanel::ratingsChanged);
    connect(browser_, &LibraryBrowser::libraryContentChanged, this,
            &LocalLibraryPanel::libraryContentChanged);

    // Covers for the albums in view.
    artwork_timer_ = new QTimer(this);
    artwork_timer_->setSingleShot(true);
    artwork_timer_->setInterval(0);
    connect(artwork_timer_, &QTimer::timeout, this, &LocalLibraryPanel::requestCovers);
    tree_->viewport()->installEventFilter(this);
    connect(tree_->verticalScrollBar(), &QScrollBar::valueChanged, artwork_timer_,
            qOverload<>(&QTimer::start));
    connect(tree_, &QTreeView::expanded, artwork_timer_, qOverload<>(&QTimer::start));
    connect(tree_, &QTreeView::collapsed, artwork_timer_, qOverload<>(&QTimer::start));
    connect(browser_->model(), &QAbstractItemModel::rowsInserted, artwork_timer_,
            qOverload<>(&QTimer::start));
    connect(browser_, &LibraryBrowser::coverLoaded, artwork_timer_, qOverload<>(&QTimer::start));
}

LocalLibraryPanel::~LocalLibraryPanel() { stop(); }

void LocalLibraryPanel::focusSearch() {
    search_->setFocus(Qt::ShortcutFocusReason);
    search_->selectAll();
}

void LocalLibraryPanel::stop() {
    if (artwork_timer_ != nullptr) {
        artwork_timer_->stop();
    }
    browser_->stop();
}

void LocalLibraryPanel::refreshScanButton() {
    const auto scanning = browser_->scanning();
    setProperty("scanning", scanning);
    scan_button_->setText(scanning ? tr("Stop") : tr("Refresh"));
    scan_button_->setToolTip(scanning ? tr("Stop scanning") : tr("Refresh"));
    scan_button_->setIcon(QIcon::fromTheme(scanning ? QStringLiteral("process-stop")
                                                    : QStringLiteral("view-refresh")));
}

bool LocalLibraryPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tree_->viewport() && artwork_timer_) {
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize) {
            artwork_timer_->start();
        } else if (event->type() == QEvent::Hide) {
            artwork_timer_->stop();
            browser_->wantCovers({});
        }
    }
    return QWidget::eventFilter(watched, event);
}

// The albums in view, whose covers are to be loaded.
void LocalLibraryPanel::requestCovers() {
    if (!tree_->isVisible()) {
        return;
    }
    QStringList keys;
    auto index = tree_->indexAt(QPoint{tree_->viewport()->width() / 2, 0});
    for (int visited = 0; index.isValid() && visited < 128;
         ++visited, index = tree_->indexBelow(index)) {
        if (tree_->visualRect(index).top() >= tree_->viewport()->height()) {
            break;
        }
        if (index.data(entry_role).isValid()) {
            const auto entry = index.data(entry_role).value<persistence::LibraryEntry>();
            if (entry.kind == persistence::LibraryEntryKind::album && entry.available > 0U) {
                keys.push_back(QString::fromStdString(entry.key));
            }
        }
    }
    browser_->wantCovers(keys);
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
    const auto entries = LibraryBrowser::selectedEntries(tree_->selectionModel()->selectedRows());
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
        // Beside "Append to current list": to any list of this engine's.
        if (action == 0) {
            auto* lists = menu->addMenu(QIcon::fromTheme(QStringLiteral("view-media-playlist")),
                                        tr("Add to list"));
            lists->setObjectName(QStringLiteral("local-library-add-to-list"));
            const auto targets = list_targets_ ? list_targets_()
                                               : std::vector<std::pair<QString, QString>>{};
            for (std::size_t target = 0; target < targets.size(); ++target) {
                const auto& [id, name] = targets[target];
                auto* choice = lists->addAction(name);
                choice->setObjectName(QStringLiteral("action-local-library-add-to-%1").arg(target));
                connect(choice, &QAction::triggered, this,
                        [this, entries, id] { emit addToListRequested(entries, id); });
            }
            lists->setEnabled(available && !targets.empty());
        }
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
            connect(choice, &QAction::triggered, this, [this, target, rating] {
                if (target.isValid()) {
                    browser_->rate(target, static_cast<int>(rating));
                }
            });
        }
    }
    if (browser_->model()->hasChildren(index)) {
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
    dialog->setWindowTitle(browser_->remote() ? tr("Library folders on %1").arg(browser_->name())
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
    auto* explanation = new QLabel(
        !browser_->engine().isLocal() && browser_->catalogues() != nullptr
            ? tr("Folders on %1 for its engine to index. Give each path as that machine "
                 "sees it. Folder changes are saved immediately. Removing a folder leaves its "
                 "files untouched.")
                  .arg(browser_->name())
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
        if (browser_->remote()) {
            // ADR-0227: a folder on the remote machine cannot be browsed from
            // here yet -- this computer's file dialog would offer this
            // computer's folders. Until the engine can list its own, the path
            // is typed as that machine sees it, and the engine checks it.
            bool accepted = false;
            const auto path = QInputDialog::getText(
                folders_widget_, tr("Add music folder"),
                tr("Folder on %1, as that machine sees it:").arg(browser_->name()),
                QLineEdit::Normal, {}, &accepted);
            if (accepted && !path.trimmed().isEmpty()) {
                browser_->addRoot(QFile::encodeName(path.trimmed()).toStdString());
            }
            return;
        }
        const auto path =
            QFileDialog::getExistingDirectory(folders_widget_, tr("Add music folder"));
        if (!path.isEmpty()) {
            browser_->addRoot(QFile::encodeName(path).toStdString());
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!folders_widget_ || roots_list_->currentItem() == nullptr) {
            return;
        }
        browser_->removeRoot(
            roots_list_->currentItem()->data(Qt::UserRole).toByteArray().toStdString());
    });
    layout->addWidget(buttons);
    refreshRoots();
    browser_->loadRoots();
    return widget;
}

void LocalLibraryPanel::refreshRoots() {
    if (!folders_widget_ || roots_list_ == nullptr) {
        return;
    }
    roots_list_->clear();
    for (const auto& root : browser_->rootList()) {
        auto* item = new QListWidgetItem(root.label, roots_list_);
        item->setData(Qt::UserRole, QByteArray::fromStdString(root.raw_path));
        item->setToolTip(root.tooltip);
    }
    roots_error_->setText(browser_->rootsError());
}

} // namespace trackknife::bench
