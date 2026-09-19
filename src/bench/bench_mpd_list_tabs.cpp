// SPDX-License-Identifier: GPL-3.0-only

// ADR-0187: server lists are MPD stored playlists — one kind of server
// list, visible to every client, playable as a context. This file holds
// the tab-strip grouping, the "Copy to server list" gesture, and the
// one-time migration of the client-owned lists ADR-0181 shipped.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"

#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QAction>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QStatusBar>
#include <QTableView>

#include <algorithm>
#include <utility>

namespace trackknife::bench {

// mpdTabInsertionIndex keeps every server-side tab grouped directly after
// the MPD Queue: new MPD tabs insert after the last existing one instead of
// trailing the local lists.
int BenchMainWindow::mpdTabInsertionIndex() {
    auto index = mpd_queue_view_ != nullptr ? tabs_->indexOf(mpd_queue_view_) : -1;
    if (index < 0) {
        return tabs_->count();
    }
    ++index;
    while (index < tabs_->count()) {
        auto* widget = tabs_->widget(index);
        if (mpdPlaylistTabForWidget(qobject_cast<QTableView*>(widget)) == nullptr &&
            mpdSearchTabForWidget(qobject_cast<QTableView*>(widget)) == nullptr) {
            break;
        }
        ++index;
    }
    return index;
}

// ADR-0187: server lists are stored playlists. "Copy to server list"
// creates or extends one on the server and opens its tab, so a list made
// here is the same object every client sees and can be played as a
// context.
void BenchMainWindow::addCopyToServerListMenu(QMenu* menu, QTableView* source_view) {
    auto* submenu = menu->addMenu(QStringLiteral("Copy to server list"));
    submenu->setObjectName(QStringLiteral("bench-copy-to-server-list-menu"));
    const auto uris = selectedMpdViewUris(source_view);
    const auto ready = !uris.isEmpty() && mpd_controller_->connected() &&
                       mpd_controller_->supportsCommand(QStringLiteral("playlistadd"));
    submenu->setEnabled(ready);
    auto* create = submenu->addAction(QStringLiteral("New list…"));
    create->setObjectName(QStringLiteral("action-copy-to-new-server-list"));
    create->setEnabled(ready);
    connect(create, &QAction::triggered, this, [this, uris] {
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("New server list"),
                                  QStringLiteral("List name:"), QLineEdit::Normal,
                                  QStringLiteral("Server list"), &accepted)
                .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        mpd_controller_->addToStoredPlaylist(name, uris, -1);
        openMpdPlaylistTab(name, true);
    });
    if (mpd_playlists_list_ != nullptr && mpd_playlists_list_->count() > 0) {
        submenu->addSeparator();
        for (int row = 0; row < mpd_playlists_list_->count(); ++row) {
            const auto name = mpd_playlists_list_->item(row)->text();
            auto* action = submenu->addAction(name);
            action->setEnabled(ready);
            connect(action, &QAction::triggered, this, [this, name, uris] {
                mpd_controller_->addToStoredPlaylist(name, uris, -1);
            });
        }
    }
}

// migrateServerListDocuments pushes each client-owned list left over from
// ADR-0181 to the server as a stored playlist and opens its tab, then
// forgets the document. Names collide with existing playlists are suffixed.
// Runs on the first connect that advertises playlistadd; offline the
// documents simply wait.
void BenchMainWindow::migrateServerListDocuments() {
    if (pending_server_lists_.empty() || mpd_controller_ == nullptr ||
        !mpd_controller_->connected() ||
        !mpd_controller_->supportsCommand(QStringLiteral("playlistadd"))) {
        return;
    }
    auto pending = std::exchange(pending_server_lists_, {});
    for (const auto& document : pending) {
        QStringList uris;
        uris.reserve(static_cast<qsizetype>(document.items.size()));
        for (const auto& item : document.items) {
            uris.push_back(displayText(item.source_reference));
        }
        if (uris.isEmpty()) {
            continue;
        }
        auto name = displayText(document.name);
        if (name.trimmed().isEmpty()) {
            name = QStringLiteral("Server list");
        }
        if (mpd_playlists_list_ != nullptr) {
            int suffix = 2;
            while (!mpd_playlists_list_->findItems(name, Qt::MatchExactly).isEmpty()) {
                name = QStringLiteral("%1 (%2)").arg(displayText(document.name)).arg(suffix++);
            }
        }
        mpd_controller_->addToStoredPlaylist(name, uris, -1);
        openMpdPlaylistTab(name, false);
        statusBar()->showMessage(
            QStringLiteral("Moved the server list “%1” to a stored playlist").arg(name), 5'000);
    }
    schedulePersist();
}

} // namespace trackknife::bench
