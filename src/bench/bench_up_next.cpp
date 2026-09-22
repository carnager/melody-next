// SPDX-License-Identifier: GPL-3.0-only
#include "bench/animated_panel_dock.hpp"
#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/settings_dialog.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/queue_table_view.hpp"
#include <QDockWidget>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <numeric>

namespace trackknife::bench {
namespace {
QJsonArray continuationIdentity(const LocalTrackRow& row) {
    const auto number = [](const auto& value) {
        return value ? QString::number(*value) : QStringLiteral("none");
    };
    return {QString::fromLatin1(QByteArray::fromStdString(row.raw_path).toBase64()),
            number(row.selection.stream_index), number(row.selection.subsong_index),
            row.segment ? QString::number(row.segment->start_sample) : QStringLiteral("none"),
            row.segment ? number(row.segment->end_sample) : QStringLiteral("none")};
}
} // namespace

void BenchMainWindow::buildUpNext() {
    const auto visible = QSettings{}.value(QStringLiteral("up-next/visible"), false).toBool();
    auto* panel = new AnimatedPanelDock(QStringLiteral("Up Next"), QStringLiteral("up-next"), this);
    up_next_dock_ = panel;
    up_next_dock_->setObjectName(QStringLiteral("bench-up-next"));
    up_next_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* content = new QWidget(up_next_dock_);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(10, 8, 6, 8);
    auto* title = new QLabel(QStringLiteral("Up Next"), content);
    auto font = title->font();
    font.setBold(true);
    title->setFont(font);
    heading->addWidget(title, 1);
    auto* close = new QToolButton(content);
    close->setObjectName(QStringLiteral("up-next-close"));
    close->setAutoRaise(true);
    close->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    close->setToolTip(QStringLiteral("Close Up Next"));
    close->setAccessibleName(close->toolTip());
    connect(close, &QToolButton::clicked, up_next_dock_,
            [this] { up_next_dock_->setVisible(false); });
    heading->addWidget(close);
    layout->addLayout(heading);
    up_next_status_ = new QLabel(content);
    up_next_status_->setObjectName(QStringLiteral("up-next-status"));
    up_next_status_->setWordWrap(true);
    up_next_status_->setMargin(10);
    up_next_view_ = new ui::QueueTableView(content);
    up_next_view_->setObjectName(QStringLiteral("up-next-tracks"));
    up_next_local_model_ = new LocalListModel(this);
    up_next_mpd_model_ = new quick::MpdQueueModel(this);
    up_next_view_->setModel(up_next_local_model_);
    auto flat = defaultTrackViewLayout(ui::TrackViewPresentation::plain_columns);
    applyTrackViewLayout(up_next_view_, flat, flat);
    up_next_view_->setAlbumGroupingEnabled(false);

    for (int col = 0; col < up_next_local_model_->columnCount(); ++col)
        up_next_view_->setColumnHidden(col, col != ui::track_artist_column &&
                                                col != ui::track_title_column &&
                                                col != ui::track_length_column);
    up_next_view_->setAlternatingRowColors(true);
    up_next_view_->setShowGrid(false);
    up_next_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    up_next_view_->horizontalHeader()->setSectionResizeMode(ui::track_artist_column,
                                                            QHeaderView::Interactive);
    up_next_view_->setColumnWidth(ui::track_artist_column, 140);
    up_next_view_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    up_next_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    up_next_view_->horizontalHeader()->setSectionResizeMode(ui::track_length_column,
                                                            QHeaderView::ResizeToContents);
    up_next_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    up_next_view_->setDragDropMode(QAbstractItemView::DragDrop);
    up_next_view_->setDragEnabled(true);
    up_next_view_->setAcceptDrops(true);
    up_next_view_->setDropIndicatorShown(true);
    up_next_view_->setDragDropOverwriteMode(false);
    up_next_view_->setDefaultDropAction(Qt::MoveAction);
    up_next_view_->setReorderCallback(
        [this](const QVariantList&, int destination) { editUpNextSelection(4, destination); });
    up_next_view_->setExternalDropCallback(
        [this](QAbstractItemView* source, const QVariantList&, int position, Qt::DropAction) {
            auto* table = qobject_cast<QTableView*>(source);
            if (!table)
                return false;
            const bool local = qobject_cast<LocalListModel*>(table->model()) != nullptr;
            if (local == isMpdContext())
                return false;
            enqueueUpNext(table, false, position);
            return true;
        });
    const auto playRequest = [this](const QModelIndex& index) {
        if (!index.isValid())
            return;
        if (isMpdContext()) {
            if (const auto* track = up_next_mpd_model_->trackAt(index.row());
                track && track->queue_id) {
                mpd::RequestQueueCommand command;
                command.operation = mpd::RequestQueueOperation::play;
                command.id = *track->queue_id;
                mpd_controller_->editRequestQueue(std::move(command));
            }
        } else if (index.row() < static_cast<int>(local_requests_.pending().size())) {
            local_requests_.move(
                local_requests_.pending()[static_cast<std::size_t>(index.row())].id, 0);
            refreshUpNext();
            static_cast<void>(playLocalRequest());
        }
    };
    up_next_view_->setActivateCallback(playRequest);
    layout->addWidget(up_next_view_, 1);
    auto* actions = new QToolBar(content);
    actions->setObjectName(QStringLiteral("up-next-toolbar"));
    actions->setIconSize(QSize(16, 16));
    actions->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto button = [&](const QString& text, const QString& icon, auto callback) {
        auto* action = actions->addAction(QIcon::fromTheme(icon), text);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };
    auto* removeAction = button(QStringLiteral("Remove from Up Next"),
                                QStringLiteral("list-remove"), [this] { editUpNextSelection(1); });
    removeAction->setObjectName(QStringLiteral("up-next-remove"));
    auto* moveUp = button(QStringLiteral("Move up"), QStringLiteral("go-up"),
                          [this] { editUpNextSelection(2); });
    moveUp->setObjectName(QStringLiteral("up-next-move-up"));
    auto* moveDown = button(QStringLiteral("Move down"), QStringLiteral("go-down"),
                            [this] { editUpNextSelection(3); });
    moveDown->setObjectName(QStringLiteral("up-next-move-down"));
    actions->addSeparator();
    auto* clearAction = button(QStringLiteral("Clear pending tracks"), QStringLiteral("edit-clear"),
                               [this] { editUpNext(0); });
    clearAction->setObjectName(QStringLiteral("up-next-clear"));
    auto* undo = button(QStringLiteral("Undo"), QStringLiteral("edit-undo"), [this] {
        if (isMpdContext()) {
            mpd::RequestQueueCommand command;
            command.operation = mpd::RequestQueueOperation::undo;
            mpd_controller_->editRequestQueue(std::move(command));
        } else {
            local_requests_.undo();
            last_requested_next_.reset();
            persistUpNext();
            refreshUpNext();
        }
    });
    undo->setObjectName(QStringLiteral("up-next-undo"));
    layout->insertWidget(1, actions);
    layout->addWidget(up_next_status_);
    auto* resume = new QPushButton(QStringLiteral("Return to playlist now"), content);
    resume->setFlat(true);
    resume->setObjectName(QStringLiteral("up-next-return"));
    layout->addWidget(resume);
    connect(resume, &QPushButton::clicked, this, [this] {
        if (isMpdContext()) {
            mpd::RequestQueueCommand command;
            command.operation = mpd::RequestQueueOperation::resume;
            mpd_controller_->editRequestQueue(std::move(command));
        } else {
            local_requests_.clear();
            if (local_requests_.active())
                playAdjacent(1);
            persistUpNext();
            refreshUpNext();
        }
    });
    auto* remove = removeAction;
    remove->setShortcut(Qt::Key_Delete);
    remove->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    up_next_view_->addAction(remove);
    up_next_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        up_next_view_, &QWidget::customContextMenuRequested, this,
        [this, actions, playRequest](const QPoint& point) {
            QMenu menu(up_next_view_);
            auto* play = menu.addAction(QIcon::fromTheme(QStringLiteral("media-playback-start")),
                                        tr("Play"));
            play->setEnabled(up_next_view_->isEnabled() && up_next_view_->currentIndex().isValid());
            connect(play, &QAction::triggered, this, [this, playRequest] {
                const auto index = up_next_view_->currentIndex();
                if (index.isValid())
                    playRequest(index);
            });
            menu.addSeparator();
            menu.addActions(actions->actions());
            menu.exec(up_next_view_->viewport()->mapToGlobal(point));
        });
    connect(
        up_next_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
        [this] { refreshUpNext(); }, Qt::QueuedConnection);
    panel->setPanelContent(content);
    addDockWidget(Qt::RightDockWidgetArea, up_next_dock_);
    up_next_dock_->setVisible(visible);
    auto* toggle = panel->panelToggleAction();
    toggle->setObjectName(QStringLiteral("action-show-up-next"));
    toggle->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+U")));
    addAction(toggle);
    connect(mpd_controller_, &quick::MpdProbeController::stateChanged, this,
            &BenchMainWindow::refreshUpNext);
}

