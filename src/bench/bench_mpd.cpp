// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/mpd_library_search_model.hpp"
#include "bench/settings_dialog.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_output_model.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "quick/mpd_search_result_model.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/audio/melody_agent.hpp"
#include "trackknife/mpd/music_root.hpp"
#include "ui/mpd_connection_dialog.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/rating_stars.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <ranges>
#include <tuple>
#include <utility>

namespace trackknife::bench {
void BenchMainWindow::buildMpdStatusControls() {
    auto* separator = new QFrame(statusBar());
    mpd_status_separator_ = separator;
    separator->setObjectName(QStringLiteral("bench-mpd-status-separator"));
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    statusBar()->addPermanentWidget(separator);

    const auto add_action_button = [this](QAction* action, const QString& object_name) {
        auto* button = new QToolButton(statusBar());
        button->setObjectName(object_name);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setDefaultAction(action);
        statusBar()->addPermanentWidget(button);
        return button;
    };

    mpd_repeat_action_ =
        new QAction(QIcon::fromTheme(QStringLiteral("media-playlist-repeat"),
                                     style()->standardIcon(QStyle::SP_BrowserReload)),
                    QStringLiteral("Repeat"), this);
    mpd_repeat_action_->setObjectName(QStringLiteral("action-mpd-repeat"));
    mpd_repeat_action_->setCheckable(true);
    mpd_repeat_button_ = add_action_button(mpd_repeat_action_, QStringLiteral("bench-mpd-repeat"));
    connect(mpd_repeat_action_, &QAction::triggered, mpd_controller_,
            &quick::MpdProbeController::setRepeatEnabled);

    mpd_random_action_ =
        new QAction(QIcon::fromTheme(QStringLiteral("media-playlist-shuffle"),
                                     style()->standardIcon(QStyle::SP_BrowserReload)),
                    QStringLiteral("Random"), this);
    mpd_random_action_->setObjectName(QStringLiteral("action-mpd-random"));
    mpd_random_action_->setCheckable(true);
    mpd_random_button_ = add_action_button(mpd_random_action_, QStringLiteral("bench-mpd-random"));
    connect(mpd_random_action_, &QAction::triggered, mpd_controller_,
            &quick::MpdProbeController::setRandomEnabled);

    mpd_single_action_ = new QAction(QStringLiteral("Cycle MPD single mode"), this);
    mpd_single_action_->setObjectName(QStringLiteral("action-mpd-single"));
    mpd_single_action_->setCheckable(true);
    mpd_single_button_ = new QToolButton(statusBar());
    mpd_single_button_->setObjectName(QStringLiteral("bench-mpd-single"));
    mpd_single_button_->setAutoRaise(true);
    mpd_single_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mpd_single_button_->setDefaultAction(mpd_single_action_);
    statusBar()->addPermanentWidget(mpd_single_button_);
    connect(mpd_single_action_, &QAction::triggered, this, [this] {
        const auto mode = mpd_controller_->singleMode();
        mpd_controller_->setSingleMode(mode < 0 || mode >= 2 ? 0 : mode + 1);
    });

    mpd_consume_action_ = new QAction(QStringLiteral("Cycle MPD consume mode"), this);
    mpd_consume_action_->setObjectName(QStringLiteral("action-mpd-consume"));
    mpd_consume_action_->setCheckable(true);
    mpd_consume_button_ = new QToolButton(statusBar());
    mpd_consume_button_->setObjectName(QStringLiteral("bench-mpd-consume"));
    mpd_consume_button_->setAutoRaise(true);
    mpd_consume_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mpd_consume_button_->setDefaultAction(mpd_consume_action_);
    statusBar()->addPermanentWidget(mpd_consume_button_);
    connect(mpd_consume_action_, &QAction::triggered, this, [this] {
        const auto mode = mpd_controller_->consumeMode();
        mpd_controller_->setConsumeMode(mode < 0 || mode >= 2 ? 0 : mode + 1);
    });

    mpd_load_local_action_ = new QAction(QIcon::fromTheme(QStringLiteral("folder-open")),
                                         QStringLiteral("Load as local files"), this);
    mpd_load_local_action_->setObjectName(QStringLiteral("action-mpd-load-local"));
    connect(mpd_load_local_action_, &QAction::triggered, this, [this] {
        loadMpdUrisAsLocalFiles(
            selectedMpdViewUris(qobject_cast<QTableView*>(tabs_->currentWidget())));
    });
    // ADR-0180: file-operation sugar on mapped selections — load as local
    // files, then open the dialog on the created tab.
    mpd_edit_tags_action_ = new QAction(QStringLiteral("Edit tags…"), this);
    mpd_edit_tags_action_->setObjectName(QStringLiteral("action-mpd-edit-tags"));
    connect(mpd_edit_tags_action_, &QAction::triggered, this, [this] {
        materializeMpdSelectionForDialog(
            selectedMpdViewUris(qobject_cast<QTableView*>(tabs_->currentWidget())),
            MaterializedDialog::edit_tags);
    });
    mpd_replaygain_action_ = new QAction(QStringLiteral("ReplayGain…"), this);
    mpd_replaygain_action_->setObjectName(QStringLiteral("action-mpd-replaygain"));
    connect(mpd_replaygain_action_, &QAction::triggered, this, [this] {
        materializeMpdSelectionForDialog(
            selectedMpdViewUris(qobject_cast<QTableView*>(tabs_->currentWidget())),
            MaterializedDialog::replay_gain);
    });
    mpd_convert_action_ = new QAction(QStringLiteral("Convert files…"), this);
    mpd_convert_action_->setObjectName(QStringLiteral("action-mpd-convert"));
    connect(mpd_convert_action_, &QAction::triggered, this, [this] {
        materializeMpdSelectionForDialog(
            selectedMpdViewUris(qobject_cast<QTableView*>(tabs_->currentWidget())),
            MaterializedDialog::convert);
    });
    mpd_go_to_artist_action_ = new QAction(QStringLiteral("Go to artist"), this);
    mpd_go_to_artist_action_->setObjectName(QStringLiteral("action-mpd-go-to-artist"));
    connect(mpd_go_to_artist_action_, &QAction::triggered, this, [this] {
        auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
        const auto index = view ? view->currentIndex() : QModelIndex{};
        if (index.isValid()) {
            goToMpdLibraryEntry(index.siblingAtColumn(0)
                                    .data(static_cast<int>(ui::track_album_artist_role))
                                    .toString(),
                                {});
        }
    });
    mpd_go_to_album_action_ = new QAction(QStringLiteral("Go to album"), this);
    mpd_go_to_album_action_->setObjectName(QStringLiteral("action-mpd-go-to-album"));
    connect(mpd_go_to_album_action_, &QAction::triggered, this, [this] {
        auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
        const auto index = view ? view->currentIndex() : QModelIndex{};
        if (index.isValid()) {
            goToMpdLibraryEntry(
                index.siblingAtColumn(0)
                    .data(static_cast<int>(ui::track_album_artist_role))
                    .toString(),
                index.siblingAtColumn(ui::track_album_column).data(Qt::DisplayRole).toString());
        }
    });
    mpd_crop_selection_action_ = new QAction(QStringLiteral("Crop list to selection"), this);
    mpd_crop_selection_action_->setObjectName(QStringLiteral("action-mpd-crop-selection"));
    connect(mpd_crop_selection_action_, &QAction::triggered, this, [this] {
        if (!isMpdContext())
            return;
        auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
        if (!view || !view->selectionModel())
            return;
        QVariantList rows;
        for (const auto& index : view->selectionModel()->selectedRows())
            rows.push_back(index.row());
        mpd_controller_->cropListToItems(view->property("bench-mpd-playlist-name").toString(), rows,
                                         view->model()->rowCount());
    });

    mpd_priority_menu_ = new QMenu(QStringLiteral("Priority"), this);
    mpd_priority_menu_->setObjectName(QStringLiteral("bench-mpd-priority-menu"));
    mpd_priority_menu_->menuAction()->setObjectName(QStringLiteral("action-mpd-queue-priority"));
    auto* priority_group = new QActionGroup(mpd_priority_menu_);
    priority_group->setExclusive(true);
    const std::array priority_choices{
        std::pair{QStringLiteral("Normal"), 0},    std::pair{QStringLiteral("Low"), 64},
        std::pair{QStringLiteral("Medium"), 128},  std::pair{QStringLiteral("High"), 192},
        std::pair{QStringLiteral("Maximum"), 255},
    };
    for (const auto& [label, priority] : priority_choices) {
        auto* action =
            mpd_priority_menu_->addAction(QStringLiteral("%1 (%2)").arg(label).arg(priority));
        action->setObjectName(QStringLiteral("action-mpd-queue-priority-%1").arg(priority));
        action->setCheckable(true);
        action->setData(priority);
        priority_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, priority] {
            auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
            if (!view || !view->selectionModel())
                return;
            auto* model = qobject_cast<quick::MpdQueueModel*>(view->model());
            if (!model)
                return;
            QList<QPair<int, QString>> selection;
            for (const auto& index : view->selectionModel()->selectedRows())
                if (const auto uri = model->uriAt(index.row()))
                    selection.push_back({index.row(), QString::fromStdString(*uri)});
            const auto* tab = currentMpdPlaylistTab();
            mpd_controller_->setListPriority(tab ? tab->name : QString{}, selection, priority);
        });
    }

    mpd_rate_menu_ = new QMenu(QStringLiteral("Rate"), this);
    mpd_rate_menu_->setObjectName(QStringLiteral("bench-mpd-rate-menu"));
    mpd_rate_menu_->menuAction()->setObjectName(QStringLiteral("action-mpd-queue-rate"));
    auto* rate_group = new QActionGroup(mpd_rate_menu_);
    rate_group->setExclusive(true);
    for (unsigned rating = 0U; rating <= 10U; rating += 2U) {
        QAction* action = nullptr;
        if (rating == 0U) {
            action = mpd_rate_menu_->addAction(ui::ratingMenuLabel(rating));
            action->setCheckable(true);
        } else {
            auto* stars = new ui::RatingMenuAction(rating, mpd_rate_menu_);
            mpd_rate_menu_->addAction(stars);
            action = stars;
        }
        action->setObjectName(QStringLiteral("action-mpd-queue-rate-%1").arg(rating));
        action->setData(rating);
        rate_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, rating] {
            mpd_controller_->setTracksRating(
                selectedMpdViewTracks(qobject_cast<QTableView*>(tabs_->currentWidget())),
                static_cast<int>(rating),
                currentMpdPlaylistTab() ? currentMpdPlaylistTab()->name : QString{});
        });
    }

    mpd_replaygain_button_ = new QToolButton(statusBar());
    mpd_replaygain_button_->setObjectName(QStringLiteral("bench-mpd-replaygain"));
    mpd_replaygain_button_->setIcon(QIcon::fromTheme(
        QStringLiteral("view-media-equalizer"), style()->standardIcon(QStyle::SP_MediaVolume)));
    mpd_replaygain_button_->setText(QStringLiteral("RG: —"));
    mpd_replaygain_button_->setAccessibleName(QStringLiteral("MPD ReplayGain mode"));
    mpd_replaygain_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mpd_replaygain_button_->setAutoRaise(true);
    mpd_replaygain_button_->setPopupMode(QToolButton::InstantPopup);
    auto* replaygain_menu = new QMenu(mpd_replaygain_button_);
    replaygain_menu->setObjectName(QStringLiteral("bench-mpd-replaygain-menu"));
    mpd_replaygain_group_ = new QActionGroup(replaygain_menu);
    mpd_replaygain_group_->setExclusive(true);
    const std::array modes{
        std::pair{QStringLiteral("Off"), QStringLiteral("off")},
        std::pair{QStringLiteral("Track"), QStringLiteral("track")},
        std::pair{QStringLiteral("Album"), QStringLiteral("album")},
        std::pair{QStringLiteral("Automatic"), QStringLiteral("auto")},
    };
    for (const auto& [label, value] : modes) {
        auto* action = replaygain_menu->addAction(label);
        action->setObjectName(QStringLiteral("action-mpd-replaygain-%1").arg(value));
        action->setCheckable(true);
        action->setData(value);
        mpd_replaygain_group_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, value] { mpd_controller_->setReplayGainMode(value); });
    }
    mpd_replaygain_button_->setMenu(replaygain_menu);
    statusBar()->addPermanentWidget(mpd_replaygain_button_);
    refreshMpdStatusControls();
}

