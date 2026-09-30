// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/playlist_transfer_bar.hpp"

#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QMenu>
#include <QStatusBar>
#include <QTableView>

namespace trackknife::bench {
void BenchMainWindow::importPlaylistDialog() {
    if (!lists_restored_ || playlist_transfer_bar_->busy())
        return;
    auto* dialog = new QFileDialog(this, tr("Import M3U8 into a new local list"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setNameFilter(tr("UTF-8 playlists (*.m3u8)"));
    dialog->setFileMode(QFileDialog::ExistingFile);
    connect(dialog, &QFileDialog::fileSelected, this,
            [this](const QString& path) { importM3u8Path(QFile::encodeName(path).toStdString()); });
    dialog->open();
}
void BenchMainWindow::importM3u8Path(std::string raw_path) {
    if (lists_restored_)
        playlist_transfer_bar_->importFile(std::move(raw_path));
}
void BenchMainWindow::exportPlaylistDialog() {
    auto* tab = currentListTab();
    if (!tab || playlist_transfer_bar_->busy())
        return;
    QPointer<LocalListModel> model = tab->model;
    auto* dialog = new QFileDialog(this, tr("Export M3U8 to a new file"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setNameFilter(tr("UTF-8 playlists (*.m3u8)"));
    dialog->setDefaultSuffix(QStringLiteral("m3u8"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setOption(QFileDialog::DontConfirmOverwrite);
    dialog->setLabelText(QFileDialog::FileName,
                         tr("New file (references, titles and durations only):"));
    connect(dialog, &QFileDialog::fileSelected, this, [this, model](const QString& path) {
        if (model)
            playlist_transfer_bar_->exportFile(QFile::encodeName(path).toStdString(), model);
    });
    dialog->open();
}
void BenchMainWindow::buildPlaylistActions(QMenu* file_menu) {
    playlist_transfer_bar_ = new PlaylistTransferBar(this);
    addToolBar(Qt::BottomToolBarArea, playlist_transfer_bar_);
    playlist_transfer_bar_->hide();
    auto* import = file_menu->addAction(tr("Import M3U8 playlist…"));
    import->setObjectName(QStringLiteral("action-import-m3u8"));
    connect(import, &QAction::triggered, this, &BenchMainWindow::importPlaylistDialog);
    export_playlist_action_ = file_menu->addAction(tr("Export list as M3U8…"));
    export_playlist_action_->setObjectName(QStringLiteral("action-export-m3u8"));
    export_playlist_action_->setToolTip(
        tr("Export local whole-file references, titles and durations. Other cached metadata and "
           "logical selections cannot be stored in M3U8."));
    connect(export_playlist_action_, &QAction::triggered, this,
            &BenchMainWindow::exportPlaylistDialog);
    connect(playlist_transfer_bar_, &PlaylistTransferBar::imported, this,
            [this](std::shared_ptr<std::vector<LocalTrackRow>> rows, const QString& name) {
                workspace_.addImportedList(std::move(*rows), name);
                refreshSelectionStatus();
            });
    connect(playlist_transfer_bar_, &PlaylistTransferBar::completed, this,
            [this](bool, const QString& message) { statusBar()->showMessage(message, 8'000); });
}
} // namespace trackknife::bench
