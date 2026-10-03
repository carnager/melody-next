// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QStatusBar>
#include <QTableView>

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

void BenchMainWindow::applyTrackViewLayout(QTableView* view, ui::TrackViewLayout& state,
                                           const ui::TrackViewLayout& layout) {
    if (view == nullptr) {
        return;
    }
    auto* queue_view = static_cast<ui::QueueTableView*>(view);
    applying_track_view_layout_ = true;
    view->setProperty(ui::track_artwork_column_property, local_artwork_column);
    view->setProperty(ui::track_artist_column_property, local_artist_column);
    view->setProperty(ui::track_number_column_property, local_track_number_column);
    view->setProperty(ui::track_title_column_property, local_title_column);
    view->setProperty(ui::track_album_column_property, local_album_column);
    view->setProperty(ui::track_date_column_property, local_date_column);
    view->setProperty(ui::track_length_column_property, local_length_column);
    view->setProperty(ui::track_separate_number_property, true);
    const auto grouped = layout.presentation == ui::TrackViewPresentation::albums_side_artwork ||
                         layout.presentation == ui::TrackViewPresentation::albums_header_artwork;
    const auto side_artwork = layout.presentation == ui::TrackViewPresentation::albums_side_artwork;
    view->setProperty(ui::track_side_artwork_property, side_artwork);
    // Albums and their headers already group the rows; stripes on top of
    // that are noise.
    view->setAlternatingRowColors(!grouped);

    auto* previous_delegate = view->itemDelegate();
    view->setItemDelegate(grouped
                              ? static_cast<QAbstractItemDelegate*>(new ui::QueueItemDelegate(view))
                              : static_cast<QAbstractItemDelegate*>(new QStyledItemDelegate(view)));
    if (previous_delegate != nullptr && previous_delegate->parent() == view) {
        previous_delegate->deleteLater();
    }
    view->verticalHeader()->setDefaultSectionSize(22);
    view->verticalHeader()->setMinimumSectionSize(18);
    queue_view->setAlbumGroupingEnabled(grouped);

    auto* header = view->horizontalHeader();
    const QSignalBlocker header_blocker{header};
    for (int visual = 0; visual < static_cast<int>(layout.columns.size()); ++visual) {
        const auto logical =
            trackColumnLogical(layout.columns[static_cast<std::size_t>(visual)].id);
        if (logical < 0) {
            continue;
        }
        const auto current_visual = header->visualIndex(logical);
        if (current_visual != visual) {
            header->moveSection(current_visual, visual);
        }
    }
    for (const auto& column : layout.columns) {
        const auto logical = trackColumnLogical(column.id);
        if (logical < 0) {
            continue;
        }
        const bool history_column =
            logical == local_play_count_column || logical == local_last_played_column;
        view->setColumnHidden(
            logical,
            !column.visible || (history_column && !qobject_cast<LocalListModel*>(view->model())));
        view->setColumnWidth(logical, column.width);
    }
    queue_view->setAlbumArtworkColumn(side_artwork ? local_artwork_column : -1);
    QHash<int, int> preferred_widths;
    QHash<int, int> minimum_widths;
    for (const auto& column : layout.columns) {
        const auto logical = trackColumnLogical(column.id);
        const auto spec = std::ranges::find(track_column_specs, logical, &TrackColumnSpec::logical);
        if (logical >= 0 && spec != track_column_specs.end()) {
            // Covers beside the rows: the cover gutter and the numbers fixed
            // (ADR-0250).
            const auto fixed = !side_artwork                          ? 0
                               : logical == local_artwork_column      ? ui::side_cover_gutter
                               : logical == local_track_number_column ? ui::side_number_width
                                                                      : 0;
            preferred_widths.insert(logical, fixed > 0 ? fixed : column.width);
            minimum_widths.insert(logical, fixed > 0 ? fixed : spec->minimum_width);
        }
    }
    queue_view->setAutoFillColumns({local_artist_column, local_title_column, local_album_column},
                                   std::move(preferred_widths), std::move(minimum_widths));
    state = layout;
    applying_track_view_layout_ = false;
    view->viewport()->update();
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
    auto* header = view->horizontalHeader();
    layout.columns.reserve(static_cast<std::size_t>(header->count()));
    for (int visual = 0; visual < header->count(); ++visual) {
        const auto logical = header->logicalIndex(visual);
        const auto id = trackColumnId(logical);
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
            .visible = !view->isColumnHidden(logical),
        });
    }
    return layout;
}

void BenchMainWindow::setTrackViewPresentation(const ui::TrackViewPresentation presentation) {
    auto* tab = currentListTab();
    if (tab == nullptr || applying_track_view_layout_) {
        return;
    }
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    applyTrackViewLayout(*tab, defaultTrackViewLayout(presentation));
    schedulePersist();
}

void BenchMainWindow::setTrackColumnVisible(const QString& column_id, const bool visible) {
    auto* tab = currentListTab();
    if (tab == nullptr || applying_track_view_layout_) {
        return;
    }
    if (workspace_.setColumnVisible(*tab, captureTrackViewLayout(*tab), column_id, visible)) {
        applyTrackViewLayout(*tab, tab->view_layout);
    } else {
        refreshTrackViewActions();
    }
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
    for (auto* action :
         {track_albums_side_action_, track_albums_header_action_, track_plain_columns_action_,
          track_compact_queue_action_, track_layout_reset_action_, track_layout_copy_action_}) {
        action->setEnabled(available);
    }
    for (auto* action : track_column_actions_) {
        action->setVisible(true);
        action->setEnabled(available);
    }
    if (!available) {
        return;
    }
    const auto layout = captureTrackViewLayout(*tab);
    const QSignalBlocker side_blocker{track_albums_side_action_};
    const QSignalBlocker header_blocker{track_albums_header_action_};
    const QSignalBlocker plain_blocker{track_plain_columns_action_};
    const QSignalBlocker compact_blocker{track_compact_queue_action_};
    track_albums_side_action_->setChecked(layout.presentation ==
                                          ui::TrackViewPresentation::albums_side_artwork);
    track_albums_header_action_->setChecked(layout.presentation ==
                                            ui::TrackViewPresentation::albums_header_artwork);
    track_plain_columns_action_->setChecked(layout.presentation ==
                                            ui::TrackViewPresentation::plain_columns);
    track_compact_queue_action_->setChecked(layout.presentation ==
                                            ui::TrackViewPresentation::compact_queue);
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
    QMenu menu(this);
    menu.addSection(QStringLiteral("Presentation"));
    menu.addAction(track_albums_side_action_);
    menu.addAction(track_albums_header_action_);
    menu.addAction(track_plain_columns_action_);
    menu.addAction(track_compact_queue_action_);
    auto* columns = menu.addMenu(QStringLiteral("Columns"));
    for (const auto& spec : track_column_specs) {
        columns->addAction(track_column_actions_.value(QString::fromLatin1(spec.id)));
    }
    menu.addSeparator();
    menu.addAction(track_layout_reset_action_);
    menu.exec(view->horizontalHeader()->mapToGlobal(position));
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
