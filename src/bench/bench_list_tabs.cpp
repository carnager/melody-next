// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "workspace/open_list_session.hpp"
#include "workspace/sources.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/playback_tab_widget.hpp"
#include "bench/remote_mount.hpp"
#include "bench/search_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/track_list_find_bar.hpp"
#include "uicommon/debug_log.hpp"
#include "uicommon/local_files_mime_data.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/rating_stars.hpp"
#include "uicommon/track_row_roles.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::bench {
void BenchMainWindow::initializePersistence() {
    workspace_.start();
    connect(list_sync_, &EngineListSync::conflicted, this, &BenchMainWindow::settleListConflict);
    connect(list_sync_, &EngineListSync::removedElsewhere, this, [this](const QString& id) {
        // Deleted by another client: closed here too, with nothing to ask.
        if (auto* tab = tabForDocument(id); tab != nullptr) {
            tab->document.dirty = false;
            tab->document.pinned = false;
            closeTabAt(tabs_->indexOf(tab->view));
        }
    });
}

int BenchMainWindow::currentRow(const ListTab& tab) {
    return tab.view != nullptr && tab.view->currentIndex().isValid()
               ? tab.view->currentIndex().row()
               : -1;
}

void BenchMainWindow::workspaceRestored(const bool restored) {
    applyLocalLibraryVisibility();
    if (restored) {
        localEngine().library =
            new LocalLibraryPanel(*localCatalogue(), EngineKey::local(), source_stack_);
        connect(localLibrary(), &LocalLibraryPanel::manageFoldersRequested, this,
                [this] { showSettingsDialog(SettingsDialog::Page::library); });
        source_stack_->addWidget(localLibrary());
        workspace_.attachLibrary(&localLibrary()->browser());
        // "Add to list": this computer's lists, in tab order.
        localLibrary()->setListTargets([this] { return listTargets(EngineKey::local()); });
        // Restored rows carry their identity hashes; load stored values
        // once the rating store is reachable.
        refreshLocalRatings();
        refreshActiveContext();
    }
}

