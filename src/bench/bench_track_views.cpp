// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
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

ui::TrackViewLayout
BenchMainWindow::defaultTrackViewLayout(const ui::TrackViewPresentation presentation) const {
    std::vector<ui::TrackViewColumnLayout> columns;
    columns.reserve(track_column_specs.size());
    for (const auto& spec : track_column_specs) {
        auto width = spec.default_width;
        // Ratings stay one click away in the Columns menu rather than
        // claiming space in every default view.
        bool visible = spec.logical != local_rating_column;
        if (presentation == ui::TrackViewPresentation::albums_header_artwork &&
            spec.logical == local_artwork_column) {
            width = 42;
        } else if (presentation == ui::TrackViewPresentation::plain_columns &&
                   spec.logical == local_artwork_column) {
            visible = false;
        } else if (presentation == ui::TrackViewPresentation::compact_queue) {
            visible = spec.logical == local_artist_column ||
                      spec.logical == local_track_number_column ||
                      spec.logical == local_title_column || spec.logical == local_album_column ||
                      spec.logical == local_length_column;
        }
        columns.push_back(ui::TrackViewColumnLayout{
            .id = QString::fromLatin1(spec.id), .width = width, .visible = visible});
    }
    return ui::TrackViewLayout{.schema_version = ui::track_view_layout_schema_version,
                               .presentation = presentation,
                               .columns = std::move(columns)};
}

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
        view->setColumnHidden(logical, !column.visible);
        view->setColumnWidth(logical, column.width);
    }
    queue_view->setAlbumArtworkColumn(side_artwork ? local_artwork_column : -1);
    QHash<int, int> preferred_widths;
    QHash<int, int> minimum_widths;
    for (const auto& column : layout.columns) {
        const auto logical = trackColumnLogical(column.id);
        const auto spec = std::ranges::find(track_column_specs, logical, &TrackColumnSpec::logical);
        if (logical >= 0 && spec != track_column_specs.end()) {
            preferred_widths.insert(logical, column.width);
            minimum_widths.insert(logical, spec->minimum_width);
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
        layout.columns.push_back(ui::TrackViewColumnLayout{
            .id = trackColumnId(logical),
            .width = std::max(24, header->sectionSize(logical)),
            .visible = !view->isColumnHidden(logical),
        });
    }
    return layout;
}

void BenchMainWindow::setTrackViewPresentation(const ui::TrackViewPresentation presentation) {
    if (isMpdContext()) {
        if (applying_track_view_layout_) {
            return;
        }
        mpd_view_layout_persistence_protected_ = false;
        preserved_mpd_view_layout_.clear();
        applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_,
                             defaultTrackViewLayout(presentation));
        schedulePersist();
        return;
    }
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
    if (isMpdContext()) {
        if (applying_track_view_layout_) {
            return;
        }
        auto layout = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
        const auto visible_count =
            std::ranges::count(layout.columns, true, &ui::TrackViewColumnLayout::visible);
        const auto found =
            std::ranges::find(layout.columns, column_id, &ui::TrackViewColumnLayout::id);
        if (found == layout.columns.end() || (!visible && found->visible && visible_count == 1)) {
            refreshTrackViewActions();
            return;
        }
        found->visible = visible;
        mpd_view_layout_persistence_protected_ = false;
        preserved_mpd_view_layout_.clear();
        applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, layout);
        schedulePersist();
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr || applying_track_view_layout_) {
        return;
    }
    auto layout = captureTrackViewLayout(*tab);
    const auto visible_count =
        std::ranges::count(layout.columns, true, &ui::TrackViewColumnLayout::visible);
    const auto found = std::ranges::find(layout.columns, column_id, &ui::TrackViewColumnLayout::id);
    if (found == layout.columns.end() || (!visible && found->visible && visible_count == 1)) {
        refreshTrackViewActions();
        return;
    }
    found->visible = visible;
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    applyTrackViewLayout(*tab, layout);
    schedulePersist();
}

void BenchMainWindow::resetTrackViewLayout() {
    if (isMpdContext()) {
        mpd_view_layout_persistence_protected_ = false;
        preserved_mpd_view_layout_.clear();
        applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, defaultTrackViewLayout());
        schedulePersist();
        return;
    }
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
    auto layout = ui::TrackViewLayout{};
    if (isMpdContext()) {
        layout = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
    } else if (auto* source = currentListTab(); source != nullptr) {
        layout = captureTrackViewLayout(*source);
    } else {
        return;
    }
    mpd_view_layout_persistence_protected_ = false;
    preserved_mpd_view_layout_.clear();
    applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, layout);
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
    const auto available = tab != nullptr || isMpdContext();
    for (auto* action :
         {track_albums_side_action_, track_albums_header_action_, track_plain_columns_action_,
          track_compact_queue_action_, track_layout_reset_action_, track_layout_copy_action_}) {
        action->setEnabled(available);
    }
    for (auto* action : track_column_actions_) {
        action->setEnabled(available);
    }
    if (!available) {
        return;
    }
    const auto layout = isMpdContext() ? captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_)
                                       : captureTrackViewLayout(*tab);
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

