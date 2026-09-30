// SPDX-License-Identifier: GPL-3.0-only

// ADR-0227: the remote engines beside this computer's, as this window shows
// them. The workspace connects them and follows them; here each one's
// library sits beside this computer's in the source switch.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/catalogue_source.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/engine_list_sync.hpp"
#include "bench/engine_playback.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/remote_engines.hpp"
#include "bench/search_dialog.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTableView>

namespace trackknife::bench {

// ADR-0234: an engine elsewhere took the id it gave. Its library and its
// source tab follow; with its lists, so do their views, and dialogs that
// offer the libraries by key are made again when next opened.
void BenchMainWindow::engineRekeyed(EngineLink& engine, const EngineKey& from,
                                    const bool lists_follow) {
    const auto to = engine.key;
    if (engine.library != nullptr) {
        engine.library->setEngine(to);
    }
    for (int index = 0; local_source_tabs_ != nullptr && index < local_source_tabs_->count();
         ++index) {
        if (local_source_tabs_->tabData(index).toString() == from.text()) {
            local_source_tabs_->setTabData(index, to.text());
        }
    }
    if (!lists_follow) {
        return;
    }
    for (auto& tab : list_tabs_) {
        if (EngineKey::of(tab->document) == to) {
            markViewEngine(tab->view, to);
        }
    }
    if (auto* dialog = findChild<DynamicPlaylistDialog*>()) {
        dialog->close();
    }
    if (search_dialog_ != nullptr) {
        search_dialog_->close();
    }
}

LocalLibraryPanel* BenchMainWindow::libraryOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->library : nullptr;
}

void BenchMainWindow::enginesSynced() {
    keepTabGroupsTogether();
    refreshListsPanel();
}

// An engine elsewhere let go of: dialogs that offer the libraries by engine
// are made again when next opened, and its source tab and library go.
void BenchMainWindow::engineRemoving(EngineLink& engine) {
    if (auto* dialog = findChild<DynamicPlaylistDialog*>()) {
        dialog->close();
    }
    if (search_dialog_ != nullptr) {
        search_dialog_->close();
    }
    for (int index = 0; local_source_tabs_ != nullptr && index < local_source_tabs_->count();
         ++index) {
        if (local_source_tabs_->tabData(index).toString() == engine.key.text()) {
            local_source_tabs_->removeTab(index);
            break;
        }
    }
    if (engine.library != nullptr) {
        disconnect(engine.library, nullptr, this, nullptr);
        source_stack_->removeWidget(engine.library);
        delete engine.library;
        engine.library = nullptr;
    }
}

void BenchMainWindow::engineRemoved() {
    selectPreferredSource();
    refreshActiveContext();
}

// Connected, the remote says what it is called: the library tab shows that
// rather than its address, and its lists' empty messages name it.
void BenchMainWindow::engineAttached(EngineLink& engine) {
    for (int index = 0; local_source_tabs_ != nullptr && index < local_source_tabs_->count();
         ++index) {
        if (local_source_tabs_->tabData(index).toString() == engine.key.text()) {
            local_source_tabs_->setTabText(index, engine.catalogue->name());
        }
    }
    for (auto& tab : list_tabs_) {
        if (EngineKey::of(tab->document) == engine.key) {
            static_cast<ui::QueueTableView*>(tab->view)->setEmptyMessage(
                emptyListTitle(EngineKey::of(tab->document)),
                emptyListHint(EngineKey::of(tab->document)));
        }
    }
}

// An engine elsewhere connected: its library beside this computer's.
void BenchMainWindow::engineConnected(EngineLink& engine, const bool first) {
    auto* link = &engine;
    link->library = new LocalLibraryPanel(*link->catalogue, link->key, source_stack_);
    link->library->setObjectName(
        first ? QStringLiteral("bench-remote-library")
              : QStringLiteral("bench-remote-library-%1").arg(engines_.size() - 1));
    source_stack_->addWidget(link->library);
    workspace_.attachLibrary(&link->library->browser());
    refreshLocalRatings();
    const auto index = local_source_tabs_->addTab(link->catalogue->name());
    // The engine's key, which the tab is found and followed by (ADR-0234).
    local_source_tabs_->setTabData(index, link->key.text());
    local_source_tabs_->setTabToolTip(index, link->catalogue->describe());
    // Now that there is a remote to show instead, or it was the one chosen.
    applyLocalLibraryVisibility();
    selectPreferredSource();

    link->library->setListTargets([this, link] { return listTargets(link->key); });
}

} // namespace trackknife::bench