void BenchMainWindow::backupWorkspace() {
    if (persistence_ == nullptr) {
        return;
    }
    auto* dialog = new QFileDialog(this, tr("Back up Trackknife workspace database"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setDefaultSuffix(QStringLiteral("sqlite"));
    dialog->setNameFilter(tr("Trackknife workspace database (*.sqlite)"));
    dialog->setOption(QFileDialog::DontConfirmOverwrite);
    dialog->selectFile(QStringLiteral("trackknife-workspace.sqlite"));
    connect(dialog, &QFileDialog::fileSelected, this,
            [this](const QString& path) { workspace_.backupWorkspace(path); });
    dialog->open();
}

void BenchMainWindow::scheduleWorkspaceRestore() {
    const auto path =
        QFileDialog::getOpenFileName(this, tr("Restore Trackknife workspace database"), {},
                                     tr("Trackknife workspace database (*.sqlite);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    const auto answer = QMessageBox::warning(
        this, tr("Restore workspace database"),
        tr("Trackknife will validate and restore this database at the next start. "
           "The current database will be retained beside it for rollback. Close Trackknife now?"),
        QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Close) {
        return;
    }
    Workspace::scheduleWorkspaceRestore(path);
    close();
}


int BenchMainWindow::engineRank(const QWidget* view) const {
    const auto engine = engineOfView(view);
    for (std::size_t index = 0; index < engines_.size(); ++index) {
        if (engines_[index]->key == engine) {
            return static_cast<int>(index);
        }
    }
    return static_cast<int>(engines_.size());
}

void BenchMainWindow::keepTabGroupsTogether() {
    if (regrouping_tabs_) {
        return;
    }
    regrouping_tabs_ = true;
    // ADR-0234: each engine's tabs together, in the order the engines are
    // -- this computer's first -- and within each group as they were left.
    // A tab dragged into another group goes back to its own.
    int next = 0;
    for (int rank = 0; rank <= static_cast<int>(engines_.size()); ++rank) {
        for (int index = next; index < tabs_->count(); ++index) {
            if (engineRank(tabs_->widget(index)) == rank) {
                if (index != next) {
                    tabs_->tabBar()->moveTab(index, next);
                }
                ++next;
            }
        }
    }
    regrouping_tabs_ = false;
    tabs_->tabBar()->update();
}

void BenchMainWindow::showOpenListDialog() {
    if (auto* existing = findChild<QDialog*>(QStringLiteral("bench-open-list"))) {
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("bench-open-list"));
    dialog->setWindowTitle(QStringLiteral("Open list"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* session = new OpenListSession(workspace_, dialog);
    auto* layout = new QVBoxLayout(dialog);
    auto* tree = new QTreeWidget(dialog);
    tree->setObjectName(QStringLiteral("bench-open-list-tree"));
    tree->setHeaderLabels({QStringLiteral("List"), QStringLiteral("Tracks")});
    tree->setRootIsDecorated(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(tree);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);
    dialog->resize(480, 420);
    const auto open = [tree, session] {
        auto* item = tree->currentItem();
        if (item == nullptr || item->parent() == nullptr) {
            return;
        }
        session->open(tree->indexOfTopLevelItem(item->parent()), item->parent()->indexOfChild(item));
    };
    connect(buttons, &QDialogButtonBox::accepted, dialog, open);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(tree, &QTreeWidget::itemActivated, dialog, open);
    connect(session, &OpenListSession::opened, dialog, &QDialog::accept);
    const auto fill = [tree, session] {
        tree->clear();
        for (const auto& group : session->groups()) {
            auto* parent = new QTreeWidgetItem(tree, {group.name});
            parent->setFlags(Qt::ItemIsEnabled);
            for (const auto& list : group.lists) {
                new QTreeWidgetItem(parent, {list.label, QString::number(list.tracks)});
            }
            if (!group.note.isEmpty()) {
                auto* none = new QTreeWidgetItem(parent, {group.note});
                none->setFlags(Qt::NoItemFlags);
            }
            parent->setExpanded(true);
        }
    };
    connect(session, &OpenListSession::changed, dialog, fill);
    fill();
    dialog->open();
}

void BenchMainWindow::settleListConflict(const QString& id) {
    auto* tab = tabForDocument(id);
    if (tab == nullptr || list_sync_ == nullptr) {
        return;
    }
    auto* question = new QMessageBox(
        QMessageBox::Question, QStringLiteral("List changed elsewhere"),
        QStringLiteral("“%1” was saved from somewhere else since you opened it. Keep which?")
            .arg(displayText(tab->document.name)),
        QMessageBox::NoButton, this);
    question->setObjectName(QStringLiteral("bench-list-conflict"));
    question->setAttribute(Qt::WA_DeleteOnClose);
    question->setOption(QMessageBox::Option::DontUseNativeDialog);
    auto* theirs = question->addButton(QStringLiteral("Reload theirs"), QMessageBox::RejectRole);
    theirs->setObjectName(QStringLiteral("bench-list-conflict-theirs"));
    auto* mine = question->addButton(QStringLiteral("Keep mine"), QMessageBox::AcceptRole);
    mine->setObjectName(QStringLiteral("bench-list-conflict-mine"));
    auto* copy =
        question->addButton(QStringLiteral("Save mine as a copy"), QMessageBox::ActionRole);
    copy->setObjectName(QStringLiteral("bench-list-conflict-copy"));
    question->setDefaultButton(copy);
    connect(question, &QMessageBox::buttonClicked, this,
            [this, id, theirs, mine](QAbstractButton* clicked) {
                auto* conflicting = tabForDocument(id);
                if (conflicting == nullptr || list_sync_ == nullptr) {
                    return;
                }
                if (clicked == mine) {
                    list_sync_->keepMine(id);
                    return;
                }
                if (clicked != theirs) {
                    // Mine as a list of its own, beside theirs.
                    auto copied = conflicting->document;
                    copied.id = core::StableId::random();
                    copied.name = utf8Bytes(
                        QStringLiteral("%1 (mine)").arg(displayText(conflicting->document.name)));
                    copied.kind = persistence::ListKind::saved;
                    copied.dirty = false;
                    copied.pinned = false;
                    copied.items.clear();
                    auto* mine_tab = addListTab(std::move(copied), false);
                    auto rows = conflicting->model->rows();
                    for (auto& row : rows) {
                        row.entry_id = core::StableId::random();
                    }
                    mine_tab->model->replaceRows(std::move(rows));
                    syncArtwork(*mine_tab);
                    schedulePersist();
                }
                list_sync_->takeTheirs(id);
            });
    question->open();
}

void BenchMainWindow::listAdded(ListTab& added, const bool select) {
    const auto id = QString::fromStdString(added.document.id.to_string());
    auto* model = added.model;
    const auto& document = added.document;

    auto* view = new ui::QueueTableView(tabs_);
    view->setObjectName(QStringLiteral("bench-list-%1").arg(id.left(8)));
    markViewEngine(view, EngineKey::of(document));
    view->setEmptyMessage(emptyListTitle(EngineKey::of(document)),
                          emptyListHint(EngineKey::of(document)));
    view->setProperty("bench-document-id", id);
    view->setModel(model);
    connect(
        model, &LocalListModel::historyRowsRestored, view, [view, model](const QList<int>& rows) {
            QItemSelection selection;
            for (const auto row : rows)
                selection.select(model->index(row, 0), model->index(row, model->columnCount() - 1));
            view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
            if (!rows.empty())
                view->selectionModel()->setCurrentIndex(model->index(rows.front(), 0),
                                                        QItemSelectionModel::NoUpdate);
        });
    connect(model, &QAbstractItemModel::rowsInserted, this,
            &BenchMainWindow::refreshListHistoryActions);
    connect(model, &QAbstractItemModel::rowsRemoved, this,
            &BenchMainWindow::refreshListHistoryActions);
    connect(model, &QAbstractItemModel::modelReset, this,
            &BenchMainWindow::refreshListHistoryActions);
    view->addAction(undo_list_action_);
    view->addAction(redo_list_action_);
    connect(model, &LocalListModel::historyChanged, this,
            &BenchMainWindow::refreshListHistoryActions);
    connect(model, &LocalListModel::historyDiscarded, this,
            [this](const QString& reason) { statusBar()->showMessage(reason, 5'000); });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex& first) {
        if (first.column() < local_play_count_column)
            refreshSelectionStatus();
    });
    connect(model, &QAbstractItemModel::rowsInserted, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::modelReset, this, [this] { refreshSelectionStatus(); });
    view->setProperty("trackknife-hover-row", -1);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
    view->verticalHeader()->setDefaultSectionSize(22);
    view->verticalHeader()->setMinimumSectionSize(18);
    view->verticalHeader()->hide();
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->horizontalHeader()->setSectionsMovable(true);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->horizontalHeader()->setMinimumSectionSize(24);
    view->horizontalHeader()->setMaximumSectionSize(4'096);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view->horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackViewHeaderMenu(view, position); });
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackContextMenu(view, position); });
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDragDropOverwriteMode(false);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setActivateCallback([this, id](const QModelIndex& index) {
        auto* tab = tabForDocument(id);
        if (tab != nullptr && index.isValid()) {
            playRow(*tab, index.row());
        }
    });
    connect(view, &QTableView::doubleClicked, this, [this, id](const QModelIndex& index) {
        auto* tab = tabForDocument(id);
        if (tab != nullptr && index.isValid()) {
            playRow(*tab, index.row());
        }
    });
    view->setReorderCallback([this, id](const QVariantList& rows, const int insertion_row) {
        auto* tab = tabForDocument(id);
        if (tab == nullptr) {
            return;
        }
        std::vector<int> row_indexes;
        row_indexes.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto& row : rows) {
            row_indexes.push_back(row.toInt());
        }
        tab->model->reorderRows(std::move(row_indexes), insertion_row);
        markTabDirty(*tab);
    });
    view->setExternalDropCallback([this, id](QAbstractItemView* source, const QVariantList& rows,
                                             const int insertion_row, const Qt::DropAction action) {
        auto* table = qobject_cast<QTableView*>(source);
        return table != nullptr &&
               transferRows(table, rows, id, action == Qt::MoveAction, insertion_row);
    });
    view->setLocalFilesDropCallback(
        [this, id](const ui::LocalFilesMimeData& files, int insertion_row) {
            auto* target = tabForDocument(id);
            if (target == nullptr) {
                return false;
            }
            Workspace::Dragged dragged;
            dragged.engine = files.engine();
            dragged.resolve = [&files](std::function<void(std::vector<std::string>)> done) {
                files.resolve(std::move(done));
            };
            return workspace_.dropOnList(std::move(dragged), *target, insertion_row, true);
        });
    view->setLocalUrlDropCallback([this, id](const QList<QUrl>& urls, const int insertion_row) {
        auto* target = tabForDocument(id);
        if (target == nullptr) {
            return false;
        }
        // Files from a file manager are this computer's.
        Workspace::Dragged dragged;
        for (const auto& url : urls) {
            if (url.isLocalFile()) {
                const auto encoded = QFile::encodeName(url.toLocalFile());
                dragged.paths.emplace_back(encoded.constData(),
                                           static_cast<std::size_t>(encoded.size()));
            }
        }
        return !dragged.paths.empty() &&
               workspace_.dropOnList(std::move(dragged), *target, insertion_row, true);
    });

    // ADR-0233, ADR-0234: at the end of its engine's group.
    int insert_at = tabs_->count();
    const auto rank = engineRank(view);
    for (int other = 0; other < tabs_->count(); ++other) {
        if (engineRank(tabs_->widget(other)) > rank) {
            insert_at = other;
            break;
        }
    }
    const auto index = tabs_->insertTab(insert_at, view, displayText(document.name));
    if (!EngineKey::of(document).isLocal()) {
        // ADR-0227: which engine a tab plays on is visible, not remembered.
        tabs_->setTabIcon(index, QIcon::fromTheme(QStringLiteral("network-server")));
        tabs_->setTabToolTip(index, tr("Plays on the remote engine"));
    }
    added.view = view;
    auto* raw_tab = &added;
    view->setProperty("bench-tab-pointer", QVariant::fromValue<void*>(raw_tab));
    const auto layout = workspace_.restoredTrackViewLayout(*raw_tab);
    applyTrackViewLayout(*raw_tab, layout);
    connect(view->horizontalHeader(), &QHeaderView::sectionMoved, this,
            [this, id](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                auto* moved_tab = tabForDocument(id);
                if (moved_tab == nullptr) {
                    return;
                }
                moved_tab->view_layout = captureTrackViewLayout(*moved_tab);
                moved_tab->view_layout_persistence_protected = false;
                moved_tab->preserved_view_layout.clear();
                schedulePersist();
                refreshTrackViewActions();
            });
    connect(view->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this, id](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                auto* resized_tab = tabForDocument(id);
                if (resized_tab == nullptr) {
                    return;
                }
                resized_tab->view_layout = captureTrackViewLayout(*resized_tab);
                resized_tab->view_layout_persistence_protected = false;
                resized_tab->preserved_view_layout.clear();
                schedulePersist();
            });
    refreshTabChrome(*raw_tab);
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    refreshTabActions();
    refreshSelectionStatus();
}

void BenchMainWindow::openSearchDialog() {
    // Opened from a tab of another engine, it searches that engine's library.
    const auto* current = currentListTab();
    const auto from = current != nullptr ? EngineKey::of(current->document) : EngineKey::local();
    if (search_dialog_ != nullptr) {
        search_dialog_->followLibrary(from);
        search_dialog_->show();
        search_dialog_->raise();
        search_dialog_->activateWindow();
        search_dialog_->focusInput();
        return;
    }
    // ADR-0153: database scope reads the workspace index; tab scope
    // snapshots the current local tab's rows and reports on-demand
    // technicals back onto every tab holding the probed file.
    search_dialog_ = new SearchDialog(
        *localCatalogue(),
        [this]() -> std::optional<SearchDialog::TabSnapshot> {
            auto* tab = currentListTab();
            if (tab == nullptr) {
                return std::nullopt;
            }
            return SearchDialog::TabSnapshot{QString::fromUtf8(tab->document.name),
                                             tab->model->rows()};
        },
        [this](std::string raw_path, LocalTrackTechnicals technicals) {
            for (const auto& tab : list_tabs_) {
                tab->model->applyTechnicals(raw_path, technicals);
            }
        },
        this,
        [this] {
            std::vector<SearchDialog::OtherLibrary> others;
            for (const auto& engine : engines_) {
                if (!engine->key.isLocal() && engine->catalogue != nullptr) {
                    others.push_back({.engine = engine->key,
                                      .catalogues = engine->catalogue.get(),
                                      .name = engine->catalogue->name()});
                }
            }
            return others;
        }());
    search_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    const auto watch_tab = [this, dialog = search_dialog_] {
        if (!dialog)
            return;
        auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
        dialog->watchCurrentModel(view ? view->model() : nullptr);
    };
    connect(tabs_, &QTabWidget::currentChanged, search_dialog_, watch_tab);
    watch_tab();
    connect(search_dialog_, &SearchDialog::rowsRequested, this,
            [this](const QString& name, std::vector<LocalTrackRow> rows,
                   const LocalLibraryAction action, const EngineKey& engine) {
                workspace_.placeFoundRows(name, std::move(rows), action, engine);
            });
    search_dialog_->followLibrary(from);
    search_dialog_->show();
    search_dialog_->focusInput();
}

BenchMainWindow::ListTab* BenchMainWindow::currentList() { return currentListTab(); }

void BenchMainWindow::showList(ListTab& tab) {
    if (tab.view != nullptr) {
        tabs_->setCurrentWidget(tab.view);
    }
}

std::vector<BenchMainWindow::ListTab*> BenchMainWindow::listsInOrder() {
    std::vector<ListTab*> lists;
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
        if (view == nullptr) {
            continue;
        }
        if (auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>())) {
            lists.push_back(tab);
        }
    }
    return lists;
}

QObject* BenchMainWindow::modelParent() { return tabs_; }

BenchMainWindow::ListTab* BenchMainWindow::currentListTab() {
    auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
    if (view == nullptr) {
        return nullptr;
    }
    return static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
}

void BenchMainWindow::refreshActiveContext() {
    if (source_stack_ != nullptr) {
        const auto index = local_source_tabs_ != nullptr ? local_source_tabs_->currentIndex() : 0;
        // A sidebar tab of another engine's library carries that engine's key.
        const auto shown = local_source_tabs_ != nullptr
                               ? local_source_tabs_->tabData(index).toString()
                               : QString{};
        auto* other_library = shown.isEmpty() ? nullptr : libraryOf(EngineKey::fromText(shown));
        auto* source = other_library != nullptr ? static_cast<QWidget*>(other_library)
                       : index == 1 && localLibrary() != nullptr
                           ? static_cast<QWidget*>(localLibrary())
                           : static_cast<QWidget*>(folder_view_);
        source_stack_->setCurrentWidget(source);
    }
    if (folder_bookmarks_ != nullptr) {
        const auto folders_visible =
            local_source_tabs_ == nullptr || local_source_tabs_->currentIndex() == 0;
        folder_bookmarks_->setVisible(folders_visible && folder_bookmarks_->count() > 0);
        folder_bookmarks_heading_->setVisible(folders_visible && folder_bookmarks_->count() > 0);
    }
    if (device_menu_ != nullptr && device_menu_->isEmpty()) {
        rebuildDeviceMenu();
    }
    refreshLocalPlaybackControls();
    if (seek_ != nullptr) {
        refreshTransport();
    }
}

bool BenchMainWindow::transferRows(QTableView* source, const QVariantList& rows,
                                   const QString& target_id, const bool move,
                                   const int insertion_row) {
    if (source == nullptr) {
        return false;
    }
    auto* source_tab = static_cast<ListTab*>(source->property("bench-tab-pointer").value<void*>());
    auto* source_model = qobject_cast<LocalListModel*>(source->model());
    std::vector<int> row_indexes;
    row_indexes.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
        row_indexes.push_back(row.toInt());
    }
    return workspace_.transferRows(source_tab, source_model, engineOfView(source),
                                   source->property("definition-owned").toBool(),
                                   std::move(row_indexes), target_id, move, insertion_row);
}