void BenchMainWindow::refreshSelectionStatus() {
    if (selection_status_ == nullptr) {
        return;
    }
    if (isMpdContext()) {
        // The menubar actions stay local-only; mapped MPD selections get the
        // context-menu sugar actions instead (ADR-0180).
        if (properties_action_ != nullptr) {
            properties_action_->setEnabled(false);
        }
        if (convert_action_ != nullptr) {
            convert_action_->setEnabled(false);
        }
        auto* playlist_tab = currentMpdPlaylistTab();
        auto* view = playlist_tab != nullptr ? playlist_tab->view : mpd_queue_view_;
        const auto label =
            playlist_tab != nullptr ? playlist_tab->name : QStringLiteral("MPD Queue");
        if (view->selectionModel() == nullptr) {
            selection_status_->setText(label);
            return;
        }
        const auto selected = view->selectionModel()->selectedRows();
        if (selected.empty()) {
            const auto count =
                playlist_tab != nullptr ? view->model()->rowCount() : mpd_controller_->queueCount();
            selection_status_->setText(QStringLiteral("%1 · %2 tracks").arg(label).arg(count));
            selection_status_->setToolTip(mpd_controller_->status());
            return;
        }
        qint64 duration_ms = 0;
        for (const auto& index : selected) {
            duration_ms += index.data(ui::track_duration_ms_role).toLongLong();
        }
        const auto summary = QStringLiteral("%1 · %2 selected · %3 total")
                                 .arg(label)
                                 .arg(selected.size())
                                 .arg(formatTime(duration_ms));
        selection_status_->setText(summary);
        selection_status_->setToolTip(summary);
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr || tab->view->selectionModel() == nullptr) {
        if (properties_action_ != nullptr) {
            properties_action_->setEnabled(false);
        }
        if (convert_action_ != nullptr) {
            convert_action_->setEnabled(false);
        }
        selection_status_->setText(QStringLiteral("No tracks selected"));
        selection_status_->setToolTip({});
        return;
    }

    const auto selected = tab->view->selectionModel()->selectedRows();
    if (properties_action_ != nullptr) {
        properties_action_->setEnabled(!selected.empty());
    }
    if (convert_action_ != nullptr) {
        convert_action_->setEnabled(!selected.empty());
    }
    if (selected.empty()) {
        selection_status_->setText(QStringLiteral("No tracks selected"));
        selection_status_->setToolTip({});
        return;
    }

    if (selected.size() == 1) {
        const auto row_index = selected.front().row();
        if (row_index < 0 || row_index >= static_cast<int>(tab->model->rows().size())) {
            selection_status_->setText(QStringLiteral("No tracks selected"));
            selection_status_->setToolTip({});
            return;
        }
        const auto& track = tab->model->rows()[static_cast<std::size_t>(row_index)];
        const auto fallback = tab->model->index(row_index, local_title_column).data().toString();
        const auto title = track.title.empty() ? fallback : displayText(track.title);
        QStringList details;
        details.push_back(track.artist.empty()
                              ? title
                              : QStringLiteral("%1 — %2").arg(displayText(track.artist), title));
        if (!track.album.empty() || !track.date.empty()) {
            auto release = displayText(track.album);
            if (!track.date.empty()) {
                release += release.isEmpty() ? displayText(track.date)
                                             : QStringLiteral(" (%1)").arg(displayText(track.date));
            }
            details.push_back(release);
        }
        if (track.duration_ms) {
            details.push_back(formatTime(*track.duration_ms));
        }
        const auto summary = details.join(QStringLiteral(" · "));
        selection_status_->setText(summary);
        selection_status_->setToolTip(
            QString::fromStdString(core::escape_raw_path(track.raw_path)));
        return;
    }

    qint64 total_duration_ms = 0;
    int unknown_durations = 0;
    for (const auto& index : selected) {
        if (index.row() < 0 || index.row() >= static_cast<int>(tab->model->rows().size())) {
            continue;
        }
        const auto& duration =
            tab->model->rows()[static_cast<std::size_t>(index.row())].duration_ms;
        if (duration) {
            total_duration_ms += *duration;
        } else {
            ++unknown_durations;
        }
    }
    auto summary = QStringLiteral("%1 tracks selected · %2 total")
                       .arg(selected.size())
                       .arg(formatTime(total_duration_ms));
    if (unknown_durations > 0) {
        summary += QStringLiteral(" · %1 duration unknown").arg(unknown_durations);
    }
    selection_status_->setText(summary);
    selection_status_->setToolTip(summary);
}

} // namespace trackknife::bench
