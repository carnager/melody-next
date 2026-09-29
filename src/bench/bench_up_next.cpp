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
    auto* library = libraryOf(files.engine());
    if (carried.isEmpty() || library == nullptr) {
        return false;
    }
    std::vector<persistence::LibraryEntry> entries;
    for (const auto& entry : carried) {
        entries.push_back(entry.value<persistence::LibraryEntry>());
    }
    library->resolveEntryRows(std::move(entries), [this, position, engine = files.engine()](
                                                      std::vector<LocalTrackRow> rows) {
        enqueueLocalRequests(std::move(rows), position, engine);
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
    const auto heading = workspace_.upNextHeading();
    if (auto* undo = up_next_dock_->findChild<QAction*>(QStringLiteral("up-next-undo")))
        undo->setEnabled(heading.can_undo);
    auto* resume = up_next_dock_->findChild<QToolButton*>(QStringLiteral("up-next-return"));
    auto* model = up_next_local_model_;
    up_next_view_->setEnabled(true);
    const bool replaced = workspace_.syncUpNextModel();
    up_next_status_->setText(heading.status);
    up_next_status_->setToolTip(heading.status_tooltip);
    if (resume != nullptr) {
        // Elided to what is left beside the edit buttons.
        const auto* toolbar = up_next_dock_->findChild<QToolBar*>(QStringLiteral("up-next-toolbar"));
        const auto room = std::max(48, up_next_dock_->width() -
                                           (toolbar ? toolbar->sizeHint().width() : 0) -
                                           resume->iconSize().width() - 36);
        resume->setText(resume->fontMetrics().elidedText(heading.back, Qt::ElideRight, room));
        resume->setToolTip(heading.back_tooltip);
        resume->setEnabled(heading.back_enabled);
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
                             engineOfView(source));
    }
    refreshUpNext();
}

// operation: remove, move up, move down, or drag to an insertion boundary.
void BenchMainWindow::editUpNextSelection(int operation, int destination) {
    const auto count = static_cast<int>(up_next_display_ids_.size());
    std::vector<bool> selected(static_cast<std::size_t>(count), false);
    for (const auto& index : up_next_view_->selectionModel()->selectedRows())
        if (index.row() >= 0 && index.row() < count)
            selected[static_cast<std::size_t>(index.row())] = true;
    workspace_.editUpNextRows(std::move(selected), operation, destination);
}

} // namespace trackknife::bench