bool BenchMainWindow::transferRowsToNewTab(QTableView* source, const QVariantList& rows,
                                           const bool move, const QString& name) {
    auto* source_tab = source == nullptr
                           ? nullptr
                           : tabForDocument(source->property("bench-document-id").toString());
    auto* source_model = source ? qobject_cast<LocalListModel*>(source->model()) : nullptr;
    const bool dynamic = source && source->property("definition-owned").toBool();
    if (!dynamic && source_tab != nullptr && source_tab->view != source) {
        return false;
    }
    std::vector<int> row_indexes;
    for (const auto& row : rows) {
        bool valid = false;
        row_indexes.push_back(row.toInt(&valid));
        if (!valid) {
            return false;
        }
    }
    auto* destination = workspace_.transferRowsToNewList(
        dynamic ? nullptr : source_tab, source_model, source ? engineOfView(source) : EngineKey{},
        dynamic, std::move(row_indexes), move, name);
    if (destination != nullptr) {
        tabs_->setCurrentWidget(destination->view);
    }
    return destination != nullptr;
}

bool BenchMainWindow::localLibraryShown() const { return bench::localLibraryShown(); }

void BenchMainWindow::applyLocalLibraryVisibility() {
    if (local_source_tabs_ == nullptr) {
        return;
    }
    constexpr int library_tab = 1;
    const bool shown = localLibraryShown();
    local_source_tabs_->setTabVisible(library_tab, shown);
    if (!shown && local_source_tabs_->currentIndex() == library_tab) {
        selectPreferredSource();
    }
}

