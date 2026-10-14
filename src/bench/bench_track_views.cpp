// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/list_view_apply.hpp"
#include "bench/list_view_dialog.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"
#include "uicommon/track_view_layout.hpp"
#include "workspace/list_view_definitions.hpp"

#include <QAction>
#include <QActionGroup>
#include <QFuture>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QStatusBar>
#include <QTableView>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace trackknife::bench {

void BenchMainWindow::applyTrackViewLayout(ListTab& tab, const ui::TrackViewLayout& layout) {
    applyTrackViewLayout(tab.view, tab.view_layout, layout);
}

ui::TrackViewLayout drawListView(QTableView* view, const lists::ListView& definition,
                                 const std::shared_ptr<const lists::CompiledListView>& compiled,
                                 const ui::TrackViewLayout& layout) {
    auto* queue_view = static_cast<ui::QueueTableView*>(view);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    auto applied = Workspace::trackViewLayoutFor(definition, layout.columns);
    if (model != nullptr && model->listView() != compiled.get()) {
        model->setListView(compiled);
    }
    const auto logical_of = [model](const QString& id) {
        return model != nullptr ? model->columnOf(id) : trackColumnLogical(id);
    };
    view->setProperty(ui::track_artwork_column_property, local_artwork_column);
    view->setProperty(ui::track_artist_column_property, local_artist_column);
    view->setProperty(ui::track_number_column_property, local_track_number_column);
    view->setProperty(ui::track_title_column_property, local_title_column);
    view->setProperty(ui::track_album_column_property, local_album_column);
    view->setProperty(ui::track_date_column_property, local_date_column);
    view->setProperty(ui::track_length_column_property, local_length_column);
    view->setProperty(ui::track_separate_number_property, true);
    // Grouped by an expression: drawn grouped once the groups are known.
    const auto by_expression = definition.grouping == lists::ViewGrouping::expression;
    const auto grouped = definition.grouping == lists::ViewGrouping::album ||
                         (by_expression && model != nullptr && model->expressionGroupsReady());
    // The cover column is the view's artwork, not a column of its own to
    // show or hide: covers beside a group's rows, in its header -- or, in a
    // list not grouped, each row's own.
    const auto artwork_shown = definition.artwork != lists::ViewArtwork::none;
    const auto side_artwork = grouped && definition.artwork == lists::ViewArtwork::beside;
    const auto row_covers = !grouped && artwork_shown;
    view->setProperty(ui::track_row_covers_property, row_covers);
    view->setProperty(ui::track_side_artwork_property, side_artwork);
    view->setProperty(ui::track_album_rules_property, !by_expression);
    // Albums and their headers already group the rows; stripes on top of
    // that are noise.
    view->setAlternatingRowColors(!grouped);

    // ADR-0284: rows of as many lines as the view says.
    const auto lines = std::clamp(definition.row_lines, 1, 3);
    view->setProperty(ui::track_row_lines_property, lines);
    auto* previous_delegate = view->itemDelegate();
    view->setItemDelegate(
        grouped ? static_cast<QAbstractItemDelegate*>(new ui::QueueItemDelegate(view))
                : static_cast<QAbstractItemDelegate*>(new ui::TrackLinesDelegate(view)));
    if (previous_delegate != nullptr && previous_delegate->parent() == view) {
        previous_delegate->deleteLater();
    }
    view->verticalHeader()->setDefaultSectionSize(ui::track_row_height +
                                                  (lines - 1) * ui::track_second_line_height);
    view->verticalHeader()->setMinimumSectionSize(18);
    queue_view->setAlbumGroupingEnabled(grouped);
    queue_view->refreshRowGeometry();

    auto* header = view->horizontalHeader();
    const QSignalBlocker header_blocker{header};
    for (int visual = 0; visual < static_cast<int>(applied.columns.size()); ++visual) {
        const auto logical = logical_of(applied.columns[static_cast<std::size_t>(visual)].id);
        if (logical < 0) {
            continue;
        }
        const auto current_visual = header->visualIndex(logical);
        if (current_visual != visual) {
            header->moveSection(current_visual, visual);
        }
    }
    for (const auto& column : applied.columns) {
        const auto logical = logical_of(column.id);
        if (logical < 0) {
            continue;
        }
        const bool history_column =
            logical == local_play_count_column || logical == local_last_played_column;
        const auto shown = logical == local_artwork_column ? artwork_shown : column.visible;
        view->setColumnHidden(logical, !shown || (history_column && model == nullptr));
        view->setColumnWidth(logical, column.width);
    }
    queue_view->setAlbumArtworkColumn(side_artwork ? local_artwork_column : -1);
    QHash<int, int> preferred_widths;
    QHash<int, int> minimum_widths;
    QList<int> expanding{local_artist_column, local_title_column, local_album_column};
    for (const auto& column : applied.columns) {
        const auto logical = logical_of(column.id);
        if (logical < 0) {
            continue;
        }
        const auto spec = std::ranges::find(track_column_specs, logical, &TrackColumnSpec::logical);
        if (spec == track_column_specs.end()) {
            // An expression column: as wide as it was made, and in on the
            // spare room when it says so.
            preferred_widths.insert(logical, column.width);
            minimum_widths.insert(logical, 48);
            const auto found = std::ranges::find(definition.columns, column.id.toStdString(),
                                                 &lists::ViewColumn::id);
            if (found != definition.columns.end() && found->expands) {
                expanding.push_back(logical);
            }
            continue;
        }
        // Covers beside the rows: the cover gutter and the numbers fixed
        // (ADR-0250).
        // A row's own cover: as wide as the row is tall.
        const auto row_cover =
            ui::track_row_height + (lines - 1) * ui::track_second_line_height + 6;
        const auto fixed = row_covers && logical == local_artwork_column ? row_cover
                           : !side_artwork                               ? 0
                           : logical == local_artwork_column             ? ui::side_cover_gutter
                           : logical == local_track_number_column        ? ui::side_number_width
                                                                         : 0;
        preferred_widths.insert(logical, fixed > 0 ? fixed : column.width);
        minimum_widths.insert(logical, fixed > 0 ? fixed : spec->minimum_width);
    }
    queue_view->setAutoFillColumns(std::move(expanding), std::move(preferred_widths),
                                   std::move(minimum_widths));
    // ADR-0283: the groups it had folded.
    queue_view->setCollapsedGroups({layout.collapsed.begin(), layout.collapsed.end()});
    applied.collapsed = layout.collapsed;
    view->viewport()->update();
    return applied;
}

