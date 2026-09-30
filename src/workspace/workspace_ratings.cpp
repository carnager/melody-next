// SPDX-License-Identifier: GPL-3.0-only

// ADR-0179: ratings live with the engine whose library holds the track, so
// each engine's lists read and store theirs there -- one at a time, off the
// UI thread, through the engine's catalogue.

#include "workspace/workspace.hpp"

#include "bench/post_back.hpp"
#include "workspace/workspace_view.hpp"

#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {

void Workspace::refreshRatings() {
    for (const auto& engine_link : engines_) {
        const auto engine = engine_link->key;
        auto* catalogue = engine_link->catalogue.get();
        if (catalogue == nullptr) {
            continue;
        }
        QStringList hashes;
        QSet<QString> unique;
        for (const auto& tab : list_tabs_) {
            if (EngineKey::of(tab->document) != engine) {
                continue;
            }
            for (const auto& hash : tab->model->ratingHashes()) {
                if (!unique.contains(hash)) {
                    unique.insert(hash);
                    hashes.push_back(hash);
                }
            }
        }
        if (hashes.isEmpty()) {
            continue;
        }
        std::vector<std::string> keys;
        keys.reserve(static_cast<std::size_t>(hashes.size()));
        for (const auto& hash : hashes) {
            keys.push_back(hash.toStdString());
        }
        const QPointer workspace{this};
        auto library = std::shared_ptr<engine::Catalogue>{catalogue->openDeferred()};
        static_cast<void>(QtConcurrent::run(
            &catalogue_work_, [workspace, library, keys = std::move(keys), hashes, engine] {
                auto values = library->ratings(keys);
                if (!values || values->size() != static_cast<std::size_t>(hashes.size())) {
                    return;
                }
                postBack(workspace, [workspace, hashes, engine, values = std::move(*values)] {
                    QHash<QString, unsigned> ratings;
                    ratings.reserve(hashes.size());
                    for (qsizetype index = 0; index < hashes.size(); ++index) {
                        ratings.insert(hashes.at(index), values[static_cast<std::size_t>(index)]);
                    }
                    for (const auto& tab : workspace->list_tabs_) {
                        if (EngineKey::of(tab->document) == engine) {
                            tab->model->applyRatings(ratings);
                        }
                    }
                });
            }));
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
        }
        for (const auto& tab : list_tabs_) {
            if (EngineKey::of(tab->document) == engine) {
                tab->model->applyRatings(applied);
            }
        }
    }
    std::vector<std::string> keys;
    for (const auto& hash : hashes) {
        keys.push_back(hash.toStdString());
    }
    const QPointer workspace{this};
    auto library = std::shared_ptr<engine::Catalogue>{catalogue->openDeferred()};
    static_cast<void>(QtConcurrent::run(
        &catalogue_work_, [workspace, library, keys = std::move(keys), album, rating] {
            QString error;
            for (const auto& key : keys) {
                if (auto stored = library->set_rating(key, album, rating); !stored) {
                    error = QString::fromStdString(stored.error().message);
                }
            }
            postBack(workspace, [workspace, error] {
                if (!error.isEmpty()) {
                    workspace->view_->showMessage(error, 5'000);
                }
                workspace->refreshRatings();
            });
        }));
}

} // namespace trackknife::bench
