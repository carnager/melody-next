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
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

#include <memory>

namespace trackknife::bench {

void BenchMainWindow::addPluginActions(
    QMenu* menu, std::function<void(const Plugin&, const PluginAction&)> run) {
    // Only plugins with actions: one that only fills a panel or finds
    // lyrics has nothing to offer here.
    auto plugins = enabledPlugins();
    std::erase_if(plugins, [](const Plugin& plugin) { return plugin.actions.empty(); });
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
    // Open before, open after: a panel made again is not one closed.
    QSet<QString> open;
    for (auto& slot : plugin_panels_) {
        if (slot->dock->isVisible()) {
            open.insert(slot->dock->objectName());
        }
        // Stopped first, so nothing answers into what goes.
        auto* run = slot->run.data();
        slot->run = nullptr;
        delete run;
        // Deleted, not removed first: removing closes it, and a panel
        // closed says so in the settings.
        delete slot->dock;
    }
    plugin_panels_.clear();
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
            watchSidePanel(dock);
            auto* raw = slot.get();
            // ADR-0312: asked again about the same when its answer asked,
            // if it still shows and nothing else is being asked.
            slot->again = new QTimer(view);
            slot->again->setSingleShot(true);
            connect(slot->again, &QTimer::timeout, this, [this, raw] {
                if (raw->dock->isHidden() || !raw->run.isNull()) {
                    return;
                }
                ++raw->agains;
                askPluginPanel(*raw);
            });
            // ADR-0303: pressed, asked again -- told what was pressed.
            connect(view, &PluginPanelView::actionRequested, this,
                    [this, raw](const QString& action, const QString& value) {
                        raw->action = PluginPanelAction{
                            .id = action, .value = value, .inputs = raw->view->inputs()};
                        askPluginPanel(*raw);
                    });
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
            // ADR-0299: enabled, its panel is there -- shown with the rest.
            dock->setVisible(open.contains(dock->objectName()) || panelsShown());
            plugin_panels_.push_back(std::move(slot));
        }
    }
    refreshPluginPanels(plugin_panel_state_);
    QTimer::singleShot(0, this, &BenchMainWindow::tabOpenSidePanels);
}

void BenchMainWindow::tabOpenSidePanels() {
    for (const auto area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea}) {
        QDockWidget* first = nullptr;
        for (auto* dock : findChildren<QDockWidget*>()) {
            if (dynamic_cast<AnimatedPanelDock*>(dock) == nullptr || !dock->isVisible() ||
                dockWidgetArea(dock) != area) {
                continue;
            }
            if (first == nullptr) {
                first = dock;
            } else if (!tabifiedDockWidgets(first).contains(dock)) {
                tabifyDockWidget(first, dock);
            }
        }
    }
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
    // Asked again about the same -- the room changed, a button pressed -- it
    // keeps what it shows until the answer comes, rather than flickering,
    // and where it was scrolled to.
    const bool again = slot.asked == entry && slot.view->hasContent();
    slot.asked = entry;
    slot.keep_place = again;
    if (!slot.again.isNull()) {
        slot.again->stop();
    }
    if (!again) {
        slot.agains = 0;
    }
    // A newer question: the one on its way is stopped.
    auto* previous = slot.run.data();
    slot.run = nullptr;
    delete previous;
    auto* raw = &slot;
    if (entry.isEmpty()) {
        slot.view->setSubject({});
        if (!slot.panel.always) {
            slot.view->showMessage(QStringLiteral("Nothing is playing"));
            return;
        }
        // ADR-0303: a panel of controls is asked with nothing playing too.
        auto* context = new QObject(this);
        slot.run = context;
        auto told = pluginPlaying(state);
        const auto action = std::exchange(slot.action, std::nullopt);
        runPluginPanel(slot.plugin, slot.panel,
                       pluginPanelInput(slot.panel, told.engine, std::nullopt, std::nullopt,
                                        told.music_folders, pluginSettings(slot.plugin), {},
                                        action),
                       context, [raw, context](const PluginOutcome& outcome) {
                           showPluginPanelOutcome(*raw, context, outcome);
                       });
        return;
    }
    auto told = pluginPlaying(state);
    showPluginPanelSubject(slot, told.track);
    if (!again) {
        slot.view->showMessage(QStringLiteral("Asking %1\u2026").arg(slot.plugin.title));
    }
    auto* context = new QObject(this);
    slot.run = context;
    // What plays is the engine's to say: the window's lists may not hold it
    // yet -- an album another client started is in none until its queue has
    // come -- and told no tags, the plugin would say the track has none.
    // Where the engine does not answer, the list's row is what there is.
    if (transport_ == nullptr || !transport_->active()) {
        runPluginPanelWith(*raw, context, std::move(told));
        return;
    }
    transport_->nowPlaying([this, raw, guard = QPointer<QObject>{context}, entry,
                            told = std::move(told)](std::optional<LocalTrackRow> engine) mutable {
        // Asked again since, or gone: this answer is not wanted.
        if (guard.isNull() || raw->run != guard.data()) {
            return;
        }
        if (engine && QString::fromStdString(engine->entry_id.to_string()) == entry) {
            told.track = PluginTrack{.path = std::move(engine->raw_path),
                                     .title = std::move(engine->title),
                                     .artist = std::move(engine->artist),
                                     .album = std::move(engine->album),
                                     .album_artist = std::move(engine->album_artist),
                                     .date = std::move(engine->date),
                                     .track_number = std::move(engine->track_number)};
            if (engine->duration_ms) {
                told.duration_ms = engine->duration_ms;
            }
            showPluginPanelSubject(*raw, told.track);
        }
        // ADR-0301: and what the library says of it -- its tags, and the
        // artist's albums for a panel that asks.
        auto library = workspace_.pluginLibraryOf(told.track.path, raw->panel.artist_albums);
        if (!library) {
            runPluginPanelWith(*raw, guard.data(), std::move(told));
            return;
        }
        auto* watcher = new QFutureWatcher<PluginPanelLibrary>(guard.data());
        connect(watcher, &QFutureWatcher<PluginPanelLibrary>::finished, guard.data(),
                [this, raw, guard, watcher, told = std::move(told)]() mutable {
                    watcher->deleteLater();
                    if (guard.isNull() || raw->run != guard.data()) {
                        return;
                    }
                    runPluginPanelWith(*raw, guard.data(), std::move(told), watcher->result());
                });
        watcher->setFuture(*library);
    });
}