void BenchMainWindow::selectPreferredSource() {
    if (local_source_tabs_ == nullptr) {
        return;
    }
    // A library unless Folders was chosen: this computer's, or the remote's
    // when this computer's is hidden or the remote's was chosen.
    const auto wanted = preferredSource();
    // A library tab of another engine carries that engine's key.
    int remote = -1;
    for (int index = 0; index < local_source_tabs_->count(); ++index) {
        if (!local_source_tabs_->tabData(index).toString().isEmpty()) {
            remote = index;
        }
    }
    const bool local = localLibraryShown();
    int target = 0;
    if (wanted == QStringLiteral("folders")) {
        target = 0;
    } else if (wanted == QStringLiteral("remote") || !local) {
        target = remote >= 0 ? remote : local ? 1 : 0;
    } else {
        target = 1;
    }
    local_source_tabs_->setCurrentIndex(target);
}

void BenchMainWindow::focusLibrarySearch() {
    if (source_stack_ == nullptr || local_source_tabs_ == nullptr) {
        return;
    }
    if (qobject_cast<LocalLibraryPanel*>(source_stack_->currentWidget()) == nullptr) {
        // On Folders, which has no search: the library it would switch to.
        const auto folders = local_source_tabs_->currentIndex();
        selectPreferredSource();
        if (local_source_tabs_->currentIndex() == folders && local_source_tabs_->count() > 1) {
            local_source_tabs_->setCurrentIndex(1);
        }
    }
    if (auto* panel = qobject_cast<LocalLibraryPanel*>(source_stack_->currentWidget())) {
        panel->focusSearch();
    }
}

