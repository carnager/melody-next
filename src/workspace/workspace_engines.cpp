// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include "bench/remote_engines.hpp"
#include "workspace/workspace_view.hpp"

#include <QCursor>
#include <QGuiApplication>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {

QString Workspace::engineName(const EngineKey& engine) const {
    if (engine.isLocal()) {
        return tr("this computer");
    }
    const auto* catalogue = catalogueOf(engine);
    return catalogue != nullptr ? catalogue->name() : tr("the remote");
}


RemoteMount Workspace::mountOf(const EngineLink& engine) const {
    for (const auto& setting : loadRemoteEngines()) {
        if (setting.address == engine.setting.address) {
            return setting.mount();
        }
    }
    return engine.setting.mount();
}


std::vector<std::string> Workspace::rootsOf(const EngineLink& engine) const {
    // Asked now, not remembered: a folder added to its library since it was
    // last asked is part of it.
    std::vector<std::string> roots;
    if (engine.catalogue == nullptr) {
        return roots;
    }
    if (auto known = engine.catalogue->open()->roots()) {
        for (auto& root : *known) {
            roots.push_back(std::move(root.raw_path));
        }
    }
    return roots;
}


// ADR-0234: a path as one engine has it, as another has it. This computer
// reaches a remote's music through its mount (RemoteMount); between two
// remotes the way is through this computer's view of both. Empty when the
// file is not reachable there.
std::optional<std::string> Workspace::crossEnginePath(const std::string& path,
                                                            const EngineKey& from,
                                                            const EngineKey& to) const {
    if (from == to) {
        return path;
    }
    // Each engine elsewhere reaches this computer through its own mount.
    std::optional<std::string> here;
    if (from.isLocal()) {
        here = path;
    } else if (const auto* source = link(from); source != nullptr) {
        here = mountOf(*source).to_local(path);
    }
    if (!here || to.isLocal()) {
        return here;
    }
    const auto* target = link(to);
    if (target == nullptr) {
        return std::nullopt;
    }
    return mountOf(*target).to_remote(*here, rootsOf(*target));
}


std::vector<std::string> Workspace::crossEnginePaths(std::vector<std::string> paths,
                                                           const EngineKey& from,
                                                           const EngineKey& to) {
    std::vector<std::string> crossed;
    crossed.reserve(paths.size());
    for (const auto& path : paths) {
        if (auto translated = crossEnginePath(path, from, to)) {
            crossed.push_back(std::move(*translated));
        }
    }
    if (const auto left = paths.size() - crossed.size(); left > 0U) {
        view_->showMessage(
            !to.isLocal()
                ? QStringLiteral("%1 of %2 tracks are not in %3's library, so it cannot play "
                                 "them; they were left out")
                      .arg(left)
                      .arg(paths.size())
                      .arg(engineName(to))
                : QStringLiteral("%1 of %2 tracks are not reachable on this computer; they were "
                                 "left out. Where the remote's music is mounted here is set in "
                                 "Settings → Engine.")
                      .arg(left)
                      .arg(paths.size()),
            10'000);
    }
    return crossed;
}


std::vector<LocalTrackRow> Workspace::crossEngineRows(std::vector<LocalTrackRow> rows,
                                                            const EngineKey& from,
                                                            const EngineKey& to) {
    std::vector<std::string> paths;
    paths.reserve(rows.size());
    for (const auto& row : rows) {
        paths.push_back(row.raw_path);
    }
    // One message for the whole move, from the path translation; rows are
    // then matched back to their translated paths in order.
    static_cast<void>(crossEnginePaths(paths, from, to));
    std::vector<LocalTrackRow> moved;
    moved.reserve(rows.size());
    for (auto& row : rows) {
        auto translated = crossEnginePath(row.raw_path, from, to);
        if (!translated) {
            continue;
        }
        if (from != to) {
            // What was known of the file on one machine says nothing of its
            // revision on the other.
            row.source_revision.reset();
        }
        row.raw_path = std::move(*translated);
        moved.push_back(std::move(row));
    }
    return moved;
}


void Workspace::renewOutdatedLocalEngine() {
    if (!localCatalogue() || !localCatalogue()->localEngineOutdated()) {
        engine_renewal_pending_ = false;
        return;
    }
    // Restarting it stops the music, so not while it plays: once it stops.
    if (localPlayback() != nullptr &&
        localPlayback()->state().status == QStringLiteral("playing")) {
        if (!engine_renewal_pending_) {
            view_->showMessage(QStringLiteral("This computer's engine is out of date; it "
                                                    "restarts when playback stops"),
                                     10'000);
        }
        engine_renewal_pending_ = true;
        return;
    }
    engine_renewal_pending_ = false;
    QGuiApplication::setOverrideCursor(QCursor{Qt::WaitCursor});
    const bool renewed = localCatalogue()->restartLocalEngine();
    QGuiApplication::restoreOverrideCursor();
    view_->showMessage(renewed ? QStringLiteral("This computer's engine was out of date and "
                                                      "has been restarted")
                                     : QStringLiteral("This computer's engine is out of date and "
                                                      "did not restart; see its log"),
                             8'000);
}


void Workspace::retireEngines() {
    for (const auto& engine : engines_) {
        if (engine->playback != nullptr) {
            engine->playback->retire();
        }
    }
    if (auto* local = catalogueOf(EngineKey::local())) {
        static_cast<void>(local->stopLocalEngine());
    }
}

} // namespace trackknife::bench
