// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"

#include <QFileDialog>
#include <QTabWidget>
#include <QTableView>

#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {

void BenchMainWindow::openFilesDialog() {
    const auto files = QFileDialog::getOpenFileNames(this, QStringLiteral("Open files"));
    std::vector<std::string> raw_paths;
    raw_paths.reserve(static_cast<std::size_t>(files.size()));
    for (const auto& file : files) {
        const auto encoded = QFile::encodeName(file);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    openLocalPaths(std::move(raw_paths));
}

void BenchMainWindow::openFolderDialog() {
    const auto folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Open folder"));
    if (folder.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(folder);
    openLocalPaths({{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
}

void BenchMainWindow::addFolderRoot() {
    const auto folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Bookmark folder"));
    if (folder.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(folder);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    addFolderBookmark(raw_path);
    revealFolderPath(raw_path);
}

void BenchMainWindow::openLocalPaths(std::vector<std::string> raw_paths) {
    if (raw_paths.empty()) {
        return;
    }
    if (!lists_restored_) {
        pending_open_paths_.insert(pending_open_paths_.end(),
                                   std::make_move_iterator(raw_paths.begin()),
                                   std::make_move_iterator(raw_paths.end()));
        return;
    }
    // Files on this computer go into a local list (ADR-0227): the one on
    // screen when it is one, else the first there is, else a new one. Into a
    // remote tab they would be the remote's paths, which they are not.
    auto* tab = currentListTab();
    if (tab == nullptr || !EngineKey::of(tab->document).isLocal()) {
        const auto local = std::ranges::find_if(list_tabs_, [](const auto& candidate) {
            return EngineKey::of(candidate->document).isLocal();
        });
        tab = local != list_tabs_.end()
                  ? local->get()
                  : addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                         .kind = persistence::ListKind::scratch,
                                                         .name = untitled_list_name,
                                                         .pinned = false,
                                                         .dirty = false,
                                                         .items = {},
                                                         .engine = {}},
                               true);
        tabs_->setCurrentWidget(tab->view);
    }
    startDiscovery(std::move(raw_paths), QString::fromStdString(tab->document.id.to_string()), -1);
}

} // namespace trackknife::bench
