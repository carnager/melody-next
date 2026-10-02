// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/remote_engines.hpp"
#include "workspace/workspace_view.hpp"

#include <QDir>
#include <QStandardPaths>

#include <algorithm>

namespace trackknife::bench {
namespace {

constexpr int persist_debounce_ms = 1'000;

} // namespace

Workspace::Workspace(QObject* parent) : QObject(parent) {
    layout_pushes_.setMaxThreadCount(1);
    catalogue_work_.setMaxThreadCount(1);
    // ADR-0234: this computer's engine is always the first link, connected
    // or not; remotes follow when configured.
    engines_.push_back(std::make_unique<EngineLink>());
    engines_.front()->key = EngineKey::local();
    // Up Next's rows, as every window shows them.
    up_next_local_model_ = new LocalListModel(this);
    // The playback modes and ReplayGain as they were left, whichever window.
    loadLocalPlaybackModes();
}

Workspace::~Workspace() {
    // The connections before the catalogues they were made from: one waits
    // for its work in hand, which may revive its engine through them.
    for (const auto& engine : engines_) {
        delete engine->playback;
        engine->playback = nullptr;
    }
}

void Workspace::start() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    database_path_ = std::filesystem::path{utf8Bytes(base + QStringLiteral("/lists.sqlite"))};
    rememberContinuationRules();
    connectLocalEngine();
    persistence_ = new ui::ListPersistenceService(database_path_, this);
    persistence_timer_ = new QTimer(this);
    persistence_timer_->setSingleShot(true);
    persistence_timer_->setInterval(persist_debounce_ms);
    connect(persistence_timer_, &QTimer::timeout, this, [this] {
        persistNow(false);
        refreshRatings();
    });
    persistence_->initialize([this](ui::PersistedWorkspace workspace, QString error) {
        if (!error.isEmpty()) {
            view_->showMessage(QStringLiteral("List restore failed: %1").arg(error), 5'000);
        }
        restored_track_view_layouts_.clear();
        for (const auto& preset : workspace.view_presets) {
            restored_track_view_layouts_.insert(
                displayText(preset.binding),
                QByteArray{preset.header_state.data(),
                           static_cast<qsizetype>(preset.header_state.size())});
        }
        restoreLists(std::move(workspace.lists));
        restoreUpNext();
        // After the lists, because the entry the engine names is looked for in
        // them before a tab is invented for it.
        reattachToEngine();
        // ADR-0227: the remote engines too, after the lists for the same
        // reason -- their tabs may already be among them.
        bool first = true;
        for (const auto& setting : loadRemoteEngines()) {
            connectRemoteEngine(setting, first);
            first = false;
        }
        view_->workspaceRestored(error.isEmpty());
    });
}

Workspace::EngineLink* Workspace::link(const EngineKey& key) const {
    const auto found = std::ranges::find(engines_, key, [](const auto& each) { return each->key; });
    return found != engines_.end() ? found->get() : nullptr;
}

EnginePlayback* Workspace::playbackOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->playback : nullptr;
}

CatalogueSource* Workspace::catalogueOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->catalogue.get() : nullptr;
}

Workspace::ListTab* Workspace::tabForDocument(const core::StableId& document_id) {
    if (document_id.is_nil()) {
        return nullptr;
    }
    return tabForDocument(QString::fromStdString(document_id.to_string()));
}

Workspace::ListTab* Workspace::tabForDocument(const QString& document_id) {
    for (const auto& tab : list_tabs_) {
        if (QString::fromStdString(tab->document.id.to_string()) == document_id) {
            return tab.get();
        }
    }
    if (detached_playback_ &&
        QString::fromStdString(detached_playback_->document.id.to_string()) == document_id) {
        return &*detached_playback_;
    }
    return nullptr;
}

void Workspace::schedulePersist() {
    if (persistence_timer_ != nullptr) {
        persistence_timer_->start();
    }
}

} // namespace trackknife::bench