void BenchMainWindow::showPluginPanelSubject(PluginPanelDock& slot, const PluginTrack& playing) {
    // ADR-0304: a panel asked with nothing playing too -- a listening room
    // -- is not about the track, and does not say it.
    if (slot.panel.always) {
        slot.view->setSubject({});
        return;
    }
    const auto artist = QString::fromStdString(playing.album_artist.empty() ? playing.artist
                                                                            : playing.album_artist);
    slot.view->setSubject(
        artist.isEmpty()
            ? QString::fromStdString(playing.title)
            : QStringLiteral("%1 \u2014 %2").arg(artist, QString::fromStdString(playing.title)));
}

void BenchMainWindow::runPluginPanelWith(PluginPanelDock& slot, QObject* context,
                                         PluginPlaying told, const PluginPanelLibrary& library) {
    auto* raw = &slot;
    const auto action = std::exchange(slot.action, std::nullopt);
    runPluginPanel(slot.plugin, slot.panel,
                   pluginPanelInput(slot.panel, told.engine, told.track, told.duration_ms,
                                    told.music_folders, pluginSettings(slot.plugin), library,
                                    action),
                   context, [raw, context](const PluginOutcome& outcome) {
                       showPluginPanelOutcome(*raw, context, outcome);
                   });
}

void BenchMainWindow::showPluginPanelOutcome(PluginPanelDock& slot, QObject* context,
                                             const PluginOutcome& outcome) {
    // Asked again since, or gone: this answer is not wanted.
    if (slot.run != context) {
        return;
    }
    slot.run = nullptr;
    context->deleteLater();
    if (!outcome.succeeded) {
        slot.view->showMessage(
            QStringLiteral("%1 failed: %2").arg(slot.plugin.title, outcome.text));
        return;
    }
    QString problem;
    const auto content = parsePluginPanel(outcome.text.toUtf8(), &problem);
    if (!content) {
        slot.view->showMessage(
            QStringLiteral("%1 answered what cannot be shown: %2").arg(slot.plugin.title, problem));
        return;
    }
    slot.view->showContent(*content, slot.keep_place);
    // ADR-0312: asked again about the same, while it shows, as it asked --
    // a few times running at most, so a plugin cannot keep it asking.
    constexpr int most_agains = 10;
    if (content->again_after && slot.agains < most_agains && !slot.again.isNull()) {
        slot.again->start(*content->again_after * 1000);
    }
}

BenchMainWindow::PluginPlaying BenchMainWindow::pluginPlaying(const EnginePlayback::State& state) {
    PluginPlaying told{.track = PluginTrack{.path = state.path.toStdString(),
                                            .title = {},
                                            .artist = {},
                                            .album = {},
                                            .album_artist = {},
                                            .date = {},
                                            .track_number = {}},
                       .duration_ms = {},
                       .engine = PluginEngine{.name = {}, .address = {}, .here = true},
                       .music_folders = {}};
    if (state.duration_ms > 0) {
        told.duration_ms = state.duration_ms;
    }
    if (const auto* row = state.entry.isEmpty() ? nullptr : playingRow(state.entry)) {
        told.track.path = row->raw_path;
        told.track.title = row->title;
        told.track.artist = row->artist;
        told.track.album = row->album;
        told.track.album_artist = row->album_artist;
        told.track.date = row->date;
        told.track.track_number = row->track_number;
        if (row->duration_ms) {
            told.duration_ms = row->duration_ms;
        }
    }
    if (const auto* link = linkOf(transport_)) {
        told.engine.name = workspace_.engineName(link->key);
        told.engine.here = link->key.isLocal();
        told.engine.address = told.engine.here ? QString{} : link->setting.address;
        if (link->browser) {
            told.music_folders = link->browser->rootPaths();
        }
    }
    return told;
}

} // namespace trackknife::bench
