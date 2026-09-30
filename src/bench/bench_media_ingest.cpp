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

} // namespace trackknife::bench
