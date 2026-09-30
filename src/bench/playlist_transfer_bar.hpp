// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "workspace/playlist_transfer.hpp"

#include <QToolBar>

class QAction;
class QLabel;
namespace trackknife::bench {
// The M3U8 transfer, as a bar under the window while it runs and after.
class PlaylistTransferBar final : public QToolBar {
    Q_OBJECT
  public:
    explicit PlaylistTransferBar(QWidget* parent = nullptr);
    ~PlaylistTransferBar() override;
    void importFile(std::string raw_path) { transfer_->importFile(std::move(raw_path)); }
    void exportFile(std::string raw_path, LocalListModel* model) {
        transfer_->exportFile(std::move(raw_path), model);
    }
    void cancel() { transfer_->cancel(); }
    void stop() { transfer_->stop(); }
    [[nodiscard]] bool busy() const noexcept { return transfer_->busy(); }
  signals:
    void imported(std::shared_ptr<std::vector<LocalTrackRow>> rows, const QString& name);
    void completed(bool success, const QString& message);

  private:
    PlaylistTransfer* transfer_;
    QLabel* status_{};
    QAction* cancel_action_{};
};
} // namespace trackknife::bench
