// SPDX-License-Identifier: GPL-3.0-only

// ADR-0179: ratings live with the engine whose library holds the track, so
// each engine's lists read and store theirs there -- one at a time, off the
// UI thread, through the engine's catalogue.

#include "workspace/workspace.hpp"

#include "bench/post_back.hpp"
#include "workspace/workspace_view.hpp"

#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace trackknife::bench {

void Workspace::refreshRatings() {
    for (const auto& engine_link : engines_) {
        engine_link->ratings.clear();
    }
    refreshNewRatings();
}

void Workspace::refreshNewRatings() {
    for (const auto& engine_link : engines_) {
        auto* catalogue = engine_link->catalogue.get();
        if (catalogue == nullptr) {
            continue;
        }
        // What is known is shown at once; only what is not is asked for --
        // a list's new rows, not every row of every list each time one
        // changes. A rating stored since is told by its engine.
        std::unordered_set<std::string> unknown;
        for (const auto& tab : list_tabs_) {
            if (EngineKey::of(tab->document) == engine_link->key) {
                tab->model->adoptKnownRatings(engine_link->ratings, unknown);
            }
        }
        std::vector<std::string> keys;
        for (const auto& hash : unknown) {
            if (engine_link->ratings_asked.insert(hash).second) {
                keys.push_back(hash);
            }
        }
        if (keys.empty()) {
            continue;
        }
        const QPointer workspace{this};
        const auto* asking = engine_link.get();
        auto library = std::shared_ptr<engine::Catalogue>{catalogue->openDeferred()};
        static_cast<void>(QtConcurrent::run(
            &catalogue_work_, [workspace, library, keys = std::move(keys), asking]() mutable {
                auto values = library->ratings(keys);
                postBack(workspace,
                         [workspace, keys = std::move(keys), asking, values = std::move(values)] {
                             // Its engine still connected: it may be gone, or known by
                             // another key now.
                             const auto found = std::ranges::find_if(
                                 workspace->engines_,
                                 [asking](const auto& each) { return each.get() == asking; });
                             if (found == workspace->engines_.end()) {
                                 return;
                             }
                             auto& link = **found;
                             for (const auto& hash : keys) {
                                 link.ratings_asked.erase(hash);
                             }
                             // Not answered: asked again when next wanted.
                             if (!values || values->size() != keys.size()) {
                                 return;
                             }
                             LocalListModel::RatingsByHash answered;
                             answered.reserve(keys.size());
                             for (std::size_t index = 0; index < keys.size(); ++index) {
                                 answered.emplace(keys[index], (*values)[index]);
                                 link.ratings.insert_or_assign(keys[index], (*values)[index]);
                             }
                             for (const auto& tab : workspace->list_tabs_) {
                                 if (EngineKey::of(tab->document) == link.key) {
                                     tab->model->applyRatings(answered);
                                 }
                             }
                         });
            }));
    }
}

void Workspace::forgetRatings(const EngineKey& engine, const std::vector<std::string>& raw_paths,
                              const bool everything) {
    auto* found = link(engine);
    if (found == nullptr || found->ratings.empty()) {
        return;
    }
    if (everything) {
        found->ratings.clear();
        return;
    }
    const RawPathsAt at{raw_paths};
    for (const auto& tab : list_tabs_) {
        if (EngineKey::of(tab->document) != engine) {
            continue;
        }
        for (const auto& row : tab->model->rows()) {
            if (at.covers(row.raw_path)) {
                found->ratings.erase(row.rating_hash);
                found->ratings.erase(row.album_rating_hash);
            }
        }
    }
}

bool Workspace::canRate(const EngineKey& engine) const { return catalogueOf(engine) != nullptr; }

void Workspace::rate(const EngineKey& engine, const QStringList& hashes, const bool album,
                     const unsigned rating) {
    auto* catalogue = catalogueOf(engine);
    if (catalogue == nullptr || hashes.isEmpty()) {
        return;
    }
    // Shown at once in every list of that engine; stored behind it.
    if (!album) {
        QHash<QString, unsigned> applied;
        for (const auto& hash : hashes) {
            applied.insert(hash, rating);
            if (auto* found = link(engine); found != nullptr) {
                found->ratings.insert_or_assign(hash.toStdString(), rating);
            }
        }
        for (const auto& tab : list_tabs_) {
            if (EngineKey::of(tab->document) == engine) {
                tab->model->applyRatings(applied);
            }
        }
        // The header, and whatever else shows that engine's ratings.
        view_->engineRatingsChanged(engine, applied);
    }
    std::vector<std::string> keys;
    for (const auto& hash : hashes) {
        keys.push_back(hash.toStdString());
    }
    const QPointer workspace{this};
    auto library = std::shared_ptr<engine::Catalogue>{catalogue->openDeferred()};
    static_cast<void>(QtConcurrent::run(
        &catalogue_work_, [workspace, library, keys = std::move(keys), album, rating, engine] {
            QString error;
            for (const auto& key : keys) {
                if (auto stored = library->set_rating(key, album, rating); !stored) {
                    error = QString::fromStdString(stored.error().message);
                }
            }
            postBack(workspace, [workspace, error, keys, engine] {
                if (!error.isEmpty()) {
                    workspace->view_->showMessage(error, 5'000);
                }
                // As stored -- or not, when it could not be: asked again.
                if (auto* found = workspace->link(engine); found != nullptr) {
                    for (const auto& key : keys) {
                        found->ratings.erase(key);
                    }
                }
                workspace->refreshNewRatings();
            });
        }));
}

} // namespace trackknife::bench
