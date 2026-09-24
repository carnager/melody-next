// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "uicommon/local_files_mime_data.hpp"
#include "bench/bench_main_window_helpers.hpp"

#include "bench/local_list_edit_bar.hpp"
#include "bench/playlist_transfer_bar.hpp"

#include "bench/local_library_panel.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "trackknife/audio/local_audition.hpp"

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QItemSelectionModel>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>

#include <QTimer>
#include <QToolButton>
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
}

void BenchMainWindow::closeEvent(QCloseEvent* event) {
    std::vector<QPointer<MetadataPropertiesDialog>> properties_tabs;
    for (auto* properties : findChildren<MetadataPropertiesDialog*>())
        properties_tabs.emplace_back(properties);
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
    if (watched == up_next_button_ &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
         event->type() == QEvent::Drop)) {
        auto* drop = static_cast<QDropEvent*>(event);
        // Out of a library: queued at the end, like a list's rows.
        if (const auto* files = dynamic_cast<const ui::LocalFilesMimeData*>(drop->mimeData());
            files != nullptr && files->property(library_entries_property).isValid()) {
            if (event->type() == QEvent::Drop && !enqueueLibraryDrop(*files, -1)) {
                drop->ignore();
                return true;
            }
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            return true;
        }
        auto* source = qobject_cast<QTableView*>(drop->source());
        if (!source || qobject_cast<LocalListModel*>(source->model()) == nullptr) {
            drop->ignore();
            return true;
        }
        if (event->type() == QEvent::Drop)
            enqueueUpNext(source, false);
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    // A double click on the tab bar's empty space -- in the bar or in the
    // strip of the tab widget beyond it -- makes a new list, as in a browser.
    if (tabs_ != nullptr && (watched == tabs_ || watched == tabs_->tabBar()) &&
        event->type() == QEvent::MouseButtonDblClick) {
        auto* click = static_cast<QMouseEvent*>(event);
        const auto position = watched == tabs_->tabBar()
                                  ? click->position().toPoint()
                                  : tabs_->tabBar()->mapFrom(tabs_, click->position().toPoint());
        if (click->button() == Qt::LeftButton && position.y() >= 0 &&
            position.y() < tabs_->tabBar()->height() && tabs_->tabBar()->tabAt(position) < 0) {
            // Not inside the mouse event: the name is asked in a dialog of its own.
            QMetaObject::invokeMethod(this, &BenchMainWindow::createList, Qt::QueuedConnection);
            return true;
        }
        return QMainWindow::eventFilter(watched, event);
    }
    if (tabs_ == nullptr || (watched != tabs_ && watched != tabs_->tabBar()) ||
        (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove &&
         event->type() != QEvent::Drop)) {
        return QMainWindow::eventFilter(watched, event);
    }
    auto* drop = static_cast<QDropEvent*>(event);
    const auto position = watched == tabs_->tabBar()
                              ? drop->position().toPoint()
                              : tabs_->tabBar()->mapFrom(tabs_, drop->position().toPoint());
    return handleTabTrackDrop(qobject_cast<QAbstractItemView*>(drop->source()), drop, position);
}

