// SPDX-License-Identifier: GPL-3.0-only

#include "bench/animated_panel_dock.hpp"
#include "bench/bench_main_window.hpp"
#include "bench/lyrics_panel.hpp"
#include "trackknife/metadata/lyrics.hpp"
#include "workspace/plugins.hpp"

#include <QDebug>
#include <QFutureWatcher>
#include <QKeySequence>
#include <QSettings>
#include <QSlider>
#include <QStringList>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <functional>
#include <memory>

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
    watchSidePanel(lyrics_dock_);
}

void BenchMainWindow::watchSidePanel(QDockWidget* dock) {
    connect(dock, &QDockWidget::visibilityChanged, this,
            [this] { QTimer::singleShot(0, this, &BenchMainWindow::refreshPanelsButton); });
}

void BenchMainWindow::refreshPanelsButton() {
    if (panels_button_ == nullptr) {
        return;
    }
    const auto docks = findChildren<QDockWidget*>();
    panels_button_->setChecked(std::ranges::any_of(docks, [](const QDockWidget* dock) {
        return dynamic_cast<const AnimatedPanelDock*>(dock) != nullptr && dock->isVisible();
    }));
}

void BenchMainWindow::togglePanels() {
    std::vector<AnimatedPanelDock*> panels;
    for (auto* dock : findChildren<QDockWidget*>()) {
        if (auto* panel = dynamic_cast<AnimatedPanelDock*>(dock)) {
            panels.push_back(panel);
        }
    }
    QStringList open;
    for (auto* panel : panels) {
        if (panel->isVisible()) {
            open << panel->objectName();
        }
    }
    if (!open.isEmpty()) {
        // Hidden together, remembered to come back together.
        QSettings{}.setValue(QStringLiteral("panels/open"), open);
        for (auto* panel : panels) {
            if (panel->isVisible()) {
                panel->setVisible(false);
            }
        }
    } else {
        auto wanted =
            QSettings{}.value(QStringLiteral("panels/open"), QStringList{}).toStringList();
        if (!std::ranges::any_of(panels, [&wanted](const AnimatedPanelDock* panel) {
                return wanted.contains(panel->objectName());
            })) {
            wanted = {QStringLiteral("bench-lyrics")};
        }
        for (auto* panel : panels) {
            if (wanted.contains(panel->objectName())) {
                panel->setVisible(true);
            }
        }
        QTimer::singleShot(0, this, &BenchMainWindow::tabOpenSidePanels);
    }
    QTimer::singleShot(0, this, &BenchMainWindow::refreshPanelsButton);
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
        // A newer track: what was asked of the providers is not wanted.
        auto* asking = lyrics_provider_run_.data();
        lyrics_provider_run_ = nullptr;
        delete asking;
        auto future = entry.isEmpty() ? std::nullopt : workspace_.lyricsOfPlaying(state);
        if (!future) {
            lyrics_panel_->showMessage(QStringLiteral("Nothing is playing"));
        } else {
            lyrics_panel_->showMessage(QStringLiteral("Looking for lyrics…"));
            auto* watcher = new QFutureWatcher<Workspace::LyricsAnswer>(this);
            connect(watcher, &QFutureWatcher<Workspace::LyricsAnswer>::finished, this,
                    [this, watcher, entry, state] {
                        watcher->deleteLater();
                        // Another track since: its own answer is on the way.
                        if (entry != lyrics_entry_) {
                            return;
                        }
                        const auto answer = watcher->result();
                        // ADR-0293: none read -- not there, or not readable
                        // by the engine -- the providers are asked; what
                        // the engine said is said when they find none.
                        if (!answer) {
                            askLyricsProviders(
                                entry, state,
                                answer.error().code == core::ErrorCode::unsupported
                                    ? QStringLiteral("The engine playing this does not read "
                                                     "lyrics. Update melodyd there.")
                                    : QStringLiteral("Lyrics could not be read: %1")
                                          .arg(QString::fromStdString(answer.error().message)));
                        } else if (!*answer) {
                            askLyricsProviders(entry, state, QStringLiteral("No lyrics"));
                        } else {
                            lyrics_panel_->showLyrics(**answer);
                        }
                    });
            watcher->setFuture(*future);
        }
    }
    lyrics_panel_->setPosition(state.position_ms, state.status == QStringLiteral("playing"));
}

void BenchMainWindow::askLyricsProviders(const QString& entry, const EnginePlayback::State& state,
                                         const QString& otherwise) {
    struct Asked {
        Plugin plugin;
        PluginProvider provider;
    };
    auto providers = std::make_shared<std::vector<Asked>>();
    for (const auto& plugin : enabledPlugins()) {
        for (const auto& provider : plugin.providers) {
            if (provider.provides == QStringLiteral("lyrics")) {
                providers->push_back({plugin, provider});
            }
        }
    }
    if (providers->empty()) {
        lyrics_panel_->showMessage(otherwise);
        return;
    }
    const auto told = std::make_shared<PluginPlaying>(pluginPlaying(state));
    auto* context = new QObject(this);
    lyrics_provider_run_ = context;
    // Each in turn, the next asked when one finds nothing or fails.
    auto ask = std::make_shared<std::function<void(std::size_t)>>();
    *ask = [this, providers, told, context, entry, otherwise,
            weak = std::weak_ptr{ask}](const std::size_t at) {
        if (lyrics_provider_run_ != context || entry != lyrics_entry_) {
            return;
        }
        if (at >= providers->size()) {
            lyrics_provider_run_ = nullptr;
            context->deleteLater();
            lyrics_panel_->showMessage(otherwise);
            return;
        }
        const auto& asked = (*providers)[at];
        lyrics_panel_->showMessage(QStringLiteral("Asking %1\u2026").arg(asked.plugin.title));
        runPluginProvider(
            asked.plugin, asked.provider,
            pluginProviderInput(asked.provider, told->engine, told->track, told->duration_ms,
                                told->music_folders, pluginSettings(asked.plugin)),
            context,
            [this, providers, context, entry, at,
             next = weak.lock()](const PluginOutcome& outcome) {
                if (lyrics_provider_run_ != context || entry != lyrics_entry_ || !next) {
                    return;
                }
                const auto& from = (*providers)[at];
                std::optional<std::optional<PluginLyrics>> found;
                QString problem;
                if (outcome.succeeded) {
                    found = parsePluginLyrics(outcome.text.toUtf8(), &problem);
                } else {
                    problem = outcome.text;
                }
                if (!found) {
                    qWarning().noquote()
                        << QStringLiteral("Lyrics from %1: %2").arg(from.plugin.title, problem);
                }
                if (found && *found) {
                    auto lyrics = metadata::parse_lyrics((*found)->text.toStdString());
                    if (!lyrics.lines.empty()) {
                        lyrics.source = "plugin:" + ((*found)->source.isEmpty() ? from.plugin.title
                                                                                : (*found)->source)
                                                        .toStdString();
                        lyrics_provider_run_ = nullptr;
                        context->deleteLater();
                        lyrics_panel_->showLyrics(lyrics);
                        return;
                    }
                }
                (*next)(at + 1);
            });
    };
    // Held from here by the run on its way, which holds the next turn.
    (*ask)(0);
}

} // namespace trackknife::bench