void BenchMainWindow::refreshUpNext() {
    if (!up_next_dock_)
        return;
    const bool server = isMpdContext();
    std::vector<std::uint64_t> selectedIds;
    for (const auto& index : up_next_view_->selectionModel()->selectedRows())
        if (index.row() >= 0 && index.row() < static_cast<int>(up_next_display_ids_.size()))
            selectedIds.push_back(up_next_display_ids_[static_cast<std::size_t>(index.row())]);
    bool replaced = false;
    if (auto* undo = up_next_dock_->findChild<QAction*>(QStringLiteral("up-next-undo")))
        undo->setEnabled(server
                             ? (mpd_controller_->connected() && mpd_controller_->requestQueue() &&
                                mpd_controller_->requestQueue()->can_undo)
                             : local_requests_.canUndo());
    if (auto* resume = up_next_dock_->findChild<QPushButton*>(QStringLiteral("up-next-return")))
        resume->setEnabled(server
                               ? (mpd_controller_->connected() && mpd_controller_->requestQueue() &&
                                  mpd_controller_->requestQueue()->active_id != 0)
                               : local_requests_.active().has_value());
    auto* model = server ? static_cast<QAbstractItemModel*>(up_next_mpd_model_)
                         : static_cast<QAbstractItemModel*>(up_next_local_model_);
    if (up_next_view_->model() != model) {
        selectedIds.clear();
        up_next_display_ids_.clear();
        up_next_local_revision_ = 0;
        up_next_remote_revision_ = 0;
        up_next_view_->setModel(model);
        auto flat = defaultTrackViewLayout(ui::TrackViewPresentation::plain_columns);
        applyTrackViewLayout(up_next_view_, flat, flat);
        up_next_view_->setAlbumGroupingEnabled(false);

        for (int col = 0; col < model->columnCount(); ++col)
            up_next_view_->setColumnHidden(col, col != ui::track_artist_column &&
                                                    col != ui::track_title_column &&
                                                    col != ui::track_length_column);
        up_next_view_->horizontalHeader()->setSectionResizeMode(ui::track_artist_column,
                                                                QHeaderView::Interactive);
        up_next_view_->setColumnWidth(ui::track_artist_column, 140);
        up_next_view_->horizontalHeader()->setSectionResizeMode(ui::track_length_column,
                                                                QHeaderView::ResizeToContents);
        connect(
            up_next_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshUpNext(); }, Qt::QueuedConnection);
        up_next_view_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    }
    if (server) {
        const auto& state = mpd_controller_->requestQueue();
        bool available = mpd_controller_->connected() &&
                         mpd_controller_->supportsCommand(QStringLiteral("melody_upnext")) &&
                         state.has_value();
        up_next_view_->setEnabled(available);
        if (available) {
            if (up_next_remote_revision_ != state->revision ||
                up_next_remote_profile_ != mpd_controller_->profileId()) {
                up_next_mpd_model_->replaceTracks(state->pending);
                replaced = true;
                up_next_display_ids_.clear();
                for (const auto& track : state->pending)
                    up_next_display_ids_.push_back(track.queue_id.value_or(0));
                up_next_remote_revision_ = state->revision;
                up_next_remote_profile_ = mpd_controller_->profileId();
            }
            up_next_status_->setText(QStringLiteral("Melody · %1 pending\nReturn to: %3")
                                         .arg(state->pending.size())
                                         .arg(state->context.empty()
                                                  ? QStringLiteral("Queue")
                                                  : QString::fromStdString(state->context)));
        } else {
            if (up_next_mpd_model_->rowCount() != 0)
                up_next_mpd_model_->replaceTracks({});
            up_next_display_ids_.clear();
            up_next_remote_revision_ = 0;
            up_next_status_->setText(QStringLiteral(
                "Up Next requires a connected Melody server with request-queue support. Stock MPD "
                "still supports inserting tracks into its normal list."));
        }
    } else {
        up_next_view_->setEnabled(true);
        if (up_next_local_revision_ != local_requests_.revision()) {
            std::vector<LocalTrackRow> rows;
            for (const auto& entry : local_requests_.pending())
                rows.push_back(entry.source);
            up_next_local_model_->replaceRows(std::move(rows));
            replaced = true;
            up_next_display_ids_.clear();
            for (const auto& entry : local_requests_.pending())
                up_next_display_ids_.push_back(entry.id);
            up_next_local_revision_ = local_requests_.revision();
        }
        auto* tab = tabForDocument(playback_document_id_);
        QString playing;
        if (local_requests_.active())
            playing = QString::fromStdString(local_requests_.active()->source.artist + " — " +
                                             local_requests_.active()->source.title);
        up_next_status_->setText(
            QStringLiteral("Local · %1 pending%2\nReturn to: %3")
                .arg(local_requests_.pending().size())
                .arg(playing.isEmpty() ? QString{} : QStringLiteral("\nPlaying: ") + playing)
                .arg(tab ? QString::fromStdString(tab->document.name)
                         : QStringLiteral("No normal playback")));
    }
    if (replaced) {
        auto* selection = up_next_view_->selectionModel();
        const QSignalBlocker blocker(selection);
        for (std::size_t i = 0; i < up_next_display_ids_.size(); ++i)
            if (std::ranges::find(selectedIds, up_next_display_ids_[i]) != selectedIds.end()) {
                const auto index = model->index(static_cast<int>(i), ui::track_title_column);
                selection->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                if (!selection->currentIndex().isValid())
                    selection->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            }
    }
    const auto selectionRows = up_next_view_->selectionModel()->selectedRows();
    int first = model->rowCount(), last = -1;
    for (const auto& index : selectionRows) {
        first = std::min(first, index.row());
        last = std::max(last, index.row());
    }
    const bool batchAvailable =
        !server || selectionRows.size() <= 1 ||
        mpd_controller_->supportsCommand(QStringLiteral("melody_upnext_edit"));
    const auto row = selectionRows.isEmpty() ? -1 : first;
    const auto count = model->rowCount();
    const auto enabled = up_next_view_->isEnabled();
    for (const auto& [name, available] : std::initializer_list<std::pair<const char*, bool>>{
             {"up-next-remove", batchAvailable && row >= 0 && row < count},
             {"up-next-move-up", batchAvailable && row > 0},
             {"up-next-move-down", batchAvailable && row >= 0 && last + 1 < count},
             {"up-next-clear", count > 0}}) {
        if (auto* action = up_next_dock_->findChild<QAction*>(QString::fromLatin1(name)))
            action->setEnabled(enabled && available);
    }
    if (count == 0 && enabled)
        up_next_status_->setText(up_next_status_->text() +
                                 tr("\nQueue tracks from any list, or drag them here."));
    if (up_next_button_)
        up_next_button_->setText(QStringLiteral("Up Next · %1").arg(model->rowCount()));
}