void BenchMainWindow::quitAndStopEngine() {
    // Closing the window leaves the music playing; quitting stops it. After
    // the close, so a close that was cancelled leaves it running.
    if (!close()) {
        return;
    }
    // Nothing may start it again. The event loop runs on after this until
    // the process ends, and an engine connection's reconnect timer revives
    // an engine that stopped: it did, and quitting left it running.
    workspace_.retireEngines();
    // Rather than waiting for the last window to be seen closing: a window
    // still open elsewhere -- a tag editor, say -- kept the process, and
    // with it anything that might start an engine.
    QCoreApplication::quit();
}

void BenchMainWindow::refreshTabChrome(ListTab& tab) {
    const auto index = tabs_->indexOf(tab.view);
    if (index < 0) {
        return;
    }
    refreshListsPanel();
    const auto chrome = workspace_.tabChrome(tab);
    tab.view->setProperty("bench-playback-active", chrome.playing);
    tabs_->tabBar()->setTabTextColor(index, QColor{});
    tabs_->tabBar()->setTabData(index, chrome.playing);
    tabs_->setTabText(index, chrome.text);
    tabs_->setTabIcon(index, chrome.remote ? QIcon::fromTheme(QStringLiteral("network-server"))
                                           : QIcon{});
    tabs_->setTabToolTip(index, chrome.tooltip);
    tab.view->setAccessibleName(chrome.accessible_name);
    if (auto* close = tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
        close->setVisible(!tab.document.pinned);
    }
}

void BenchMainWindow::refreshTabActions() {
    refreshListHistoryActions();
    const auto* tab = currentListTab();
    const bool available = tab != nullptr;
    if (export_playlist_action_)
        export_playlist_action_->setEnabled(available);
    if (list_edit_bar_)
        list_edit_bar_->setView(available ? tab->view : nullptr);
    if (list_find_bar_ != nullptr) {
        auto* find_view = available ? tab->view : nullptr;
        list_find_bar_->setView(find_view);
        find_list_action_->setEnabled(find_view != nullptr);
        find_next_action_->setEnabled(find_view != nullptr);
        find_previous_action_->setEnabled(find_view != nullptr);
    }
    for (auto* action :
         {duplicate_tab_action_, pin_tab_action_, save_tab_action_, rename_tab_action_}) {
        if (action != nullptr) {
            action->setEnabled(available);
        }
    }
    if (pin_tab_action_ != nullptr) {
        const QSignalBlocker blocker{pin_tab_action_};
        pin_tab_action_->setChecked(available && tab->document.pinned);
    }
    if (close_tab_action_ != nullptr) {
        const auto properties_tab =
            qobject_cast<MetadataPropertiesDialog*>(tabs_->currentWidget()) != nullptr;
        close_tab_action_->setEnabled(properties_tab || (available && !tab->document.pinned));
    }
}

// Closing returns you to the tab you were on before this one, not to
// whichever tab happens to sit next to it — the neighbour is rarely where
// you came from.
void BenchMainWindow::rememberTabVisit(QWidget* tab) {
    if (tab == nullptr) {
        return;
    }
    tab_visit_history_.removeIf(
        [tab](const QPointer<QWidget>& seen) { return seen == nullptr || seen == tab; });
    tab_visit_history_.push_front(tab);
    constexpr qsizetype remembered_tabs = 32;
    while (tab_visit_history_.size() > remembered_tabs) {
        tab_visit_history_.pop_back();
    }
}

// The tab to fall back to when `closed` goes away: the most recent visit
// that is not it. Removing a tab makes Qt select a neighbour, which pushes
// that neighbour onto the history — so the answer is computed first, before
// the close runs.
QPointer<QWidget> BenchMainWindow::previouslyVisitedTab(QWidget* closed) const {
    for (const auto& candidate : tab_visit_history_) {
        if (candidate != nullptr && candidate != closed && tabs_->indexOf(candidate) >= 0) {
            return candidate;
        }
    }
    return {};
}

void BenchMainWindow::closeTabAt(const int index) {
    // The tab being closed may not be the current one, and the close path
    // runs asynchronously for some kinds, so the restore happens after.
    auto* closing = tabs_->widget(index);
    const QPointer<QWidget> guard{closing};
    if (closing == tabs_->currentWidget()) {
        const auto restore = previouslyVisitedTab(closing);
        // Some close paths ask for confirmation or finish asynchronously, so
        // the restore only applies once the tab is actually gone.
        QTimer::singleShot(0, this, [this, guard, restore] {
            const auto closed = guard == nullptr || tabs_->indexOf(guard) < 0;
            if (closed && restore != nullptr && tabs_->indexOf(restore) >= 0) {
                tabs_->setCurrentWidget(restore);
            }
        });
    }
    if (auto* properties = qobject_cast<MetadataPropertiesDialog*>(tabs_->widget(index))) {
        properties->close();
        return;
    }
    auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
    if (view == nullptr) {
        return;
    }
    auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    if (tab == nullptr || tab->document.pinned) {
        statusBar()->showMessage(QStringLiteral("Unpin this list before closing it"), 3'000);
        return;
    }
    if (tab->document.dirty) {
        QMessageBox confirmation{
            QMessageBox::Question,
            QStringLiteral("Close unsaved list"),
            QStringLiteral("Discard the unsaved contents of “%1”?")
                .arg(displayText(tab->document.name)),
            QMessageBox::Yes | QMessageBox::No,
            this,
        };
        confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
        confirmation.setDefaultButton(QMessageBox::No);
        if (confirmation.exec() != QMessageBox::Yes) {
            return;
        }
    }
    tabs_->removeTab(index);
    view->deleteLater();
    workspace_.closeList(*tab);
    refreshTabActions();
}

void BenchMainWindow::closeCurrentTab() { closeTabAt(tabs_->currentIndex()); }

void BenchMainWindow::createList() {
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("New list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, QString{}, &accepted);
    if (accepted) {
        workspace_.createList(name);
    }
}

void BenchMainWindow::duplicateCurrentTab() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    if (auto* duplicated = workspace_.duplicateList(*tab); duplicated != nullptr) {
        applyTrackViewLayout(*duplicated, duplicated->view_layout);
    }
}

void BenchMainWindow::toggleCurrentTabPinned() {
    if (auto* tab = currentListTab(); tab != nullptr) {
        workspace_.togglePinned(*tab);
        refreshTabActions();
    }
}

void BenchMainWindow::saveCurrentList() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    QString name;
    if (tab->document.kind == persistence::ListKind::scratch) {
        bool accepted = false;
        name = QInputDialog::getText(this, QStringLiteral("Save working list"),
                                     QStringLiteral("Name:"), QLineEdit::Normal,
                                     displayText(tab->document.name), &accepted);
        if (!accepted) {
            return;
        }
    }
    workspace_.saveList(*tab, name);
}