void BenchMainWindow::applyTrackViewLayout(QTableView* view, ui::TrackViewLayout& state,
                                           const ui::TrackViewLayout& layout) {
    if (view == nullptr) {
        return;
    }
    // ADR-0282: the list view the tab is drawn by; one that is gone, the
    // default.
    auto view_id = layout.view.isEmpty() ? default_list_view_id : layout.view;
    auto definition = findListView(view_id);
    auto compiled = definition ? compiledListView(view_id) : nullptr;
    if (!definition || !compiled) {
        view_id = default_list_view_id;
        definition = findListView(view_id);
        compiled = compiledListView(view_id);
    }
    applying_track_view_layout_ = true;
    state = drawListView(view, *definition, compiled, layout);
    applying_track_view_layout_ = false;
    refreshTrackViewActions();
}

ui::TrackViewLayout BenchMainWindow::captureTrackViewLayout(const ListTab& tab) const {
    return captureTrackViewLayout(tab.view, tab.view_layout);
}

ui::TrackViewLayout
BenchMainWindow::captureTrackViewLayout(const QTableView* view,
                                        const ui::TrackViewLayout& state) const {
    auto layout = state;
    layout.schema_version = ui::track_view_layout_schema_version;
    layout.columns.clear();
    if (const auto* queue_view = dynamic_cast<const ui::QueueTableView*>(view)) {
        const auto& folded = queue_view->collapsedGroups();
        layout.collapsed = QStringList{folded.begin(), folded.end()};
        layout.collapsed.sort();
    }
    const auto* model = qobject_cast<const LocalListModel*>(view->model());
    auto* header = view->horizontalHeader();
    layout.columns.reserve(static_cast<std::size_t>(header->count()));
    for (int visual = 0; visual < header->count(); ++visual) {
        const auto logical = header->logicalIndex(visual);
        const auto id = model != nullptr ? model->columnId(logical) : trackColumnId(logical);
        if (id.isEmpty()) {
            continue;
        }
        const auto saved = std::ranges::find(state.columns, id, &ui::TrackViewColumnLayout::id);
        // Qt reports zero for hidden sections. Preserve the preferred width so
        // toggling a column on does not collapse it to the minimum width.
        // Beside-the-rows covers fix the gutter and number widths; what the
        // user chose for them stays for the other presentations.
        const bool fixed =
            state.presentation == ui::TrackViewPresentation::albums_side_artwork &&
            (logical == local_artwork_column || logical == local_track_number_column);
        const auto width = (view->isColumnHidden(logical) || fixed) && saved != state.columns.end()
                               ? saved->width
                               : header->sectionSize(logical);
        layout.columns.push_back(ui::TrackViewColumnLayout{
            .id = id,
            .width = std::max(24, width),
            .visible = saved != state.columns.end() && (logical == local_play_count_column ||
                                                        logical == local_last_played_column)
                           ? saved->visible
                           : !view->isColumnHidden(logical),
        });
    }
    return layout;
}