bool BenchMainWindow::handleTabTrackDrop(QAbstractItemView* source, QDropEvent* drop,
                                         const QPoint& position) {
    // The QTabWidget receives drops in the unused strip beyond the bar's width.
    if (position.y() < 0 || position.y() >= tabs_->tabBar()->height()) {
        drop->ignore();
        return true;
    }
    // Files -- from a file manager, or dragged out of a library -- rather
    // than rows of another tab.
    if (qobject_cast<QTableView*>(source) == nullptr &&
        (dynamic_cast<const ui::LocalFilesMimeData*>(drop->mimeData()) != nullptr ||
         drop->mimeData()->hasUrls())) {
        return handleTabFileDrop(drop, tabs_->tabBar()->tabAt(position));
    }
    auto* source_table = qobject_cast<QTableView*>(source);
    auto* source_tab = source_table == nullptr
                           ? nullptr
                           : tabForDocument(source->property("bench-document-id").toString());
    const auto tab_index = tabs_->tabBar()->tabAt(position);
    auto* target = tab_index < 0 ? nullptr : qobject_cast<QTableView*>(tabs_->widget(tab_index));
    auto* target_tab = target == nullptr
                           ? nullptr
                           : tabForDocument(target->property("bench-document-id").toString());
    const bool dynamic = source && source->property("definition-owned").toBool() &&
                         qobject_cast<LocalListModel*>(source->model());
    if ((!dynamic && (source_tab == nullptr || source_tab->view != source)) ||
        source->selectionModel() == nullptr || source->selectionModel()->selectedRows().isEmpty() ||
        (tab_index >= 0 &&
         (target_tab == nullptr || target_tab->view != target || source == target))) {
        drop->ignore();
        return true;
    }
    const auto action = dynamic || drop->modifiers().testFlag(Qt::ControlModifier) ? Qt::CopyAction
                                                                                   : Qt::MoveAction;
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
                ? transferRowsToNewTab(source_table, rows, action == Qt::MoveAction,
                                       tr("Selection"))
                : transferRows(source_table, rows, target->property("bench-document-id").toString(),
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

bool BenchMainWindow::handleTabFileDrop(QDropEvent* drop, const int tab_index) {
    const auto* library = dynamic_cast<const ui::LocalFilesMimeData*>(drop->mimeData());
    std::vector<std::string> urls;
    if (library == nullptr) {
        for (const auto& url : drop->mimeData()->urls()) {
            if (url.isLocalFile()) {
                const auto encoded = QFile::encodeName(url.toLocalFile());
                urls.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
            }
        }
        if (urls.empty()) {
            drop->ignore();
            return true;
        }
    }
    if (drop->type() != QEvent::Drop) {
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    // Whose files they are: a library says; a file manager's are this
    // computer's.
    const bool remote_files = library != nullptr && library->remote();
    auto* view = tab_index < 0 ? nullptr : qobject_cast<QTableView*>(tabs_->widget(tab_index));
    auto* target =
        view == nullptr ? nullptr : tabForDocument(view->property("bench-document-id").toString());
    if (target == nullptr) {
        // Empty space in the bar: a new tab for them, of their engine, named
        // after the folder when they are one.
        auto name = QStringLiteral("Dropped files");
        if (urls.size() == 1U) {
            name = QFileInfo{QFile::decodeName(QByteArray::fromStdString(urls.front()))}.fileName();
        } else if (library != nullptr) {
            name = QStringLiteral("Library selection");
        }
        target = addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                      .kind = persistence::ListKind::scratch,
                                                      .name = utf8Bytes(name),
                                                      .pinned = false,
                                                      .dirty = false,
                                                      .items = {},
                                                      .remote = remote_files},
                            true);
        schedulePersist();
    }
    tabs_->setCurrentWidget(target->view);
    const auto id = QString::fromStdString(target->document.id.to_string());
    const bool into_remote = target->document.remote;
    const QPointer<BenchMainWindow> window{this};
    const auto place = [window, id, into_remote, remote_files](std::vector<std::string> paths) {
        auto* destination = window ? window->tabForDocument(id) : nullptr;
        if (destination == nullptr) {
            return;
        }
        if (remote_files != into_remote) {
            paths = window->crossEnginePaths(std::move(paths), into_remote);
        }
        if (paths.empty()) {
            return;
        }
        if (into_remote) {
            window->insertRemotePaths(*destination, std::move(paths), -1);
        } else {
            window->startDiscovery(std::move(paths), id, -1);
        }
    };
    if (library != nullptr) {
        library->resolve(place);
    } else {
        place(std::move(urls));
    }
    drop->setDropAction(Qt::CopyAction);
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
