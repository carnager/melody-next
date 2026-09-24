// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_library_panel.hpp"
#include "trackknife/persistence/local_library.hpp"

#include <QFrame>
#include <QFutureWatcher>

#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QTimer;

namespace trackknife::engine {
class Catalogue;
}

namespace trackknife::bench {

enum class QuickPickKind { album, track };

// A popup for putting an album or a track somewhere from the keyboard: type
// words -- every one must appear in its artist, title, album or date
// ("doors 67") -- then choose what to do with it without reaching for the
// mouse.
class QuickPickPopup final : public QFrame {
    Q_OBJECT

  public:
    // `catalogue` is the library searched -- this computer's or the remote's,
    // named by `scope`.
    QuickPickPopup(QuickPickKind kind, std::shared_ptr<engine::Catalogue> catalogue,
                   const QString& scope, QWidget* parent = nullptr);
    ~QuickPickPopup() override;

    // Shows the popup below the top edge of `over`, centred.
    void popUp(const QWidget* over);
    [[nodiscard]] QLineEdit* input() const noexcept { return input_; }
    [[nodiscard]] QListWidget* results() const noexcept { return results_; }

  signals:
    void chosen(std::vector<persistence::LibraryEntry> picked, LocalLibraryAction action);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void search();
    void showResults();
    void choose(LocalLibraryAction action);
    [[nodiscard]] QString idleText() const;

    struct Found {
        std::vector<persistence::LibraryEntry> entries;
        bool more{false};
        // The newest in the library, asked for with nothing typed.
        bool newest{false};
        QString error;
        quint64 generation{0};
    };

    const QuickPickKind kind_;
    std::shared_ptr<engine::Catalogue> catalogue_;
    QLineEdit* input_{nullptr};
    QListWidget* results_{nullptr};
    QLabel* status_{nullptr};
    QTimer* debounce_{nullptr};
    QFutureWatcher<Found> watcher_;
    quint64 generation_{0};
    bool pending_{false};
    std::vector<persistence::LibraryEntry> entries_;
};

} // namespace trackknife::bench