void BenchMainWindow::addUpNextActions(QMenu* menu, QTableView* source) {
    const bool remote = qobject_cast<LocalListModel*>(source->model()) == nullptr;
    for (bool prepend : {true, false}) {
        auto* action = findChild<QAction*>(prepend ? QStringLiteral("action-queue-next")
                                                   : QStringLiteral("action-queue-end"));
        if (!action)
            continue;
        auto* scoped = menu->addAction(action->text());
        scoped->setObjectName(action->objectName());
        scoped->setShortcuts(action->shortcuts());
        connect(scoped, &QAction::triggered, source,
                [this, source, prepend] { enqueueUpNext(source, prepend); });
        scoped->setEnabled(
            !source->selectionModel()->selectedRows().isEmpty() &&
            (!remote || (mpd_controller_->connected() &&
                         mpd_controller_->supportsCommand(QStringLiteral("melody_upnext")))));
    }
}

void BenchMainWindow::enqueueUpNext(QTableView* source, bool prepend, int position) {
    if (auto* local = qobject_cast<LocalListModel*>(source->model())) {
        std::vector<LocalTrackRow> rows;
        auto indices = source->selectionModel()->selectedRows();
        std::sort(indices.begin(), indices.end(),
                  [](const auto& a, const auto& b) { return a.row() < b.row(); });
        for (const auto& index : indices)
            rows.push_back(local->rows().at(static_cast<std::size_t>(index.row())));
        enqueueLocalRequests(std::move(rows), position >= 0 ? position : (prepend ? 0 : -1));
    } else {
        mpd::RequestQueueCommand command;
        command.operation = position >= 0 ? mpd::RequestQueueOperation::insert
                                          : (prepend ? mpd::RequestQueueOperation::prepend
                                                     : mpd::RequestQueueOperation::append);
        if (position >= 0)
            command.position = static_cast<unsigned>(position);
        for (const auto& track : selectedMpdViewTracks(source))
            command.uris.push_back(track.uri);
        if (!command.uris.empty())
            mpd_controller_->editRequestQueue(std::move(command));
    }
    refreshUpNext();
}

