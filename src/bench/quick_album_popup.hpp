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

// A popup for putting an album somewhere from the keyboard: type words, and
// every word must appear in the album's artist, title or date ("doors 67"),
// then choose what to do with it without reaching for the mouse.
class QuickAlbumPopup final : public QFrame {
    Q_OBJECT

  public:
    // `catalogue` is the library searched -- this computer's or the remote's,
    // named by `scope`.
    QuickAlbumPopup(std::shared_ptr<engine::Catalogue> catalogue, const QString& scope,
                    QWidget* parent = nullptr);
    ~QuickAlbumPopup() override;

    // Shows the popup below the top edge of `over`, centred.
    void popUp(const QWidget* over);
    [[nodiscard]] QLineEdit* input() const noexcept { return input_; }
    [[nodiscard]] QListWidget* results() const noexcept { return results_; }

  signals:
    void chosen(std::vector<persistence::LibraryEntry> albums, LocalLibraryAction action);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void search();
    void showResults();
    void choose(LocalLibraryAction action);

    struct Found {
        std::vector<persistence::LibraryEntry> albums;
        bool more{false};
        QString error;
        quint64 generation{0};
    };

    std::shared_ptr<engine::Catalogue> catalogue_;
    QLineEdit* input_{nullptr};
    QListWidget* results_{nullptr};
    QLabel* status_{nullptr};
    QTimer* debounce_{nullptr};
    QFutureWatcher<Found> watcher_;
    quint64 generation_{0};
    bool pending_{false};
    std::vector<persistence::LibraryEntry> albums_;
};

} // namespace trackknife::bench
