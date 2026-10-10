// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/lyrics.hpp"

#include <QElapsedTimer>
#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QListWidget;
class QVariantAnimation;

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
    // The engine's position, a moment old, as each refresh has it: carried
    // on here while it plays, so the highlight moves with the music between
    // reports. Small differences from a report are let be; a large one is a
    // seek made elsewhere.
    void setPosition(qint64 position_ms, bool playing);
    // A seek this window asked for: the highlight goes there now, and the
    // reports still on their way from before it are not believed.
    void seekTo(qint64 position_ms);
    // Where the panel takes the music to be now.
    [[nodiscard]] qint64 position() const;

    // The highlighted line; -1, none.
    [[nodiscard]] int currentLine() const noexcept { return current_; }
    [[nodiscard]] bool synced() const noexcept { return synced_; }

  signals:
    void seekRequested(qint64 position_ms);
    void closeRequested();

  protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    void follow(int line);

    // The last report seen, and the clock the highlight runs on: a position
    // at a moment, carried on while playing.
    std::optional<qint64> reported_ms_;
    bool playing_{false};
    qint64 anchor_ms_{0};
    QElapsedTimer anchor_clock_;
    // A seek asked for here, and when: until a report agrees, older ones
    // are not believed.
    std::optional<qint64> seek_ms_;
    QElapsedTimer seek_clock_;
    // Scrolling to the line sung, and a pause in it after the person
    // scrolls by hand.
    QVariantAnimation* scroll_{nullptr};
    QElapsedTimer manual_scroll_;
};

} // namespace trackknife::bench