void BenchMainWindow::enqueueLocalRequests(std::vector<LocalTrackRow> rows, int position) {
    const auto count = rows.size();
    if (!local_requests_.insert(std::move(rows), position < 0
                                                     ? local_requests_.pending().size()
                                                     : static_cast<std::size_t>(position))) {
        statusBar()->showMessage(QStringLiteral("Up Next holds at most 500 tracks."), 5000);
        return;
    }
    last_requested_next_.reset();
    persistUpNext();
    refreshUpNext();
    statusBar()->showMessage(QStringLiteral("Added %1 to Up Next").arg(count), 3000);
}

// operation: remove, move up, move down, or drag to an insertion boundary.
void BenchMainWindow::editUpNextSelection(int operation, int destination) {
    const auto count = static_cast<int>(up_next_display_ids_.size());
    std::vector<bool> selected(static_cast<std::size_t>(count), false);
    for (const auto& index : up_next_view_->selectionModel()->selectedRows())
        if (index.row() >= 0 && index.row() < count)
            selected[static_cast<std::size_t>(index.row())] = true;
    if (std::ranges::find(selected, true) == selected.end())
        return;
    if (isMpdContext() && !mpd_controller_->supportsCommand(QStringLiteral("melody_upnext_edit"))) {
        const auto rows = up_next_view_->selectionModel()->selectedRows();
        if (rows.size() != 1) {
            statusBar()->showMessage(
                tr("Update Melody to edit multiple Up Next requests together."), 5000);
            return;
        }
        const auto row = rows.front().row();
        editUpNext(operation == 1 ? 1 : 2, row,
                   operation == 2   ? row - 1
                   : operation == 3 ? row + 1
                                    : destination - (destination > row ? 1 : 0));
        return;
    }
    std::vector<int> order(static_cast<std::size_t>(count));
    std::iota(order.begin(), order.end(), 0);
    if (operation == 1) {
        std::erase_if(order, [&](int row) { return selected[static_cast<std::size_t>(row)]; });
    } else if (operation == 2) {
        if (selected.front())
            return;
        for (int i = 1; i < count; ++i)
            if (selected[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])] &&
                !selected[static_cast<std::size_t>(order[static_cast<std::size_t>(i - 1)])])
                std::swap(order[static_cast<std::size_t>(i)],
                          order[static_cast<std::size_t>(i - 1)]);
    } else if (operation == 3) {
        if (selected.back())
            return;
        for (int i = count - 2; i >= 0; --i)
            if (selected[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])] &&
                !selected[static_cast<std::size_t>(order[static_cast<std::size_t>(i + 1)])])
                std::swap(order[static_cast<std::size_t>(i)],
                          order[static_cast<std::size_t>(i + 1)]);
    } else {
        destination = std::clamp(destination, 0, count);
        std::vector<int> moving;
        int before = 0;
        for (int i = 0; i < count; ++i)
            if (selected[static_cast<std::size_t>(i)]) {
                moving.push_back(i);
                if (i < destination)
                    ++before;
            }
        std::erase_if(order, [&](int row) { return selected[static_cast<std::size_t>(row)]; });
        order.insert(order.begin() + destination - before, moving.begin(), moving.end());
    }
    std::vector<std::uint64_t> ids;
    for (const auto row : order)
        ids.push_back(up_next_display_ids_[static_cast<std::size_t>(row)]);
    if (ids == up_next_display_ids_)
        return;
    if (isMpdContext()) {
        mpd::RequestQueueCommand command;
        command.operation = mpd::RequestQueueOperation::retain;
        command.ids.assign(ids.begin(), ids.end());
        mpd_controller_->editRequestQueue(std::move(command));
    } else if (local_requests_.retain(ids)) {
        last_requested_next_.reset();
        persistUpNext();
        refreshUpNext();
    }
}

