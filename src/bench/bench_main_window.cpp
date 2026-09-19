// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/playlist_transfer_bar.hpp"

#include "bench/local_library_panel.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/audio/melody_agent.hpp"

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QItemSelectionModel>
#include <QMetaObject>
#include <QMimeData>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>

#include <QTimer>
#include <algorithm>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#if defined(TRACKKNIFE_THREAD_SANITIZER)
extern "C" void __tsan_acquire(void* address);
#endif

namespace trackknife::bench {

void BenchMainWindow::stopBackgroundWork() {
    if (playlist_transfer_bar_)
        playlist_transfer_bar_->stop();
    if (list_edit_bar_ != nullptr) {
        list_edit_bar_->cancel();
    }
    if (local_library_ != nullptr) {
        local_library_->stop();
    }
    probe_cancellation_.request_cancellation();
    metadata_operation_cancellation_.request_cancellation();
    probe_queue_.clear();
    artwork_queue_.clear();
    if (probe_watcher_.isRunning()) {
        probe_watcher_.waitForFinished();
    }
    if (artwork_watcher_.isRunning()) {
        artwork_watcher_.waitForFinished();
    }
#if defined(TRACKKNIFE_THREAD_SANITIZER)
    if (artwork_outcome_) {
        __tsan_acquire(artwork_outcome_.get());
    }
#endif
    if (discovery_watcher_.isRunning()) {
        discovery_watcher_.waitForFinished();
    }
    if (metadata_operation_watcher_.isRunning()) {
        metadata_operation_watcher_.waitForFinished();
    }
    probe_running_ = false;
    artwork_running_ = false;
    discovery_running_ = false;
    metadata_operation_running_ = false;
    if (transport_timer_ != nullptr) {
        transport_timer_->stop();
    }
    player_ = nullptr;
    player_storage_.reset();
}

void BenchMainWindow::closeEvent(QCloseEvent* event) {
    std::vector<QPointer<MetadataPropertiesDialog>> properties_tabs;
    for (auto index = 0; index < tabs_->count(); ++index) {
        if (auto* properties = qobject_cast<MetadataPropertiesDialog*>(tabs_->widget(index))) {
            properties_tabs.emplace_back(properties);
        }
    }
    for (const auto& properties : properties_tabs) {
        if (properties != nullptr && !properties->close()) {
            event->ignore();
            return;
        }
    }
    stopBackgroundWork();
    persistNow(true);
    event->accept();
}

bool BenchMainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (tabs_ == nullptr || (watched != tabs_ && watched != tabs_->tabBar()) ||
        (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove &&
         event->type() != QEvent::Drop)) {
        return QMainWindow::eventFilter(watched, event);
    }
    auto* drop = static_cast<QDropEvent*>(event);
    const auto position = watched == tabs_->tabBar()
                              ? drop->position().toPoint()
                              : tabs_->tabBar()->mapFrom(tabs_, drop->position().toPoint());
    return handleTabTrackDrop(qobject_cast<QTableView*>(drop->source()), drop, position);
}

bool BenchMainWindow::handleTabTrackDrop(QTableView* source, QDropEvent* drop,
                                         const QPoint& position) {
    // The QTabWidget receives drops in the unused strip beyond the bar's width.
    if (position.y() < 0 || position.y() >= tabs_->tabBar()->height()) {
        drop->ignore();
        return true;
    }
    // ADR-0191: dropping server rows on the tab strip builds a working list
    // on the server — the drag equivalent of "Send to tab > New list…".
    if (source != nullptr && tabForDocument(source->property("bench-document-id").toString()) ==
                                 nullptr &&
        qobject_cast<quick::MpdQueueModel*>(source->model()) != nullptr) {
        const auto tab_index = tabs_->tabBar()->tabAt(position);
        auto* target_view = tab_index < 0 ? nullptr : qobject_cast<QTableView*>(
                                                          tabs_->widget(tab_index));
        auto* target_list = target_view == nullptr ? nullptr : mpdPlaylistTabForWidget(target_view);
        if (tab_index >= 0 && target_list == nullptr) {
            drop->ignore();
            return true;
        }
        if (drop->type() == QEvent::Drop) {
            const auto uris = selectedMpdViewUris(source);
            if (uris.isEmpty()) {
                drop->ignore();
                return true;
            }
            if (target_list != nullptr) {
                mpd_controller_->addToStoredPlaylist(target_list->name, uris, -1);
                tabs_->setCurrentWidget(target_list->view);
            } else {
                createScratchListTab(uniqueScratchListName(), uris);
            }
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    auto* source_tab = source == nullptr
                           ? nullptr
                           : tabForDocument(source->property("bench-document-id").toString());
    const auto tab_index = tabs_->tabBar()->tabAt(position);
    auto* target = tab_index < 0 ? nullptr : qobject_cast<QTableView*>(tabs_->widget(tab_index));
    auto* target_tab = target == nullptr
                           ? nullptr
                           : tabForDocument(target->property("bench-document-id").toString());
    if (source_tab == nullptr || source_tab->view != source ||
        source->selectionModel() == nullptr || source->selectionModel()->selectedRows().isEmpty() ||
        (tab_index >= 0 &&
         (target_tab == nullptr || target_tab->view != target || source == target))) {
        drop->ignore();
        return true;
    }
    const auto action =
        drop->modifiers().testFlag(Qt::ControlModifier) ? Qt::CopyAction : Qt::MoveAction;
    if (!drop->possibleActions().testFlag(action)) {
        drop->ignore();
        return true;
    }
    if (drop->type() == QEvent::Drop) {
        auto selected = source->selectionModel()->selectedRows(0);
        std::ranges::sort(selected, {}, &QModelIndex::row);
        QVariantList rows;
        for (const auto& index : selected)
            rows.push_back(index.row());
        const auto transferred =
            target_tab == nullptr
                ? transferRowsToNewTab(source, rows, action == Qt::MoveAction, tr("Selection"))
                : transferRows(source, rows, target->property("bench-document-id").toString(),
                               action == Qt::MoveAction, -1);
        if (!transferred) {
            drop->ignore();
            return true;
        }
        if (target_tab != nullptr)
            tabs_->setCurrentWidget(target);
    }
    drop->setDropAction(action);
    drop->accept();
    return true;
}

void BenchMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void BenchMainWindow::dropEvent(QDropEvent* event) {
    std::vector<std::string> raw_paths;
    const auto urls = event->mimeData()->urls();
    raw_paths.reserve(static_cast<std::size_t>(urls.size()));
    for (const auto& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const auto encoded = QFile::encodeName(url.toLocalFile());
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    if (raw_paths.empty()) {
        return;
    }
    event->acceptProposedAction();
    openLocalPaths(std::move(raw_paths));
}

} // namespace trackknife::bench