void BenchMainWindow::refreshMpdStatusControls() {
    if (mpd_repeat_action_ == nullptr) {
        return;
    }
    const auto visible = isMpdContext();
    const auto connected = mpd_controller_->connected();
    const auto command_ready = connected && !mpd_controller_->commandBusy();
    mpd_status_separator_->setVisible(visible);
    mpd_repeat_button_->setVisible(visible);
    mpd_repeat_action_->setVisible(visible);
    mpd_repeat_action_->setEnabled(connected);
    mpd_repeat_action_->setChecked(mpd_controller_->repeatEnabled());
    mpd_repeat_action_->setToolTip(
        QStringLiteral("Repeat: %1")
            .arg(mpd_controller_->repeatEnabled() ? QStringLiteral("On") : QStringLiteral("Off")));
    mpd_random_button_->setVisible(visible);
    mpd_random_action_->setVisible(visible);
    mpd_random_action_->setEnabled(connected);
    mpd_random_action_->setChecked(mpd_controller_->randomEnabled());
    mpd_random_action_->setToolTip(
        QStringLiteral("Random: %1")
            .arg(mpd_controller_->randomEnabled() ? QStringLiteral("On") : QStringLiteral("Off")));

    mpd_single_button_->setVisible(visible);
    mpd_single_action_->setEnabled(command_ready);
    mpd_single_action_->setChecked(mpd_controller_->singleMode() > 0);
    mpd_single_action_->setText(mpd_controller_->singleMode() == 2 ? QStringLiteral("1×")
                                                                   : QStringLiteral("1"));
    mpd_single_action_->setToolTip(
        QStringLiteral("Single: %1")
            .arg(mpd_controller_->singleMode() == 2   ? QStringLiteral("One-shot")
                 : mpd_controller_->singleMode() == 1 ? QStringLiteral("On")
                                                      : QStringLiteral("Off")));

    mpd_consume_button_->setVisible(visible);
    mpd_consume_action_->setEnabled(command_ready);
    mpd_consume_action_->setChecked(mpd_controller_->consumeMode() > 0);
    mpd_consume_action_->setText(mpd_controller_->consumeMode() == 2 ? QStringLiteral("C×")
                                                                     : QStringLiteral("C"));
    mpd_consume_action_->setToolTip(
        QStringLiteral("Consume: %1")
            .arg(mpd_controller_->consumeMode() == 2   ? QStringLiteral("One-shot")
                 : mpd_controller_->consumeMode() == 1 ? QStringLiteral("On")
                                                       : QStringLiteral("Off")));

    const auto replaygain_visible = visible && mpd_controller_->supportsReplayGain();
    mpd_replaygain_button_->setVisible(replaygain_visible);
    mpd_replaygain_button_->setEnabled(command_ready);
    const auto replaygain = mpd_controller_->replayGainMode();
    QString replaygain_label = QStringLiteral("Unavailable");
    for (auto* action : mpd_replaygain_group_->actions()) {
        const auto selected = action->data().toString() == replaygain;
        action->setChecked(selected);
        if (selected) {
            replaygain_label = action->text();
        }
    }
    mpd_replaygain_button_->setText(QStringLiteral("RG: %1").arg(replaygain_label));
    mpd_replaygain_button_->setToolTip(QStringLiteral("ReplayGain mode: %1").arg(replaygain_label));
    mpd_replaygain_button_->setAccessibleDescription(
        QStringLiteral("Current ReplayGain mode is %1; activate to choose another mode")
            .arg(replaygain_label));
    if (visible && device_menu_ != nullptr && !device_menu_->isVisible()) {
        rebuildDeviceMenu();
    }

    if (mpd_search_field_ != nullptr) {
        mpd_search_field_->setVisible(visible);
        mpd_search_field_->setEnabled(connected || !mpd_search_field_->text().isEmpty());
        if (visible && connected && !mpd_search_field_->text().trimmed().isEmpty() &&
            mpd_search_field_->text().trimmed() != mpd_controller_->lastSearchQuery() &&
            !mpd_search_timer_->isActive()) {
            mpd_search_timer_->start();
        }
        mpd_search_field_->setToolTip(connected ? QStringLiteral("Search the MPD server library")
                                                : QStringLiteral("Connect to search MPD"));
    }
    if (mpd_search_tree_model_ != nullptr) {
        const auto query = mpd_search_field_->text().trimmed();
        mpd_search_tree_model_->setMore(connected && mpd_controller_->hasMoreSearchResults() &&
                                        query == mpd_controller_->lastSearchQuery() &&
                                        !mpd_search_timer_->isActive());
        if (!query.isEmpty() && query == mpd_controller_->lastSearchQuery() &&
            !mpd_search_timer_->isActive()) {
            mpd_search_status_->setText(mpd_controller_->libraryStatus());
        }
    }
}

