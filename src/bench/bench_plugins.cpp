// SPDX-License-Identifier: GPL-3.0-only

// ADR-0288: plugin actions in the context menus -- the tracks chosen handed
// to a plugin's program, its result a notification or the clipboard.

#include "bench/bench_main_window.hpp"

#include "bench/animated_panel_dock.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/plugin_list_window.hpp"
#include "bench/plugin_panel_view.hpp"
#include "workspace/library_browser.hpp"
#include "workspace/plugins.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QSettings>
#include <QStatusBar>

#include <memory>

namespace trackknife::bench {

void BenchMainWindow::addPluginActions(
    QMenu* menu, std::function<void(const Plugin&, const PluginAction&)> run) {
    const auto plugins = enabledPlugins();
    if (plugins.empty()) {
        return;
    }
    menu->addSeparator();
    // A few entries stand in the menu itself; more go in a submenu for each
    // plugin, under its name, so many plugins still leave a short menu.
    std::size_t count = 0U;
    for (const auto& plugin : plugins) {
        count += plugin.actions.size();
    }
    const bool grouped = count > flat_plugin_actions;
    for (const auto& plugin : plugins) {
        auto* into = menu;
        if (grouped) {
            into = menu->addMenu(plugin.title);
            into->setObjectName(QStringLiteral("menu-plugin-%1").arg(plugin.name));
        }
        for (const auto& action : plugin.actions) {
            auto* entry = into->addAction(action.label);
            entry->setObjectName(QStringLiteral("action-plugin-%1-%2").arg(plugin.name, action.id));
            entry->setToolTip(plugin.description.isEmpty() ? plugin.title : plugin.description);
            connect(entry, &QAction::triggered, this,
                    [run, plugin, action] { run(plugin, action); });
        }
    }
}

void BenchMainWindow::runPlugin(const Plugin& plugin, const PluginAction& action,
                                std::vector<PluginTrack> tracks, const EngineKey& engine) {
    if (tracks.empty()) {
        return;
    }
    std::vector<std::string> music_folders;
    PluginEngine told{.name = workspace_.engineName(engine), .address = {}, .here = true};
    if (const auto* link = workspace_.link(engine)) {
        if (link->browser) {
            music_folders = link->browser->rootPaths();
        }
        told.here = engine.isLocal();
        told.address = engine.isLocal() ? QString{} : link->setting.address;
        if (link->catalogue != nullptr) {
            told.name = link->catalogue->name();
        }
    }
    auto albums = pluginAlbums(std::move(tracks), music_folders);
    // One run per album, or one with them all.
    std::vector<QByteArray> inputs;
    if (action.scope == PluginAction::Scope::albums) {
        for (const auto& album : albums) {
            inputs.push_back(
                pluginInput(action, told, {album}, music_folders, pluginSettings(plugin)));
        }
    } else {
        inputs.push_back(pluginInput(action, told, albums, music_folders, pluginSettings(plugin)));
    }
    for (auto& input : inputs) {
        showMessage(QStringLiteral("%1…").arg(action.label), 0);
        runPluginAction(
            plugin, action, std::move(input), this,
            [this, title = plugin.title, label = action.label,
             result = action.result](const PluginOutcome& outcome) {
                if (!outcome.succeeded) {
                    showMessage(QStringLiteral("%1 failed: %2").arg(label, outcome.text), 10'000);
                    return;
                }
                // ADR-0288: rows in a window of their own.
                if (result == PluginAction::Result::list) {
                    QString problem;
                    const auto list = parsePluginList(outcome.text.toUtf8(), &problem);
                    if (!list) {
                        showMessage(QStringLiteral("%1: %2").arg(label, problem), 10'000);
                        return;
                    }
                    auto* window = new PluginListWindow(title, *list, this);
                    window->setAttribute(Qt::WA_DeleteOnClose);
                    window->show();
                    showMessage(QStringLiteral("%1: done").arg(label), 4'000);
                    return;
                }
                const bool notify = result == PluginAction::Result::notify ||
                                    result == PluginAction::Result::notify_and_copy;
                const bool copy = result == PluginAction::Result::copy ||
                                  result == PluginAction::Result::notify_and_copy;
                if (copy && !outcome.text.isEmpty()) {
                    QGuiApplication::clipboard()->setText(outcome.text);
                }
                if (notify && notifier_ != nullptr && !outcome.text.isEmpty()) {
                    notifier_->announce(title, outcome.text);
                }
                showMessage(outcome.text.isEmpty() ? QStringLiteral("%1: done").arg(label)
                            : copy ? QStringLiteral("%1: %2 (copied)").arg(label, outcome.text)
                                   : QStringLiteral("%1: %2").arg(label, outcome.text),
                            10'000);
            });
    }
}

void BenchMainWindow::addLibraryPluginActions(LocalLibraryPanel* panel, const EngineKey& engine) {
    const QPointer<LocalLibraryPanel> guard{panel};
    panel->setMenuExtras([this, guard, engine](QMenu* menu,
                                               std::vector<persistence::LibraryEntry> entries) {
        addPluginActions(menu, [this, guard, engine, entries](const Plugin& plugin,
                                                              const PluginAction& action) {
            if (!guard || entries.empty()) {
                return;
            }
            // Each entry on its own, so an album's tracks keep what its
            // entry says of it; the plugin runs once all are known.
            struct Gathered {
                std::size_t waiting{0};
                std::vector<PluginTrack> tracks;
            };
            auto gathered = std::make_shared<Gathered>();
            gathered->waiting = entries.size();
            for (const auto& entry : entries) {
                guard->browser().resolveEntries(
                    {entry}, [this, gathered, entry, plugin, action,
                              engine](const std::vector<std::string>& paths) {
                        const bool album = entry.kind == persistence::LibraryEntryKind::album;
                        const bool track = entry.kind == persistence::LibraryEntryKind::track;
                        const bool artist = entry.kind == persistence::LibraryEntryKind::artist;
                        // What the entry says of its tracks: an artist its
                        // name, an album its own, a track all of it.
                        const auto artist_name = entry.artist.empty() ? entry.label : entry.artist;
                        const auto album_name =
                            album && entry.album.empty() ? entry.label : entry.album;
                        for (const auto& path : paths) {
                            gathered->tracks.push_back(PluginTrack{
                                .path = path,
                                .title = track ? entry.title : std::string{},
                                .artist = track    ? entry.artist
                                          : artist ? artist_name
                                                   : std::string{},
                                .album = album || track ? album_name : std::string{},
                                .album_artist = album || artist ? artist_name : std::string{},
                                .date = album || track ? entry.date : std::string{},
                                .track_number = {}});
                        }
                        if (--gathered->waiting == 0U) {
                            runPlugin(plugin, action, std::move(gathered->tracks), engine);
                        }
                    });
            }
        });
    });
}

void BenchMainWindow::buildPluginPanels() {
    for (auto& slot : plugin_panels_) {
        // Stopped first, so nothing answers into what goes.
        auto* run = slot->run.data();
        slot->run = nullptr;
        delete run;
        removeDockWidget(slot->dock);
        delete slot->dock;
    }
    plugin_panels_.clear();
    QAction* lyrics = findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    QMenu* menu = nullptr;
    QAction* after_lyrics = nullptr;
    for (auto* candidate : menuBar()->findChildren<QMenu*>()) {
        const auto actions = candidate->actions();
        if (const auto at = actions.indexOf(lyrics); lyrics != nullptr && at >= 0) {
            menu = candidate;
            after_lyrics = actions.value(at + 1);
            break;
        }
    }
    for (const auto& plugin : enabledPlugins()) {
        for (const auto& panel : plugin.panels) {
            auto slot = std::make_unique<PluginPanelDock>();
            slot->plugin = plugin;
            slot->panel = panel;
            const auto id = QStringLiteral("%1-%2").arg(plugin.name, panel.id);
            auto* dock = new AnimatedPanelDock(panel.label,
                                               QStringLiteral("plugin-panels/%1").arg(id), this);
            dock->setObjectName(QStringLiteral("bench-plugin-panel-%1").arg(id));
            dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
            auto* view = new PluginPanelView(panel.label, dock);
            dock->setPanelContent(view);
            addDockWidget(Qt::RightDockWidgetArea, dock);
            slot->dock = dock;
            slot->view = view;
            auto* raw = slot.get();
            connect(view, &PluginPanelView::closeRequested, dock,
                    [dock] { dock->setVisible(false); });
            connect(view, &PluginPanelView::refreshRequested, this,
                    [this, raw] { askPluginPanel(*raw); });
            auto* toggle = dock->panelToggleAction();
            toggle->setObjectName(QStringLiteral("action-show-plugin-panel-%1").arg(id));
            toggle->setToolTip(plugin.description.isEmpty() ? plugin.title : plugin.description);
            // Opened, it asks about what plays now.
            connect(toggle, &QAction::triggered, this, [this, raw](const bool shown) {
                if (shown) {
                    askPluginPanel(*raw);
                } else {
                    raw->asked.reset();
                }
            });
            if (menu != nullptr) {
                menu->insertAction(after_lyrics, toggle);
            }
            dock->setVisible(QSettings{}
                                 .value(QStringLiteral("plugin-panels/%1/visible").arg(id), false)
                                 .toBool());
            plugin_panels_.push_back(std::move(slot));
        }
    }
    refreshPluginPanels(plugin_panel_state_);
}

void BenchMainWindow::refreshPluginPanels(const EnginePlayback::State& state) {
    plugin_panel_state_ = state;
    const auto stopped = state.status == QStringLiteral("stopped") || state.entry.isEmpty();
    const auto entry = stopped ? QString{} : state.entry;
    for (auto& slot : plugin_panels_) {
        // Hidden, it asks nothing; shown again, it asks for what plays then.
        if (slot->dock->isHidden()) {
            slot->asked.reset();
            continue;
        }
        if (slot->asked != entry) {
            askPluginPanel(*slot);
        }
    }
}

void BenchMainWindow::askPluginPanel(PluginPanelDock& slot) {
    const auto& state = plugin_panel_state_;
    const auto stopped = state.status == QStringLiteral("stopped") || state.entry.isEmpty();
    const auto entry = stopped ? QString{} : state.entry;
    slot.asked = entry;
    // A newer question: the one on its way is stopped.
    auto* previous = slot.run.data();
    slot.run = nullptr;
    delete previous;
    if (entry.isEmpty()) {
        slot.view->setSubject({});
        slot.view->showMessage(QStringLiteral("Nothing is playing"));
        return;
    }
    PluginTrack playing{.path = state.path.toStdString(),
                        .title = {},
                        .artist = {},
                        .album = {},
                        .album_artist = {},
                        .date = {},
                        .track_number = {}};
    std::optional<std::int64_t> duration;
    if (state.duration_ms > 0) {
        duration = state.duration_ms;
    }
    if (const auto* row = playingRow(entry)) {
        playing.path = row->raw_path;
        playing.title = row->title;
        playing.artist = row->artist;
        playing.album = row->album;
        playing.album_artist = row->album_artist;
        playing.date = row->date;
        playing.track_number = row->track_number;
        if (row->duration_ms) {
            duration = row->duration_ms;
        }
    }
    const auto artist = QString::fromStdString(playing.album_artist.empty() ? playing.artist
                                                                            : playing.album_artist);
    slot.view->setSubject(
        artist.isEmpty()
            ? QString::fromStdString(playing.title)
            : QStringLiteral("%1 \u2014 %2").arg(artist, QString::fromStdString(playing.title)));
    slot.view->showMessage(QStringLiteral("Asking %1\u2026").arg(slot.plugin.title));
    std::vector<std::string> music_folders;
    PluginEngine told{.name = {}, .address = {}, .here = true};
    if (const auto* link = linkOf(transport_)) {
        told.name = workspace_.engineName(link->key);
        told.here = link->key.isLocal();
        told.address = told.here ? QString{} : link->setting.address;
        if (link->browser) {
            music_folders = link->browser->rootPaths();
        }
    }
    auto* context = new QObject(this);
    slot.run = context;
    auto* raw = &slot;
    runPluginPanel(
        slot.plugin, slot.panel,
        pluginPanelInput(slot.panel, told, playing, duration, music_folders,
                         pluginSettings(slot.plugin)),
        context, [raw, context](const PluginOutcome& outcome) {
            // Asked again since, or gone: this answer is not wanted.
            if (raw->run != context) {
                return;
            }
            raw->run = nullptr;
            context->deleteLater();
            if (!outcome.succeeded) {
                raw->view->showMessage(
                    QStringLiteral("%1 failed: %2").arg(raw->plugin.title, outcome.text));
                return;
            }
            QString problem;
            const auto content = parsePluginPanel(outcome.text.toUtf8(), &problem);
            if (!content) {
                raw->view->showMessage(QStringLiteral("%1 answered what cannot be shown: %2")
                                           .arg(raw->plugin.title, problem));
                return;
            }
            raw->view->showContent(*content);
        });
}

} // namespace trackknife::bench
