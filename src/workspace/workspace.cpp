// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/workspace.hpp"

#include <algorithm>

namespace trackknife::bench {

Workspace::Workspace(QObject* parent) : QObject(parent) {}

Workspace::~Workspace() {
    // The connections before the catalogues they were made from: one waits
    // for its work in hand, which may revive its engine through them.
    for (const auto& engine : engines_) {
        delete engine->playback;
        engine->playback = nullptr;
    }
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