void BenchMainWindow::buildMpdWorkspace() {
    mpd_controller_ = new quick::MpdProbeController(this);
    server_library_model_ = new ui::ServerLibraryTreeModel(this);
    server_library_model_->setArtworkEnabled(true);

    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(mpd_controller_->queueModel());
    Q_ASSERT(queue_model != nullptr);
    queue_model->setArtworkEnabled(true);

    auto* view = new ui::QueueTableView(tabs_);
    mpd_queue_view_ = view;
    view->setObjectName(QStringLiteral("bench-mpd-queue"));
    view->setProperty("bench-mpd-queue", true);
    view->setAccessibleName(QStringLiteral("MPD Queue"));
    view->setModel(queue_model);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
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
    view->setDragDropMode(QAbstractItemView::InternalMove);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setActivateCallback([controller = mpd_controller_](const QModelIndex& index) {
        if (index.isValid()) {
            controller->playListContext({}, index.row());
        }
    });
    view->setReorderCallback(
        [controller = mpd_controller_](const QVariantList& rows, const int insertion_row) {
            controller->moveQueueItems(rows, insertion_row);
        });
    view->setExternalDropCallback([this](QAbstractItemView* source, const QVariantList&,
                                         const int insertion_row, const Qt::DropAction) {
        if (source == mpd_search_view_ && source->selectionModel() != nullptr) {
            const auto indexes = source->selectionModel()->selectedRows(0);
            const auto entry = std::ranges::find_if(indexes, MpdLibrarySearchModel::actionable);
            if (entry == indexes.end())
                return false;
            activateMpdSearchResult(*entry, 0, insertion_row);
            return true;
        }
        if (mpdPlaylistTabForWidget(source) != nullptr) {
            const auto playlist_uris = selectedMpdViewUris(qobject_cast<QTableView*>(source));
            if (playlist_uris.isEmpty()) {
                return false;
            }
            mpd_controller_->addUrisAt(playlist_uris, insertion_row);
            return true;
        }
        if (source != server_library_view_ || source->selectionModel() == nullptr) {
            return false;
        }
        QStringList uris;
        QSet<QString> seen;
        const auto indexes = source->selectionModel()->selectedRows(0);
        for (const auto& index : indexes) {
            for (const auto& track : server_library_model_->tracks(index)) {
                const auto uri = displayText(track.uri);
                if (!seen.contains(uri)) {
                    seen.insert(uri);
                    uris.push_back(uri);
                }
            }
        }
        if (uris.isEmpty()) {
            if (indexes.size() == 1 && server_library_model_->canFetchMore(indexes.front())) {
                pending_mpd_library_index_ = indexes.front();
                pending_mpd_library_action_ = MpdLibraryAction::insert;
                pending_mpd_library_insertion_row_ = insertion_row;
                server_library_view_->expand(indexes.front());
                server_library_model_->fetchMore(indexes.front());
                return true;
            }
            statusBar()->showMessage(QStringLiteral("This library entry contains no tracks"),
                                     3'000);
            return false;
        }
        mpd_controller_->addUrisAt(uris, insertion_row);
        return true;
    });
    connect(view, &QTableView::doubleClicked, this,
            [controller = mpd_controller_](const QModelIndex& index) {
                if (!index.isValid()) {
                    return;
                }
                controller->playListContext({}, index.row());
            });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::modelReset, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::rowsInserted, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::rowsRemoved, this,
            [this] { refreshSelectionStatus(); });

    mpd_view_layout_ = defaultTrackViewLayout();
    applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, mpd_view_layout_);
    connect(view->horizontalHeader(), &QHeaderView::sectionMoved, this,
            [this](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                mpd_view_layout_ = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
                mpd_view_layout_persistence_protected_ = false;
                preserved_mpd_view_layout_.clear();
                schedulePersist();
                refreshTrackViewActions();
            });
    connect(view->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                mpd_view_layout_ = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
                mpd_view_layout_persistence_protected_ = false;
                preserved_mpd_view_layout_.clear();
                schedulePersist();
            });
    const auto queue_index = tabs_->addTab(view, QIcon::fromTheme(QStringLiteral("network-server")),
                                           QStringLiteral("MPD Queue"));
    tabs_->setTabToolTip(queue_index,
                         QStringLiteral("Authoritative queue on the connected MPD server"));
    if (auto* close = tabs_->tabBar()->tabButton(queue_index, QTabBar::RightSide)) {
        close->hide();
    }
    server_library_view_ = new ui::ServerLibraryTreeView(source_stack_);
    server_library_view_->setObjectName(QStringLiteral("bench-mpd-library"));
    server_library_view_->setAccessibleName(QStringLiteral("MPD server library"));
    server_library_view_->setModel(server_library_model_);
    server_library_view_->setHeaderHidden(true);
    server_library_view_->setExpandsOnDoubleClick(false);
    server_library_view_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    server_library_view_->setDragEnabled(true);
    server_library_view_->setDragDropMode(QAbstractItemView::DragOnly);
    server_library_view_->setDefaultDropAction(Qt::CopyAction);
    const std::array library_action_icons{
        QIcon::fromTheme(QStringLiteral("list-add"),
                         style()->standardIcon(QStyle::SP_DialogOpenButton)),
        QIcon::fromTheme(QStringLiteral("go-next"), style()->standardIcon(QStyle::SP_ArrowRight)),
        QIcon::fromTheme(QStringLiteral("media-playback-start"),
                         style()->standardIcon(QStyle::SP_MediaPlay)),
    };
    server_library_view_->setItemDelegate(
        new ui::ServerLibraryTreeDelegate(server_library_view_, library_action_icons));
    server_library_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(server_library_view_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showMpdLibraryContextMenu);
    server_library_view_->setActionCallback([this](const QModelIndex& index, const int action) {
        activateMpdLibraryAction(index, action);
    });
    source_stack_->addWidget(server_library_view_);
    buildMpdSearch();

    connect(server_library_model_, &ui::ServerLibraryTreeModel::rootRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryRoot);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::branchRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryBranch);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    connect(queue_model, &quick::MpdQueueModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    connect(mpd_controller_, &quick::MpdProbeController::newestRootOrderLoaded, this,
            [this](const QStringList& values, const QString& error) {
                if (!error.isEmpty()) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not load the newest ordering: %1").arg(error), 5'000);
                    return;
                }
                if (library_order_ != nullptr && library_order_->currentIndex() == 1) {
                    server_library_model_->setRootOrdering(values);
                }
            });
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryRootLoaded,
            server_library_model_, &ui::ServerLibraryTreeModel::acceptRoot);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryRootLoaded, this,
            [this](quint64, const QString&, const QStringList&, const QString& error) {
                if (error.isEmpty())
                    applyLibraryOrder(false);
            });
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryAlbumCountsLoaded,
            server_library_model_, &ui::ServerLibraryTreeModel::acceptAlbumCounts);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryBranchLoaded,
            server_library_model_, &ui::ServerLibraryTreeModel::acceptBranch);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryArtworkLoaded, this,
            [this, queue_model](const quint64 token, const QByteArray& bytes) {
                auto* watcher = new QFutureWatcher<QImage>(this);
                connect(watcher, &QFutureWatcher<QImage>::finished, this,
                        [this, watcher, token, queue_model] {
                            const auto image = watcher->result();
                            watcher->deleteLater();
                            server_library_model_->acceptArtwork(token, image);
                            mpd_search_model_->acceptArtwork(token, image);
                            queue_model->acceptArtwork(token, image);
                            for (const auto& playlist_tab : mpd_playlist_tabs_) {
                                playlist_tab->model->acceptArtwork(token, image);
                            }
                        });
                watcher->setFuture(QtConcurrent::run([bytes] {
                    auto image = QImage::fromData(bytes);
                    if (!image.isNull()) {
                        image =
                            image.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    return image;
                }));
            });
    // Browse reloads invalidate artwork in the preserved search results too.
    connect(server_library_model_, &QAbstractItemModel::modelReset, mpd_search_model_,
            &quick::MpdSearchResultModel::refreshArtwork);
    connect(mpd_controller_, &quick::MpdProbeController::serverDatabaseChanged,
            server_library_model_, &ui::ServerLibraryTreeModel::reload);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::browseError, this,
            [this](const QString& error) {
                pending_mpd_library_action_.reset();
                pending_mpd_library_index_ = QPersistentModelIndex{};
                pending_mpd_library_insertion_row_ = -1;
                pending_library_apply_ = {};
                pending_library_selection_ = QPersistentModelIndex{};
                server_library_view_->cancelPendingExpansions();
                statusBar()->showMessage(
                    QStringLiteral("Could not browse the MPD library: %1").arg(error), 5'000);
            });
    connect(server_library_model_, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, const int, const int) {
                QTimer::singleShot(0, this, [this] {
                    server_library_view_->completePendingExpansions();
                    completePendingMpdLibraryAction();
                });
            });
    connect(mpd_controller_, &quick::MpdProbeController::notificationRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5'000); });
    connect(mpd_controller_, &quick::MpdProbeController::searchFinished, this,
            &BenchMainWindow::finishMpdSearch);
    connect(mpd_controller_, &quick::MpdProbeController::stateChanged, this, [this] {
        const auto connected = mpd_controller_->connected();
        // The flag must flip before issuing commands: browseStoredPlaylists()
        // re-emits stateChanged synchronously and would otherwise re-enter
        // this connect-transition branch without bound.
        const auto was_connected = mpd_was_connected_;
        mpd_was_connected_ = connected;
        if (connected && !was_connected) {
            server_library_model_->reload();
            if (mpd_controller_->supportsCommand(QStringLiteral("listplaylists"))) {
                mpd_controller_->browseStoredPlaylists();
                mpd_controller_->browseScratchLists();
            }
        }
        if (!connected && was_connected) {
            acceptMpdStoredPlaylistNames({});
        }
        // The Melody endpoint belongs to the MPD session, not to whichever
        // authority tab happens to be visible when auto-connect completes.
        refreshMelodyEndpoint();
        refreshActiveContext();
        refreshTransport();
        refreshSelectionStatus();
        refreshMpdStatusControls();
        refreshMpdPlaylistContextMarkers();
    });
    auto* output_model = mpd_controller_->outputModel();
    const auto refresh_outputs = [this] {
        if (isMpdContext() && device_menu_ != nullptr) {
            rebuildDeviceMenu();
        }
    };
    connect(output_model, &QAbstractItemModel::modelReset, this, refresh_outputs);
    connect(output_model, &QAbstractItemModel::rowsInserted, this,
            [refresh_outputs](const QModelIndex&, const int, const int) { refresh_outputs(); });
    connect(output_model, &QAbstractItemModel::rowsRemoved, this,
            [refresh_outputs](const QModelIndex&, const int, const int) { refresh_outputs(); });
    connect(output_model, &QAbstractItemModel::dataChanged, this,
            [refresh_outputs](const QModelIndex&, const QModelIndex&, const QList<int>&) {
                refresh_outputs();
            });
}

// The tag-organized library only reaches MPD's path-scoped update through
// its files: the common directory prefix of a node's track URIs is the
// folder that provably contains everything the node showed.
namespace {
[[nodiscard]] QString commonMpdDirectory(const QStringList& uris) {
    QStringList common;
    bool first = true;
    for (const auto& uri : uris) {
        const auto slash = uri.lastIndexOf(QLatin1Char('/'));
        auto components = slash <= 0 ? QStringList{}
                                     : uri.left(slash).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (first) {
            common = std::move(components);
            first = false;
            continue;
        }
        qsizetype shared = 0;
        while (shared < common.size() && shared < components.size() &&
               common[shared] == components[shared]) {
            ++shared;
        }
        common = common.mid(0, shared);
        if (common.isEmpty()) {
            break;
        }
    }
    return common.join(QLatin1Char('/'));
}
} // namespace

