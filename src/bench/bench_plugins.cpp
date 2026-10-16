// SPDX-License-Identifier: GPL-3.0-only

// ADR-0288: plugin actions in the context menus -- the tracks chosen handed
// to a plugin's program, its result a notification or the clipboard.

#include "bench/bench_main_window.hpp"

#include "bench/local_library_panel.hpp"
#include "workspace/library_browser.hpp"
#include "workspace/plugins.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QPointer>
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
            inputs.push_back(pluginInput(action, told, {album}, music_folders));
        }
    } else {
        inputs.push_back(pluginInput(action, told, albums, music_folders));
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
                        for (const auto& path : paths) {
                            gathered->tracks.push_back(
                                PluginTrack{.path = path,
                                            .title = track ? entry.title : std::string{},
                                            .artist = track ? entry.artist : std::string{},
                                            .album = album && entry.album.empty() ? entry.label
                                                     : album || track             ? entry.album
                                                                                  : std::string{},
                                            .album_artist = album ? entry.artist : std::string{},
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

} // namespace trackknife::bench