void BenchMainWindow::editUpNext(int operation, int row, int destination) {
    if (isMpdContext()) {
        const auto& state = mpd_controller_->requestQueue();
        if (!state)
            return;
        mpd::RequestQueueCommand command;
        if (operation == 0)
            command.operation = mpd::RequestQueueOperation::clear;
        else {
            if (row < 0 || row >= static_cast<int>(state->pending.size()))
                return;
            command.id = state->pending[static_cast<std::size_t>(row)].queue_id.value_or(0);
            if (operation == 1)
                command.operation = mpd::RequestQueueOperation::remove;
            else {
                if (destination < 0 || destination >= static_cast<int>(state->pending.size()))
                    return;
                command.operation = mpd::RequestQueueOperation::move;
                command.position = static_cast<unsigned>(destination);
            }
        }
        mpd_controller_->editRequestQueue(std::move(command));
    } else {
        if (operation == 0)
            local_requests_.clear();
        else {
            if (row < 0 || row >= static_cast<int>(local_requests_.pending().size()))
                return;
            auto id = local_requests_.pending()[static_cast<std::size_t>(row)].id;
            if (operation == 1)
                local_requests_.remove(id);
            else {
                if (destination < 0)
                    return;
                local_requests_.move(id, static_cast<std::size_t>(destination));
            }
        }
        last_requested_next_.reset();
        persistUpNext();
        refreshUpNext();
    }
}

