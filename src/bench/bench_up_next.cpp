// SPDX-License-Identifier: GPL-3.0-only
#include "bench/animated_panel_dock.hpp"
#include "bench/bench_main_window.hpp"
#include "bench/up_next_delegate.hpp"
#include "uicommon/local_files_mime_data.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/queue_table_view.hpp"
#include <QDockWidget>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFrame>
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
    // Header: what this is, and whose, with how many wait.
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(12, 10, 6, 8);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto* title = new QLabel(QStringLiteral("Up Next"), content);
    auto font = title->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.08);
    title->setFont(font);
    titles->addWidget(title);
    up_next_status_ = new QLabel(content);
    up_next_status_->setObjectName(QStringLiteral("up-next-status"));
    up_next_status_->setWordWrap(true);
    up_next_status_->setForegroundRole(QPalette::PlaceholderText);
    titles->addWidget(up_next_status_);
    heading->addLayout(titles, 1);
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
    up_next_view_ = new ui::QueueTableView(content);
    up_next_view_->setObjectName(QStringLiteral("up-next-tracks"));
    up_next_local_model_ = new LocalListModel(this);
    up_next_view_->setModel(up_next_local_model_);
    auto flat = defaultTrackViewLayout(ui::TrackViewPresentation::plain_columns);
    applyTrackViewLayout(up_next_view_, flat, flat);
    up_next_view_->setAlbumGroupingEnabled(false);

    // A stack of tracks, not a table: one column, drawn two lines high with
    // cover, title, artist and length.
    for (int col = 0; col < up_next_local_model_->columnCount(); ++col)
        up_next_view_->setColumnHidden(col, col != local_title_column);
    up_next_view_->setItemDelegate(
        new UpNextDelegate(local_artist_column, local_length_column, up_next_view_));
    up_next_view_->horizontalHeader()->hide();
    up_next_view_->verticalHeader()->hide();
    up_next_view_->horizontalHeader()->setSectionResizeMode(local_title_column,
                                                            QHeaderView::Stretch);
    up_next_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    up_next_view_->verticalHeader()->setDefaultSectionSize(UpNextDelegate::row_height);
    up_next_view_->setAlternatingRowColors(false);
    up_next_view_->setShowGrid(false);
    up_next_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    up_next_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
            if (qobject_cast<LocalListModel*>(table->model()) == nullptr)
                return false;
            enqueueUpNext(table, false, position);
            return true;
        });
    // From the library: its entries, resolved to tagged rows by the library
    // they came from.
    up_next_view_->setLocalFilesDropCallback(
        [this](const ui::LocalFilesMimeData& files, const int position) {
            return enqueueLibraryDrop(files, position);
        });
    const auto playRequest = [this](const QModelIndex& index) {
        if (!index.isValid())
            return;
        if (index.row() < static_cast<int>(playback_.requests.pending().size())) {
            playback_.requests.move(
                playback_.requests.pending()[static_cast<std::size_t>(index.row())].id, 0);
            refreshUpNext();
            // The engine plays asks before the list, so Next is this one.
            if (playingOnEngine())
                transport_->next();
        }
    };
    up_next_view_->setActivateCallback(playRequest);
    layout->addWidget(up_next_view_, 1);
    // Footer: the edits on the left, the way back to the list on the right,
    // under a hairline.
    auto* footer = new QFrame(content);
    footer->setObjectName(QStringLiteral("up-next-footer"));
    {
        const auto ground = palette().color(QPalette::Window);
        const auto ink = palette().color(QPalette::Text);
        const auto mix = [](const int a, const int b) { return (a * 88 + b * 12) / 100; };
        footer->setStyleSheet(
            QStringLiteral("QFrame#up-next-footer { border-top: 1px solid %1; }")
                .arg(QColor::fromRgb(mix(ground.red(), ink.red()), mix(ground.green(), ink.green()),
                                     mix(ground.blue(), ink.blue()))
                         .name()));
    }
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(4, 4, 8, 4);
    footer_layout->setSpacing(4);
    auto* actions = new QToolBar(footer);
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
        playback_.requests.undo();
        persistUpNext();
        refreshUpNext();
    });
    undo->setObjectName(QStringLiteral("up-next-undo"));
    // Its buttons always shown: the way back gives up width first.
    actions->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    footer_layout->addWidget(actions);
    footer_layout->addStretch(1);
    auto* resume = new QToolButton(footer);
    resume->setObjectName(QStringLiteral("up-next-return"));
    resume->setText(QStringLiteral("Back to the list"));
    resume->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    resume->setIcon(QIcon::fromTheme(QStringLiteral("go-next")));
    resume->setLayoutDirection(Qt::RightToLeft);
    resume->setAutoRaise(true);
    footer_layout->addWidget(resume);
    layout->addWidget(footer);
    connect(resume, &QToolButton::clicked, this, [this] {
        const bool asking = playback_.requests.active().has_value();
        playback_.requests.clear();
        persistUpNext();
        refreshUpNext();
        // With no asks left, the engine's Next returns to where it left off.
        if (asking && playingOnEngine())
            transport_->next();
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
}

bool BenchMainWindow::enqueueLibraryDrop(const ui::LocalFilesMimeData& files, const int position) {
    const auto carried = files.property(library_entries_property).toList();
    auto* library = files.remote() ? remote_library_ : local_library_;
    if (carried.isEmpty() || library == nullptr) {
        return false;
    }
    std::vector<persistence::LibraryEntry> entries;
    for (const auto& entry : carried) {
        entries.push_back(entry.value<persistence::LibraryEntry>());
    }
    const bool remote = files.remote();
    library->resolveEntryRows(std::move(entries),
                              [this, position, remote](std::vector<LocalTrackRow> rows) {
                                  enqueueLocalRequests(std::move(rows), position, remote);
                              });
    return true;
}

void BenchMainWindow::refreshUpNext() {
    if (!up_next_dock_)
        return;
    std::vector<std::uint64_t> selectedIds;
    for (const auto& index : up_next_view_->selectionModel()->selectedRows())
        if (index.row() >= 0 && index.row() < static_cast<int>(up_next_display_ids_.size()))
            selectedIds.push_back(up_next_display_ids_[static_cast<std::size_t>(index.row())]);
    bool replaced = false;
    if (auto* undo = up_next_dock_->findChild<QAction*>(QStringLiteral("up-next-undo")))
        undo->setEnabled(playback_.requests.canUndo());
    auto* resume = up_next_dock_->findChild<QToolButton*>(QStringLiteral("up-next-return"));
    auto* model = up_next_local_model_;
    {
        up_next_view_->setEnabled(true);
        if (up_next_local_revision_ != playback_.requests.revision()) {
            std::vector<LocalTrackRow> rows;
            for (const auto& entry : playback_.requests.pending())
                rows.push_back(entry.source);
            up_next_local_model_->replaceRows(std::move(rows));
            replaced = true;
            up_next_display_ids_.clear();
            for (const auto& entry : playback_.requests.pending())
                up_next_display_ids_.push_back(entry.id);
            up_next_local_revision_ = playback_.requests.revision();
        }
        // Covers as the lists have them, or fetched when no list does.
        for (int row = 0; row < up_next_local_model_->rowCount(); ++row) {
            const auto key = up_next_local_model_->groupKey(row);
            if (up_next_local_model_->hasArtwork(key)) {
                continue;
            }
            if (const auto cover = coverFor(
                    up_next_local_model_->rows()[static_cast<std::size_t>(row)], up_next_remote_);
                !cover.isNull()) {
                up_next_local_model_->setArtwork(key, cover);
            }
        }
        // The engine decides what plays next, so it has to be told. Guarded on
        // the order actually changing, because this runs on every refresh.
        syncEngineRequests();
        auto* tab = tabForDocument(playback_.anchors.document);
        QString playing;
        if (playback_.requests.active())
            playing = QString::fromStdString(playback_.requests.active()->source.artist + " — " +
                                             playback_.requests.active()->source.title);
        // Up Next belongs to the engine that is playing (ADR-0227), so it is
        // named by it -- "Local" was the MPD era's word for this computer.
        const auto engine = transport_ != nullptr && transport_ == remote_playback_ &&
                                    remote_catalogue_source_
                                ? remote_catalogue_source_->name()
                                : QStringLiteral("This computer");
        up_next_status_->setText(QStringLiteral("%1 · %2 waiting")
                                     .arg(engine)
                                     .arg(playback_.requests.pending().size()));
        up_next_status_->setToolTip(playing.isEmpty() ? QString{}
                                                      : QStringLiteral("Playing: ") + playing);
        // Where playback goes once these are done, and a way there now.
        if (resume != nullptr) {
            const auto back = tab ? QString::fromStdString(tab->document.name) : QString{};
            const auto label = back.isEmpty() ? tr("Back to the list") : tr("Back to %1").arg(back);
            // Elided to what is left beside the edit buttons.
            const auto* toolbar = up_next_dock_->findChild<QToolBar*>(QStringLiteral("up-next-toolbar"));
            const auto room = std::max(48, up_next_dock_->width() -
                                               (toolbar ? toolbar->sizeHint().width() : 0) -
                                               resume->iconSize().width() - 36);
            resume->setText(resume->fontMetrics().elidedText(label, Qt::ElideRight, room));
            resume->setToolTip(tr("Skip what is waiting and return to the list now"));
            resume->setEnabled(playback_.requests.active().has_value() ||
                               !playback_.requests.pending().empty());
        }
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
    const auto row = selectionRows.isEmpty() ? -1 : first;
    const auto count = model->rowCount();
    const auto enabled = up_next_view_->isEnabled();
    for (const auto& [name, available] : std::initializer_list<std::pair<const char*, bool>>{
             {"up-next-remove", row >= 0 && row < count},
             {"up-next-move-up", row > 0},
             {"up-next-move-down", row >= 0 && last + 1 < count},
             {"up-next-clear", count > 0}}) {
        if (auto* action = up_next_dock_->findChild<QAction*>(QString::fromLatin1(name)))
            action->setEnabled(enabled && available);
    }
    up_next_view_->setEmptyMessage(enabled ? tr("Nothing waiting") : QString{},
                                   tr("Drag tracks here from a list or the library."));
    setUpNextCount(model->rowCount());
}

void BenchMainWindow::addUpNextActions(QMenu* menu, QTableView* source) {
    const bool local = qobject_cast<LocalListModel*>(source->model()) != nullptr;
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
        scoped->setEnabled(local && !source->selectionModel()->selectedRows().isEmpty());
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
        enqueueLocalRequests(std::move(rows), position >= 0 ? position : (prepend ? 0 : -1),
                             isRemoteView(source));
    }
    refreshUpNext();
}

void BenchMainWindow::enqueueLocalRequests(std::vector<LocalTrackRow> rows, int position,
                                           const bool remote) {
    const auto count = rows.size();
    // ADR-0227: an ask is a file on one engine's machine, and a queue cannot
    // play files from two. Up Next is one engine's until it is empty again.
    const bool holding =
        !playback_.requests.pending().empty() || playback_.requests.active().has_value();
    if (holding && remote != up_next_remote_) {
        statusBar()->showMessage(
            remote ? QStringLiteral("Up Next holds this computer's tracks; finish or clear it "
                                    "before adding the remote engine's")
                   : QStringLiteral("Up Next holds the remote engine's tracks; finish or clear "
                                    "it before adding this computer's"),
            6000);
        return;
    }
    if (!holding) {
        up_next_remote_ = remote;
        engine_requests_.clear();
    }
    // Each ask is an occurrence of its own (ADR-0221): the same track asked
    // for twice plays twice, and playing it does not move the list, whose row
    // it was copied from. The engine holds these apart from the list; a copied
    // identity made both asks one entry, and the second never played.
    for (auto& row : rows) {
        row.entry_id = core::StableId::random();
    }
    if (!playback_.requests.insert(std::move(rows), position < 0
                                                        ? playback_.requests.pending().size()
                                                        : static_cast<std::size_t>(position))) {
        statusBar()->showMessage(QStringLiteral("Up Next holds at most 500 tracks."), 5000);
        return;
    }
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
    if (playback_.requests.retain(ids)) {
        persistUpNext();
        refreshUpNext();
    }
}

void BenchMainWindow::editUpNext(int operation, int row, int destination) {
    if (operation == 0)
        playback_.requests.clear();
    else {
        if (row < 0 || row >= static_cast<int>(playback_.requests.pending().size()))
            return;
        auto id = playback_.requests.pending()[static_cast<std::size_t>(row)].id;
        if (operation == 1)
            playback_.requests.remove(id);
        else {
            if (destination < 0)
                return;
            playback_.requests.move(id, static_cast<std::size_t>(destination));
        }
    }
    persistUpNext();
    refreshUpNext();
}

void BenchMainWindow::persistUpNext() {
    if (!persistence_ || !up_next_restored_)
        return;
    QJsonArray rows;
    const auto append = [&](const LocalTrackRow& row) {
        QJsonObject item{{QStringLiteral("path"),
                          QString::fromLatin1(QByteArray::fromStdString(row.raw_path).toBase64())},
                         {QStringLiteral("title"), QString::fromStdString(row.title)},
                         {QStringLiteral("artist"), QString::fromStdString(row.artist)},
                         // The identity the engine holds it by: kept, so after a
                         // restart both still name the same entry.
                         {QStringLiteral("entry"), QString::fromStdString(row.entry_id.to_string())}};
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
    if (playback_.requests.active())
        append(playback_.requests.active()->source);
    for (const auto& entry : playback_.requests.pending())
        append(entry.source);
    QJsonObject state{
        {QStringLiteral("version"), 1},
        {QStringLiteral("rows"), rows},
        // Whose files these are: Up Next holds one engine's asks.
        {QStringLiteral("remote"), up_next_remote_},
        {QStringLiteral("document"), document_text(playback_.anchors.document)},
        {QStringLiteral("row"), resolvePlaybackRow(tabForDocument(playback_.anchors.document))}};
    state[QStringLiteral("anchor")] = QString::fromLatin1(
        QByteArray::fromStdString(playback_.anchors.source.raw_path).toBase64());
    if (!playback_.anchors.request_return.is_nil()) {
        if (auto* tab = tabForDocument(playback_.anchors.document); tab != nullptr) {
            if (const auto row = tab->model->rowOfEntry(playback_.anchors.request_return, -1);
                row >= 0) {
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
    const auto request_revision = playback_.requests.revision();
    persistence_->loadUiState(QStringLiteral("playback/up-next/v1"), [this, request_revision](
                                                                         QByteArray payload,
                                                                         QString error) {
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
            for (const auto& value : state.value(QStringLiteral("rows")).toArray()) {
                const auto item = value.toObject();
                LocalTrackRow row;
                row.raw_path =
                    QByteArray::fromBase64(item.value(QStringLiteral("path")).toString().toLatin1())
                        .toStdString();
                if (row.raw_path.empty())
                    continue;
                row.title = item.value(QStringLiteral("title")).toString().toStdString();
                row.artist = item.value(QStringLiteral("artist")).toString().toStdString();
                // Saved before identities were: a new one, as then.
                if (const auto identity = core::StableId::parse(
                        item.value(QStringLiteral("entry")).toString().toStdString())) {
                    row.entry_id = *identity;
                }
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
            const bool untouched = request_revision == playback_.requests.revision() &&
                                   playback_.requests.pending().empty() &&
                                   !playback_.requests.active();
            if (untouched) {
                // Saved before the engine was: whose the tab it returns to is.
                if (state.contains(QStringLiteral("remote"))) {
                    up_next_remote_ = state.value(QStringLiteral("remote")).toBool();
                } else if (const auto* returns_to =
                               tabForDocument(state.value(QStringLiteral("document")).toString())) {
                    up_next_remote_ = returns_to->document.remote;
                }
                if (!playback_.requests.insert(std::move(rows), 0)) {
                    statusBar()->showMessage(
                        tr("Saved Up Next exceeds the pending queue limit; it has been preserved."),
                        8000);
                    return;
                }
            }
            if (playback_.anchors.document.is_nil() && !playback_.requests.pending().empty()) {
                const auto id = state.value(QStringLiteral("document")).toString();
                const auto anchor = QByteArray::fromBase64(
                                        state.value(QStringLiteral("anchor")).toString().toLatin1())
                                        .toStdString();
                if (auto* tab = tabForDocument(id)) {
                    const auto row = state.value(QStringLiteral("row")).toInt(-1);
                    if (row >= 0 && row < tab->model->rowCount() &&
                        tab->model->rawPath(row) == anchor) {
                        playback_.anchors.document = document_identity(id);
                        playback_.anchors.current =
                            tab->model->rows().at(static_cast<std::size_t>(row)).entry_id;
                        playback_.row = row;
                        playback_.anchors.source = tab->model->source(row);
                    }
                }
            }
        }
        playback_.requests.forgetUndo();
        up_next_restored_ = true;
        refreshUpNext();
    });
}
} // namespace trackknife::bench
