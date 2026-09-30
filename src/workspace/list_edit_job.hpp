// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/lists/edit_plan.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>
#include <vector>

namespace trackknife::bench {

class LocalListModel;

// A whole-list edit -- sorted by a tkfmt-1 expression, reversed, albums
// shuffled or duplicates removed -- planned off the UI thread from a
// snapshot of the list's rows, and applied as one step to undo. A change to
// the list while it is planned stops it. Both windows' edit bars draw it.
class ListEditJob final : public QObject {
    Q_OBJECT

  public:
    explicit ListEditJob(QObject* parent = nullptr);
    ~ListEditJob() override;

    // The list edits apply to; a new one stops what was under way.
    void setModel(LocalListModel* model);
    [[nodiscard]] LocalListModel* model() const { return model_; }
    void start(lists::EditRequest request);
    void cancel();

    [[nodiscard]] bool active() const noexcept { return active_; }
    // Whether the last request was a sort: the bar shows its expression.
    [[nodiscard]] bool sorting() const noexcept { return request_.kind == lists::EditKind::sort; }
    [[nodiscard]] const lists::EditRequest& request() const noexcept { return request_; }
    [[nodiscard]] QString status() const { return status_; }

    [[nodiscard]] static QString label(lists::EditKind kind);

  signals:
    // Anything the bar shows changed: whether it runs, what it says.
    void changed();
    // It needs to be seen: an edit started, or a message was left.
    void shown();
    // The list was edited, as one step to undo.
    void edited(LocalListModel* model);

  private:
    void capture();
    void finish();
    void invalidate();
    void stopWithMessage(const QString& message);
    void setStatus(const QString& status);

    QPointer<LocalListModel> model_;
    std::vector<QMetaObject::Connection> connections_;
    QTimer capture_timer_;
    QTimer progress_timer_;
    QThreadPool pool_;
    QFutureWatcher<core::Result<lists::EditPlan>> watcher_;
    core::CancellationSource cancellation_;
    std::shared_ptr<std::vector<lists::Entry>> snapshot_;
    std::shared_ptr<std::atomic<std::size_t>> progress_;
    lists::EditRequest request_;
    QString status_;
    std::uint64_t generation_{0}, job_generation_{0};
    std::size_t bytes_{0};
    int total_{0};
    bool active_{false}, worker_busy_{false};
};

} // namespace trackknife::bench