void BenchMainWindow::renameCurrentList() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("Rename list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, displayText(tab->document.name), &accepted);
    if (accepted) {
        workspace_.renameList(*tab, name);
    }
}

void BenchMainWindow::showTabContextMenu(const QPoint& position) {
    const auto index = tabs_->tabBar()->tabAt(position);
    if (index < 0) {
        return;
    }
    tabs_->setCurrentIndex(index);
    refreshTabActions();
    fillContinueMenu();
    tab_context_menu_->popup(tabs_->tabBar()->mapToGlobal(position));
}

void BenchMainWindow::fillContinueMenu() {
    if (continue_menu_ == nullptr) {
        return;
    }
    continue_menu_->clear();
    const auto* tab = currentListTab();
    continue_menu_->setEnabled(tab != nullptr);
    if (tab == nullptr) {
        return;
    }
    const auto id = QString::fromStdString(tab->document.id.to_string());
    const auto current = workspace_.continuationOf(*tab);
    auto* group = new QActionGroup(continue_menu_);
    const auto choose = [this, id](const QString& rule_id) {
        if (auto* chosen = workspace_.tabForDocument(id); chosen != nullptr) {
            workspace_.setContinuation(*chosen, rule_id);
        }
    };
    auto* nothing = continue_menu_->addAction(tr("Nothing — the list ends"));
    nothing->setObjectName(QStringLiteral("continue-nothing"));
    nothing->setCheckable(true);
    nothing->setChecked(!current);
    group->addAction(nothing);
    connect(nothing, &QAction::triggered, this, [choose] { choose({}); });
    const auto choices = workspace_.continuationChoices();
    if (!choices.empty()) {
        continue_menu_->addSeparator();
    }
    bool listed = !current;
    for (const auto& choice : choices) {
        auto* action = continue_menu_->addAction(
            choice.name.isEmpty() ? tr("Unnamed dynamic playlist") : choice.name);
        action->setObjectName(QStringLiteral("continue-rule-") + choice.rule_id);
        action->setCheckable(true);
        action->setChecked(current && current->rule_id == choice.rule_id);
        listed = listed || action->isChecked();
        group->addAction(action);
        connect(action, &QAction::triggered, this,
                [choose, rule = choice.rule_id] { choose(rule); });
    }
    // Chosen elsewhere, from a rule this window does not have: shown, kept.
    if (!listed) {
        auto* other = continue_menu_->addAction(current->name);
        other->setCheckable(true);
        other->setChecked(true);
        other->setEnabled(false);
        group->addAction(other);
    }
    if (choices.empty()) {
        auto* none = continue_menu_->addAction(tr("No rule-based dynamic playlists yet"));
        none->setEnabled(false);
    }
}