void BenchMainWindow::persistUpNext() {
    if (!persistence_ || !up_next_restored_)
        return;
    QJsonArray rows;
    const auto append = [&](const LocalTrackRow& row) {
        QJsonObject item{{QStringLiteral("path"),
                          QString::fromLatin1(QByteArray::fromStdString(row.raw_path).toBase64())},
                         {QStringLiteral("title"), QString::fromStdString(row.title)},
                         {QStringLiteral("artist"), QString::fromStdString(row.artist)}};
        if (row.selection.stream_index)
            item[QStringLiteral("stream")] = *row.selection.stream_index;
        if (row.selection.subsong_index)
            item[QStringLiteral("subsong")] = static_cast<int>(*row.selection.subsong_index);
        if (row.segment) {
            item[QStringLiteral("start")] = QString::number(row.segment->start_sample);
            if (row.segment->end_sample)
                item[QStringLiteral("end")] = QString::number(*row.segment->end_sample);
        }
        item[QStringLiteral("album")] = QString::fromStdString(row.album);
        item[QStringLiteral("albumArtist")] = QString::fromStdString(row.album_artist);
        item[QStringLiteral("date")] = QString::fromStdString(row.date);
        if (row.logical_reference)
            item[QStringLiteral("logical")] = QString::fromStdString(*row.logical_reference);
        if (row.duration_ms)
            item[QStringLiteral("duration")] = QString::number(*row.duration_ms);
        QJsonArray fields;
        for (const auto& field : row.metadata.fields) {
            QJsonArray values;
            for (const auto& value : field.values)
                values.push_back(QString::fromStdString(value));
            QJsonObject fieldJson{
                {QStringLiteral("name"), QString::fromStdString(field.canonical_name)},
                {QStringLiteral("native"), QString::fromStdString(field.native_name)},
                {QStringLiteral("values"), values},
                {QStringLiteral("provenance"), static_cast<int>(field.provenance)}};
            if (field.qualifier.language)
                fieldJson[QStringLiteral("language")] =
                    QString::fromStdString(*field.qualifier.language);
            if (field.qualifier.description)
                fieldJson[QStringLiteral("description")] =
                    QString::fromStdString(*field.qualifier.description);
            fields.push_back(fieldJson);
        }
        item[QStringLiteral("fields")] = fields;
        rows.push_back(item);
    };
    if (local_requests_.active())
        append(local_requests_.active()->source);
    if (local_requests_.active() && player_ &&
        QSettings{}.value(QLatin1String(SettingsDialog::restore_playback_key), false).toBool()) {
        const auto snapshot = player_->snapshot();
        const auto& source = local_requests_.active()->source;
        if ((snapshot.state == audio::LocalAuditionState::paused ||
             snapshot.state == audio::LocalAuditionState::playing ||
             snapshot.state == audio::LocalAuditionState::buffering ||
             snapshot.state == audio::LocalAuditionState::draining) &&
            snapshot.raw_path == source.raw_path && snapshot.selection == source.selection &&
            snapshot.chain_transitions == last_chain_transitions_ &&
            snapshot.segment == source.segment && snapshot.source_revision && snapshot.format &&
            snapshot.format->sample_rate > 0 && snapshot.position_sample >= 0 &&
            (!snapshot.end_sample || snapshot.position_sample < *snapshot.end_sample)) {
            const auto& revision = *snapshot.source_revision;
            const auto rate = snapshot.format->sample_rate;
            auto item = rows[0].toObject();
            item[QStringLiteral("resume")] = QJsonObject{
                {QStringLiteral("version"), 1},
                {QStringLiteral("position"),
                 QString::number(snapshot.position_sample / rate * 1000 +
                                 snapshot.position_sample % rate * 1000 / rate)},
                {QStringLiteral("revision"),
                 QJsonArray{QString::number(revision.device), QString::number(revision.inode),
                            QString::number(revision.size),
                            QString::number(revision.modification_time_seconds),
                            QString::number(revision.modification_time_nanoseconds)}}};
            rows[0] = item;
        }
    }
    for (const auto& entry : local_requests_.pending())
        append(entry.source);
    QJsonObject state{
        {QStringLiteral("version"), 1},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("document"), playback_document_id_},
        {QStringLiteral("row"), resolvePlaybackRow(tabForDocument(playback_document_id_))}};
    state[QStringLiteral("anchor")] =
        QString::fromLatin1(QByteArray::fromStdString(playback_source_.raw_path).toBase64());
    if (!request_return_entry_.is_nil()) {
        if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
            if (const auto row = tab->model->rowOfEntry(request_return_entry_, -1); row >= 0) {
                state[QStringLiteral("returnRow")] = row;
                state[QStringLiteral("returnSource")] =
                    continuationIdentity(tab->model->rows().at(static_cast<std::size_t>(row)));
            }
        }
    }
    persistence_->saveUiState(
        QStringLiteral("playback/up-next/v1"), QJsonDocument(state).toJson(QJsonDocument::Compact),
        [this](QString error) {
            if (!error.isEmpty())
                statusBar()->showMessage(
                    QStringLiteral("Up Next could not be saved: %1").arg(error), 8000);
        });
}