void BenchMainWindow::activateMpdLibraryAction(const QModelIndex& index, const int action) {
    const auto local_actions = action == static_cast<int>(MpdLibraryAction::load_local) ||
                               action == static_cast<int>(MpdLibraryAction::update_directory) ||
                               action == static_cast<int>(MpdLibraryAction::edit_tags) ||
                               action == static_cast<int>(MpdLibraryAction::replay_gain) ||
                               action == static_cast<int>(MpdLibraryAction::convert);
    if (!index.isValid() ||
        (!local_actions && (action < static_cast<int>(MpdLibraryAction::append) ||
                            action > static_cast<int>(MpdLibraryAction::replace)))) {
        return;
    }
    server_library_view_->setCurrentIndex(index);
    if (!mpd_controller_->connected()) {
        statusBar()->showMessage(QStringLiteral("Connect to MPD to use the server library"), 3'000);
        return;
    }
    if (server_library_model_->canFetchMore(index)) {
        pending_mpd_library_index_ = index;
        pending_mpd_library_action_ = static_cast<MpdLibraryAction>(action);
        server_library_view_->expand(index);
        server_library_model_->fetchMore(index);
        return;
    }

    const auto tracks = server_library_model_->tracks(index);
    if (tracks.empty()) {
        statusBar()->showMessage(QStringLiteral("This library entry contains no tracks"), 3'000);
        return;
    }
    QStringList uris;
    uris.reserve(static_cast<qsizetype>(tracks.size()));
    for (const auto& track : tracks) {
        uris.push_back(displayText(track.uri));
    }
    const auto requested = static_cast<MpdLibraryAction>(action);
    if (requested == MpdLibraryAction::load_local) {
        loadMpdUrisAsLocalFiles(uris);
        return;
    }
    if (requested == MpdLibraryAction::edit_tags) {
        materializeMpdSelectionForDialog(uris, MaterializedDialog::edit_tags);
        return;
    }
    if (requested == MpdLibraryAction::replay_gain) {
        materializeMpdSelectionForDialog(uris, MaterializedDialog::replay_gain);
        return;
    }
    if (requested == MpdLibraryAction::convert) {
        materializeMpdSelectionForDialog(uris, MaterializedDialog::convert);
        return;
    }
    if (requested == MpdLibraryAction::update_directory) {
        const auto directory = commonMpdDirectory(uris);
        mpd_controller_->updateDatabase(directory);
        statusBar()->showMessage(
            directory.isEmpty() ? QStringLiteral("Requested an MPD update of the whole database")
                                : QStringLiteral("Requested an MPD update of %1").arg(directory),
            5'000);
        return;
    }
    // ADR-0190: library actions land in the tab the user is looking at.
    auto mode = MpdSendMode::append;
    if (requested == MpdLibraryAction::replace) {
        mode = MpdSendMode::replace;
    } else if (requested == MpdLibraryAction::next) {
        mode = MpdSendMode::insert_next;
    }
    sendTracksToMpdTab(
        visibleMpdTabTarget().value_or(MpdTabTarget{.kind = MpdTabTarget::Kind::queue,
                                                    .label = QStringLiteral("MPD Queue"),
                                                    .playlist = {}}),
        tracks, mode);
}

// The menu's direct actions run through the same lazy-fetch path as the
// delegate's inline buttons, so an unexpanded folder still answers.
void BenchMainWindow::sendMpdLibraryEntryToTab(const QModelIndex& index, const MpdSendMode mode) {
    auto action = MpdLibraryAction::append;
    switch (mode) {
    case MpdSendMode::append:
        action = MpdLibraryAction::append;
        break;
    case MpdSendMode::insert_next:
        action = MpdLibraryAction::next;
        break;
    case MpdSendMode::replace:
        action = MpdLibraryAction::replace;
        break;
    }
    activateMpdLibraryAction(index, static_cast<int>(action));
}

// A library branch is fetched on demand, so a selection can be acted on
// before its tracks exist here — dragging an unexpanded artist is the
// ordinary case. The branch is requested and the work finishes a round trip
// later, when the rows land.
bool BenchMainWindow::resolveLibraryTracks(const QModelIndexList& indexes,
                                           std::function<void(std::vector<mpd::Track>)> apply) {
    std::vector<mpd::Track> tracks;
    QSet<QString> seen;
    for (const auto& index : indexes) {
        for (const auto& track : server_library_model_->tracks(index)) {
            const auto uri = displayText(track.uri);
            if (!seen.contains(uri)) {
                seen.insert(uri);
                tracks.push_back(track);
            }
        }
    }
    if (!tracks.empty()) {
        apply(std::move(tracks));
        return true;
    }
    if (indexes.size() == 1 && server_library_model_->canFetchMore(indexes.front())) {
        pending_library_selection_ = QPersistentModelIndex{indexes.front()};
        pending_library_apply_ = std::move(apply);
        server_library_view_->expand(indexes.front());
        server_library_model_->fetchMore(indexes.front());
        return true;
    }
    return false;
}

void BenchMainWindow::completePendingMpdLibraryAction() {
    if (pending_library_apply_) {
        if (!pending_library_selection_.isValid()) {
            pending_library_apply_ = {};
        } else if (auto tracks =
                       server_library_model_->tracks(QModelIndex{pending_library_selection_});
                   !tracks.empty()) {
            auto apply = std::exchange(pending_library_apply_, {});
            pending_library_selection_ = QPersistentModelIndex{};
            apply(std::move(tracks));
        }
    }
    if (!pending_mpd_library_action_) {
        return;
    }
    if (!pending_mpd_library_index_.isValid()) {
        pending_mpd_library_action_.reset();
        return;
    }
    const auto index = QModelIndex{pending_mpd_library_index_};
    const auto requested = *pending_mpd_library_action_;
    pending_mpd_library_action_.reset();
    pending_mpd_library_index_ = QPersistentModelIndex{};
    if (requested == MpdLibraryAction::insert) {
        const auto tracks = server_library_model_->tracks(index);
        QStringList uris;
        uris.reserve(static_cast<qsizetype>(tracks.size()));
        for (const auto& track : tracks) {
            uris.push_back(displayText(track.uri));
        }
        if (!uris.isEmpty() && pending_mpd_library_insertion_row_ >= 0) {
            mpd_controller_->addUrisAt(uris, pending_mpd_library_insertion_row_);
        }
        pending_mpd_library_insertion_row_ = -1;
        return;
    }
    pending_mpd_library_insertion_row_ = -1;
    activateMpdLibraryAction(index, static_cast<int>(requested));
}

void BenchMainWindow::showMpdLibraryContextMenu(const QPoint& position) {
    if (mpd_library_context_menu_ == nullptr) {
        return;
    }
    const auto index = server_library_view_->indexAt(position).siblingAtColumn(0);
    if (!index.isValid()) {
        return;
    }
    server_library_view_->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const auto target = QPersistentModelIndex{index};
    const auto command_ready = mpd_controller_->connected() && !mpd_controller_->commandBusy();
    mpd_library_context_menu_->clear();
    // ADR-0190: the two direct actions act on the tab you are looking at —
    // every MPD-side tab is a list, and the visible one is the obvious
    // destination. "Send to tab" below aims anywhere else.
    const auto visible = visibleMpdTabTarget();
    const auto visible_label = visible ? visible->label : QStringLiteral("MPD Queue");
    const std::array actions{
        std::tuple{QStringLiteral("Add to %1").arg(visible_label), QStringLiteral("list-add"),
                   MpdSendMode::append},
        std::tuple{QStringLiteral("Replace %1").arg(visible_label),
                   QStringLiteral("media-playback-start"), MpdSendMode::replace},
    };
    for (const auto& [label, icon, mode] : actions) {
        auto* command = mpd_library_context_menu_->addAction(QIcon::fromTheme(icon), label);
        command->setObjectName(
            QStringLiteral("action-mpd-library-%1").arg(mode == MpdSendMode::append ? 0 : 2));
        command->setEnabled(command_ready);
        connect(command, &QAction::triggered, this, [this, target, mode] {
            if (target.isValid()) {
                sendMpdLibraryEntryToTab(target, mode);
            }
        });
    }
    for (bool prepend : {true, false}) {
        auto* request = mpd_library_context_menu_->addAction(
            prepend ? QStringLiteral("Queue next") : QStringLiteral("Queue at end"));
        request->setEnabled(command_ready &&
                            mpd_controller_->supportsCommand(QStringLiteral("melody_upnext")));
        connect(
            request, &QAction::triggered, this,
            [this, target, prepend, profile = mpd_controller_->profileId()] {
                if (!target.isValid())
                    return;
                resolveLibraryTracks({QModelIndex{target}}, [this, prepend, profile](
                                                                std::vector<mpd::Track> tracks) {
                    if (profile != mpd_controller_->profileId() || !mpd_controller_->connected())
                        return;
                    mpd::RequestQueueCommand command;
                    command.operation = prepend ? mpd::RequestQueueOperation::prepend
                                                : mpd::RequestQueueOperation::append;
                    for (const auto& track : tracks)
                        command.uris.push_back(track.uri);
                    if (!command.uris.empty())
                        mpd_controller_->editRequestQueue(std::move(command));
                });
            });
    }
    addSendToTabMenu(
        mpd_library_context_menu_,
        [this, target] {
            return target.isValid() ? server_library_model_->tracks(QModelIndex{target})
                                    : std::vector<mpd::Track>{};
        },
        [this, target](const std::function<void(std::vector<mpd::Track>)>& apply) {
            return target.isValid() && resolveLibraryTracks({QModelIndex{target}}, apply);
        });
    mpd_library_context_menu_->addSeparator();
    auto* update_directory =
        mpd_library_context_menu_->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                             QStringLiteral("Update this folder in MPD"));
    update_directory->setObjectName(QStringLiteral("action-mpd-library-update"));
    update_directory->setEnabled(command_ready);
    connect(update_directory, &QAction::triggered, this, [this, target] {
        if (target.isValid()) {
            activateMpdLibraryAction(target, static_cast<int>(MpdLibraryAction::update_directory));
        }
    });
    auto* load_local = mpd_library_context_menu_->addAction(
        QIcon::fromTheme(QStringLiteral("folder-open")), QStringLiteral("Load as local files"));
    load_local->setObjectName(QStringLiteral("action-mpd-library-load-local"));
    load_local->setEnabled(command_ready);
    connect(load_local, &QAction::triggered, this, [this, target] {
        if (target.isValid()) {
            activateMpdLibraryAction(target, static_cast<int>(MpdLibraryAction::load_local));
        }
    });
    const auto mapped_ready = command_ready && !effectiveMpdMusicRoot().isEmpty();
    const std::array sugar{
        std::tuple{QStringLiteral("Edit tags…"), QStringLiteral("edit-tags"),
                   MpdLibraryAction::edit_tags},
        std::tuple{QStringLiteral("ReplayGain…"), QStringLiteral("replaygain"),
                   MpdLibraryAction::replay_gain},
        std::tuple{QStringLiteral("Convert files…"), QStringLiteral("convert"),
                   MpdLibraryAction::convert},
    };
    for (const auto& [label, slug, library_action] : sugar) {
        auto* command = mpd_library_context_menu_->addAction(label);
        command->setObjectName(QStringLiteral("action-mpd-library-%1").arg(slug));
        command->setEnabled(mapped_ready);
        connect(command, &QAction::triggered, this, [this, target, library_action] {
            if (target.isValid()) {
                activateMpdLibraryAction(target, static_cast<int>(library_action));
            }
        });
    }
    if (server_library_model_->hasChildren(index)) {
        mpd_library_context_menu_->addSeparator();
        auto* expand = mpd_library_context_menu_->addAction(server_library_view_->isExpanded(index)
                                                                ? QStringLiteral("Collapse")
                                                                : QStringLiteral("Expand"));
        connect(expand, &QAction::triggered, this, [this, target] {
            if (target.isValid()) {
                server_library_view_->setExpanded(target,
                                                  !server_library_view_->isExpanded(target));
            }
        });
    }
    mpd_library_context_menu_->popup(server_library_view_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::buildMpdSearch() {
    mpd_library_panel_ = new QWidget(source_stack_);
    mpd_library_panel_->setObjectName(QStringLiteral("bench-mpd-library-panel"));
    source_stack_->addWidget(mpd_library_panel_);
    auto* panel_layout = new QVBoxLayout(mpd_library_panel_);
    panel_layout->setContentsMargins(4, 4, 4, 4);
    panel_layout->setSpacing(4);
    // The ADR-0130 sidebar tab bar switches these two full-height pages:
    // the library search/browse surface and the stored-playlist list.
    mpd_source_pages_ = new QStackedWidget(mpd_library_panel_);
    mpd_source_pages_->setObjectName(QStringLiteral("bench-mpd-source-pages"));
    auto* library_page = new QWidget(mpd_source_pages_);
    auto* library_layout = new QVBoxLayout(library_page);
    library_layout->setContentsMargins(0, 0, 0, 0);
    library_layout->setSpacing(4);
    auto* field = new QLineEdit(library_page);
    mpd_search_field_ = field;
    field->setObjectName(QStringLiteral("bench-mpd-search"));
    field->setAccessibleName(QStringLiteral("Search MPD library"));
    field->setClearButtonEnabled(true);
    field->setPlaceholderText(QStringLiteral("Search albums and tracks"));
    library_layout->addWidget(field);
    auto* tools = new QHBoxLayout;
    tools->addWidget(new QLabel(QStringLiteral("Sort:"), library_page));
    tools->addWidget(library_order_);
    tools->addStretch();
    library_layout->addLayout(tools);
    mpd_library_stack_ = new QStackedWidget(library_page);
    source_stack_->removeWidget(server_library_view_);
    mpd_library_stack_->addWidget(server_library_view_);
    library_layout->addWidget(mpd_library_stack_, 1);
    mpd_source_pages_->addWidget(library_page);
    panel_layout->addWidget(mpd_source_pages_, 1);
    buildMpdPlaylists();
    auto* surface = new QWidget(mpd_library_stack_);
    surface->setObjectName(QStringLiteral("bench-mpd-search-surface"));
    mpd_library_stack_->addWidget(surface);
    mpd_search_surface_ = surface;
    auto* layout = new QVBoxLayout(surface);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    mpd_search_model_ = new quick::MpdSearchResultModel(surface);
    mpd_search_model_->setArtworkEnabled(true);
    mpd_search_model_->setAlbumPlaceholder(QIcon::fromTheme(
        QStringLiteral("media-optical-audio"), style()->standardIcon(QStyle::SP_FileIcon)));
    mpd_search_tree_model_ = new MpdLibrarySearchModel(mpd_search_model_, surface);
    auto* results = new ui::ServerLibraryTreeView(surface);
    mpd_search_view_ = results;
    results->setObjectName(QStringLiteral("bench-mpd-search-results"));
    results->setAccessibleName(QStringLiteral("MPD library search results"));
    results->setModel(mpd_search_tree_model_);
    results->setHeaderHidden(true);
    results->setSelectionMode(QAbstractItemView::ExtendedSelection);
    results->setDragEnabled(true);
    results->setDragDropMode(QAbstractItemView::DragOnly);
    results->setDefaultDropAction(Qt::CopyAction);
    results->setExpandsOnDoubleClick(false);
    results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results->setActionsAvailable(
        [](const QModelIndex& index) { return MpdLibrarySearchModel::actionable(index); });
    results->setActionCallback([this](const QModelIndex& index, const int action) {
        activateMpdSearchResult(index, action);
    });
    results->setItemDelegate(new ui::ServerLibraryTreeDelegate(
        results,
        {QIcon::fromTheme(QStringLiteral("list-add"),
                          style()->standardIcon(QStyle::SP_DialogOpenButton)),
         QIcon::fromTheme(QStringLiteral("go-next"), style()->standardIcon(QStyle::SP_ArrowRight)),
         QIcon::fromTheme(QStringLiteral("media-playback-start"),
                          style()->standardIcon(QStyle::SP_MediaPlay))},
        [](const QModelIndex& index) {
            using Kind = quick::MpdSearchResultModel::ResultKind;
            const auto kind = index.data(MpdLibrarySearchModel::KindRole);
            const auto album = kind.isValid() && kind.toInt() == static_cast<int>(Kind::album);
            return ui::ServerLibraryTreeDelegate::Presentation{
                .track = kind.isValid() && kind.toInt() == static_cast<int>(Kind::track),
                .album = album,
                .root = !index.parent().isValid(),
                .secondary =
                    album ? index.data(MpdLibrarySearchModel::ArtistRole).toString() : QString{}};
        }));
    layout->addWidget(results, 1);
    mpd_search_status_ = new QLabel(surface);
    mpd_search_status_->setObjectName(QStringLiteral("bench-mpd-search-status"));
    mpd_search_status_->setWordWrap(true);
    layout->addWidget(mpd_search_status_);
    connect(mpd_search_tree_model_, &MpdLibrarySearchModel::rebuilt, results, [this, results] {
        for (int row = 0; row < mpd_search_tree_model_->rowCount(); ++row) {
            results->expand(mpd_search_tree_model_->index(row, 0));
        }
    });
    connect(results, &QTreeView::expanded, mpd_search_tree_model_,
            &MpdLibrarySearchModel::loadAlbum);
    connect(mpd_search_tree_model_, &MpdLibrarySearchModel::albumRequested, mpd_controller_,
            &quick::MpdProbeController::loadSearchAlbum);
    connect(mpd_controller_, &quick::MpdProbeController::searchAlbumLoaded, mpd_search_tree_model_,
            &MpdLibrarySearchModel::acceptAlbum);
    connect(mpd_search_tree_model_, &MpdLibrarySearchModel::problem, this,
            [this](const QString& error) { statusBar()->showMessage(error, 5'000); });
    connect(results, &QTreeView::activated, this, [this](const QModelIndex& index) {
        if (index.data(MpdLibrarySearchModel::MoreRole).toBool()) {
            mpd_controller_->continueSearch();
        } else if (MpdLibrarySearchModel::actionable(index) &&
                   index.data(MpdLibrarySearchModel::KindRole).toInt() ==
                       static_cast<int>(quick::MpdSearchResultModel::ResultKind::track)) {
            activateMpdSearchResult(index, 0);
        }
    });
    results->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(results, &QTreeView::customContextMenuRequested, this,
            [this, results](const QPoint& point) {
                const QPersistentModelIndex index{results->indexAt(point)};
                if (!index.isValid())
                    return;
                if (!results->selectionModel()->isSelected(index))
                    results->setCurrentIndex(index);
                QMenu menu(results);
                if (MpdLibrarySearchModel::actionable(index)) {
                    const QStringList labels{tr("Add to MPD Queue"), tr("Insert next in MPD Queue"),
                                             tr("Replace MPD Queue and play")};
                    for (int action = 0; action < labels.size(); ++action) {
                        menu.addAction(labels[action], this, [this, index, action] {
                            activateMpdSearchResult(index, action);
                        });
                    }
                }
                if (mpd_search_tree_model_->hasChildren(index)) {
                    menu.addAction(results->isExpanded(index) ? tr("Collapse") : tr("Expand"),
                                   results, [results, index] {
                                       results->setExpanded(index, !results->isExpanded(index));
                                   });
                }
                if (!menu.isEmpty())
                    menu.exec(results->viewport()->mapToGlobal(point));
            });
    mpd_search_timer_ = new QTimer(this);
    mpd_search_timer_->setSingleShot(true);
    mpd_search_timer_->setInterval(200);
    connect(field, &QLineEdit::textChanged, this, [this] {
        mpd_search_timer_->stop();
        pending_mpd_search_commit_.clear();
        mpd_search_model_->replaceTracks({});
        mpd_search_status_->setText(tr("Searching…"));
        updateMpdSearchPresentation();
        if (isMpdContext())
            mpd_search_timer_->start();
    });
    connect(mpd_search_timer_, &QTimer::timeout, this, &BenchMainWindow::previewMpdSearch);
    // ADR-0140: Enter keeps the finished search's hits as a durable
    // query-keyed tab; the live surface stays the transient default.
    connect(field, &QLineEdit::returnPressed, this, &BenchMainWindow::commitMpdSearchTab);
    connect(mpd_search_model_, &quick::MpdSearchResultModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    auto* focus_search = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(focus_search, &QShortcut::activated, this, [this] {
        QLineEdit* target_field = mpd_search_field_;
        if (isMpdContext()) {
            if (mpd_source_tabs_ != nullptr) {
                mpd_source_tabs_->setCurrentIndex(0);
            }
        } else {
            if (local_library_ == nullptr || local_source_tabs_ == nullptr)
                return;
            local_source_tabs_->setCurrentIndex(1);
            target_field =
                local_library_->findChild<QLineEdit*>(QStringLiteral("local-library-search"));
        }
        if (target_field == nullptr)
            return;
        target_field->setFocus();
        target_field->selectAll();
    });
}

void BenchMainWindow::previewMpdSearch() {
    if (!isMpdContext() || mpd_search_field_ == nullptr)
        return;
    mpd_controller_->searchLibrary(mpd_search_field_->text().trimmed());
    mpd_search_status_->setText(mpd_controller_->libraryStatus());
}

void BenchMainWindow::finishMpdSearch(const QString& query, const bool success) {
    if (mpd_search_field_ == nullptr || query != mpd_search_field_->text().trimmed() ||
        query.isEmpty())
        return;
    if (success) {
        std::vector<mpd::Track> tracks;
        if (const auto* source =
                qobject_cast<const quick::MpdQueueModel*>(mpd_controller_->libraryModel())) {
            tracks = source->tracksSnapshot();
        }
        mpd_search_model_->replaceSearchResults(mpd_controller_->libraryAlbumsSnapshot(),
                                                std::move(tracks));
    } else {
        mpd_search_model_->replaceTracks({});
    }
    mpd_search_status_->setText(success ? mpd_controller_->libraryStatus()
                                        : tr("Search did not complete"));
    mpd_search_tree_model_->setMore(success && mpd_controller_->hasMoreSearchResults() &&
                                    query == mpd_controller_->lastSearchQuery());
    if (!pending_mpd_search_commit_.isEmpty() && query == pending_mpd_search_commit_) {
        pending_mpd_search_commit_.clear();
        if (success) {
            commitMpdSearchTab();
        }
    }
}

void BenchMainWindow::activateMpdSearchResult(const QModelIndex& index, const int action,
                                              const int insertion_row) {
    if (!isMpdContext() || mpd_search_timer_->isActive() ||
        !MpdLibrarySearchModel::actionable(index))
        return;
    if (!mpd_search_view_->selectionModel()->isSelected(index))
        mpd_search_view_->setCurrentIndex(index);
    const auto profile = mpd_controller_->profileId();
    mpd_search_tree_model_->resolve(
        mpd_search_view_->selectionModel()->selectedRows(0),
        [this, profile, action, insertion_row](const QStringList& uris, const QString& error) {
            if (!error.isEmpty()) {
                statusBar()->showMessage(error, 5'000);
                return;
            }
            if (uris.isEmpty() || profile != mpd_controller_->profileId() || !isMpdContext())
                return;
            if (insertion_row >= 0)
                mpd_controller_->addUrisAt(uris, insertion_row);
            else if (action == 0)
                mpd_controller_->addUris(uris, false);
            else if (action == 1)
                mpd_controller_->addUris(uris, true);
            else
                mpd_controller_->replaceQueueWithUris(uris);
        });
}

void BenchMainWindow::updateMpdSearchPresentation() {
    if (isMpdContext() && mpd_library_stack_ != nullptr) {
        source_stack_->setCurrentWidget(mpd_library_panel_);
        mpd_library_stack_->setCurrentWidget(mpd_search_field_->text().trimmed().isEmpty()
                                                 ? static_cast<QWidget*>(server_library_view_)
                                                 : mpd_search_surface_);
    }
}

void BenchMainWindow::openMpdConnectionDialog() {
    if (auto* existing = findChild<ui::MpdConnectionDialog*>(); existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    if (mpd_queue_view_ != nullptr) {
        tabs_->setCurrentWidget(mpd_queue_view_);
    }
    auto* dialog = new ui::MpdConnectionDialog(this, mpd_profiles_, mpd_controller_->profileId());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(
        dialog, &ui::MpdConnectionDialog::connectionRequested, this,
        [this](const QString& profile_id, const QString& profile_name, const QString& host,
               const int port, const QString& password, const QString& music_root,
               const bool auto_connect) {
            const auto parsed_id = core::StableId::parse(profile_id.toStdString());
            if (!parsed_id) {
                statusBar()->showMessage(
                    QStringLiteral("Connection profile has an invalid identity"), 5'000);
                return;
            }
            persistence::ConnectionProfile updated{
                .id = *parsed_id,
                .name = utf8Bytes(profile_name),
                .host = utf8Bytes(host),
                .port = static_cast<unsigned>(port),
                .local_music_root =
                    music_root.isEmpty()
                        ? std::nullopt
                        : std::optional<std::string>{QFile::encodeName(music_root).toStdString()},
                .auto_connect = auto_connect,
            };
            if (auto_connect) {
                for (auto& profile : mpd_profiles_) {
                    profile.auto_connect = false;
                }
            }
            const auto existing_profile =
                std::ranges::find(mpd_profiles_, updated.id, &persistence::ConnectionProfile::id);
            if (existing_profile == mpd_profiles_.end()) {
                mpd_profiles_.push_back(std::move(updated));
            } else {
                *existing_profile = std::move(updated);
            }
            const auto profiles = mpd_profiles_;
            if (persistence_ == nullptr) {
                mpd_controller_->probeProfile(profile_id, host, port, password, music_root);
                return;
            }
            persistence_->saveProfiles(profiles, [this, profiles, profile_id, host, port, password,
                                                  music_root](const QString& error) {
                if (!error.isEmpty()) {
                    statusBar()->showMessage(error, 5'000);
                    return;
                }
                mpd_profiles_ = profiles;
                mpd_controller_->probeProfile(profile_id, host, port, password, music_root);
            });
        });
    dialog->show();
}

void BenchMainWindow::autoConnectMpd() {
    const auto profile =
        std::ranges::find(mpd_profiles_, true, &persistence::ConnectionProfile::auto_connect);
    if (profile == mpd_profiles_.end()) {
        return;
    }
    const auto music_root = profile->local_music_root
                                ? QFile::decodeName(QByteArray{
                                      profile->local_music_root->data(),
                                      static_cast<qsizetype>(profile->local_music_root->size())})
                                : QString{};
    mpd_controller_->probeProfile(QString::fromStdString(profile->id.to_string()),
                                  displayText(profile->host), static_cast<int>(profile->port),
                                  QString{}, music_root);
}

QVariantList BenchMainWindow::selectedMpdQueueRows() const {
    QVariantList rows;
    if (mpd_queue_view_ == nullptr || mpd_queue_view_->selectionModel() == nullptr) {
        return rows;
    }
    auto selected = mpd_queue_view_->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }
    return rows;
}

QStringList BenchMainWindow::selectedMpdQueueUris() const {
    QStringList uris;
    const auto* model = qobject_cast<const quick::MpdQueueModel*>(mpd_queue_view_->model());
    if (model == nullptr) {
        return uris;
    }
    for (const auto& value : selectedMpdQueueRows()) {
        if (const auto uri = model->uriAt(value.toInt())) {
            uris.push_back(displayText(*uri));
        }
    }
    return uris;
}

// Resolves MPD URIs below the configured music folder and opens the hits
// as ordinary local files in a fresh tab (ADR-0112) — from there tagging,
// conversion, and ReplayGain behave exactly like any local selection.
// Applies the chosen library root ordering: Latest asks MPD for the
// newest-first artist ranking, A-Z restores the alphabetical order.
void BenchMainWindow::applyLibraryOrder(const bool persist) {
    const auto latest = library_order_ != nullptr && library_order_->currentIndex() == 1;
    if (persist) {
        QSettings settings;
        settings.setValue(QStringLiteral("mpd/library-order"),
                          latest ? QStringLiteral("latest") : QStringLiteral("az"));
    }
    if (server_library_model_ == nullptr) {
        return;
    }
    if (latest) {
        if (mpd_controller_ != nullptr && mpd_controller_->connected()) {
            mpd_controller_->loadNewestRootOrder(server_library_model_->activeRootTag());
        }
    } else {
        server_library_model_->clearRootOrdering();
    }
}

std::optional<core::StableId> BenchMainWindow::currentMpdProfileId() const {
    if (mpd_controller_ != nullptr) {
        if (const auto parsed = core::StableId::parse(mpd_controller_->profileId().toStdString())) {
            return *parsed;
        }
    }
    if (!mpd_profiles_.empty()) {
        return mpd_profiles_.front().id;
    }
    return std::nullopt;
}

QString BenchMainWindow::effectiveMpdMusicRoot() const {
    if (mpd_controller_ != nullptr) {
        const auto parsed = core::StableId::parse(mpd_controller_->profileId().toStdString());
        const auto profile =
            parsed ? std::ranges::find(mpd_profiles_, *parsed, &persistence::ConnectionProfile::id)
                   : mpd_profiles_.end();
        if (profile != mpd_profiles_.end() && profile->local_music_root &&
            !profile->local_music_root->empty()) {
            return QFile::decodeName(
                QByteArray{profile->local_music_root->data(),
                           static_cast<qsizetype>(profile->local_music_root->size())});
        }
    }
    const QSettings settings;
    return settings.value(QLatin1String(SettingsDialog::music_root_key)).toString().trimmed();
}

void BenchMainWindow::loadMpdUrisAsLocalFiles(const QStringList& uris) {
    materializeMpdSelectionAsLocalTab(uris);
}

BenchMainWindow::ListTab*
BenchMainWindow::materializeMpdSelectionAsLocalTab(const QStringList& uris) {
    if (uris.isEmpty()) {
        return nullptr;
    }
    const auto root = effectiveMpdMusicRoot();
    if (root.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("Set the local music root in the MPD connection dialog or the "
                           "MPD music folder in Edit → Settings… first"),
            5'000);
        return nullptr;
    }
    const auto encoded_root = QFile::encodeName(root);
    const std::filesystem::path root_path{
        std::string{encoded_root.constData(), static_cast<std::size_t>(encoded_root.size())}};
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(uris.size()));
    int missing = 0;
    for (const auto& uri : uris) {
        const auto uri_bytes = uri.toUtf8();
        const auto resolved = mpd::resolve_below_music_root(
            root_path,
            std::string_view{uri_bytes.constData(), static_cast<std::size_t>(uri_bytes.size())});
        if (!resolved ||
            !QFileInfo::exists(QFile::decodeName(QByteArray::fromStdString(resolved->native())))) {
            ++missing;
            continue;
        }
        paths.push_back(resolved->native());
    }
    if (paths.empty()) {
        statusBar()->showMessage(
            QStringLiteral("None of the selected tracks could be opened under %1").arg(root),
            5'000);
        return nullptr;
    }
    auto* tab = addListTab(
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::scratch,
            .name = "Local files",
            .pinned = false,
            .dirty = false,
            .items = {},
        },
        true);
    openLocalPaths(std::move(paths));
    if (missing > 0) {
        statusBar()->showMessage(QStringLiteral("%1 track%2 could not be opened under %3")
                                     .arg(missing)
                                     .arg(missing == 1 ? QString{} : QStringLiteral("s"))
                                     .arg(root),
                                 5'000);
    }
    schedulePersist();
    return tab;
}

void BenchMainWindow::showMpdMetadataProperties(const QStringList& uris) {
    if (uris.isEmpty())
        return;
    const auto root = effectiveMpdMusicRoot();
    if (root.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Set the MPD music folder in Settings first"),
                                 5000);
        return;
    }
    const auto encoded = QFile::encodeName(root);
    const std::filesystem::path root_path{
        std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}};
    std::vector<std::string> paths;
    for (const auto& uri : uris) {
        const auto bytes = uri.toUtf8();
        const auto path = mpd::resolve_below_music_root(
            root_path, std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())});
        if (!path) {
            statusBar()->showMessage(
                QStringLiteral(
                    "The selection contains a URI that cannot be mapped to the MPD music folder"),
                5000);
            return;
        }
        paths.push_back(path->native());
    }
    const auto count = paths.size();
    // Mapping is lexical only. The editor captures files and revisions on its worker;
    // even checking existence here could stall the UI on a network mount.
    openMetadataProperties(
        count,
        [paths = std::move(paths)](std::size_t index) -> std::optional<MetadataPropertiesSource> {
            if (index >= paths.size())
                return std::nullopt;
            return MetadataPropertiesSource{
                .source = {.raw_path = paths[index],
                           .source_revision = {},
                           .baseline = {},
                           .needs_metadata_capture = true},
                .track_label = QFile::decodeName(QByteArray::fromStdString(paths[index])),
            };
        });
}

void BenchMainWindow::materializeMpdSelectionForDialog(const QStringList& uris,
                                                       const MaterializedDialog dialog) {
    if (dialog == MaterializedDialog::edit_tags) {
        showMpdMetadataProperties(uris);
        return;
    }
    auto* tab = materializeMpdSelectionAsLocalTab(uris);
    if (tab == nullptr) {
        return;
    }
    // The tab fills asynchronously; the dialog opens from finishDiscovery.
    // Arm the follow-up only when the discovery we just started targets this
    // tab — a concurrently running scan already refused ours with a message.
    if (discovery_running_ &&
        discovery_target_document_ == QString::fromStdString(tab->document.id.to_string())) {
        discovery_dialog_follow_up_ = dialog;
    }
}

void BenchMainWindow::refreshMpdPriorityMenu() {
    if (mpd_priority_menu_ == nullptr) {
        return;
    }
    auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
    auto* model = view ? qobject_cast<quick::MpdQueueModel*>(view->model()) : nullptr;
    const auto rows =
        view && view->selectionModel() ? view->selectionModel()->selectedRows() : QModelIndexList{};
    const auto* tab = currentMpdPlaylistTab();
    const auto context = tab ? tab->name : QString{};
    const bool active = context == mpd_controller_->activeContextName();
    mpd_priority_menu_->setEnabled(active && !rows.isEmpty() && mpd_controller_->connected() &&
                                   !mpd_controller_->commandBusy() &&
                                   mpd_controller_->supportsCommand(QStringLiteral("prioid")));
    std::optional<unsigned> selected_priority;
    bool priorities_match = active && !rows.isEmpty();
    for (const auto& index : rows) {
        const auto uri = model ? model->uriAt(index.row()) : std::nullopt;
        const auto* track = uri ? mpd_controller_->listPriorityTrack(context, index.row(),
                                                                     QString::fromStdString(*uri))
                                : nullptr;
        if (!track) {
            priorities_match = false;
            break;
        }
        const auto numeric = track->priority.value_or(0U);
        if (!selected_priority)
            selected_priority = numeric;
        else if (*selected_priority != numeric) {
            priorities_match = false;
            break;
        }
    }
    for (auto* action : mpd_priority_menu_->actions()) {
        const QSignalBlocker blocker{action};
        action->setChecked(priorities_match && selected_priority &&
                           action->data().toUInt() == *selected_priority);
    }
}

void BenchMainWindow::refreshMpdRateMenu() {
    if (mpd_rate_menu_ == nullptr) {
        return;
    }
    auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
    const auto rows =
        view && view->selectionModel() ? view->selectionModel()->selectedRows() : QModelIndexList{};
    const auto ready = !rows.isEmpty() && mpd_controller_->connected() &&
                       !mpd_controller_->commandBusy() && mpd_controller_->supportsRatings();
    mpd_rate_menu_->setEnabled(ready);
    std::optional<unsigned> selected_rating;
    bool ratings_match = !rows.isEmpty();
    for (const auto& value : rows) {
        const auto rating =
            view->model()->index(value.row(), 0).data(quick::MpdQueueModel::RatingRole).toUInt();
        if (!selected_rating) {
            selected_rating = rating;
        } else if (*selected_rating != rating) {
            ratings_match = false;
            break;
        }
    }
    for (auto* action : mpd_rate_menu_->actions()) {
        const QSignalBlocker blocker{action};
        action->setChecked(ratings_match && selected_rating &&
                           action->data().toUInt() == *selected_rating);
    }
}

void BenchMainWindow::refreshMpdTransport() {
    const auto connected = mpd_controller_->connected();
    const auto command_ready = connected && !mpd_controller_->commandBusy();
    const auto has_queue = mpd_controller_->queueCount() > 0;
    const auto active = mpd_controller_->playing();
    previous_action_->setEnabled(command_ready && has_queue);
    next_action_->setEnabled(command_ready && has_queue);
    play_pause_action_->setEnabled(command_ready);
    play_pause_action_->setText(active ? QStringLiteral("Pause") : QStringLiteral("Play"));
    if (transport_icon_playing_ != std::optional{active}) {
        transport_icon_playing_ = active;
        play_pause_action_->setIcon(
            style()->standardIcon(active ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    }
    stop_action_->setEnabled(command_ready && (active || mpd_controller_->paused()));

    if (connected) {
        now_playing_->setText(mpd_controller_->nowPlayingTitle());
        now_playing_context_->setText(mpd_controller_->nowPlayingDetail());
        now_playing_->setToolTip(QStringLiteral("MPD · %1").arg(mpd_controller_->status()));
        now_playing_context_->setToolTip(mpd_controller_->details());
    } else {
        now_playing_->setText(QStringLiteral("MPD not connected"));
        now_playing_context_->setText(QStringLiteral("File → Connect to MPD…"));
        now_playing_->setToolTip(mpd_controller_->status());
        now_playing_context_->setToolTip(mpd_controller_->details());
    }

    const auto position_ms = mpd_controller_->elapsedMs();
    const auto duration_ms = mpd_controller_->durationMs();
    elapsed_->setText(formatTime(position_ms));
    duration_->setText(formatTime(duration_ms));
    const auto bounded = std::clamp<qint64>(duration_ms, 0, std::numeric_limits<int>::max());
    seek_->setEnabled(command_ready && bounded > 0);
    seek_->setRange(0, static_cast<int>(bounded));
    if (!seeking_) {
        const QSignalBlocker blocker{seek_};
        seek_->setValue(
            static_cast<int>(std::clamp<qint64>(position_ms, 0, std::numeric_limits<int>::max())));
    }
    volume_->setEnabled(command_ready && mpd_controller_->volume() >= 0);
    if (!changing_volume_ && mpd_controller_->volume() >= 0) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(mpd_controller_->volume());
    }
    refreshMuteButton();
    device_button_->setEnabled(connected);
    auto output_tooltip = QStringLiteral("MPD output: %1").arg(mpd_controller_->activeOutputName());
    if (mpd_controller_->supportsReplayGain()) {
        // The server's mode is the one the ReplayGain menu sets and every
        // output obeys; the endpoint line below only describes what this
        // machine's decoder did with it, and reads Off until the server
        // pushes the mode to an enabled Trackknife output.
        const auto server_mode = mpd_controller_->replayGainMode();
        output_tooltip += QStringLiteral("\nServer ReplayGain: %1")
                              .arg(server_mode.isEmpty() ? QStringLiteral("unknown") : server_mode);
    }
    if (melody_endpoint_ != nullptr) {
        const auto endpoint = melody_endpoint_->snapshot();
        const auto mode =
            endpoint.replay_gain_mode == audio::ReplayGainMode::track   ? QStringLiteral("Track")
            : endpoint.replay_gain_mode == audio::ReplayGainMode::album ? QStringLiteral("Album")
                                                                        : QStringLiteral("Off");
        const auto selected_gain =
            endpoint.replay_gain_mode == audio::ReplayGainMode::album && endpoint.album_gain_db
                ? endpoint.album_gain_db
                : endpoint.track_gain_db;
        output_tooltip += QStringLiteral("\nThis machine's playback: %1").arg(mode);
        if (selected_gain) {
            output_tooltip += QStringLiteral(" · %1 dB · %2×")
                                  .arg(*selected_gain, 0, 'f', 2)
                                  .arg(endpoint.effective_gain_multiplier, 0, 'f', 3);
        } else if (endpoint.replay_gain_mode != audio::ReplayGainMode::off) {
            const auto received = endpoint.replay_gain_mode == audio::ReplayGainMode::album &&
                                          endpoint.received_album_gain_db
                                      ? endpoint.received_album_gain_db
                                      : endpoint.received_track_gain_db;
            output_tooltip +=
                received
                    ? QStringLiteral(" · queue %1 dB, player missing").arg(*received, 0, 'f', 2)
                : std::abs(endpoint.effective_gain_multiplier - 1.0F) > 0.0001F
                    ? QStringLiteral(" · decoder metadata · %1×")
                          .arg(endpoint.effective_gain_multiplier, 0, 'f', 3)
                    : QStringLiteral(" · no gain metadata");
        }
    }
    device_button_->setToolTip(output_tooltip);
    device_button_->setAccessibleDescription(mpd_controller_->activeOutputName());
    publishMprisState();
}

void BenchMainWindow::refreshMelodyEndpoint() {
    const auto profile_id = mpd_controller_->profileId();
    const auto eligible = mpd_controller_->connected() &&
                          mpd_controller_->supportsCommand(QStringLiteral("melody_version"));
    if (!eligible || profile_id.isEmpty()) {
        melody_endpoint_.reset();
        melody_player_storage_.reset();
        melody_endpoint_profile_.clear();
        return;
    }
    if (melody_endpoint_ != nullptr && melody_endpoint_profile_ == profile_id) {
        return;
    }
    const auto parsed = core::StableId::parse(profile_id.toStdString());
    const auto profile =
        parsed ? std::ranges::find(mpd_profiles_, *parsed, &persistence::ConnectionProfile::id)
               : mpd_profiles_.end();
    if (profile == mpd_profiles_.end()) {
        return;
    }
    melody_endpoint_.reset();
    melody_player_storage_.reset();
    auto player = audio::LocalAuditionService::create();
    if (!player) {
        statusBar()->showMessage(QStringLiteral("Melody output audio worker failed to start"),
                                 5'000);
        return;
    }
    melody_player_storage_ = std::move(*player);
    audio::MelodyAgentConfig config{
        .name = audio::default_melody_agent_name(),
        .host = profile->host,
        .port = profile->port,
        .local_music_root = profile->local_music_root,
        .stream_base_url = std::nullopt,
        .stream_format = {},
        .maximum_bit_rate = std::nullopt,
        .reconnect_delay = std::chrono::milliseconds{2'000},
        .report_period = std::chrono::milliseconds{2'000},
    };
    auto endpoint = audio::MelodyAgentService::create(std::move(config), *melody_player_storage_);
    if (!endpoint) {
        melody_player_storage_.reset();
        statusBar()->showMessage(displayText(endpoint.error().message), 5'000);
        return;
    }
    melody_endpoint_ = std::move(*endpoint);
    melody_endpoint_profile_ = profile_id;
}

// "Go to Artist/Album": reveal the queue row's artist (and optionally its
// album) in the MPD library tree, fetching lazy levels as needed — the
// in-app navigation Cantata offered.
void BenchMainWindow::goToMpdLibraryEntry(const QString& artist, const QString& album) {
    if (mpd_source_tabs_ != nullptr) {
        mpd_source_tabs_->setCurrentIndex(0);
    }
    if (mpd_search_field_ != nullptr) {
        mpd_search_field_->clear();
    }
    if (artist.isEmpty() || server_library_model_ == nullptr || server_library_view_ == nullptr) {
        statusBar()->showMessage(QStringLiteral("This queue entry names no library artist"), 4'000);
        return;
    }
    if (server_library_model_->rowCount() == 0) {
        auto connections =
            std::make_shared<std::pair<QMetaObject::Connection, QMetaObject::Connection>>();
        const auto resume = [this, connections, artist, album] {
            if (server_library_model_->rowCount() == 0) {
                return;
            }
            disconnect(connections->first);
            disconnect(connections->second);
            completeMpdLibraryGoTo(artist, album);
        };
        connections->first =
            connect(server_library_model_, &QAbstractItemModel::rowsInserted, this, resume);
        connections->second =
            connect(server_library_model_, &QAbstractItemModel::modelReset, this, resume);
        server_library_model_->reload();
        return;
    }
    completeMpdLibraryGoTo(artist, album);
}

void BenchMainWindow::completeMpdLibraryGoTo(const QString& artist, const QString& album) {
    QModelIndex artist_index;
    for (int row = 0; row < server_library_model_->rowCount(); ++row) {
        const auto candidate = server_library_model_->index(row, 0);
        if (candidate.data(ui::ServerLibraryTreeModel::QueryValueRole).toString() == artist) {
            artist_index = candidate;
            break;
        }
    }
    if (!artist_index.isValid()) {
        statusBar()->showMessage(
            QStringLiteral("\u201C%1\u201D is not in the library tree").arg(artist), 4'000);
        return;
    }
    if (album.isEmpty()) {
        server_library_view_->expand(artist_index);
        server_library_view_->setCurrentIndex(artist_index);
        server_library_view_->scrollTo(artist_index, QAbstractItemView::PositionAtCenter);
        return;
    }
    if (server_library_model_->canFetchMore(artist_index)) {
        const QPersistentModelIndex persistent{artist_index};
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection =
            connect(server_library_model_, &QAbstractItemModel::rowsInserted, this,
                    [this, connection, persistent, artist, album](const QModelIndex& parent) {
                        if (parent != QModelIndex{persistent}) {
                            return;
                        }
                        disconnect(*connection);
                        completeMpdLibraryGoTo(artist, album);
                    });
        server_library_model_->fetchMore(artist_index);
        return;
    }
    server_library_view_->expand(artist_index);
    // Album-level grouping values are definition-specific composites
    // (release id, or album|date); match the plain album against the
    // grouping value, its album prefix, or the displayed label.
    for (int row = 0; row < server_library_model_->rowCount(artist_index); ++row) {
        const auto candidate = server_library_model_->index(row, 0, artist_index);
        const auto query_value =
            candidate.data(ui::ServerLibraryTreeModel::QueryValueRole).toString();
        const auto label = candidate.data(Qt::DisplayRole).toString();
        const auto matches = query_value == album ||
                             query_value.startsWith(album + QLatin1Char('|')) || label == album ||
                             label.startsWith(album + QStringLiteral(" ("));
        if (matches) {
            server_library_view_->setCurrentIndex(candidate);
            server_library_view_->scrollTo(candidate, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
    server_library_view_->setCurrentIndex(artist_index);
    server_library_view_->scrollTo(artist_index, QAbstractItemView::PositionAtCenter);
    statusBar()->showMessage(
        QStringLiteral("\u201C%1\u201D has no album \u201C%2\u201D in the library tree")
            .arg(artist, album),
        4'000);
}

} // namespace trackknife::bench
