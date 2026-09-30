// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bench/local_list_model.hpp"
#include "trackknife/lists/m3u8.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

namespace trackknife::bench {
// Importing an M3U8 playlist into a new list, and exporting a list as one
// (ADR-0100). One bounded worker; imports publish a complete detached list,
// exports capture the selected local model in short slices before
// serializing or touching disk. Both windows' transfer bars draw it.
class PlaylistTransfer final : public QObject {
    Q_OBJECT
  public:
    explicit PlaylistTransfer(QObject* parent = nullptr);
    ~PlaylistTransfer() override;
    void importFile(std::string raw_path);
    void exportFile(std::string raw_path, LocalListModel* model);
    void cancel();
    void stop();
    [[nodiscard]] bool busy() const noexcept { return active_ || worker_busy_; }
    // Running (Cancel), or done and still shown (Close).
    [[nodiscard]] bool active() const noexcept { return active_; }
    // Shown while it runs and after, until closed.
    [[nodiscard]] bool shown() const noexcept { return shown_; }
    [[nodiscard]] QString status() const { return status_; }
    // Cancel while running; Close once done.
    void dismiss();
  signals:
    void changed();
    void imported(std::shared_ptr<std::vector<LocalTrackRow>> rows, const QString& name);
    void completed(bool success, const QString& message);

  private:
    struct Outcome {
        std::shared_ptr<std::vector<LocalTrackRow>> rows;
        std::string name;
        std::optional<core::Error> error;
    };
    bool begin(std::string raw_path);
    void capture();
    void finish();
    void clearCapture();
    void report(bool success, const QString& message);
    void setStatus(const QString& status);
    QString status_;
    bool shown_{false};
    QTimer capture_timer_;
    QTimer progress_timer_;
    QThreadPool pool_;
    QFutureWatcher<Outcome> watcher_;
    core::CancellationSource cancellation_;
    std::shared_ptr<std::atomic<std::size_t>> progress_;
    std::shared_ptr<std::vector<lists::PlaylistEntry>> snapshot_;
    QPointer<LocalListModel> model_;
    std::vector<QMetaObject::Connection> connections_;
    std::string path_;
    QString operation_;
    std::size_t bytes_{0};
    int total_{0};
    bool active_{false}, worker_busy_{false};
};
} // namespace trackknife::bench
