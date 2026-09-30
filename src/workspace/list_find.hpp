// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <vector>

namespace trackknife::bench {

class LocalListModel;

// Find in the list shown (ADR-0142, ADR-0153): every value a row carries,
// its duration, its audio format and its path, from the current row on,
// wrapping round. One cancellable, bounded traversal; workers receive
// detached text batches and never touch the model. Both windows' find bars
// draw it.
class ListFind final : public QObject {
    Q_OBJECT

  public:
    explicit ListFind(QObject* parent = nullptr);
    ~ListFind() override;

    // The list searched, and where a search starts from (its current row,
    // or -1).
    void setList(LocalListModel* model, std::function<int()> current_row);
    [[nodiscard]] bool available() const { return model_ != nullptr; }

    [[nodiscard]] QString query() const { return query_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool shown() const { return shown_; }

    void setQuery(const QString& query);
    void open();
    void findNext(bool backwards = false);
    void dismiss();
    // The selection moved under a running search: it is abandoned.
    void selectionChanged();

  signals:
    void changed();
    // A match: the row to make current and bring into view.
    void found(int row);
    // Opened: the query wants the keyboard.
    void opened();
    // Closed: the list wants the keyboard back.
    void dismissed();

  private:
    struct BatchResult {
        std::uint64_t generation{};
        int row{-1};
        QString error;
    };
    void cancel();
    void start(bool backwards, bool include_current);
    void pump();
    void finish();
    void invalidate();
    void setStatus(const QString& status);

    QPointer<LocalListModel> model_;
    std::function<int()> current_row_;
    std::vector<QMetaObject::Connection> connections_;
    QString query_;
    QString status_;
    bool shown_{false};
    QTimer debounce_;
    QThreadPool pool_;
    QFutureWatcher<BatchResult> watcher_;
    core::CancellationSource cancellation_;
    std::uint64_t generation_{0};
    int total_{0};
    int visited_{0};
    int cursor_{0};
    int direction_{1};
    bool wrapped_{false};
    bool searching_{false};
    bool worker_busy_{false};
};

} // namespace trackknife::bench