void BenchMainWindow::showTrackContextMenu(QTableView* view, const QPoint& position) {
    if (view == nullptr || view->selectionModel() == nullptr || track_context_menu_ == nullptr) {
        return;
    }
    const auto target = view->indexAt(position);
    if (!target.isValid()) {
        return;
    }
    tabs_->setCurrentWidget(view);

    const auto* grouped_delegate = qobject_cast<const ui::QueueItemDelegate*>(view->itemDelegate());
    const auto relative_y = position.y() - view->visualRect(target).top();
    if (grouped_delegate != nullptr && grouped_delegate->isAlbumHeaderHit(target, relative_y)) {
        const auto [first, last] = grouped_delegate->albumRowRange(target);
        const QItemSelection album{view->model()->index(first, 0),
                                   view->model()->index(last, view->model()->columnCount() - 1)};
        view->selectionModel()->select(album, QItemSelectionModel::ClearAndSelect |
                                                  QItemSelectionModel::Rows);
        view->selectionModel()->setCurrentIndex(view->model()->index(first, local_title_column),
                                                QItemSelectionModel::NoUpdate);
    } else {
        if (!view->selectionModel()->isRowSelected(target.row(), target.parent())) {
            view->selectionModel()->select(target, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
        }
        view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
    }
    refreshSelectionStatus();

    const auto has_selection = !view->selectionModel()->selectedRows().isEmpty();
    track_context_menu_->clear();
    play_selected_action_->setEnabled(view->currentIndex().isValid());
    remove_selected_action_->setEnabled(has_selection);
    track_context_menu_->addAction(play_selected_action_);
    addUpNextActions(track_context_menu_, view);

    track_context_menu_->addSeparator();
    auto* tools = track_context_menu_->addMenu(tr("Tools"));
    tools->addAction(properties_action_);
    tools->addAction(replaygain_action_);
    tools->addAction(convert_action_);

    auto* source_tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    if (source_tab != nullptr && target.row() < source_tab->model->rowCount()) {
        const auto path =
            source_tab->model->rows()[static_cast<std::size_t>(target.row())].raw_path;
        track_context_menu_->addSeparator();
        // Found in the library of the engine the tab plays on: a remote
        // tab's file is in the remote's library, which also shows it.
        const auto engine = EngineKey::of(source_tab->document);
        auto* library = libraryOf(engine);
        const bool available = library != nullptr && (!engine.isLocal() || localLibraryShown());
        for (const bool album : {false, true}) {
            auto* locate = track_context_menu_->addAction(album ? QStringLiteral("Locate album")
                                                                : QStringLiteral("Locate artist"));
            locate->setObjectName(album ? QStringLiteral("action-local-locate-album")
                                        : QStringLiteral("action-local-locate-artist"));
            locate->setEnabled(available);
            connect(locate, &QAction::triggered, this, [this, path, album, engine] {
                auto* finder = libraryOf(engine);
                if (finder == nullptr)
                    return;
                // The sidebar's tab for that engine's library.
                int source = 1;
                for (int index = 0; !engine.isLocal() && index < local_source_tabs_->count();
                     ++index) {
                    if (local_source_tabs_->tabData(index).toString() == engine.text()) {
                        source = index;
                    }
                }
                local_source_tabs_->setCurrentIndex(source);
                refreshActiveContext();
                finder->locatePath(path, album);
            });
        }
    }
    addLocalRateMenus(view, source_tab);
    track_context_menu_->addSeparator();
    if (source_tab != nullptr) {
        auto* copy_menu = track_context_menu_->addMenu(QStringLiteral("Copy to list"));
        copy_menu->setObjectName(QStringLiteral("bench-track-copy-menu"));
        auto* move_menu = track_context_menu_->addMenu(QStringLiteral("Move to list"));
        move_menu->setObjectName(QStringLiteral("bench-track-move-menu"));
        for (auto* menu : {copy_menu, move_menu}) {
            const auto move = menu == move_menu;
            auto* create = menu->addAction(tr("New tab…"));
            create->setObjectName(move ? QStringLiteral("action-move-to-new-tab")
                                       : QStringLiteral("action-copy-to-new-tab"));
            connect(create, &QAction::triggered, this, [this, view, move] {
                std::vector<QPersistentModelIndex> selected_rows;
                auto selected = view->selectionModel()->selectedRows(0);
                std::ranges::sort(selected, {}, &QModelIndex::row);
                for (const auto& index : selected)
                    selected_rows.emplace_back(index);
                bool accepted = false;
                const auto name =
                    QInputDialog::getText(this, tr("New tab"), tr("Name:"), QLineEdit::Normal,
                                          tr("Selection"), &accepted)
                        .trimmed();
                if (!accepted || name.isEmpty())
                    return;
                QVariantList rows;
                for (const auto& index : selected_rows) {
                    if (!index.isValid())
                        return;
                    rows.push_back(index.row());
                }
                transferRowsToNewTab(view, rows, move, name);
            });
            if (list_tabs_.size() > 1U)
                menu->addSeparator();
        }
        for (const auto& destination : list_tabs_) {
            if (destination.get() == source_tab) {
                continue;
            }
            const auto target_id = QString::fromStdString(destination->document.id.to_string());
            const auto label = displayText(destination->document.name);
            auto* copy = copy_menu->addAction(label);
            connect(copy, &QAction::triggered, this,
                    [this, view, target_id] { transferSelectedRows(view, target_id, false); });
            auto* move = move_menu->addAction(label);
            connect(move, &QAction::triggered, this,
                    [this, view, target_id] { transferSelectedRows(view, target_id, true); });
        }
    }
    track_context_menu_->addAction(remove_selected_action_);
    refreshListHistoryActions();
    track_context_menu_->addSeparator();
    track_context_menu_->addMenu(sort_list_menu_);
    track_context_menu_->addAction(reverse_list_action_);
    track_context_menu_->addAction(shuffle_albums_action_);
    track_context_menu_->addAction(deduplicate_list_action_);
    track_context_menu_->addSeparator();
    track_context_menu_->addAction(undo_list_action_);
    track_context_menu_->addAction(redo_list_action_);
    track_context_menu_->addSeparator();
    addLastFmActions(track_context_menu_, view);
    track_context_menu_->popup(view->viewport()->mapToGlobal(position));
}

// Dynamic results from an engine's library show a rating it stored, and
// rules on ratings look again.
void BenchMainWindow::engineRatingsChanged(const EngineKey& engine,
                                           const QHash<QString, unsigned>& ratings) {
    if (auto* dialog = findChild<DynamicPlaylistDialog*>(); dialog && dialog->engine() == engine) {
        if (auto* results = qobject_cast<LocalListModel*>(dialog->view()->model())) {
            results->applyRatings(ratings);
        }
        dialog->libraryChanged();
    }
}


void BenchMainWindow::addLocalRateMenus(QTableView* view, ListTab* source_tab) {
    if (source_tab)
        addLocalRateMenus(track_context_menu_, view);
}

void BenchMainWindow::addLocalRateMenus(QMenu* menu, QTableView* view) {
    auto* model = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (!menu || !model || !view->selectionModel()) {
        return;
    }
    const auto& rows = model->rows();
    QStringList track_hashes;
    QStringList album_hashes;
    QSet<QString> unique_tracks;
    QSet<QString> unique_albums;
    std::optional<unsigned> common_rating;
    bool ratings_match = true;
    std::optional<unsigned> common_album_rating;
    bool album_ratings_match = true;
    for (const auto& index : view->selectionModel()->selectedRows()) {
        if (index.row() < 0 || index.row() >= static_cast<int>(rows.size())) {
            continue;
        }
        const auto& row = rows[static_cast<std::size_t>(index.row())];
        if (!common_rating) {
            common_rating = row.rating;
        } else if (*common_rating != row.rating) {
            ratings_match = false;
        }
        if (!common_album_rating) {
            common_album_rating = row.album_rating;
        } else if (*common_album_rating != row.album_rating) {
            album_ratings_match = false;
        }
        const auto track_hash = QString::fromStdString(row.rating_hash);
        if (!track_hash.isEmpty() && !unique_tracks.contains(track_hash)) {
            unique_tracks.insert(track_hash);
            track_hashes.push_back(track_hash);
        }
        const auto album_hash = QString::fromStdString(row.album_rating_hash);
        if (!album_hash.isEmpty() && !unique_albums.contains(album_hash)) {
            unique_albums.insert(album_hash);
            album_hashes.push_back(album_hash);
        }
    }
    if (track_hashes.isEmpty()) {
        return;
    }
    // A rating belongs to the engine whose library holds the track: a
    // remote tab's ratings are stored there, where its queries see them.
    const auto engine = engineOfView(view);
    const auto store_ready = workspace_.canRate(engine);
    menu->addSeparator();
    auto* rate_menu = menu->addMenu(QStringLiteral("Rate"));
    rate_menu->setObjectName(QStringLiteral("bench-local-rate-menu"));
    rate_menu->setEnabled(store_ready);
    auto* album_rate_menu = menu->addMenu(QStringLiteral("Rate album"));
    album_rate_menu->setObjectName(QStringLiteral("bench-local-album-rate-menu"));
    album_rate_menu->setEnabled(store_ready && !album_hashes.isEmpty());
    const auto make_rating_action = [](QMenu* target_menu, const unsigned rating) -> QAction* {
        if (rating == 0U) {
            auto* unrate = target_menu->addAction(ui::ratingMenuLabel(rating));
            unrate->setCheckable(true);
            return unrate;
        }
        auto* stars = new ui::RatingMenuAction(rating, target_menu);
        target_menu->addAction(stars);
        return stars;
    };
    for (unsigned rating = 0U; rating <= 10U; rating += 2U) {
        auto* rate = make_rating_action(rate_menu, rating);
        rate->setObjectName(QStringLiteral("action-local-rate-%1").arg(rating));
        rate->setChecked(ratings_match && common_rating == rating);
        connect(rate, &QAction::triggered, this, [this, engine, track_hashes, rating] {
            workspace_.rate(engine, track_hashes, false, rating);
        });
        auto* album_rate = make_rating_action(album_rate_menu, rating);
        album_rate->setObjectName(QStringLiteral("action-local-album-rate-%1").arg(rating));
        album_rate->setChecked(album_ratings_match && common_album_rating == rating);
        connect(album_rate, &QAction::triggered, this, [this, engine, album_hashes, rating] {
            workspace_.rate(engine, album_hashes, true, rating);
        });
    }
}

void BenchMainWindow::showFolderContextMenu(const QPoint& position) {
    if (folder_context_menu_ == nullptr) {
        return;
    }
    const auto target = folder_view_->indexAt(position);
    if (!target.isValid()) {
        return;
    }
    folder_view_->selectionModel()->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
    const auto directory = folder_model_->isDirectory(target);
    folder_add_to_list_action_->setText(directory ? QStringLiteral("Add folder to current list")
                                                  : QStringLiteral("Add file to current list"));
    folder_toggle_expanded_action_->setText(
        folder_view_->isExpanded(target) ? QStringLiteral("Collapse") : QStringLiteral("Expand"));
    folder_toggle_expanded_action_->setEnabled(directory);
    folder_context_menu_->clear();
    folder_context_menu_->addAction(folder_add_to_list_action_);
    if (directory) {
        folder_context_menu_->addAction(folder_toggle_expanded_action_);
        folder_context_menu_->addAction(folder_bookmark_add_action_);
    }
    folder_context_menu_->popup(folder_view_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::playCurrentRow() {
    auto* tab = currentListTab();
    if (tab != nullptr && tab->view->currentIndex().isValid()) {
        playRow(*tab, tab->view->currentIndex().row());
    }
}

void BenchMainWindow::refreshListHistoryActions() {
    if (undo_list_action_ == nullptr || redo_list_action_ == nullptr)
        return;
    const auto* tab = currentListTab();
    const auto* model = tab == nullptr ? nullptr : tab->model;
    if (shuffle_albums_action_) {
        shuffle_albums_action_->setEnabled(model != nullptr && model->rowCount() > 1);
    }
    if (sort_list_menu_) {
        const auto editable = model != nullptr && model->rowCount() > 1;
        sort_list_menu_->setEnabled(editable);
        reverse_list_action_->setEnabled(editable);
        deduplicate_list_action_->setEnabled(editable);
    }
    const auto texts = workspace_.historyTexts(tab);
    undo_list_action_->setEnabled(texts.can_undo);
    redo_list_action_->setEnabled(texts.can_redo);
    undo_list_action_->setText(texts.undo);
    redo_list_action_->setText(texts.redo);
}

void BenchMainWindow::replayListEdit(const bool undo) {
    if (workspace_.replayListEdit(currentListTab(), undo)) {
        refreshSelectionStatus();
    }
    refreshListHistoryActions();
}

void BenchMainWindow::removeSelectedRows() {
    // What is chosen where the keyboard is: Up Next's tracks while its list
    // has it, else the list's rows.
    if (up_next_view_ != nullptr && up_next_view_->hasFocus()) {
        editUpNextSelection(1);
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr || tab->view->selectionModel() == nullptr) {
        return;
    }
    std::vector<int> rows;
    const auto selection = tab->view->selectionModel()->selectedRows();
    rows.reserve(static_cast<std::size_t>(selection.size()));
    for (const auto& index : selection) {
        rows.push_back(index.row());
    }
    workspace_.removeRows(*tab, std::move(rows));
}

void BenchMainWindow::transferSelectedRows(QTableView* source, const QString& target_id,
                                           const bool move) {
    if (source == nullptr || source->selectionModel() == nullptr) {
        return;
    }
    auto selected = source->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    QVariantList rows;
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }
    if (!rows.isEmpty()) {
        static_cast<void>(transferRows(source, rows, target_id, move, -1));
    }
}

} // namespace trackknife::bench
