// SPDX-License-Identifier: GPL-3.0-only

#include "bench/animated_panel_dock.hpp"
#include "bench/bench_main_window.hpp"
#include "bench/lyrics_panel.hpp"

#include <QFutureWatcher>
#include <QKeySequence>
#include <QSettings>
#include <QSlider>

namespace trackknife::bench {

void BenchMainWindow::buildLyrics() {
    auto* panel = new AnimatedPanelDock(QStringLiteral("Lyrics"), QStringLiteral("lyrics"), this);
    lyrics_dock_ = panel;
    lyrics_dock_->setObjectName(QStringLiteral("bench-lyrics"));
    lyrics_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    lyrics_panel_ = new LyricsPanel(lyrics_dock_);
    connect(lyrics_panel_, &LyricsPanel::closeRequested, lyrics_dock_,
            [this] { lyrics_dock_->setVisible(false); });
    connect(lyrics_panel_, &LyricsPanel::seekRequested, this, [this](const qint64 position_ms) {
        if (seek_ != nullptr && seek_->isEnabled()) {
            seekToMs(position_ms);
        }
    });
    panel->setPanelContent(lyrics_panel_);
    addDockWidget(Qt::RightDockWidgetArea, lyrics_dock_);
    // ADR-0292: the side panels share their room as tabs, not one above
    // another.
    setTabPosition(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea, QTabWidget::North);

    lyrics_dock_->setVisible(QSettings{}.value(QStringLiteral("lyrics/visible"), false).toBool());
    auto* toggle = panel->panelToggleAction();
    toggle->setObjectName(QStringLiteral("action-show-lyrics"));
    toggle->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    addAction(toggle);
}

// Every seek from this window -- the slider, the desktop, a lyric line --
// reaches the Lyrics panel at once, before the engine reports it.
void BenchMainWindow::seekToMs(const qint64 position_ms) {
    workspace_.seekToMs(position_ms);
    if (lyrics_panel_ != nullptr) {
        lyrics_panel_->seekTo(position_ms);
    }
}

void BenchMainWindow::refreshLyrics(const EnginePlayback::State& state) {
    // Hidden, it asks nothing; shown again, it asks for what plays then.
    if (lyrics_panel_ == nullptr || lyrics_dock_ == nullptr || lyrics_dock_->isHidden()) {
        lyrics_entry_.reset();
        return;
    }
    const auto stopped = state.status == QStringLiteral("stopped") || state.entry.isEmpty();
    const auto entry = stopped ? QString{} : state.entry;
    if (entry != lyrics_entry_) {
        lyrics_entry_ = entry;
        auto future = entry.isEmpty() ? std::nullopt : workspace_.lyricsOfPlaying(state);
        if (!future) {
            lyrics_panel_->showMessage(QStringLiteral("Nothing is playing"));
        } else {
            lyrics_panel_->showMessage(QStringLiteral("Looking for lyrics…"));
            auto* watcher = new QFutureWatcher<Workspace::LyricsAnswer>(this);
            connect(watcher, &QFutureWatcher<Workspace::LyricsAnswer>::finished, this,
                    [this, watcher, entry] {
                        watcher->deleteLater();
                        // Another track since: its own answer is on the way.
                        if (entry != lyrics_entry_) {
                            return;
                        }
                        const auto answer = watcher->result();
                        if (!answer) {
                            lyrics_panel_->showMessage(
                                answer.error().code == core::ErrorCode::unsupported
                                    ? QStringLiteral("The engine playing this does not read "
                                                     "lyrics. Update melodyd there.")
                                    : QStringLiteral("Lyrics could not be read: %1")
                                          .arg(QString::fromStdString(answer.error().message)));
                        } else if (!*answer) {
                            lyrics_panel_->showMessage(QStringLiteral("No lyrics"));
                        } else {
                            lyrics_panel_->showLyrics(**answer);
                        }
                    });
            watcher->setFuture(*future);
        }
    }
    lyrics_panel_->setPosition(state.position_ms, state.status == QStringLiteral("playing"));
}

} // namespace trackknife::bench
