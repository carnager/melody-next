// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/lists_catalog.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_view.hpp"

#include <QPointer>

#include <algorithm>
#include <unordered_map>

namespace trackknife::bench {

ListsCatalog::ListsCatalog(Workspace& work, QObject* parent) : QObject(parent), work_(work) {
    fetch_timer_.setSingleShot(true);
    fetch_timer_.setInterval(150);
    connect(&fetch_timer_, &QTimer::timeout, this, [this] {
        for (const auto& engine : work_.engines_) {
            auto* playback = engine->playback;
            if (playback == nullptr || !playback->active()) {
                engine->lists.reset();
                engine->lists_error = QStringLiteral("Not connected");
                continue;
            }
            const QPointer self{this};
            playback->request(
                QStringLiteral("list.all"), protocol::Json::object(),
                [self, key = engine->key](const core::Result<protocol::Json>& answer) {
                    if (!self) {
                        return;
                    }
                    auto* answered = self->work_.link(key);
                    if (answered == nullptr) {
                        return;
                    }
                    if (answer) {
                        answered->lists = answer->value("lists", std::vector<protocol::Json>{});
                        answered->lists_error.clear();
                    } else {
                        answered->lists.reset();
                        answered->lists_error = QString::fromStdString(answer.error().message);
                    }
                    self->present();
                });
        }
        present();
    });
}

void ListsCatalog::fetch() { fetch_timer_.start(); }

void ListsCatalog::present() {
    const auto playing_id = document_text(work_.playback_.anchors.document);
    const auto open_tabs = work_.view()->listsInOrder();
    groups_.clear();
    for (const auto& engine : work_.engines_) {
        Group group;
        group.engine = engine->key;
        group.name = engine->key.isLocal() ? tr("This computer")
                     : engine->catalogue   ? engine->catalogue->name()
                                           : tr("Remote");
        std::unordered_map<std::string, std::size_t> at;
        if (engine->lists) {
            for (const auto& list : *engine->lists) {
                const auto id = list.value("id", std::string{});
                at.emplace(id, group.lists.size());
                group.lists.push_back(List{
                    .id = QString::fromStdString(id),
                    .name = QString::fromStdString(list.value("name", std::string{})),
                    .saved = list.value("kind", std::string{}) == "saved",
                    .open = false,
                    .dirty = false,
                    .pinned = false,
                    .playing = false,
                    .tracks = list.value("tracks", -1),
                });
            }
        }
        // What is open here is as this window has it: newer than the engine's
        // until it is sent, and there even with the engine away.
        for (const auto* tab : open_tabs) {
            if (EngineKey::of(tab->document) != engine->key) {
                continue;
            }
            const auto id = document_text(tab->document.id);
            const auto known = at.find(id.toStdString());
            if (known == at.end()) {
                at.emplace(id.toStdString(), group.lists.size());
                group.lists.emplace_back();
                group.lists.back().id = id;
            }
            auto& list = group.lists[at.at(id.toStdString())];
            list.name = displayText(tab->document.name);
            list.saved = tab->document.kind == persistence::ListKind::saved;
            list.open = true;
            list.dirty = tab->document.dirty;
            list.pinned = tab->document.pinned;
            list.playing = id == playing_id;
            list.tracks = tab->model->rowCount();
        }
        // Saved lists first, by name: they are the ones kept for a reason.
        std::ranges::stable_sort(group.lists, [](const auto& left, const auto& right) {
            if (left.saved != right.saved) {
                return left.saved;
            }
            return left.saved && QString::localeAwareCompare(left.name, right.name) < 0;
        });
        if (group.lists.empty()) {
            group.note = engine->lists_error.isEmpty()
                             ? tr("No lists")
                             : tr("No lists: %1").arg(engine->lists_error);
        }
        groups_.push_back(std::move(group));
    }
    emit changed();
}

void ListsCatalog::open(const EngineKey& engine, const QString& id) {
    work_.openEngineList(engine, id);
}

void ListsCatalog::close(const EngineKey&, const QString& id) {
    if (auto* tab = work_.tabForDocument(id); tab != nullptr && !tab->document.pinned) {
        emit closeWanted(id);
    }
}

void ListsCatalog::save(const EngineKey& engine, const QString& id) {
    const QPointer self{this};
    work_.openEngineList(engine, id, [self] {
        if (self) {
            emit self->saveWanted();
        }
    });
}

void ListsCatalog::rename(const EngineKey& engine, const QString& id, const QString& name) {
    const auto chosen = name.trimmed();
    if (chosen.isEmpty()) {
        return;
    }
    if (auto* tab = work_.tabForDocument(id); tab != nullptr) {
        work_.renameList(*tab, chosen);
        present();
        return;
    }
    auto* playback = work_.playbackOf(engine);
    if (playback == nullptr) {
        return;
    }
    const QPointer self{this};
    playback->request(QStringLiteral("list.rename"),
                      protocol::Json{{"id", id.toStdString()}, {"name", chosen.toStdString()}},
                      [self](const core::Result<protocol::Json>& answer) {
                          if (!self) {
                              return;
                          }
                          if (!answer) {
                              self->work_.view()->showMessage(
                                  tr("Could not rename the list: %1")
                                      .arg(QString::fromStdString(answer.error().message)),
                                  5'000);
                          }
                          self->fetch();
                      });
}

void ListsCatalog::remove(const EngineKey& engine, const QString& id) {
    if (auto* open = work_.tabForDocument(id); open != nullptr) {
        // Deleted, so nothing unsaved to ask about.
        open->document.dirty = false;
        open->document.pinned = false;
        emit closeWanted(id);
    }
    auto* playback = work_.playbackOf(engine);
    if (playback == nullptr) {
        return;
    }
    const QPointer self{this};
    playback->request(QStringLiteral("list.delete"), protocol::Json{{"id", id.toStdString()}},
                      [self](const core::Result<protocol::Json>&) {
                          if (self) {
                              self->fetch();
                          }
                      });
}

} // namespace trackknife::bench
