// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/lyrics.hpp"

#include <QElapsedTimer>
#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QListWidget;

namespace trackknife::bench {

// ADR-0276: the playing track's lyrics. Synced lyrics highlight the line
// being sung and keep it in the middle; clicking a line asks to seek there.
// Plain lyrics are text to scroll.
class LyricsPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit LyricsPanel(QWidget* parent = nullptr);

    void showLyrics(const metadata::Lyrics& lyrics);
    // A word in place of lyrics: none, on their way, or why not.
    void showMessage(const QString& message);
    // The engine's position, a moment old: carried on here while it plays,
    // so the highlight moves with the music between reports.
    void setPosition(qint64 position_ms, bool playing);

    // The highlighted line; -1, none.
    [[nodiscard]] int currentLine() const noexcept { return current_; }
    [[nodiscard]] bool synced() const noexcept { return synced_; }

  signals:
    void seekRequested(qint64 position_ms);
    void closeRequested();

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void highlight(int line);
    // Every line in the colour it has now: a scheme changed under them.
    void recolor();
    [[nodiscard]] int lineAt(qint64 position_ms) const;

    QLabel* source_{nullptr};
    QListWidget* lines_{nullptr};
    std::vector<qint64> times_;
    bool synced_{false};
    bool message_{false};
    int current_{-1};
    std::optional<qint64> reported_ms_;
    bool playing_{false};
    QElapsedTimer since_report_;
};

} // namespace trackknife::bench