void BenchMainWindow::restoreUpNext() {
    if (!persistence_)
        return;
    const auto generation = resume_intent_generation_;
    const auto request_revision = local_requests_.revision();
    persistence_->loadUiState(QStringLiteral("playback/up-next/v1"), [this, generation,
                                                                      request_revision](
                                                                         QByteArray payload,
                                                                         QString error) {
        const auto finish = qScopeGuard([this, generation] {
            if (resume_restore_pending_ && generation == resume_intent_generation_)
                restoreLocalResume();
            else
                resume_restore_pending_ = false;
        });
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 5000);
            return;
        }
        if (!payload.isEmpty()) {
            const auto document = QJsonDocument::fromJson(payload);
            const auto state = document.object();
            if (state.value(QStringLiteral("version")).toInt() != 1) {
                statusBar()->showMessage(
                    QStringLiteral("Unsupported Up Next state; it has been preserved."), 8000);
                return;
            }
            std::vector<LocalTrackRow> rows;
            if (state.value(QStringLiteral("rows")).toArray().size() >
                static_cast<qsizetype>(audio::RequestQueue<LocalTrackRow>::limit + 1)) {
                statusBar()->showMessage(
                    tr("Saved Up Next exceeds the queue limit; it has been preserved."), 8000);
                return;
            }
            std::optional<std::int64_t> resume_position;
            bool first = true;
            for (const auto& value : state.value(QStringLiteral("rows")).toArray()) {
                const auto item = value.toObject();
                const bool was_first = std::exchange(first, false);
                LocalTrackRow row;
                row.raw_path =
                    QByteArray::fromBase64(item.value(QStringLiteral("path")).toString().toLatin1())
                        .toStdString();
                if (row.raw_path.empty())
                    continue;
                if (was_first) {
                    const auto resume = item.value(QStringLiteral("resume")).toObject();
                    const auto revision = resume.value(QStringLiteral("revision")).toArray();
                    bool valid = false;
                    const auto position =
                        resume.value(QStringLiteral("position")).toString().toLongLong(&valid);
                    if (resume.value(QStringLiteral("version")).toInt() == 1 && valid &&
                        position >= 0 && position <= 365LL * 24 * 60 * 60 * 1000 &&
                        revision.size() == 5) {
                        core::LocalSourceRevision observed;
                        bool ok[5]{};
                        observed.device = revision[0].toString().toULongLong(&ok[0]);
                        observed.inode = revision[1].toString().toULongLong(&ok[1]);
                        observed.size = revision[2].toString().toULongLong(&ok[2]);
                        observed.modification_time_seconds =
                            revision[3].toString().toLongLong(&ok[3]);
                        observed.modification_time_nanoseconds =
                            revision[4].toString().toLongLong(&ok[4]);
                        if (std::ranges::all_of(ok, [](bool parsed) { return parsed; }) &&
                            observed.inode != 0 && observed.modification_time_nanoseconds >= 0 &&
                            observed.modification_time_nanoseconds < 1000000000) {
                            row.source_revision = observed;
                            resume_position = position;
                        }
                    }
                }
                row.title = item.value(QStringLiteral("title")).toString().toStdString();
                row.artist = item.value(QStringLiteral("artist")).toString().toStdString();
                if (item.contains(QStringLiteral("stream")))
                    row.selection.stream_index = item.value(QStringLiteral("stream")).toInt();
                if (item.contains(QStringLiteral("subsong")))
                    row.selection.subsong_index = item.value(QStringLiteral("subsong")).toInt();
                if (item.contains(QStringLiteral("start")))
                    row.segment = formats::SampleRange{
                        item.value(QStringLiteral("start")).toString().toLongLong(),
                        item.contains(QStringLiteral("end"))
                            ? std::optional<std::int64_t>{item.value(QStringLiteral("end"))
                                                              .toString()
                                                              .toLongLong()}
                            : std::nullopt};
                row.album = item.value(QStringLiteral("album")).toString().toStdString();
                row.album_artist =
                    item.value(QStringLiteral("albumArtist")).toString().toStdString();
                row.date = item.value(QStringLiteral("date")).toString().toStdString();
                if (item.contains(QStringLiteral("logical")))
                    row.logical_reference =
                        item.value(QStringLiteral("logical")).toString().toStdString();
                if (item.contains(QStringLiteral("duration")))
                    row.duration_ms =
                        item.value(QStringLiteral("duration")).toString().toLongLong();
                for (const auto& fieldValue : item.value(QStringLiteral("fields")).toArray()) {
                    const auto fieldJson = fieldValue.toObject();
                    metadata::MetadataField field;
                    field.canonical_name =
                        fieldJson.value(QStringLiteral("name")).toString().toStdString();
                    field.native_name =
                        fieldJson.value(QStringLiteral("native")).toString().toStdString();
                    const auto provenance = fieldJson.value(QStringLiteral("provenance")).toInt();
                    if (provenance < 0 ||
                        provenance > static_cast<int>(metadata::FieldProvenance::sidecar))
                        continue;
                    field.provenance = static_cast<metadata::FieldProvenance>(provenance);
                    for (const auto& valueText :
                         fieldJson.value(QStringLiteral("values")).toArray())
                        field.values.push_back(valueText.toString().toStdString());
                    if (fieldJson.contains(QStringLiteral("language")))
                        field.qualifier.language =
                            fieldJson.value(QStringLiteral("language")).toString().toStdString();
                    if (fieldJson.contains(QStringLiteral("description")))
                        field.qualifier.description =
                            fieldJson.value(QStringLiteral("description")).toString().toStdString();
                    row.metadata.fields.push_back(std::move(field));
                }
                rows.push_back(std::move(row));
            }
            const bool untouched = generation == resume_intent_generation_ &&
                                   request_revision == local_requests_.revision() &&
                                   local_requests_.pending().empty() && !local_requests_.active();
            const bool resume_request =
                untouched && resume_position && !rows.empty() && player_ &&
                player_->snapshot().state == audio::LocalAuditionState::empty &&
                QSettings{}
                    .value(QLatin1String(SettingsDialog::restore_playback_key), false)
                    .toBool();
            std::vector<LocalTrackRow> remaining;
            if (untouched) {
                if (resume_request) {
                    local_requests_.insert({rows.front()}, 0);
                    remaining.assign(std::make_move_iterator(rows.begin() + 1),
                                     std::make_move_iterator(rows.end()));
                } else if (!local_requests_.insert(std::move(rows), 0)) {
                    statusBar()->showMessage(
                        tr("Saved Up Next exceeds the pending queue limit; it has been preserved."),
                        8000);
                    return;
                }
            }
            if (playback_document_id_.isEmpty() && !local_requests_.pending().empty()) {
                const auto id = state.value(QStringLiteral("document")).toString();
                const auto anchor = QByteArray::fromBase64(
                                        state.value(QStringLiteral("anchor")).toString().toLatin1())
                                        .toStdString();
                if (auto* tab = tabForDocument(id)) {
                    const auto row = state.value(QStringLiteral("row")).toInt(-1);
                    if (row >= 0 && row < tab->model->rowCount() &&
                        tab->model->rawPath(row) == anchor) {
                        playback_document_id_ = id;
                        playback_entry_ =
                            tab->model->rows().at(static_cast<std::size_t>(row)).entry_id;
                        playback_row_ = row;
                        playback_source_ = tab->model->source(row);
                        resetPlaybackOrder();
                    }
                }
            }
            if (resume_request) {
                // Restore the saved continuation, not a newly computed one. In
                // Consume mode the originating row may already have been removed.
                const auto id = state.value(QStringLiteral("document")).toString();
                if (auto* tab = tabForDocument(id)) {
                    const auto row = state.value(QStringLiteral("returnRow")).toInt(-1);
                    if (row >= 0 && row < tab->model->rowCount() &&
                        continuationIdentity(tab->model->rows().at(static_cast<std::size_t>(
                            row))) == state.value(QStringLiteral("returnSource")).toArray()) {
                        playback_document_id_ = id;
                        request_return_entry_ =
                            tab->model->rows().at(static_cast<std::size_t>(row)).entry_id;
                        if (playback_source_.raw_path.empty())
                            playback_source_ = tab->model->source(row);
                    }
                }
                refreshUpNext();
                static_cast<void>(playLocalRequest(*resume_position));
                if (!remaining.empty())
                    local_requests_.insert(std::move(remaining), local_requests_.pending().size());
            }
        }
        local_requests_.forgetUndo();
        up_next_restored_ = true;
        refreshUpNext();
    });
}
} // namespace trackknife::bench
