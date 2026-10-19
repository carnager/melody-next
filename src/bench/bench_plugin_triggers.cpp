// SPDX-License-Identifier: GPL-3.0-only

// ADR-0295: plugins' triggers -- programs run on the library's news, every
// so often, when Melody starts, or from Workspace › Run.

#include "bench/bench_main_window.hpp"

#include "workspace/library_browser.hpp"
#include "workspace/plugins.hpp"

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace trackknife::bench {

namespace {

// How long the library is to be still before its triggers run:
// $MELODY_LIBRARY_SETTLE_SECONDS, for tests, else half a minute.
[[nodiscard]] std::chrono::milliseconds librarySettle() {
    bool number = false;
    const auto seconds = qEnvironmentVariableIntValue("MELODY_LIBRARY_SETTLE_SECONDS", &number);
    return std::chrono::seconds{number && seconds >= 0 ? seconds : 30};
}

} // namespace

void BenchMainWindow::buildPluginTriggers() {
    if (library_settle_ == nullptr) {
        library_settle_ = new QTimer(this);
        library_settle_->setSingleShot(true);
        connect(library_settle_, &QTimer::timeout, this,
                [this] { runPluginTriggers(PluginTrigger::When::library_changed); });
    }
    // The menu the hand-run ones are in: Workspace › Run.
    QMenu* run_menu = findChild<QMenu*>(QStringLiteral("menu-run-plugins"));
    if (run_menu == nullptr) {
        auto* search = findChild<QAction*>(QStringLiteral("action-search-dialog"));
        for (auto* menu : menuBar()->findChildren<QMenu*>()) {
            if (search != nullptr && menu->actions().contains(search)) {
                menu->addSeparator();
                run_menu = menu->addMenu(tr("Run"));
                run_menu->setObjectName(QStringLiteral("menu-run-plugins"));
                break;
            }
        }
    }

    // What runs now, of a trigger still there, goes on: Settings saved does
    // not stop a conversion half done.
    std::vector<std::unique_ptr<PluginTriggerRun>> kept;
    const auto plugins = enabledPlugins();
    for (const auto& plugin : plugins) {
        for (const auto& trigger : plugin.triggers) {
            std::unique_ptr<PluginTriggerRun> slot;
            if (const auto found =
                    std::ranges::find_if(plugin_triggers_,
                                         [&](const auto& old) {
                                             return old && old->plugin.name == plugin.name &&
                                                    old->trigger.id == trigger.id;
                                         });
                found != plugin_triggers_.end()) {
                slot = std::move(*found);
            } else {
                slot = std::make_unique<PluginTriggerRun>();
            }
            slot->plugin = plugin;
            slot->trigger = trigger;
            delete slot->every;
            slot->every = nullptr;
            delete slot->hand;
            slot->hand = nullptr;
            auto* raw = slot.get();
            if (const auto minutes = pluginTriggerMinutes(plugin, trigger); minutes > 0) {
                slot->every = new QTimer(this);
                slot->every->setInterval(std::chrono::minutes{minutes});
                connect(slot->every, &QTimer::timeout, this,
                        [this, raw] { runPluginTrigger(*raw, PluginTrigger::When::every, {}); });
                slot->every->start();
            }
            if (run_menu != nullptr &&
                std::ranges::contains(trigger.on, PluginTrigger::When::hand)) {
                slot->hand = run_menu->addAction(trigger.label);
                slot->hand->setObjectName(
                    QStringLiteral("action-run-plugin-%1-%2").arg(plugin.name, trigger.id));
                slot->hand->setToolTip(plugin.description.isEmpty() ? plugin.title
                                                                    : plugin.description);
                connect(slot->hand, &QAction::triggered, this,
                        [this, raw] { runPluginTrigger(*raw, PluginTrigger::When::hand, {}); });
            }
            kept.push_back(std::move(slot));
        }
    }
    // The rest stop: deleting a run stops its program.
    for (auto& gone : plugin_triggers_) {
        if (gone) {
            delete gone->run.data();
            delete gone->every;
            delete gone->hand;
        }
    }
    plugin_triggers_ = std::move(kept);
    if (run_menu != nullptr) {
        run_menu->menuAction()->setVisible(!run_menu->actions().isEmpty());
    }
    // Once, when Melody starts -- not when a plugin is enabled later.
    if (!triggers_started_) {
        triggers_started_ = true;
        QTimer::singleShot(0, this, [this] { runPluginTriggers(PluginTrigger::When::start); });
    }
}

void BenchMainWindow::libraryChanged() {
    const auto wanted = std::ranges::any_of(plugin_triggers_, [](const auto& slot) {
        return std::ranges::contains(slot->trigger.on, PluginTrigger::When::library_changed);
    });
    if (wanted && library_settle_ != nullptr) {
        library_settle_->start(librarySettle());
    }
}

void BenchMainWindow::albumsAdded(const std::vector<AlbumArrival>& albums) {
    runPluginTriggers(PluginTrigger::When::albums_added, albums);
}

void BenchMainWindow::runPluginTriggers(const PluginTrigger::When when,
                                        const std::vector<AlbumArrival>& albums) {
    for (auto& slot : plugin_triggers_) {
        if (std::ranges::contains(slot->trigger.on, when)) {
            runPluginTrigger(*slot, when, albums);
        }
    }
}

void BenchMainWindow::runPluginTrigger(PluginTriggerRun& slot, const PluginTrigger::When when,
                                       std::vector<AlbumArrival> albums) {
    // Running: once more after it, with every album that came meanwhile.
    if (slot.run) {
        slot.again = when;
        std::ranges::move(albums, std::back_inserter(slot.again_albums));
        return;
    }
    // The engine holding the library, and its music folders.
    const auto library = workspace_.libraryKey();
    PluginEngine told{.name = workspace_.engineName(library), .address = {}, .here = true};
    std::vector<std::string> music_folders;
    if (const auto* link = workspace_.link(library)) {
        if (link->browser) {
            music_folders = link->browser->rootPaths();
        }
        told.here = library.isLocal();
        told.address = library.isLocal() ? QString{} : link->setting.address;
        if (link->catalogue != nullptr) {
            told.name = link->catalogue->name();
        }
    }
    std::vector<PluginTrack> tracks;
    for (const auto& album : albums) {
        for (const auto& path : album.paths) {
            tracks.push_back(PluginTrack{.path = path,
                                         .title = {},
                                         .artist = album.artist,
                                         .album = album.album,
                                         .album_artist = album.artist,
                                         .date = album.date,
                                         .track_number = {}});
        }
    }
    const auto input =
        pluginTriggerInput(slot.trigger, when, told, pluginAlbums(std::move(tracks), music_folders),
                           music_folders, pluginSettings(slot.plugin));
    const bool by_hand = when == PluginTrigger::When::hand;
    const auto label = slot.trigger.label.isEmpty() ? slot.plugin.title : slot.trigger.label;
    if (by_hand) {
        showMessage(QStringLiteral("%1…").arg(label), 0);
    }
    auto* context = new QObject(this);
    slot.run = context;
    auto* raw = &slot;
    bench::runPluginTrigger(
        slot.plugin, slot.trigger, input, context,
        [this, raw, context, label, by_hand](const PluginOutcome& outcome) {
            // Stopped: its trigger gone, or the window.
            if (raw->run != context) {
                return;
            }
            raw->run = nullptr;
            context->deleteLater();
            if (!outcome.succeeded) {
                showMessage(QStringLiteral("%1 failed: %2").arg(label, outcome.text), 10'000);
                if (notifier_ != nullptr) {
                    notifier_->announce(raw->plugin.title,
                                        QStringLiteral("%1 failed: %2").arg(label, outcome.text));
                }
            } else {
                if (raw->trigger.notify && notifier_ != nullptr && !outcome.text.isEmpty()) {
                    notifier_->announce(raw->plugin.title, outcome.text);
                }
                if (by_hand) {
                    showMessage(outcome.text.isEmpty()
                                    ? QStringLiteral("%1: done").arg(label)
                                    : QStringLiteral("%1: %2").arg(label, outcome.text),
                                6'000);
                }
            }
            if (const auto again = raw->again) {
                raw->again.reset();
                auto waiting = std::move(raw->again_albums);
                raw->again_albums.clear();
                QTimer::singleShot(
                    0, this, [this, raw, again, waiting = std::move(waiting)]() mutable {
                        // Still there: Settings saved may have taken it.
                        if (std::ranges::any_of(plugin_triggers_, [raw](const auto& other) {
                                return other.get() == raw;
                            })) {
                            runPluginTrigger(*raw, *again, std::move(waiting));
                        }
                    });
            }
        });
}

} // namespace trackknife::bench