void BenchMainWindow::setTrackViewPresentation(const ui::TrackViewPresentation presentation) {
    chooseListView(ui::trackViewPresentationId(presentation));
}

void BenchMainWindow::chooseListView(const QString& id) {
    auto* tab = currentListTab();
    const auto definition = findListView(id);
    if (tab == nullptr || applying_track_view_layout_ || !definition) {
        return;
    }
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    // The tab's widths, but a view chosen that says how wide a column starts
    // has it so.
    auto widths = tab->view_layout.columns;
    std::erase_if(widths, [&definition](const ui::TrackViewColumnLayout& column) {
        const auto found =
            std::ranges::find(definition->columns, column.id.toStdString(), &lists::ViewColumn::id);
        return found != definition->columns.end() && found->width > 0;
    });
    applyTrackViewLayout(*tab, Workspace::trackViewLayoutFor(*definition, widths));
    schedulePersist();
}

void BenchMainWindow::rearrangeListView(ListTab& tab, const ui::TrackViewLayout& captured) {
    auto definition = findListView(captured.view);
    if (!definition) {
        return;
    }
    // The view's columns as the header has them now.
    std::vector<lists::ViewColumn> arranged;
    arranged.reserve(definition->columns.size());
    for (const auto& column : captured.columns) {
        const auto found =
            std::ranges::find(definition->columns, column.id.toStdString(), &lists::ViewColumn::id);
        if (found != definition->columns.end()) {
            auto placed = *found;
            placed.visible = column.visible;
            arranged.push_back(std::move(placed));
        }
    }
    for (const auto& column : definition->columns) {
        if (!std::ranges::contains(arranged, column.id, &lists::ViewColumn::id)) {
            arranged.push_back(column);
        }
    }
    tab.view_layout_persistence_protected = false;
    tab.preserved_view_layout.clear();
    if (arranged == definition->columns) {
        tab.view_layout = captured;
        schedulePersist();
        return;
    }
    if (definition->builtin) {
        // A shipped view is not changed: this tab gets a copy arranged so.
        auto copy = listViewCopy(*definition, std::move(arranged),
                                 QString::fromStdString(definition->name) +
                                     QStringLiteral(" (customised)"));
        if (!copy) {
            statusBar()->showMessage(QString::fromStdString(copy.error().message), 7'000);
            applyTrackViewLayout(tab, tab.view_layout);
            return;
        }
        auto layout = captured;
        layout.view = *copy;
        applyTrackViewLayout(tab, layout);
        schedulePersist();
        return;
    }
    auto custom = customListViews();
    if (!custom) {
        statusBar()->showMessage(QString::fromStdString(custom.error().message), 7'000);
        applyTrackViewLayout(tab, tab.view_layout);
        return;
    }
    if (const auto found = std::ranges::find(*custom, definition->id, &lists::ListView::id);
        found != custom->end()) {
        found->columns = std::move(arranged);
    }
    tab.view_layout = captured;
    // Every tab of the view is drawn again (ListViewCatalog::changed).
    if (auto saved = saveCustomListViews(*custom); !saved) {
        statusBar()->showMessage(QString::fromStdString(saved.error().message), 7'000);
        applyTrackViewLayout(tab, tab.view_layout);
    }
    schedulePersist();
}

void BenchMainWindow::redrawListViews() {
    for (auto& tab : list_tabs_) {
        applyTrackViewLayout(*tab, tab->view_layout);
    }
}

void BenchMainWindow::setTrackColumnVisible(const QString& column_id, const bool visible) {
    auto* tab = currentListTab();
    if (tab == nullptr || applying_track_view_layout_) {
        return;
    }
    auto layout = captureTrackViewLayout(*tab);
    const auto found = std::ranges::find(layout.columns, column_id, &ui::TrackViewColumnLayout::id);
    // The last column shown stays.
    const auto shown = std::ranges::count_if(layout.columns, [](const auto& column) {
        return column.visible && column.id != QStringLiteral("artwork");
    });
    if (found == layout.columns.end() || found->visible == visible ||
        (!visible && shown == 1 && column_id != QStringLiteral("artwork"))) {
        refreshTrackViewActions();
        return;
    }
    found->visible = visible;
    if (visible &&
        (column_id == QStringLiteral("play-count") || column_id == QStringLiteral("last-played"))) {
        tab->model->invalidateListeningHistory();
    }
    rearrangeListView(*tab, layout);
}

void BenchMainWindow::sortListByColumn(QTableView* view, const int logical) {
    ListTab* tab = nullptr;
    for (auto& candidate : list_tabs_) {
        if (candidate->view == view) {
            tab = candidate.get();
        }
    }
    if (tab == nullptr || tab->model == nullptr || logical == local_artwork_column) {
        return;
    }
    const auto id = tab->model->columnId(logical);
    auto compiled = compiledListView(tab->view_layout.view);
    if (id.isEmpty() || !compiled) {
        return;
    }
    // Again, nothing else changed: the other way.
    const auto descending = tab->sorted_by == id &&
                            tab->sorted_revision == tab->model->contentRevision() &&
                            !tab->sorted_descending;
    const auto title = tab->model->headerData(logical, Qt::Horizontal, Qt::DisplayRole).toString();
    const auto label =
        descending ? tr("Sort by %1, descending").arg(title) : tr("Sort by %1").arg(title);
    const auto revision = tab->model->contentRevision();
    const auto document = tab->document.id;
    QtConcurrent::run([compiled, rows = tab->model->viewRows(), column = id.toStdString(),
                       descending]() -> core::Result<std::vector<int>> {
        std::vector<lists::ViewRow> views;
        views.reserve(rows.size());
        for (const auto& row : rows) {
            views.push_back(row.view());
        }
        auto keys = lists::list_sort_keys(*compiled, column, views);
        if (!keys) {
            return std::unexpected(std::move(keys.error()));
        }
        return lists::list_sort_order(*keys, descending);
    })
        .then(this, [this, document, id, descending, revision, label,
                     title](core::Result<std::vector<int>> order) {
            auto* sorted = tabForDocument(document);
            if (sorted == nullptr) {
                return;
            }
            if (!order) {
                statusBar()->showMessage(order.error().code == core::ErrorCode::unsupported
                                             ? tr("%1 cannot sort a list").arg(title)
                                             : QString::fromStdString(order.error().message),
                                         5'000);
                return;
            }
            // Changed while it was sorted: not sorted from what it was.
            if (sorted->model->contentRevision() != revision) {
                statusBar()->showMessage(tr("The list changed while it was sorted; sort it again"),
                                         5'000);
                return;
            }
            sorted->model->applyPermutation(*order, label);
            sorted->sorted_by = id;
            sorted->sorted_descending = descending;
            sorted->sorted_revision = sorted->model->contentRevision();
            markTabDirty(*sorted);
            syncArtwork(*sorted);
            refreshSelectionStatus();
        });
}

void BenchMainWindow::resetTrackViewLayout() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    applyTrackViewLayout(*tab, defaultTrackViewLayout());
    schedulePersist();
}

void BenchMainWindow::copyTrackViewLayoutToAllTabs() {
    auto* source = currentListTab();
    if (source == nullptr) {
        return;
    }
    const auto layout = captureTrackViewLayout(*source);
    for (auto& tab : list_tabs_) {
        tab->view_layout_persistence_protected = false;
        tab->preserved_view_layout.clear();
        applyTrackViewLayout(*tab, layout);
    }
    schedulePersist();
}

void BenchMainWindow::refreshTrackViewActions() {
    if (track_presentation_group_ == nullptr) {
        return;
    }
    auto* tab = currentListTab();
    const auto available = tab != nullptr;
    for (auto* action : {track_albums_side_action_, track_plain_columns_action_,
                         track_layout_reset_action_, track_layout_copy_action_}) {
        action->setEnabled(available);
    }
    for (auto it = track_column_actions_.cbegin(); it != track_column_actions_.cend(); ++it) {
        // The cover column follows the view's artwork, not this menu.
        it.value()->setVisible(it.key() != QStringLiteral("artwork"));
        it.value()->setEnabled(available);
    }
    if (!available) {
        return;
    }
    const auto layout = captureTrackViewLayout(*tab);
    const auto shipped = [&layout](const ui::TrackViewPresentation presentation) {
        return layout.view == ui::trackViewPresentationId(presentation);
    };
    const QSignalBlocker side_blocker{track_albums_side_action_};
    const QSignalBlocker plain_blocker{track_plain_columns_action_};
    // A view of the user's own is none of the shipped four.
    track_presentation_group_->setExclusive(false);
    track_albums_side_action_->setChecked(shipped(ui::TrackViewPresentation::albums_side_artwork));
    track_plain_columns_action_->setChecked(shipped(ui::TrackViewPresentation::plain_columns));
    track_presentation_group_->setExclusive(true);
    for (const auto& column : layout.columns) {
        auto* action = track_column_actions_.value(column.id, nullptr);
        if (action == nullptr) {
            continue;
        }
        const QSignalBlocker blocker{action};
        action->setChecked(column.visible);
    }
}

void BenchMainWindow::showTrackViewHeaderMenu(QTableView* view, const QPoint& position) {
    if (view == nullptr) {
        return;
    }
    tabs_->setCurrentWidget(view);
    auto* tab = currentListTab();
    QMenu menu(this);
    menu.addSection(QStringLiteral("View"));
    auto* views = new QActionGroup(&menu);
    for (const auto& definition : listViews()) {
        const auto id = QString::fromStdString(definition.id);
        auto* action = menu.addAction(QString::fromStdString(definition.name));
        action->setObjectName(QStringLiteral("action-list-view-%1").arg(id));
        action->setCheckable(true);
        action->setChecked(tab != nullptr && tab->view_layout.view == id);
        views->addAction(action);
        connect(action, &QAction::triggered, this, [this, id] { chooseListView(id); });
    }
    menu.addSeparator();
    auto* edit = menu.addAction(QStringLiteral("Edit view…"));
    edit->setObjectName(QStringLiteral("action-edit-list-view"));
    connect(edit, &QAction::triggered, this, [this] { editListView(false); });
    auto* make = menu.addAction(QStringLiteral("New view…"));
    make->setObjectName(QStringLiteral("action-new-list-view"));
    connect(make, &QAction::triggered, this, [this] { editListView(true); });
    auto* columns = menu.addMenu(QStringLiteral("Columns"));
    for (const auto& spec : track_column_specs) {
        if (spec.logical != local_artwork_column) {
            columns->addAction(track_column_actions_.value(QString::fromLatin1(spec.id)));
        }
    }
    // The view's own columns, after the built-in ones.
    if (const auto* model = tab != nullptr ? tab->model : nullptr;
        model != nullptr && model->expressionColumnCount() > 0) {
        columns->addSeparator();
        const auto layout = captureTrackViewLayout(*tab);
        for (int column = local_column_count; column < model->columnCount(); ++column) {
            const auto id = model->columnId(column);
            auto* action = columns->addAction(
                model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
            action->setObjectName(QStringLiteral("action-track-column-%1").arg(id));
            action->setCheckable(true);
            const auto found =
                std::ranges::find(layout.columns, id, &ui::TrackViewColumnLayout::id);
            action->setChecked(found != layout.columns.end() && found->visible);
            connect(action, &QAction::toggled, this,
                    [this, id](const bool shown) { setTrackColumnVisible(id, shown); });
        }
    }
    // ADR-0283: every group folded, or none.
    if (auto* queue_view = static_cast<ui::QueueTableView*>(view);
        queue_view->albumGroupingEnabled()) {
        menu.addSeparator();
        auto* fold = menu.addAction(QStringLiteral("Collapse all groups"));
        fold->setObjectName(QStringLiteral("action-collapse-groups"));
        connect(fold, &QAction::triggered, queue_view,
                [queue_view] { queue_view->setAllGroupsCollapsed(true); });
        auto* unfold = menu.addAction(QStringLiteral("Expand all groups"));
        unfold->setObjectName(QStringLiteral("action-expand-groups"));
        connect(unfold, &QAction::triggered, queue_view,
                [queue_view] { queue_view->setAllGroupsCollapsed(false); });
    }
    menu.addSeparator();
    menu.addAction(track_layout_reset_action_);
    menu.exec(view->horizontalHeader()->mapToGlobal(position));
}

void BenchMainWindow::editListView(const bool make_new) {
    auto* tab = currentListTab();
    auto current = findListView(tab != nullptr ? tab->view_layout.view : default_list_view_id);
    if (!current) {
        current = findListView(default_list_view_id);
    }
    auto edited = *current;
    // A shipped view is not changed: the editor makes a copy of it.
    if (make_new || edited.builtin) {
        edited.builtin = false;
        edited.id = newListViewId().toStdString();
        edited.name = make_new ? "New view" : current->name + " (copy)";
    }
    // A few of the list's rows, and their covers, to preview.
    std::vector<LocalTrackRow> sample;
    QHash<QString, QImage> covers;
    if (tab != nullptr && tab->model != nullptr) {
        const auto& rows = tab->model->rows();
        sample.assign(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(
                                                       std::min<std::size_t>(rows.size(), 40U)));
        covers = tab->model->artworks();
    }
    const auto removable = !make_new && !current->builtin;
    auto* dialog =
        new ListViewDialog(std::move(edited), std::move(sample), covers, removable, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const auto document = tab != nullptr ? std::optional{tab->document.id} : std::nullopt;
    connect(dialog, &ListViewDialog::saved, this, [this, document](const lists::ListView& view) {
        auto custom = customListViews();
        if (!custom) {
            statusBar()->showMessage(QString::fromStdString(custom.error().message), 7'000);
            return;
        }
        const auto found = std::ranges::find(*custom, view.id, &lists::ListView::id);
        if (found != custom->end()) {
            *found = view;
        } else {
            custom->push_back(view);
        }
        if (auto saved = saveCustomListViews(*custom); !saved) {
            statusBar()->showMessage(QString::fromStdString(saved.error().message), 7'000);
            return;
        }
        // The list it was opened from is drawn by it.
        if (auto* edited_tab = document ? tabForDocument(*document) : nullptr) {
            applyTrackViewLayout(
                *edited_tab, Workspace::trackViewLayoutFor(view, edited_tab->view_layout.columns));
            schedulePersist();
        }
    });
    connect(dialog, &ListViewDialog::removed, this, [this](const QString& id) {
        auto custom = customListViews();
        if (!custom) {
            statusBar()->showMessage(QString::fromStdString(custom.error().message), 7'000);
            return;
        }
        std::erase_if(*custom, [&id](const auto& view) { return view.id == id.toStdString(); });
        // Its lists are drawn as albums again.
        for (auto& listed : list_tabs_) {
            if (listed->view_layout.view == id) {
                listed->view_layout.view = default_list_view_id;
            }
        }
        if (auto saved = saveCustomListViews(*custom); !saved) {
            statusBar()->showMessage(QString::fromStdString(saved.error().message), 7'000);
        }
        schedulePersist();
    });
    dialog->open();
}

// The table the tab strip is currently showing, whichever kind of list it
// holds. Selection-driven actions ask this rather than each surface.
QTableView* BenchMainWindow::activeTrackView() {
    if (auto* tab = currentListTab()) {
        return tab->view;
    }
    return nullptr;
}

// Delete and Play are menubar actions, so their enabled state has to follow
// the selection continuously — not only when a context menu happens to open.
void BenchMainWindow::refreshSelectionActions() {
    auto* view = activeTrackView();
    const auto has_selection = view != nullptr && view->selectionModel() != nullptr &&
                               !view->selectionModel()->selectedRows().isEmpty();
    // Delete also takes Up Next's chosen tracks while the keyboard is there.
    const auto up_next_selection = up_next_view_ != nullptr &&
                                   up_next_view_->selectionModel() != nullptr &&
                                   !up_next_view_->selectionModel()->selectedRows().isEmpty();
    if (remove_selected_action_ != nullptr) {
        remove_selected_action_->setEnabled(has_selection || up_next_selection);
    }
    if (play_selected_action_ != nullptr) {
        play_selected_action_->setEnabled(view != nullptr && view->currentIndex().isValid());
    }
}

void BenchMainWindow::refreshSelectionStatus() {
    if (tabs_) {
        auto* source = qobject_cast<QTableView*>(tabs_->currentWidget());
        const bool can_queue = source && source->selectionModel() &&
                               !source->selectionModel()->selectedRows().isEmpty() &&
                               qobject_cast<LocalListModel*>(source->model()) != nullptr;
        for (const auto& name :
             {QStringLiteral("action-queue-next"), QStringLiteral("action-queue-end")})
            if (auto* action = findChild<QAction*>(name))
                action->setEnabled(can_queue);
    }
    refreshSelectionActions();
    if (selection_status_ == nullptr) {
        return;
    }
    auto* tab = currentListTab();
    const bool viewed = tab != nullptr && tab->view->selectionModel() != nullptr;
    std::vector<int> rows;
    if (viewed) {
        for (const auto& index : tab->view->selectionModel()->selectedRows()) {
            rows.push_back(index.row());
        }
    }
    if (properties_action_ != nullptr) {
        properties_action_->setEnabled(!rows.empty());
    }
    if (convert_action_ != nullptr) {
        convert_action_->setEnabled(!rows.empty());
    }
    if (identify_albums_action_ != nullptr) {
        identify_albums_action_->setEnabled(viewed && tab->model->rowCount() > 0);
    }
    const auto summary = workspace_.selectionSummary(viewed ? tab : nullptr, rows);
    selection_status_->setText(summary.text);
    selection_status_->setToolTip(summary.tooltip);
}

} // namespace trackknife::bench
