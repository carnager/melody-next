// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_library_panel.hpp"
#include "workspace/quick_pick_session.hpp"

#include <QFrame>

#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;

namespace trackknife::bench {

// A popup for putting an album or a track somewhere from the keyboard: type
// words -- every one must appear in its artist, title, album or date
// ("doors 67") -- then choose what to do with it without reaching for the
// mouse.
class QuickPickPopup final : public QFrame {
    Q_OBJECT

  public:
    // `catalogue` is the library searched -- this computer's or the remote's,
    // named by `scope`. `alone`: a window of its own, as melody-pick shows
    // it (ADR-0346) -- no tabs to open, and gone when it loses the focus.
    QuickPickPopup(QuickPickKind kind, std::shared_ptr<engine::Catalogue> catalogue,
                   const QString& scope, QWidget* parent = nullptr, bool alone = false);
    ~QuickPickPopup() override;

    // Shows the popup below the top edge of `over`, centred.
    void popUp(const QWidget* over);
    // Shows it as a window of its own, in the middle of the screen.
    void showAlone();
    // Said in place of the status line: what went wrong with a choice.
    void say(const QString& text);
    [[nodiscard]] QLineEdit* input() const noexcept { return input_; }
    [[nodiscard]] QListWidget* results() const noexcept { return results_; }

  signals:
    void chosen(std::vector<persistence::LibraryEntry> picked, LocalLibraryAction action);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void showResults();

    QuickPickSession* session_;
    QLineEdit* input_{nullptr};
    QListWidget* results_{nullptr};
    QLabel* status_{nullptr};
    const bool alone_;
    // Alone: it has had the focus, so losing it now means the user left.
    bool focused_{false};
};

} // namespace trackknife::bench
